/*
 *  linux/fs/hfs/hfs_fs.h
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 * This file may be distributed under the terms of the GNU General Public License.
 */

#ifndef _LINUX_HFS_FS_H
#define _LINUX_HFS_FS_H

#include <linux/slab.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/workqueue.h>

#include <asm/byteorder.h>
#include <linux/uaccess.h>

#include "hfs.h"

/*
 * struct hfs_inode_info
 *
 * The HFS-specific part of a Linux (struct inode)
 */
struct hfs_inode_info {
	atomic_t opencnt;

	unsigned int flags;

	/* to deal with localtime ugliness */
	int tz_secondswest;

	struct hfs_cat_key cat_key;

	struct inode *rsrc_inode;

	struct mutex extents_lock;

	u16 alloc_blocks, clump_blocks;
	sector_t fs_blocks;
	/* Allocation extents from catlog record or volume header */
	hfs_extent_rec first_extents;
	u16 first_blocks;
	hfs_extent_rec cached_extents;
	u16 cached_start, cached_blocks;

	loff_t phys_size;
	struct inode vfs_inode;
};

#define HFS_FLG_RSRC		0x0001
#define HFS_FLG_EXT_DIRTY	0x0002
#define HFS_FLG_EXT_NEW		0x0004

#define HFS_IS_RSRC(inode)	(HFS_I(inode)->flags & HFS_FLG_RSRC)

/*
 * struct hfs_sb_info
 *
 * The HFS-specific part of a Linux (struct super_block)
 */
struct hfs_sb_info {
	struct mutex mdb_lock;			/* MDB operations lock */
	struct buffer_head *mdb_bh;		/* The hfs_buffer
						   holding the real
						   superblock (aka VIB
						   or MDB) */
	unsigned int mdb_offset;		/* byte offset of the MDB
						   sector within mdb_bh's
						   data */
	struct hfs_mdb *mdb;			/* in-memory copy of the MDB */
	struct buffer_head *alt_mdb_bh;		/* The hfs_buffer holding
						   the alternate superblock */
	unsigned int alt_mdb_offset;		/* byte offset of the alternate
						   MDB sector within
						   alt_mdb_bh's data */
	struct hfs_mdb *alt_mdb;		/* in-memory copy of the
						   alternate MDB */
	__be32 *bitmap;				/* The page holding the
						   allocation bitmap */
	struct hfs_btree *ext_tree;			/* Information about
						   the extents b-tree */
	struct hfs_btree *cat_tree;			/* Information about
						   the catalog b-tree */
	atomic64_t file_count;			/* The number of
						   regular files in
						   the filesystem */
	atomic64_t folder_count;		/* The number of
						   directories in the
						   filesystem */
	atomic64_t next_id;			/* The next available
						   file id number */
	u32 clumpablks;				/* The number of allocation
						   blocks to try to add when
						   extending a file */
	u32 fs_start;				/* The first 512-byte
						   block represented
						   in the bitmap */
	u32 part_start;
	u16 root_files;				/* The number of
						   regular
						   (non-directory)
						   files in the root
						   directory */
	u16 root_dirs;				/* The number of
						   directories in the
						   root directory */
	u16 fs_ablocks;				/* The number of
						   allocation blocks
						   in the filesystem */
	u16 free_ablocks;			/* the number of unused
						   allocation blocks
						   in the filesystem */
	u32 alloc_blksz;			/* The size of an
						   "allocation block" */
	int s_quiet;				/* Silent failure when
						   changing owner or mode? */
	__be32 s_type;				/* Type for new files */
	__be32 s_creator;			/* Creator for new files */
	umode_t s_file_umask;			/* The umask applied to the
						   permissions on all files */
	umode_t s_dir_umask;			/* The umask applied to the
						   permissions on all dirs */
	kuid_t s_uid;				/* The uid of all files */
	kgid_t s_gid;				/* The gid of all files */

	int session, part;
	struct nls_table *nls_io, *nls_disk;
	struct mutex bitmap_lock;
	unsigned long flags;
	u16 blockoffset;
	int fs_div;
	struct super_block *sb;
	int work_queued;		/* non-zero delayed work is queued */
	struct delayed_work mdb_work;	/* MDB flush delayed work */
	spinlock_t work_lock;		/* protects mdb_work and work_queued */
};

#define HFS_FLG_BITMAP_DIRTY	0
#define HFS_FLG_MDB_DIRTY	1
#define HFS_FLG_ALT_MDB_DIRTY	2

/* bitmap.c */
extern u32 hfs_vbm_search_free(struct super_block *sb, u32 goal, u32 *num_bits);
extern int hfs_clear_vbm_bits(struct super_block *sb, u16 start, u16 count);

