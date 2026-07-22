// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/hfs/file.c
 *
 * File operations: open/release/fsync and iomap-based read/write/seek
 */

#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/mount.h>
#include <linux/iomap.h>

#include "hfs_fs.h"
#include "iomap.h"

static int hfs_file_open(struct inode *inode, struct file *file)
{
	if (HFS_IS_RSRC(inode))
		inode = HFS_I(inode)->rsrc_inode;
	if (!(file->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;
	atomic_inc(&HFS_I(inode)->opencnt);
	file->f_mode |= FMODE_CAN_ODIRECT;
	return 0;
}

static int hfs_file_release(struct inode *inode, struct file *file)
{
	//struct super_block *sb = inode->i_sb;

	if (HFS_IS_RSRC(inode))
		inode = HFS_I(inode)->rsrc_inode;
	if (atomic_dec_and_test(&HFS_I(inode)->opencnt)) {
		inode_lock(inode);
		hfs_file_truncate(inode);
		//if (inode->i_flags & S_DEAD) {
		//	hfs_delete_cat(inode->i_ino, HFSPLUS_SB(sb).hidden_dir, NULL);
		//	hfs_delete_inode(inode);
		//}
		inode_unlock(inode);
	}
	return 0;
}

static int hfs_file_fsync(struct file *filp, loff_t start, loff_t end,
			  int datasync)
{
	struct inode *inode = filp->f_mapping->host;
	struct super_block *sb;
	int ret, err;

	ret = file_write_and_wait_range(filp, start, end);
	if (ret)
		return ret;

	/*
	 * This does not need inode_lock: write_inode_now()/the MDB flush
	 * below use their own dedicated locking. Taking inode_lock here as
	 * well would deadlock against callers that invoke fsync while
	 * already holding it themselves, such as swapon()'s
	 * setup_swap_extents().
	 */

	/* sync the inode to buffers */
	ret = write_inode_now(inode, 0);

	/* sync the superblock to buffers */
	sb = inode->i_sb;
	flush_delayed_work(&HFS_SB(sb)->mdb_work);
	/* .. finally sync the buffers to disk */
	err = sync_blockdev(sb->s_bdev);
	if (!ret)
		ret = err;
	return ret;
}

/*
 * hfs_fallback_buffered_write() - fall back to buffered I/O for the
 * tail of a write that iomap_dio_rw() could not perform directly
 * (unaligned tail, or no blocks could be mapped without allocation
 * outside the direct path).
 */
static ssize_t hfs_fallback_buffered_write(struct kiocb *iocb,
					   struct iov_iter *from)
{
	loff_t offset = iocb->ki_pos, end;
	ssize_t written;
	int ret;

	iocb->ki_flags &= ~IOCB_DIRECT;

	written = iomap_file_buffered_write(iocb, from,
					    &hfs_write_iomap_ops,
					    NULL, NULL);
	if (written < 0)
		return written;

	end = iocb->ki_pos + written - 1;
	ret = filemap_write_and_wait_range(iocb->ki_filp->f_mapping,
					   offset, end);
	if (ret)
		return -EIO;

	invalidate_mapping_pages(iocb->ki_filp->f_mapping,
				 offset >> PAGE_SHIFT,
				 end >> PAGE_SHIFT);

	return written;
}

static ssize_t hfs_dio_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret;

	ret = iomap_dio_rw(iocb, from,
			   &hfs_write_iomap_ops,
			   &hfs_write_dio_ops,
			   0, NULL, 0);
	if (ret == -ENOTBLK)
		ret = 0;
	else if (ret < 0)
		return ret;

	if (iov_iter_count(from)) {
		ssize_t written;

		written = hfs_fallback_buffered_write(iocb, from);
		if (written < 0)
			return written;
		ret += written;
	}

	return ret;
}

static ssize_t hfs_file_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct hfs_sb_info *sbi = HFS_SB(inode->i_sb);
	loff_t total_capacity;
	ssize_t ret;
	int err;

	inode_lock(inode);

	ret = generic_write_checks(iocb, iter);
	if (ret <= 0)
		goto unlock;

	/*
	 * HFS has no sparse files, so a write starting at or beyond the
	 * volume's actual capacity would force hfs_iomap_cont_expand()
	 * below to zero-fill the whole gap and exhaust free space, turning
	 * into -ENOSPC instead of -EFBIG. Reject it up front, mirroring
	 * what used to live in hfs_write_begin() before regular file I/O
	 * moved to the iomap path (that check is now unreachable for this
	 * path, since iomap_file_buffered_write()/iomap_dio_rw() never call
	 * into aops->write_begin).
	 */
	total_capacity = (loff_t)sbi->fs_ablocks * sbi->alloc_blksz;
	if (iocb->ki_pos >= total_capacity) {
		ret = -EFBIG;
		goto unlock;
	}

	err = file_modified(file);
	if (err) {
		ret = err;
		goto unlock;
	}

	/*
	 * HFS has no sparse files: a write starting beyond the current
	 * end of file must first zero-fill (and physically allocate) the
	 * gap. i_size has to cover the gap before we zero it, otherwise
	 * iomap_zero_range() will warn that the zeroed folios are beyond
	 * i_size and won't be written back.
	 */
	if (iocb->ki_pos > i_size_read(inode)) {
		loff_t old_size = i_size_read(inode);
		loff_t new_size = iocb->ki_pos;

		/*
		 * A concurrent overlapping AIO direct write further ahead in
		 * the file may already have advanced hip->phys_size past this
		 * write's start (mapping/allocation happens synchronously
		 * at submission time, serialized by inode_lock, well before
		 * that other write's data actually lands on disk). Never
		 * drop i_size below phys_size in that case: doing so makes
		 * this write's own tail look like it extends past EOF to
		 * iomap_dio_bio_iter(), which then zero-fills the trailing
		 * partial block "past EOF" - clobbering the other write's
		 * still in-flight data in the same shared block.
		 *
		 * Restricted to O_DIRECT: phys_size is rounded up to the fs
		 * block size (see hfs_write_iomap_end()), so for ordinary
		 * buffered writes - which are fully synchronous under
		 * inode_lock and never race with a concurrent completion -
		 * using it here would inflate i_size to a rounded value
		 * instead of the exact requested size.
		 */
		if (iocb->ki_flags & IOCB_DIRECT)
			new_size = max_t(loff_t, new_size,
					  HFS_I(inode)->phys_size);

		i_size_write(inode, new_size);
		err = hfs_iomap_cont_expand(inode, iocb->ki_pos);
		if (err) {
			i_size_write(inode, old_size);
			ret = err;
			goto unlock;
		}
		mark_inode_dirty(inode);
	} else if ((iocb->ki_flags & IOCB_DIRECT) &&
		   HFS_I(inode)->phys_size > i_size_read(inode)) {
		/*
		 * This write's own start doesn't extend past i_size, so the
		 * gap-filling branch above didn't run - but a different,
		 * concurrent overlapping AIO write further ahead in the file
		 * may have already pushed phys_size past this write's own
		 * end (mapping happens synchronously at submission time,
		 * serialized by inode_lock, well before that other write's
		 * data actually lands on disk). Same reasoning as above:
		 * leaving i_size behind phys_size here makes this write's
		 * own tail look like it extends past EOF to
		 * iomap_dio_bio_iter(), which then zero-fills the trailing
		 * partial block "past EOF" - clobbering the other write's
		 * still in-flight data in the same shared block.
		 */
		i_size_write(inode, HFS_I(inode)->phys_size);
		mark_inode_dirty(inode);
	}

	if (iocb->ki_flags & IOCB_DIRECT)
		ret = hfs_dio_write_iter(iocb, iter);
	else {
		ret = iomap_file_buffered_write(iocb, iter,
						&hfs_write_iomap_ops,
						NULL, NULL);
	}

unlock:
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);

	return ret;
}

