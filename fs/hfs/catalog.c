/*
 *  linux/fs/hfs/catalog.c
 *
 * Copyright (C) 1995-1997  Paul H. Hargrove
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 * This file may be distributed under the terms of the GNU General Public License.
 *
 * This file contains the functions related to the catalog B-tree.
 *
 * Cache code shamelessly stolen from
 *     linux/fs/inode.c Copyright (C) 1991, 1992  Linus Torvalds
 *     re-shamelessly stolen Copyright (C) 1997 Linus Torvalds
 */

#include "hfs_fs.h"
#include "btree.h"

/*
 * hfs_cat_build_key()
 *
 * Given the ID of the parent and the name build a search key.
 */
void hfs_cat_build_key(struct super_block *sb, btree_key *key, u32 parent, const struct qstr *name)
{
	key->cat.reserved = 0;
	key->cat.ParID = cpu_to_be32(parent);
	if (name) {
		hfs_asc2mac(sb, &key->cat.CName, name);
		key->key_len = 6 + key->cat.CName.len;
	} else {
		memset(&key->cat.CName, 0, sizeof(struct hfs_name));
		key->key_len = 6;
	}
}

static int hfs_cat_build_record(hfs_cat_rec *rec, u32 cnid, struct inode *inode)
{
	__be32 mtime = hfs_mtime();

	memset(rec, 0, sizeof(*rec));
	if (S_ISDIR(inode->i_mode)) {
		rec->type = HFS_CDR_DIR;
		rec->dir.DirID = cpu_to_be32(cnid);
		rec->dir.CrDat = mtime;
		rec->dir.MdDat = mtime;
		rec->dir.BkDat = 0;
		rec->dir.UsrInfo.frView = cpu_to_be16(0xff);
		return sizeof(struct hfs_cat_dir);
	} else {
		/* init some fields for the file record */
		rec->type = HFS_CDR_FIL;
		rec->file.Flags = HFS_FIL_USED | HFS_FIL_THD;
		if (!(inode->i_mode & S_IWUSR))
			rec->file.Flags |= HFS_FIL_LOCK;
		rec->file.FlNum = cpu_to_be32(cnid);
		rec->file.CrDat = mtime;
		rec->file.MdDat = mtime;
		rec->file.BkDat = 0;
		rec->file.UsrWds.fdType = HFS_SB(inode->i_sb)->s_type;
		rec->file.UsrWds.fdCreator = HFS_SB(inode->i_sb)->s_creator;
		return sizeof(struct hfs_cat_file);
	}
}

static int hfs_cat_build_thread(struct super_block *sb,
				hfs_cat_rec *rec, int type,
				u32 parentid, const struct qstr *name)
{
	rec->type = type;
	memset(rec->thread.reserved, 0, sizeof(rec->thread.reserved));
	rec->thread.ParID = cpu_to_be32(parentid);
	hfs_asc2mac(sb, &rec->thread.CName, name);
	return sizeof(struct hfs_cat_thread);
}

/*
 * create_entry()
 *
 * Add a new file or directory to the catalog B-tree and
 * return a (struct hfs_cat_entry) for it in '*result'.
 */
int hfs_cat_create(u32 cnid, struct inode *dir, const struct qstr *str, struct inode *inode)
{
	struct hfs_find_data fd;
	struct super_block *sb;
	union hfs_cat_rec entry;
	int entry_size;
	int err;

	hfs_dbg("name %s, cnid %u, i_nlink %d\n",
		str->name, cnid, inode->i_nlink);
	if (dir->i_size >= HFS_MAX_VALENCE)
		return -ENOSPC;

	sb = dir->i_sb;
	err = hfs_find_init(HFS_SB(sb)->cat_tree, &fd);
	if (err)
		return err;

	/*
	 * Fail early and avoid ENOSPC during the btree operations. We may
	 * have to split the root node at most once.
	 */
	err = hfs_bmap_reserve(fd.tree, 2 * fd.tree->depth);
	if (err)
		goto err2;

	hfs_cat_build_key(sb, fd.search_key, cnid, NULL);
	entry_size = hfs_cat_build_thread(sb, &entry, S_ISDIR(inode->i_mode) ?
			HFS_CDR_THD : HFS_CDR_FTH,
			dir->i_ino, str);
	err = hfs_brec_find(&fd);
	if (err != -ENOENT) {
		if (!err)
			err = -EEXIST;
		goto err2;
	}
	err = hfs_brec_insert(&fd, &entry, entry_size);
	if (err)
		goto err2;

	hfs_cat_build_key(sb, fd.search_key, dir->i_ino, str);
	entry_size = hfs_cat_build_record(&entry, cnid, inode);
	err = hfs_brec_find(&fd);
	if (err != -ENOENT) {
		/* panic? */
		if (!err)
			err = -EEXIST;
		goto err1;
	}
	err = hfs_brec_insert(&fd, &entry, entry_size);
	if (err)
		goto err1;

	dir->i_size++;
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	hfs_find_exit(&fd);
	return 0;

err1:
	hfs_cat_build_key(sb, fd.search_key, cnid, NULL);
	if (!hfs_brec_find(&fd))
		hfs_brec_remove(&fd);
err2:
	hfs_find_exit(&fd);
	return err;
}