/* catalog.c */
extern int hfs_cat_keycmp(const btree_key *key1, const btree_key *key2);
struct hfs_find_data;
extern int hfs_cat_find_brec(struct super_block *sb, u32 cnid,
			     struct hfs_find_data *fd);
extern int hfs_cat_create(u32 cnid, struct inode *dir,
			  const struct qstr *str, struct inode *inode);
extern int hfs_cat_delete(u32 cnid, struct inode *dir, const struct qstr *str);
extern int hfs_cat_move(u32 cnid, struct inode *src_dir,
			const struct qstr *src_name,
			struct inode *dst_dir,
			const struct qstr *dst_name);
extern void hfs_cat_build_key(struct super_block *sb, btree_key *key,
			      u32 parent, const struct qstr *name);

/*
 * Validate the CNID of a catalog record.
 */
static inline bool hfs_is_valid_cnid(u32 cnid, u8 type)
{
	if (likely(cnid >= HFS_FIRSTUSER_CNID))
		return true;

	switch (cnid) {
	case HFS_ROOT_CNID:
		return type == HFS_CDR_DIR;
	case HFS_EXT_CNID:
	case HFS_CAT_CNID:
		return type == HFS_CDR_FIL;
	default:
		return false;
	}
}

/* dir.c */
extern const struct file_operations hfs_dir_operations;
extern const struct inode_operations hfs_dir_inode_operations;

/* extent.c */
extern int hfs_ext_keycmp(const btree_key *key1, const btree_key *key2);
extern u16 hfs_ext_find_block(struct hfs_extent *ext, u16 off);
extern int hfs_free_fork(struct super_block *sb,
			 struct hfs_cat_file *file, int type);
extern int hfs_ext_write_extent(struct inode *inode);
extern int hfs_extend_file(struct inode *inode);
extern void hfs_file_truncate(struct inode *inode);

extern int hfs_get_block(struct inode *inode, sector_t block,
			 struct buffer_head *bh_result, int create);

/* inode.c */
extern const struct address_space_operations hfs_aops;
extern const struct address_space_operations hfs_btree_aops;

struct file_kattr;
int hfs_fileattr_get(struct dentry *dentry, struct file_kattr *fa);
int hfs_write_begin(const struct kiocb *iocb, struct address_space *mapping,
		    loff_t pos, unsigned int len, struct folio **foliop,
		    void **fsdata);
extern struct inode *hfs_new_inode(struct inode *dir, const struct qstr *name,
				   umode_t mode);
extern void hfs_inode_write_fork(struct inode *inode, struct hfs_extent *ext,
				 __be32 *log_size, __be32 *phys_size);
extern int hfs_write_inode(struct inode *inode, struct writeback_control *wbc);
extern int hfs_inode_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			     struct iattr *attr);
time64_t hfs_cat_read_mtime(hfs_cat_rec *rec);
void hfs_cat_write_mtime(hfs_cat_rec *rec, time64_t ut);
extern void hfs_inode_read_fork(struct inode *inode, struct hfs_extent *ext,
				__be32 __log_size, __be32 phys_size,
				u32 clump_size);
extern struct inode *hfs_iget(struct super_block *sb, struct hfs_cat_key *key,
				hfs_cat_rec *rec);
extern void hfs_evict_inode(struct inode *inode);
extern void hfs_delete_inode(struct inode *inode);

/* attr.c */
extern const struct xattr_handler * const hfs_xattr_handlers[];

/* mdb.c */
extern bool is_hfs_cnid_counts_valid(struct super_block *sb);
extern int hfs_mdb_get(struct super_block *sb);
extern int hfs_mdb_commit(struct super_block *sb);
extern void hfs_mdb_close(struct super_block *sb);
extern void hfs_mdb_put(struct super_block *sb);

/* part_tbl.c */
extern int hfs_part_find(struct super_block *sb,
			 sector_t *part_start, sector_t *part_size);

/* string.c */
extern const struct dentry_operations hfs_dentry_operations;

extern int hfs_hash_dentry(const struct dentry *dentry, struct qstr *this);
extern int hfs_strcmp(const unsigned char *s1, unsigned int len1,
		      const unsigned char *s2, unsigned int len2);
extern int hfs_compare_dentry(const struct dentry *dentry,
				unsigned int len, const char *str,
				const struct qstr *name);

/* trans.c */
extern void hfs_asc2mac(struct super_block *sb,
			struct hfs_name *out, const struct qstr *in);
extern int hfs_mac2asc(struct super_block *sb,
			char *out, const struct hfs_name *in);

/* super.c */
extern void hfs_mark_mdb_dirty(struct super_block *sb);

