// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/hfsplus/iomap.c
 *
 * iomap callback functions for the hfsplus filesystem
 */

#include <linux/iomap.h>
#include <linux/pagemap.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"
#include "iomap.h"

static int __hfsplus_iomap_begin(struct inode *inode, loff_t offset,
				 loff_t length, unsigned int flags,
				 struct iomap *iomap, bool may_alloc)
{
	struct super_block *sb = inode->i_sb;
	struct hfsplus_sb_info *sbi = HFSPLUS_SB(sb);
	struct hfsplus_inode_info *hip = HFSPLUS_I(inode);
	u32 ablock, dblock, max_blocks;
	loff_t ablock_offset, ablock_bytes;
	loff_t block_start;
	bool is_new;
	int err;

	if (!may_alloc) {
		/* Completely beyond EOF. Treat as hole */
		if (i_size_read(inode) <= offset) {
			iomap->type = IOMAP_HOLE;
			iomap->addr = IOMAP_NULL_ADDR;
			iomap->offset = offset;
			iomap->length = length;
			return 0;
		}

		/* Clamp length if the requested range goes beyond i_size */
		if (offset + length > i_size_read(inode)) {
			loff_t i_size = i_size_read(inode);
			unsigned int blocksize = i_blocksize(inode);
			length = round_up(i_size, blocksize) - offset;
		}
	}

	ablock = offset >> sbi->alloc_blksz_shift;

	err = hfsplus_map_extent(inode, ablock, may_alloc, &dblock,
				 &max_blocks, NULL);
	if (err)
		return err;

	ablock_offset = offset & (sbi->alloc_blksz - 1);
	ablock_bytes = (loff_t)max_blocks << sbi->alloc_blksz_shift;

	length = min_t(loff_t, length, ablock_bytes - ablock_offset);

	/*
	 * HFS+ has no on-disk "unwritten extent" bit, so a clump allocated
	 * by hfsplus_file_extend() is never physically zeroed: blocks past
	 * hip->phys_size (the physically-initialized high-water mark) hold
	 * stale disk content until IOMAP_F_NEW tells iomap core to
	 * zero-fill them instead of reading them in. A single extend() can
	 * cover more than one folio's worth of blocks, so a call for a
	 * later folio within that same still-uninitialized clump would
	 * find the allocation already done and wrongly skip IOMAP_F_NEW.
	 * Key it off phys_size instead of offset.
	 *
	 * Buffered writes apply this call's IOMAP_F_NEW to every block of
	 * the containing folio (see iomap_block_needs_zeroing() /
	 * __iomap_write_begin() in fs/iomap/buffered-io.c), not just
	 * [offset, offset+length). phys_size is a byte-granular high-water
	 * mark and can land mid-block, so a write starting exactly at
	 * phys_size can share its block with earlier, already-valid data.
	 * Decide is_new from the block containing offset, not offset
	 * itself, so that case isn't flagged new and doesn't get the
	 * earlier real data in the same block zeroed out from under it.
	 * O_DIRECT offsets are always block-aligned, so this rounding is a
	 * no-op there and the multi-chunk splitting below (stopping the
	 * mapped range at phys_size so the remainder gets its own,
	 * correctly-flagged iomap_begin() call) is unaffected.
	 */
	block_start = round_down(offset, i_blocksize(inode));
	is_new = may_alloc && block_start >= hip->phys_size;
	if (may_alloc && !is_new && offset < hip->phys_size)
		length = min_t(loff_t, length, hip->phys_size - offset);

	iomap->bdev = sb->s_bdev;
	iomap->offset = offset;
	iomap->length = length;
	iomap->addr = hfsplus_ablock_to_phys_bytes(sb, dblock) + ablock_offset;
	iomap->type = IOMAP_MAPPED;
	iomap->flags = IOMAP_F_MERGED;

	if (is_new)
		iomap->flags |= IOMAP_F_NEW;

	return 0;
}

static int hfsplus_iomap_begin(struct inode *inode, loff_t offset,
				loff_t length, unsigned int flags,
				struct iomap *iomap, struct iomap *srcmap)
{
	return __hfsplus_iomap_begin(inode,
				     offset, length, flags,
				     iomap, false);
}

