// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/hfsplus/file.c
 *
 * File operations: open/release/fsync and iomap-based read/write/seek
 */

#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/mount.h>
#include <linux/iomap.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"
#include "iomap.h"

static int hfsplus_file_open(struct inode *inode, struct file *file)
{
	if (HFSPLUS_IS_RSRC(inode))
		inode = HFSPLUS_I(inode)->rsrc_inode;
	if (!(file->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EOVERFLOW;
	atomic_inc(&HFSPLUS_I(inode)->opencnt);
	file->f_mode |= FMODE_CAN_ODIRECT;
	return 0;
}

static int hfsplus_file_release(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;

	if (HFSPLUS_IS_RSRC(inode))
		inode = HFSPLUS_I(inode)->rsrc_inode;
	if (atomic_dec_and_test(&HFSPLUS_I(inode)->opencnt)) {
		inode_lock(inode);
		hfsplus_file_truncate(inode);
		if (inode->i_flags & S_DEAD) {
			hfsplus_delete_cat(inode->i_ino,
					   HFSPLUS_SB(sb)->hidden_dir, NULL);
			hfsplus_delete_inode(inode);
		}
		inode_unlock(inode);
	}
	return 0;
}

int hfsplus_file_fsync(struct file *file, loff_t start, loff_t end,
		       int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct hfsplus_inode_info *hip = HFSPLUS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(inode->i_sb);
	struct hfsplus_vh *vhdr = sbi->s_vhdr;
	int error = 0, error2;

	hfs_dbg("inode->i_ino %llu, start %llu, end %llu\n",
		inode->i_ino, start, end);

	error = file_write_and_wait_range(file, start, end);
	if (error)
		return error;

	/*
	 * Sync inode metadata into the catalog and extent trees. This does
	 * not need inode_lock: the catalog/extent b-trees, allocation file
	 * and volume header are already serialized by their own dedicated
	 * locks (extents_lock, tree_lock, alloc_mutex, vh_mutex). Taking
	 * inode_lock here as well would deadlock against callers that
	 * invoke fsync while already holding it themselves, such as
	 * swapon()'s setup_swap_extents().
	 */
	sync_inode_metadata(inode, 1);

	/*
	 * And explicitly write out the btrees.
	 */
	if (test_and_clear_bit(HFSPLUS_I_CAT_DIRTY,
				&HFSPLUS_I(HFSPLUS_CAT_TREE_I(sb))->flags)) {
		clear_bit(HFSPLUS_I_CAT_DIRTY, &hip->flags);
		error = filemap_write_and_wait(sbi->cat_tree->inode->i_mapping);
	}

	if (test_and_clear_bit(HFSPLUS_I_EXT_DIRTY,
				&HFSPLUS_I(HFSPLUS_EXT_TREE_I(sb))->flags)) {
		clear_bit(HFSPLUS_I_EXT_DIRTY, &hip->flags);
		error2 =
			filemap_write_and_wait(sbi->ext_tree->inode->i_mapping);
		if (!error)
			error = error2;
	}

	if (sbi->attr_tree) {
		if (test_and_clear_bit(HFSPLUS_I_ATTR_DIRTY,
				&HFSPLUS_I(HFSPLUS_ATTR_TREE_I(sb))->flags)) {
			clear_bit(HFSPLUS_I_ATTR_DIRTY, &hip->flags);
			error2 =
				filemap_write_and_wait(
					    sbi->attr_tree->inode->i_mapping);
			if (!error)
				error = error2;
		}
	} else {
		if (test_and_clear_bit(HFSPLUS_I_ATTR_DIRTY, &hip->flags))
			pr_err("sync non-existent attributes tree\n");
	}

	if (test_and_clear_bit(HFSPLUS_I_ALLOC_DIRTY,
				&HFSPLUS_I(sbi->alloc_file)->flags)) {
		clear_bit(HFSPLUS_I_ALLOC_DIRTY, &hip->flags);
		error2 = filemap_write_and_wait(sbi->alloc_file->i_mapping);
		if (!error)
			error = error2;
	}

	mutex_lock(&sbi->vh_mutex);
	hfsplus_prepare_volume_header_for_commit(vhdr);
	mutex_unlock(&sbi->vh_mutex);

	error2 = hfsplus_commit_superblock(inode->i_sb);
	if (!error)
		error = error2;

	if (!test_bit(HFSPLUS_SB_NOBARRIER, &sbi->flags))
		blkdev_issue_flush(inode->i_sb->s_bdev);

	return error;
}

/*
 * hfsplus_fallback_buffered_write - fall back to buffered I/O for the
 * tail of a write that iomap_dio_rw() could not perform directly
 * (unaligned tail, or no blocks could be mapped without allocation
 * outside the direct path).
 */
static ssize_t hfsplus_fallback_buffered_write(struct kiocb *iocb,
						struct iov_iter *from)
{
	loff_t offset = iocb->ki_pos, end;
	ssize_t written;
	int ret;

	iocb->ki_flags &= ~IOCB_DIRECT;

	written = iomap_file_buffered_write(iocb, from,
					    &hfsplus_write_iomap_ops,
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

static ssize_t hfsplus_dio_write_iter(struct kiocb *iocb,
					struct iov_iter *from)
{
	ssize_t ret;

	ret = iomap_dio_rw(iocb, from,
			   &hfsplus_write_iomap_ops,
			   &hfsplus_write_dio_ops,
			   0, NULL, 0);
	if (ret == -ENOTBLK)
		ret = 0;
	else if (ret < 0)
		return ret;

	if (iov_iter_count(from)) {
		ssize_t written;

		written = hfsplus_fallback_buffered_write(iocb, from);
		if (written < 0)
			return written;
		ret += written;
	}

	return ret;
}

static ssize_t hfsplus_file_write_iter(struct kiocb *iocb,
					struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(inode->i_sb);
	loff_t total_capacity;
	ssize_t ret;
	int err;

	inode_lock(inode);

	ret = generic_write_checks(iocb, iter);
	if (ret <= 0)
		goto unlock;

	/*
	 * HFS+ has no sparse files, so a write starting at or beyond the
	 * volume's actual capacity would force hfsplus_iomap_cont_expand()
	 * below to zero-fill the whole gap and exhaust free space, turning
	 * into -ENOSPC instead of -EFBIG. Reject it up front, mirroring
	 * what used to live in hfsplus_write_begin() before regular file
	 * I/O moved to the iomap path (that check is now unreachable for
	 * this path, since iomap_file_buffered_write()/iomap_dio_rw() never
	 * call into aops->write_begin).
	 */
	total_capacity = (loff_t)sbi->total_blocks << sbi->alloc_blksz_shift;
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
	 * HFS+ has no sparse files: a write starting beyond the current
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
		 * block size (see hfsplus_write_iomap_end()), so for ordinary
		 * buffered writes - which are fully synchronous under
		 * inode_lock and never race with a concurrent completion -
		 * using it here would inflate i_size to a rounded value
		 * instead of the exact requested size.
		 */
		if (iocb->ki_flags & IOCB_DIRECT)
			new_size = max_t(loff_t, new_size,
					  HFSPLUS_I(inode)->phys_size);

		i_size_write(inode, new_size);
		err = hfsplus_iomap_cont_expand(inode, iocb->ki_pos);
		if (err) {
			i_size_write(inode, old_size);
			ret = err;
			goto unlock;
		}
		mark_inode_dirty(inode);
	} else if ((iocb->ki_flags & IOCB_DIRECT) &&
		   HFSPLUS_I(inode)->phys_size > i_size_read(inode)) {
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
		i_size_write(inode, HFSPLUS_I(inode)->phys_size);
		mark_inode_dirty(inode);
	}

	if (iocb->ki_flags & IOCB_DIRECT)
		ret = hfsplus_dio_write_iter(iocb, iter);
	else {
		ret = iomap_file_buffered_write(iocb, iter,
						&hfsplus_write_iomap_ops,
						NULL, NULL);
	}

unlock:
	inode_unlock(inode);

	if (ret > 0)
		ret = generic_write_sync(iocb, ret);

	return ret;
}

static ssize_t hfsplus_file_read_iter(struct kiocb *iocb,
					struct iov_iter *iter)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	inode_lock_shared(inode);

	if (iocb->ki_flags & IOCB_DIRECT) {
		file_accessed(iocb->ki_filp);
		ret = iomap_dio_rw(iocb, iter,
				   &hfsplus_iomap_ops,
				   NULL, 0, NULL, 0);
	} else
		ret = generic_file_read_iter(iocb, iter);

	inode_unlock_shared(inode);

	return ret;
}

static loff_t hfsplus_file_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;

	switch (whence) {
	case SEEK_HOLE:
		inode_lock_shared(inode);
		offset = iomap_seek_hole(inode, offset, &hfsplus_iomap_ops);
		inode_unlock_shared(inode);
		break;
	case SEEK_DATA:
		inode_lock_shared(inode);
		offset = iomap_seek_data(inode, offset, &hfsplus_iomap_ops);
		inode_unlock_shared(inode);
		break;
	default:
		return generic_file_llseek(file, offset, whence);
	}

	if (offset < 0)
		return offset;

	return vfs_setpos(file, offset, inode->i_sb->s_maxbytes);
}

const struct file_operations hfsplus_file_operations = {
	.llseek		= hfsplus_file_llseek,
	.read_iter	= hfsplus_file_read_iter,
	.write_iter	= hfsplus_file_write_iter,
	.mmap_prepare	= generic_file_mmap_prepare,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.fsync		= hfsplus_file_fsync,
	.open		= hfsplus_file_open,
	.release	= hfsplus_file_release,
	.unlocked_ioctl = hfsplus_ioctl,
};