static ssize_t hfs_file_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	inode_lock_shared(inode);

	if (iocb->ki_flags & IOCB_DIRECT) {
		file_accessed(iocb->ki_filp);
		ret = iomap_dio_rw(iocb, iter,
				   &hfs_iomap_ops,
				   NULL, 0, NULL, 0);
	} else
		ret = generic_file_read_iter(iocb, iter);

	inode_unlock_shared(inode);

	return ret;
}

static loff_t hfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;

	switch (whence) {
	case SEEK_HOLE:
		inode_lock_shared(inode);
		offset = iomap_seek_hole(inode, offset, &hfs_iomap_ops);
		inode_unlock_shared(inode);
		break;
	case SEEK_DATA:
		inode_lock_shared(inode);
		offset = iomap_seek_data(inode, offset, &hfs_iomap_ops);
		inode_unlock_shared(inode);
		break;
	default:
		return generic_file_llseek(file, offset, whence);
	}

	if (offset < 0)
		return offset;

	return vfs_setpos(file, offset, inode->i_sb->s_maxbytes);
}

const struct file_operations hfs_file_operations = {
	.llseek		= hfs_file_llseek,
	.read_iter	= hfs_file_read_iter,
	.write_iter	= hfs_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.fsync		= hfs_file_fsync,
	.open		= hfs_file_open,
	.release	= hfs_file_release,
};