static int hfsplus_write_iomap_begin(struct inode *inode, loff_t offset,
				     loff_t length, unsigned int flags,
				     struct iomap *iomap, struct iomap *srcmap)
{
	return __hfsplus_iomap_begin(inode,
				     offset, length, flags,
				     iomap, true);
}

const struct iomap_ops hfsplus_iomap_ops = {
	.iomap_begin = hfsplus_iomap_begin,
};

/*
 * hfsplus_write_iomap_end()
 *
 * Advance the allocated-and-zeroed high-water mark
 * (hip->phys_size / hip->fs_blocks) to cover the newly written range.
 */
static int hfsplus_write_iomap_end(struct inode *inode, loff_t pos,
				   loff_t length, ssize_t written,
				   unsigned int flags, struct iomap *iomap)
{
	struct hfsplus_inode_info *hip = HFSPLUS_I(inode);
	struct super_block *sb = inode->i_sb;
	loff_t end;
	bool dirtied = false;

	if (!written)
		return 0;

	end = round_up(pos + written, sb->s_blocksize);

	if (hip->phys_size < end) {
		inode_add_bytes(inode, end - hip->phys_size);
		hip->phys_size = end;
		hip->fs_blocks = end >> sb->s_blocksize_bits;
		dirtied = true;
	}

	if (dirtied)
		mark_inode_dirty(inode);

	return written;
}

const struct iomap_ops hfsplus_write_iomap_ops = {
	.iomap_begin	= hfsplus_write_iomap_begin,
	.iomap_end	= hfsplus_write_iomap_end,
};

/*
 * hfsplus_iomap_cont_expand()
 *
 * Zero-extend the backing store from the current phys_size up to 'size'.
 * Used both by hfsplus_setattr() and by hfsplus_file_truncate().
 */
int hfsplus_iomap_cont_expand(struct inode *inode, loff_t size)
{
	struct hfsplus_inode_info *hip = HFSPLUS_I(inode);
	loff_t start = hip->phys_size;

	if (size <= start)
		return 0;

	return iomap_zero_range(inode, start, size - start, NULL,
				&hfsplus_write_iomap_ops, NULL, NULL);
}

/*
 * hfsplus_writeback_range() - map folio during writeback
 *
 * Called for each folio during writeback. If the folio falls outside
 * the current iomap, remaps by calling __hfsplus_iomap_begin() again.
 */
static ssize_t hfsplus_writeback_range(struct iomap_writepage_ctx *wpc,
					struct folio *folio, u64 offset,
					unsigned int len, u64 end_pos)
{
	int err;

	if (offset < wpc->iomap.offset ||
	    offset >= wpc->iomap.offset + wpc->iomap.length) {
		err = __hfsplus_iomap_begin(wpc->inode,
					    offset, len, 0,
					    &wpc->iomap, false);
		if (err)
			return err;
	}

	return iomap_add_to_ioend(wpc, folio, offset, end_pos, len);
}

const struct iomap_writeback_ops hfsplus_writeback_ops = {
	.writeback_range	= hfsplus_writeback_range,
	.writeback_submit	= iomap_ioend_writeback_submit,
};

/*
 * hfsplus_file_write_dio_end_io() - Direct I/O write completion handler
 */
static int hfsplus_file_write_dio_end_io(struct kiocb *iocb, ssize_t size,
					 int error, unsigned int flags)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (error)
		return error;

	if (size && i_size_read(inode) < iocb->ki_pos + size) {
		i_size_write(inode, iocb->ki_pos + size);
		mark_inode_dirty(inode);
	}

	return 0;
}

const struct iomap_dio_ops hfsplus_write_dio_ops = {
	.end_io		= hfsplus_file_write_dio_end_io,
};

int hfsplus_iomap_swap_activate(struct swap_info_struct *sis,
				 struct file *file, sector_t *span)
{
	return iomap_swapfile_activate(sis, file, span, &hfsplus_iomap_ops);
}
