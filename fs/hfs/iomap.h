/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  linux/fs/hfs/iomap.h
 *
 * iomap callback declarations for the hfs filesystem
 */

#ifndef _LINUX_HFS_IOMAP_H
#define _LINUX_HFS_IOMAP_H

extern const struct iomap_ops hfs_iomap_ops;
extern const struct iomap_ops hfs_write_iomap_ops;
extern const struct iomap_writeback_ops hfs_writeback_ops;
extern const struct iomap_dio_ops hfs_write_dio_ops;

int hfs_iomap_cont_expand(struct inode *inode, loff_t size);
int hfs_iomap_swap_activate(struct swap_info_struct *sis,
			    struct file *file, sector_t *span);

#endif /* _LINUX_HFS_IOMAP_H */