/*
 * time helpers: convert between 1904-base and 1970-base timestamps
 *
 * HFS keeps timestamps as unsigned 32-bit number of seconds since
 * January 1, 1904 in local time. The on-disk timestamps cover the range
 * from HFS_MIN_TIMESTAMP_SECS (Jan. 1, 1904) till HFS_MAX_TIMESTAMP_SECS
 * (Feb. 6, 2040) shifted by the local time zone offset. The timestamps
 * out of this range are clamped.
 */
static inline time64_t __hfs_m_to_utime(__be32 mt)
{
	time64_t ut = (time64_t)be32_to_cpu(mt) - HFS_UTC_OFFSET;

	return ut + sys_tz.tz_minuteswest * 60;
}

static inline __be32 __hfs_u_to_mtime(time64_t ut)
{
	ut -= sys_tz.tz_minuteswest * 60;
	ut = clamp_t(time64_t, ut,
		     HFS_MIN_TIMESTAMP_SECS, HFS_MAX_TIMESTAMP_SECS);
	ut += HFS_UTC_OFFSET;

	return cpu_to_be32(lower_32_bits(ut));
}

/*
 * Linux specific extension of HFS on-disk layout that makes possible
 * to support timestamps after HFS_MAX_TIMESTAMP_SECS.
 *
 * If the modification date cannot be represented by the 32-bit on-disk
 * field, then the maximal value (HFS_EXT_TIMESTAMP_MARK) is stored into
 * MdDat of the catalog record and the real value is stored into
 * the spare bytes of the same catalog record as big-endian 64-bit
 * number of seconds since January 1, 1904 (local time):
 *
 *   file record:      FndrInfo.fdIconID and FndrInfo.fdUnused[0..5]
 *   directory record: Resrv[0..7]
 *
 * fsck_hfs does not check these bytes. Another implementation that
 * updates MdDat removes the mark. As a result, the stale value in
 * the spare bytes is ignored.
 */
#define HFS_EXT_TIMESTAMP_MARK		cpu_to_be32(U32_MAX)
#define HFS_EXT_TIMESTAMP_SIZE		sizeof(__be64)
/* settimeofday() limits sys_tz.tz_minuteswest by +/-15 hours */
#define HFS_MAX_TZ_SECS			(15 * 60 * 60)
/* maximal 64-bit on-disk value that can be converted into time64_t */
#define HFS_EXT_MAX_MAC_DATE		((u64)TIME64_MAX - HFS_MAX_TZ_SECS)
#define HFS_EXT_MAX_TIMESTAMP_SECS	\
	((time64_t)HFS_EXT_MAX_MAC_DATE - HFS_UTC_OFFSET - HFS_MAX_TZ_SECS)

static inline bool hfs_mt_is_ext_timestamp(__be32 mt)
{
	return mt == HFS_EXT_TIMESTAMP_MARK;
}

static inline bool hfs_ext_mt_is_valid(u64 mt)
{
	return mt > U32_MAX && mt <= HFS_EXT_MAX_MAC_DATE;
}

static inline time64_t __hfs_ext_m_to_utime(u64 mt)
{
	time64_t ut = (time64_t)mt - HFS_UTC_OFFSET;

	return ut + sys_tz.tz_minuteswest * 60;
}

static inline u64 __hfs_u_to_ext_mtime(time64_t ut)
{
	ut = clamp_t(time64_t, ut,
		     HFS_MIN_TIMESTAMP_SECS - HFS_MAX_TZ_SECS,
		     HFS_EXT_MAX_TIMESTAMP_SECS);
	ut -= sys_tz.tz_minuteswest * 60;
	ut += HFS_UTC_OFFSET;

	return ut < 0 ? 0 : ut;
}
#define HFS_I(inode)	(container_of(inode, struct hfs_inode_info, vfs_inode))
#define HFS_SB(sb)	((struct hfs_sb_info *)(sb)->s_fs_info)

#define hfs_mtime()		__hfs_u_to_mtime(ktime_get_real_seconds())

static inline const char *hfs_mdb_name(struct super_block *sb)
{
	return sb->s_id;
}

static inline void hfs_bitmap_dirty(struct super_block *sb)
{
	set_bit(HFS_FLG_BITMAP_DIRTY, &HFS_SB(sb)->flags);
	hfs_mark_mdb_dirty(sb);
}

#define sb_bread512(sb, sec, data) ({			\
	struct buffer_head *__bh;			\
	sector_t __block;				\
	loff_t __start;					\
	int __offset;					\
							\
	__start = (loff_t)(sec) << HFS_SECTOR_SIZE_BITS;\
	__block = __start >> (sb)->s_blocksize_bits;	\
	__offset = __start & ((sb)->s_blocksize - 1);	\
	__bh = sb_bread((sb), __block);			\
	if (likely(__bh != NULL))			\
		data = (void *)(__bh->b_data + __offset);\
	else						\
		data = NULL;				\
	__bh;						\
})

#endif
