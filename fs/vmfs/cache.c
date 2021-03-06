/*
 *  cache.c
 *
 * Copyright (C) 1997 by Bill Hawes
 *
 * Routines to support directory cacheing using the page cache.
 * This cache code is almost directly taken from ncpfs.
 *
 * Please add a note about your changes to vmfs_ in the ChangeLog file.
 */

#include <linux/time.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/dirent.h>
#include "vmfs_fs.h"
#include <linux/pagemap.h>
#include <linux/net.h>
#include <linux/version.h>

#include <asm/page.h>

#include "vmfs_debug.h"
#include "proto.h"

/*
 * Force the next attempt to use the cache to be a timeout.
 * If we can't find the page that's fine, it will cause a refresh.
 */
void vmfs_invalid_dir_cache(struct inode *dir)
{
	struct vmfs_sb_info *server = server_from_inode(dir);
	union vmfs_dir_cache *cache = NULL;
	struct page *page = NULL;

	page = grab_cache_page(&dir->i_data, 0);
	if (!page)
		goto out;

	if (!PageUptodate(page))
		goto out_unlock;

	cache = kmap(page);
	cache->head.time = jiffies - VMFS_MAX_AGE(server);

	kunmap(page);
	SetPageUptodate(page);
out_unlock:
	unlock_page(page);
	page_cache_release(page);
out:
	return;
}

/*
 * Mark all dentries for 'parent' as invalid, forcing them to be re-read
 */
void vmfs_invalidate_dircache_entries(struct dentry *parent)
{
	struct vmfs_sb_info *server = server_from_dentry(parent);
	struct list_head *next;
	struct dentry *dentry;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 38)
	spin_lock(&dcache_lock);
#else
	spin_lock(&parent->d_lock);
#endif
	next = parent->d_subdirs.next;
	while (next != &parent->d_subdirs) {
		dentry = list_entry(next, struct dentry, d_u.d_child);
		dentry->d_fsdata = NULL;
		vmfs_age_dentry(server, dentry);
		next = next->next;
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 38)
	spin_unlock(&dcache_lock);
#else
	spin_unlock(&parent->d_lock);
#endif
}

/*
 * dget, but require that fpos and parent matches what the dentry contains.
 * dentry is not known to be a valid pointer at entry.
 */
struct dentry *vmfs_dget_fpos(struct dentry *dentry, struct dentry *parent,
			      unsigned long fpos)
{
	struct dentry *dent = dentry;
	struct list_head *next;

	if (d_validate(dent, parent)) {
		if (dent->d_name.len <= VMFS_MAXNAMELEN &&
		    (unsigned long)dent->d_fsdata == fpos) {
			if (!dent->d_inode) {
				dput(dent);
				dent = NULL;
			}
			return dent;
		}
		dput(dent);
	}

	/* If a pointer is invalid, we search the dentry. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 38)
	spin_lock(&dcache_lock);
#else
	spin_lock(&parent->d_lock);
#endif
	next = parent->d_subdirs.next;
	while (next != &parent->d_subdirs) {
		dent = list_entry(next, struct dentry, d_u.d_child);
		if ((unsigned long)dent->d_fsdata == fpos) {
			if (dent->d_inode) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 38)
				dget_locked(dent);
#else
				dget(dent);
#endif
			} else {
				dent = NULL;
			}
			goto out_unlock;
		}
		next = next->next;
	}
	dent = NULL;
out_unlock:
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 38)
	spin_unlock(&dcache_lock);
#else
	spin_unlock(&parent->d_lock);
#endif
	return dent;
}

/*
 * Create dentry/inode for this file and add it to the dircache.
 */
int
vmfs_fill_cache(struct file *filp, void *dirent, filldir_t filldir,
		struct vmfs_cache_control *ctrl, struct qstr *qname,
		struct vmfs_fattr *entry)
{
	struct dentry *newdent, *dentry = filp->f_path.dentry;
	struct inode *newino, *inode = dentry->d_inode;
	struct vmfs_cache_control ctl = *ctrl;
	int valid = 0;
	int hashed = 0;
	ino_t ino = 0;

	DEBUG1("name=%s\n", qname->name);

	qname->hash = full_name_hash(qname->name, qname->len);

	if (dentry->d_op && dentry->d_op->d_hash) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 38)
		if (dentry->d_op->d_hash(dentry, qname) != 0)
#else
		if (dentry->d_op->d_hash(dentry, inode, qname) != 0)
#endif
			goto end_advance;
	}

	newdent = d_lookup(dentry, qname);

	if (!newdent) {
		newdent = d_alloc(dentry, qname);
		if (!newdent)
			goto end_advance;
	} else {
		hashed = 1;
		memcpy((char *)newdent->d_name.name, qname->name,
		       newdent->d_name.len);
	}

	if (!newdent->d_inode) {
		vmfs_renew_times(newdent);
		entry->f_ino = iunique(inode->i_sb, 2);
		newino = vmfs_iget(inode->i_sb, entry);
		if (newino) {
			vmfs_new_dentry(newdent);
			d_instantiate(newdent, newino);
			if (!hashed)
				d_rehash(newdent);
		}
	} else
		vmfs_set_inode_attr(newdent->d_inode, entry);

	if (newdent->d_inode) {
		ino = newdent->d_inode->i_ino;
		newdent->d_fsdata = (void *)ctl.fpos;
		vmfs_new_dentry(newdent);
	}

	if (ctl.idx >= VMFS_DIRCACHE_SIZE) {
		if (ctl.page) {
			kunmap(ctl.page);
			SetPageUptodate(ctl.page);
			unlock_page(ctl.page);
			page_cache_release(ctl.page);
		}
		ctl.cache = NULL;
		ctl.idx -= VMFS_DIRCACHE_SIZE;
		ctl.ofs += 1;
		ctl.page = grab_cache_page(&inode->i_data, ctl.ofs);
		if (ctl.page)
			ctl.cache = kmap(ctl.page);
	}
	if (ctl.cache) {
		ctl.cache->dentry[ctl.idx] = newdent;
		valid = 1;
	}
	dput(newdent);

end_advance:
	if (!valid)
		ctl.valid = 0;
	if (!ctl.filled && (ctl.fpos == filp->f_pos)) {
		if (!ino)
			ino = find_inode_number(dentry, qname);
		if (!ino)
			ino = iunique(inode->i_sb, 2);
		ctl.filled = filldir(dirent, qname->name, qname->len,
				     filp->f_pos, ino, DT_UNKNOWN);
		if (!ctl.filled)
			filp->f_pos += 1;
	}
	ctl.fpos += 1;
	ctl.idx += 1;
	*ctrl = ctl;
	return ctl.valid || !ctl.filled;
}