/*
 * hfs_cat_compare()
 *
 * Description:
 *   This is the comparison function used for the catalog B-tree.  In
 *   comparing catalog B-tree entries, the parent id is the most
 *   significant field (compared as unsigned ints).  The name field is
 *   the least significant (compared in "Macintosh lexical order",
 *   see hfs_strcmp() in string.c)
 * Input Variable(s):
 *   struct hfs_cat_key *key1: pointer to the first key to compare
 *   struct hfs_cat_key *key2: pointer to the second key to compare
 * Output Variable(s):
 *   NONE
 * Returns:
 *   int: negative if key1<key2, positive if key1>key2, and 0 if key1==key2
 * Preconditions:
 *   key1 and key2 point to "valid" (struct hfs_cat_key)s.
 * Postconditions:
 *   This function has no side-effects
 */
int hfs_cat_keycmp(const btree_key *key1, const btree_key *key2)
{
	__be32 k1p, k2p;

	k1p = key1->cat.ParID;
	k2p = key2->cat.ParID;

	if (k1p != k2p)
		return be32_to_cpu(k1p) < be32_to_cpu(k2p) ? -1 : 1;

	return hfs_strcmp(key1->cat.CName.name, key1->cat.CName.len,
			  key2->cat.CName.name, key2->cat.CName.len);
}

static int hfs_cat_validate_found_cnid(struct hfs_find_data *fd, u32 cnid)
{
	hfs_cat_rec rec;
	u32 found_cnid;
	int rec_len;

	rec_len = fd->entrylength;
	if (rec_len <= 0 || (size_t)rec_len > sizeof(rec))
		return -EIO;

	memset(&rec, 0, sizeof(rec));
	hfs_bnode_read(fd->bnode, &rec, fd->entryoffset, rec_len);

	switch (rec.type) {
	case HFS_CDR_FIL:
		if ((size_t)rec_len != sizeof(struct hfs_cat_file))
			return -EIO;
		found_cnid = be32_to_cpu(rec.file.FlNum);
		break;
	case HFS_CDR_DIR:
		if ((size_t)rec_len != sizeof(struct hfs_cat_dir))
			return -EIO;
		found_cnid = be32_to_cpu(rec.dir.DirID);
		break;
	default:
		return -EIO;
	}

	if (found_cnid != cnid)
		return -EIO;

	return 0;
}

/* Try to get a catalog entry for given catalog id */
// move to read_super???
int hfs_cat_find_brec(struct super_block *sb, u32 cnid,
		      struct hfs_find_data *fd)
{
	hfs_cat_rec rec;
	int res, len, type;

	hfs_cat_build_key(sb, fd->search_key, cnid, NULL);
	res = hfs_brec_read(fd, &rec, sizeof(rec));
	if (res)
		return res;

	type = rec.type;
	if (type != HFS_CDR_THD && type != HFS_CDR_FTH) {
		pr_err("found bad thread record in catalog\n");
		return -EIO;
	}

	fd->search_key->cat.ParID = rec.thread.ParID;
	len = fd->search_key->cat.CName.len = rec.thread.CName.len;
	if (len > HFS_NAMELEN) {
		pr_err("bad catalog namelength\n");
		return -EIO;
	}
	memcpy(fd->search_key->cat.CName.name, rec.thread.CName.name, len);

	res = hfs_brec_find(fd);
	if (res)
		return res;

	return hfs_cat_validate_found_cnid(fd, cnid);
}

