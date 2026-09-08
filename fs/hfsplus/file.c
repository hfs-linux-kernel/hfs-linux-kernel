// SPDX-License-Identifier: GPL-2.0
/*
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
	 * Sync inode metadata into the catalog and extent trees.
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
 * hfsplus_fallback_buffered_write() - fall back to buffered I/O for the
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
					struct iov_iter *from,
					unsigned int dio_flags)
{
	ssize_t ret;

	ret = iomap_dio_rw(iocb, from,
			   &hfsplus_write_iomap_ops,
			   &hfsplus_write_dio_ops,
			   dio_flags, NULL, 0);
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

	if (iocb->ki_pos > i_size_read(inode)) {
		loff_t old_size = i_size_read(inode);

		/*
		 * Do not promote the new i_size to HFSPLUS_I(inode)->phys_size
		 * here: phys_size is legitimately allowed to sit ahead of
		 * i_size, rounded up to the end of the last block a prior
		 * extending truncate created, and that gap is not part of the
		 * file. i_size must become exactly iocb->ki_pos, matching what
		 * hfsplus_iomap_cont_expand() below zero-fills up to; the
		 * generic iomap_dio_end_io() completion path (hooked up as
		 * hfsplus_write_dio_ops.end_io) then correctly extends i_size
		 * further, to this write's exact end, once its data actually
		 * lands. A prior version of this code promoted i_size to
		 * phys_size here too, silently growing the file past
		 * iocb->ki_pos into that rounding gap.
		 */
		i_size_write(inode, iocb->ki_pos);
		err = hfsplus_iomap_cont_expand(inode, iocb->ki_pos);
		if (err) {
			i_size_write(inode, old_size);
			ret = err;
			goto unlock;
		}
		mark_inode_dirty(inode);
	}

	/*
	 * Do not otherwise touch i_size here just because
	 * HFSPLUS_I(inode)->phys_size > i_size_read(inode) and this is a
	 * direct write: phys_size is legitimately allowed to sit ahead of
	 * i_size, rounded up to the end of the last block a prior extending
	 * truncate created. That gap is not part of the file; nothing about
	 * an unrelated, fully in-bounds write justifies exposing it.
	 *
	 * IOCB_DIRECT is not a reliable signal that this is a deliberate
	 * direct write to begin with: copy_file_range() on a file with no
	 * ->copy_file_range/->remap_file_range falls back to
	 * do_splice_direct() -> iter_file_splice_write(), whose kiocb
	 * inherits IOCB_DIRECT from the destination fd's flags regardless
	 * of how small or in-bounds the copy is. A prior version of this
	 * function promoted i_size to phys_size in that case too, silently
	 * growing the file past what the copy actually touched.
	 */

	if (iocb->ki_flags & IOCB_DIRECT) {
		unsigned int dio_flags = 0;

		/*
		 * __hfsplus_iomap_begin() decides IOMAP_F_NEW (and hence
		 * whether the generic iomap direct-I/O code will zero-fill
		 * the unwritten tail of a partially-written block) by
		 * comparing against hip->phys_size, which only advances at
		 * I/O completion. Two direct writes to the same inode that
		 * are both in flight at once can each reach a block neither
		 * has finished populating yet, each see it as new, and each
		 * try to zero-fill the part they don't personally cover --
		 * racing against the other write's real data for those same
		 * bytes, with whichever completes last silently winning.
		 *
		 * inode_lock already serializes entry to this function per
		 * inode, so the mapping decisions above never race with each
		 * other. The gap is that this function normally returns (and
		 * releases inode_lock) as soon as the write is submitted,
		 * without waiting for it to actually complete and advance
		 * phys_size. Forcing a synchronous wait here for any write
		 * that reaches past the current phys_size closes that gap:
		 * the next write can't even start deciding IOMAP_F_NEW for
		 * this inode until this one's completion has genuinely
		 * updated phys_size. Writes that stay within the existing
		 * phys_size can never hit IOMAP_F_NEW in the first place, so
		 * they keep full async concurrency.
		 */
		if (iocb->ki_pos + iov_iter_count(iter) >
		    HFSPLUS_I(inode)->phys_size)
			dio_flags |= IOMAP_DIO_FORCE_WAIT;

		ret = hfsplus_dio_write_iter(iocb, iter, dio_flags);
	} else {
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