/*
 * hfs_find_max_CNID()
 *
 * Find the biggest CNID of the file and folder records in Catalog Tree.
 * Catalog records are sorted by parent CNID and name, so the record with
 * the biggest CNID can be anywhere in the tree. Moreover, file thread
 * records are optional in HFS. As a result, all leaf nodes have to be
 * scanned.
 */
static
int hfs_find_max_CNID(struct hfs_btree *cat_tree, u32 *max_cnid)
{
	struct hfs_bnode *node;
	u32 node_id = cat_tree->leaf_head;
	u32 nodes_count = 0;

	*max_cnid = 0;

	while (node_id) {
		int i;

		if (++nodes_count > cat_tree->node_count) {
			pr_err("leaf nodes chain is corrupted\n");
			return -EIO;
		}

		node = hfs_bnode_find(cat_tree, node_id);
		if (IS_ERR(node)) {
			pr_err("fail to find leaf node: node ID %u\n",
				node_id);
			return PTR_ERR(node);
		}

		if (node->type != HFS_NODE_LEAF) {
			pr_err("node is corrupted: node ID %u, type %d\n",
				node_id, node->type);
			hfs_bnode_put(node);
			return -EIO;
		}

		for (i = 0; i < node->num_recs; i++) {
			hfs_cat_rec rec;
			u16 off, len, keylen;
			int entrylength;
			u32 found_cnid;

			len = hfs_brec_lenoff(node, i, &off);
			keylen = hfs_brec_keylen(node, i);
			if (keylen == 0 || keylen >= len) {
				pr_err("fail to get the keylen: node_id %u, record index %d\n",
				       node_id, i);
				hfs_bnode_put(node);
				return -EIO;
			}

			entrylength = len - keylen;
			if (entrylength > sizeof(rec))
				entrylength = sizeof(rec);

			memset(&rec, 0, sizeof(rec));
			hfs_bnode_read(node, &rec, off + keylen, entrylength);

			if (rec.type == HFS_CDR_DIR)
				found_cnid = be32_to_cpu(rec.dir.DirID);
			else if (rec.type == HFS_CDR_FIL)
				found_cnid = be32_to_cpu(rec.file.FlNum);
			else
				continue;

			if (found_cnid > *max_cnid)
				*max_cnid = found_cnid;
		}

		node_id = node->next;
		hfs_bnode_put(node);
	}

	return 0;
}

/*
 * hfs_correct_next_unused_CNID()
 *
 * Correct the next unused CNID of Catalog Tree.
 */
static
int hfs_correct_next_unused_CNID(struct super_block *sb, u32 cnid)
{
	u32 max_cnid;
	s64 next_id;
	int err;

	hfs_dbg("cnid %u, next_id %lld\n",
		cnid, atomic64_read(&HFS_SB(sb)->next_id));

	if ((cnid + 1) < atomic64_read(&HFS_SB(sb)->next_id)) {
		/* next ID should be unchanged */
		return 0;
	}

	err = hfs_find_max_CNID(HFS_SB(sb)->cat_tree, &max_cnid);
	if (err)
		return err;

	hfs_dbg("max_cnid %u\n", max_cnid);

	next_id = max_t(s64, (s64)max_cnid + 1, HFS_FIRSTUSER_CNID);
	atomic64_cmpxchg(&HFS_SB(sb)->next_id, (s64)cnid + 1, next_id);

	return 0;
}

/*
 * hfs_cat_delete()
 *
 * Delete the indicated file or directory.
 * The associated thread is also removed unless ('with_thread'==0).
 */
int hfs_cat_delete(u32 cnid, struct inode *dir, const struct qstr *str)
{
	struct super_block *sb;
	struct hfs_find_data fd;
	int res, type;

	hfs_dbg("name %s, cnid %u\n", str ? str->name : NULL, cnid);
	sb = dir->i_sb;
	res = hfs_find_init(HFS_SB(sb)->cat_tree, &fd);
	if (res)
		return res;

	hfs_cat_build_key(sb, fd.search_key, dir->i_ino, str);
	res = hfs_brec_find(&fd);
	if (res)
		goto out;

	type = hfs_bnode_read_u8(fd.bnode, fd.entryoffset);
	if (type == HFS_CDR_FIL) {
		struct hfs_cat_file file;
		hfs_bnode_read(fd.bnode, &file, fd.entryoffset, sizeof(file));
		if (be32_to_cpu(file.FlNum) == cnid) {
#if 0
			hfs_free_fork(sb, &file, HFS_FK_DATA);
#endif
			hfs_free_fork(sb, &file, HFS_FK_RSRC);
		}
	}

	res = hfs_brec_remove(&fd);
	if (res)
		goto out;

	hfs_cat_build_key(sb, fd.search_key, cnid, NULL);
	res = hfs_brec_find(&fd);
	if (!res) {
		res = hfs_brec_remove(&fd);
		if (res)
			goto out;
	}

	dir->i_size--;
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);

	res = hfs_correct_next_unused_CNID(sb, cnid);
	if (res)
		goto out;

	res = 0;
out:
	hfs_find_exit(&fd);

	return res;
}

/*
 * hfs_cat_move()
 *
 * Rename a file or directory, possibly to a new directory.
 * If the destination exists it is removed and a
 * (struct hfs_cat_entry) for it is returned in '*result'.
 */
int hfs_cat_move(u32 cnid, struct inode *src_dir, const struct qstr *src_name,
		 struct inode *dst_dir, const struct qstr *dst_name)
{
	struct super_block *sb;
	struct hfs_find_data src_fd, dst_fd;
	union hfs_cat_rec entry;
	int entry_size, type;
	int err;

	hfs_dbg("cnid %u - (ino %llu, name %s) - (ino %llu, name %s)\n",
		cnid, src_dir->i_ino, src_name->name,
		dst_dir->i_ino, dst_name->name);
	sb = src_dir->i_sb;
	err = hfs_find_init(HFS_SB(sb)->cat_tree, &src_fd);
	if (err)
		return err;
	dst_fd = src_fd;

	/*
	 * Fail early and avoid ENOSPC during the btree operations. We may
	 * have to split the root node at most once.
	 */
	err = hfs_bmap_reserve(src_fd.tree, 2 * src_fd.tree->depth);
	if (err)
		goto out;

	/* find the old dir entry and read the data */
	hfs_cat_build_key(sb, src_fd.search_key, src_dir->i_ino, src_name);
	err = hfs_brec_find(&src_fd);
	if (err)
		goto out;
	if (src_fd.entrylength > sizeof(entry) || src_fd.entrylength < 0) {
		err = -EIO;
		goto out;
	}

	hfs_bnode_read(src_fd.bnode, &entry, src_fd.entryoffset,
			    src_fd.entrylength);

	/* create new dir entry with the data from the old entry */
	hfs_cat_build_key(sb, dst_fd.search_key, dst_dir->i_ino, dst_name);
	err = hfs_brec_find(&dst_fd);
	if (err != -ENOENT) {
		if (!err)
			err = -EEXIST;
		goto out;
	}

	err = hfs_brec_insert(&dst_fd, &entry, src_fd.entrylength);
	if (err)
		goto out;
	dst_dir->i_size++;
	inode_set_mtime_to_ts(dst_dir, inode_set_ctime_current(dst_dir));
	mark_inode_dirty(dst_dir);

	/* finally remove the old entry */
	hfs_cat_build_key(sb, src_fd.search_key, src_dir->i_ino, src_name);
	err = hfs_brec_find(&src_fd);
	if (err)
		goto out;
	err = hfs_brec_remove(&src_fd);
	if (err)
		goto out;
	src_dir->i_size--;
	inode_set_mtime_to_ts(src_dir, inode_set_ctime_current(src_dir));
	mark_inode_dirty(src_dir);

	type = entry.type;
	if (type == HFS_CDR_FIL && !(entry.file.Flags & HFS_FIL_THD))
		goto out;

	/* remove old thread entry */
	hfs_cat_build_key(sb, src_fd.search_key, cnid, NULL);
	err = hfs_brec_find(&src_fd);
	if (err)
		goto out;
	err = hfs_brec_remove(&src_fd);
	if (err)
		goto out;

	/* create new thread entry */
	hfs_cat_build_key(sb, dst_fd.search_key, cnid, NULL);
	entry_size = hfs_cat_build_thread(sb, &entry, type == HFS_CDR_FIL ? HFS_CDR_FTH : HFS_CDR_THD,
					dst_dir->i_ino, dst_name);
	err = hfs_brec_find(&dst_fd);
	if (err != -ENOENT) {
		if (!err)
			err = -EEXIST;
		goto out;
	}
	err = hfs_brec_insert(&dst_fd, &entry, entry_size);
out:
	hfs_bnode_put(dst_fd.bnode);
	hfs_find_exit(&src_fd);
	return err;
}
