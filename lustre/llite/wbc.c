/*
 * LGPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * LGPL version 2.1 or (at your discretion) any later version.
 * LGPL version 2.1 accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * LGPL HEADER END
 */
/*
 * Copyright (c) 2019-2021, DDN Storage Corporation.
 */
/*
 * lustre/llite/wbc.c
 *
 * Lustre Metadata Writeback Caching (WBC)
 *
 * Author: Qian Yingjin <qian@ddn.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/namei.h>
#include <linux/file.h>
#include <lustre_compat.h>
#include <linux/security.h>
#include <linux/swap.h>
#include "llite_internal.h"

void wbc_super_root_add(struct inode *inode)
{
	struct wbc_super *super = ll_i2wbcs(inode);
	struct wbc_inode *wbci = ll_i2wbci(inode);

	LASSERT(wbci->wbci_flags & WBC_STATE_FL_ROOT);
	spin_lock(&super->wbcs_lock);
	if (wbc_flush_mode_lazy(wbci))
		list_add(&wbci->wbci_root_list, &super->wbcs_lazy_roots);
	else
		list_add(&wbci->wbci_root_list, &super->wbcs_roots);
	spin_unlock(&super->wbcs_lock);
}

void wbc_super_root_del(struct inode *inode)
{
	struct wbc_super *super = ll_i2wbcs(inode);
	struct wbc_inode *wbci = ll_i2wbci(inode);

	LASSERT(wbci->wbci_flags & WBC_STATE_FL_ROOT);
	spin_lock(&super->wbcs_lock);
	if (!list_empty(&wbci->wbci_root_list))
		list_del_init(&wbci->wbci_root_list);
	spin_unlock(&super->wbcs_lock);
}

/*
 * Wait for writeback on an inode to complete. Called with i_lock held.
 * Caller must make sure inode cannot go away when we drop i_lock.
 * linux/fs/fs-writeback.c
 */
void __inode_wait_for_writeback(struct inode *inode)
	__releases(inode->i_lock)
	__acquires(inode->i_lock)
{
	DEFINE_WAIT_BIT(wq, &inode->i_state, __I_SYNC);
	wait_queue_head_t *wqh;

	wqh = bit_waitqueue(&inode->i_state, __I_SYNC);
	while (inode->i_state & I_SYNC) {
		spin_unlock(&inode->i_lock);
		__wait_on_bit(wqh, &wq, bit_wait,
			      TASK_UNINTERRUPTIBLE);
		spin_lock(&inode->i_lock);
	}
}

/*
 * Wait for writeback on an inode to complete. Caller must have inode pinned.
 * linux/fs/fs-writeback.c
 */
void inode_wait_for_writeback(struct inode *inode)
{
	spin_lock(&inode->i_lock);
	__inode_wait_for_writeback(inode);
	spin_unlock(&inode->i_lock);
}

void __wbc_inode_wait_for_writeback(struct inode *inode)
	__releases(inode->i_lock)
	__acquires(inode->i_lock)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	DEFINE_WAIT_BIT(wq, &wbci->wbci_flags, __WBC_STATE_FL_WRITEBACK);
	wait_queue_head_t *wqh;

	wqh = bit_waitqueue(&wbci->wbci_flags, __WBC_STATE_FL_WRITEBACK);
	while (wbci->wbci_flags & WBC_STATE_FL_WRITEBACK) {
		spin_unlock(&inode->i_lock);
		__wait_on_bit(wqh, &wq, bit_wait, TASK_UNINTERRUPTIBLE);
		spin_lock(&inode->i_lock);
	}
}

static void wbc_inode_wait_for_writeback(struct inode *inode,
					 struct writeback_control_ext *wbcx)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	DEFINE_WAIT_BIT(wq, &wbci->wbci_flags, __WBC_STATE_FL_WRITEBACK);
	wait_queue_head_t *wqh;

	spin_lock(&inode->i_lock);
	wqh = bit_waitqueue(&wbci->wbci_flags, __WBC_STATE_FL_WRITEBACK);
	while (wbci->wbci_flags & WBC_STATE_FL_WRITEBACK) {
		spin_unlock(&inode->i_lock);
		if (wbcx->has_ioctx)
			(void)wbcfs_context_commit(inode->i_sb, &wbcx->context);
		__wait_on_bit(wqh, &wq, bit_wait, TASK_UNINTERRUPTIBLE);
		spin_lock(&inode->i_lock);
	}
	spin_unlock(&inode->i_lock);
}

void wbc_inode_writeback_complete(struct inode *inode)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);

	wbci->wbci_flags &= ~WBC_STATE_FL_WRITEBACK;
	/*
	 * Waiters must see WBC_STATE_FL_WRITEBACK cleared before
	 * being woken up.
	 */
	smp_mb();
	wake_up_bit(&wbci->wbci_flags, __WBC_STATE_FL_WRITEBACK);
}

int wbc_reserve_inode(struct wbc_super *super)
{
	struct wbc_conf *conf = &super->wbcs_conf;
	int rc = 0;

	if (conf->wbcc_max_inodes) {
		spin_lock(&super->wbcs_lock);
		if (!conf->wbcc_free_inodes)
			rc = -ENOSPC;
		else
			conf->wbcc_free_inodes--;
		spin_unlock(&super->wbcs_lock);
		if (wbc_cache_too_much_inodes(conf))
			wake_up_process(super->wbcs_reclaim_task);
	}
	return rc;
}

void wbc_unreserve_inode(struct inode *inode)
{
	struct wbc_super *super = ll_i2wbcs(inode);
	struct wbc_inode *wbci = ll_i2wbci(inode);

	wbci->wbci_flags &= ~WBC_STATE_FL_INODE_RESERVED;
	if (super->wbcs_conf.wbcc_max_inodes) {
		spin_lock(&super->wbcs_lock);
		if (!list_empty(&wbci->wbci_rsvd_lru))
			list_del_init(&wbci->wbci_rsvd_lru);
		super->wbcs_conf.wbcc_free_inodes++;
		spin_unlock(&super->wbcs_lock);
	}
}

void wbc_reserved_inode_lru_add(struct inode *inode)
{
	struct wbc_super *super = ll_i2wbcs(inode);

	if (super->wbcs_conf.wbcc_max_inodes) {
		spin_lock(&super->wbcs_lock);
		list_add_tail(&ll_i2wbci(inode)->wbci_rsvd_lru,
			      &super->wbcs_rsvd_inode_lru);
		spin_unlock(&super->wbcs_lock);
	}
}

void wbc_reserved_inode_lru_del(struct inode *inode)
{
	struct wbc_super *super = ll_i2wbcs(inode);

	if (super->wbcs_conf.wbcc_max_inodes) {
		spin_lock(&super->wbcs_lock);
		list_del_init(&ll_i2wbci(inode)->wbci_rsvd_lru);
		super->wbcs_conf.wbcc_free_inodes++;
		spin_unlock(&super->wbcs_lock);
	}
}

void wbc_free_inode(struct inode *inode)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);

	if (wbc_inode_root(wbci))
		wbc_super_root_del(inode);
	if (wbc_inode_reserved(wbci))
		wbc_unreserve_inode(inode);
}

void wbc_inode_unreserve_dput(struct inode *inode,
					    struct dentry *dentry)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);

	if (wbc_inode_reserved(wbci)) {
		wbci->wbci_flags &= ~WBC_STATE_FL_INODE_RESERVED;
		wbc_unreserve_inode(inode);
		/* Unpin the dentry now as it is stable. */
		dput(dentry);
	}
}

void wbc_inode_data_lru_add(struct inode *inode, struct file *file)
{
	struct wbc_super *super = ll_i2wbcs(inode);
	struct ll_file_data *fd = file->private_data;

	/*
	 * FIXME: It whould better to add @inode into cache shrinking list
	 * when the file is actual modified, i.e. at close() time with data
	 * modified, but not at file open time.
	 */
	if (super->wbcs_conf.wbcc_max_pages && fd->fd_omode & FMODE_WRITE) {
		struct wbc_inode *wbci = ll_i2wbci(inode);

		spin_lock(&super->wbcs_data_lru_lock);
		if (list_empty(&wbci->wbci_data_lru))
			list_add_tail(&wbci->wbci_data_lru,
				      &super->wbcs_data_inode_lru);
		spin_unlock(&super->wbcs_data_lru_lock);
	}
}

void wbc_inode_data_lru_del(struct inode *inode)
{
	struct wbc_super *super = ll_i2wbcs(inode);

	if (super->wbcs_conf.wbcc_max_pages) {
		struct wbc_inode *wbci = ll_i2wbci(inode);

		spin_lock(&super->wbcs_data_lru_lock);
		if (!list_empty(&wbci->wbci_data_lru))
			list_del_init(&wbci->wbci_data_lru);
		spin_unlock(&super->wbcs_data_lru_lock);
	}
}

static inline void wbc_clear_dirty_for_flush(struct wbc_inode *wbci,
					     unsigned int *valid)
{
	*valid = wbci->wbci_dirty_attr;
	wbci->wbci_dirty_attr = 0;
	wbci->wbci_dirty_flags = WBC_DIRTY_FL_FLUSHING;
}

static inline bool wbc_flush_need_exlock(struct wbc_inode *wbci,
					 struct writeback_control_ext *wbcx)
{
	return wbc_mode_lock_drop(wbci) || wbcx->for_callback;
}

/**
 * Initialize synchronous io wait \a anchor for \a nr updates.
 * \param anchor owned by caller, initialized here.
 * \param nr number of updates initially pending in sync.
 */
void wbc_sync_io_init(struct wbc_sync_io *anchor, int nr)
{
	ENTRY;
	memset(anchor, 0, sizeof(*anchor));
	init_waitqueue_head(&anchor->wsi_waitq);
	atomic_set(&anchor->wsi_sync_nr, nr);
}

/**
 * Wait until all IO completes. Transfer completion routine has to call
 * wbc_sync_io_note() for every entity.
 */
int wbc_sync_io_wait(struct wbc_sync_io *anchor, long timeout)
{
	int rc = 0;

	ENTRY;

	LASSERT(timeout >= 0);
	if (timeout > 0 &&
	    wait_event_idle_timeout(anchor->wsi_waitq,
				    atomic_read(&anchor->wsi_sync_nr) == 0,
				    cfs_time_seconds(timeout)) == 0) {
		rc = -ETIMEDOUT;
		CERROR("IO failed: %d, still wait for %d remaining entries\n",
		       rc, atomic_read(&anchor->wsi_sync_nr));
	}

	wait_event_idle(anchor->wsi_waitq,
			atomic_read(&anchor->wsi_sync_nr) == 0);
	if (!rc)
		rc = anchor->wsi_sync_rc;

	/* We take the lock to ensure that cl_sync_io_note() has finished */
	spin_lock(&anchor->wsi_waitq.lock);
	if (atomic_read(&anchor->wsi_sync_nr) != 0)
		CWARN("Pending number is %d, not zero\n",
		      atomic_read(&anchor->wsi_sync_nr));
	//LASSERT(atomic_read(&anchor->wsi_sync_nr) == 0);
	spin_unlock(&anchor->wsi_waitq.lock);

	RETURN(rc);
}

/**
 * Indicate that transfer of a single update completed.
 */
void wbc_sync_io_note(struct wbc_sync_io *anchor, int ioret)
{
	ENTRY;
	if (anchor->wsi_sync_rc == 0 && ioret < 0)
		anchor->wsi_sync_rc = ioret;

	/* Completion is used to signal the end of IO. */
	LASSERT(atomic_read(&anchor->wsi_sync_nr) > 0);
	if (atomic_dec_and_lock(&anchor->wsi_sync_nr,
				&anchor->wsi_waitq.lock)) {
		wake_up_locked(&anchor->wsi_waitq);
		spin_unlock(&anchor->wsi_waitq.lock);
	}
	EXIT;
}

long wbc_flush_opcode_get(struct inode *inode, struct dentry *dchild,
			  struct writeback_control_ext *wbcx,
			  unsigned int *valid)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	long opc = MD_OP_NONE;
	bool decomp_keep;

	ENTRY;

	decomp_keep = wbcx->for_decomplete && wbc_mode_lock_keep(wbci);
	spin_lock(&inode->i_lock);
	if (wbc_mode_lock_keep(wbci)) {
		if (wbci->wbci_flags & WBC_STATE_FL_FREEING) {
			spin_unlock(&inode->i_lock);
			RETURN(MD_OP_NONE);
		}

		if (wbcx->for_callback && inode->i_state & I_SYNC)
			__inode_wait_for_writeback(inode);

		if (wbcx->for_pflush &&
		    (wbci->wbci_flags & WBC_STATE_FL_WRITEBACK)) {
			spin_unlock(&inode->i_lock);
			RETURN(MD_OP_NONE);
		}

		if (!wbcx->for_fsync &&
		    wbci->wbci_flags & WBC_STATE_FL_WRITEBACK)
			__wbc_inode_wait_for_writeback(inode);
	} else if (wbc_mode_lock_drop(wbci)) {
		LASSERT(!(inode->i_state & I_SYNC));
	}

	/*
	 * The inode was redirtied.
	 * TODO: handle more dirty flags: I_DIRTY_TIME | I_DIRTY_TIME_EXPIRED
	 * in the latest Linux kernel.
	 */

	if (wbc_inode_none(wbci)) {
		opc = MD_OP_NONE;
	} else if (wbc_inode_was_flushed(wbci)) {
		if (decomp_keep) {
			LASSERT(dchild != NULL);
			opc = MD_OP_NONE;
			if (wbcx->unrsv_children_decomp)
				wbc_inode_unreserve_dput(inode, dchild);
		} else if (wbc_inode_remove_dirty(wbci) &&
			   !wbc_flush_need_exlock(wbci, wbcx)) {
			LASSERT(S_ISDIR(inode->i_mode));
			LASSERT(wbc_mode_lock_keep(wbci));
			wbc_clear_dirty_for_flush(wbci, valid);
			opc = MD_OP_REMOVE_LOCKLESS;
		}else if (wbc_inode_attr_dirty(wbci)) {
			wbc_clear_dirty_for_flush(wbci, valid);
			opc = wbc_flush_need_exlock(wbci, wbcx) ?
			      MD_OP_SETATTR_EXLOCK : MD_OP_SETATTR_LOCKLESS;
		} else if (wbc_flush_need_exlock(wbci, wbcx)) {
			opc = MD_OP_EXLOCK_ONLY;
		}
	} else {
		/*
		 * TODO: Update the metadata attributes on MDT together with
		 * the file creation.
		 */
		wbc_clear_dirty_for_flush(wbci, valid);
		opc = wbc_flush_need_exlock(wbci, wbcx) ?
		      MD_OP_CREATE_EXLOCK : MD_OP_CREATE_LOCKLESS;
	}

	if (wbci->wbci_flags & WBC_STATE_FL_FREEING)
		opc = MD_OP_NONE;

	if (opc != MD_OP_NONE) {
		wbci->wbci_flags |= WBC_STATE_FL_WRITEBACK;
		wbc_unacct_inode_dirtied(ll_i2mwb(inode));
	}
	spin_unlock(&inode->i_lock);

	RETURN(opc);
}

static int wbc_flush_ancestors_topdown(struct list_head *fsync_list)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0, /* metadata-only */
	};
	struct writeback_control_ext *wbcx =
			(struct writeback_control_ext *)&wbc;
	struct wbc_dentry *wbcd, *tmp;
	int rc = 0;

	ENTRY;

	wbcx->for_fsync = 1;
	list_for_each_entry_safe(wbcd, tmp, fsync_list, wbcd_fsync_item) {
		struct ll_dentry_data *lld;
		struct dentry *dentry;
		struct inode *inode;
		struct wbc_inode *wbci;

		lld = container_of(wbcd, struct ll_dentry_data, lld_wbc_dentry);
		dentry = lld->lld_dentry;
		inode = dentry->d_inode;
		wbci = ll_i2wbci(inode);

		list_del_init(&wbcd->wbcd_fsync_item);

		/* Add @inode into the dirty list, otherwise sync_inode() will
		 * skip to write out the inode as the inode is not marked as
		 * I_DIRTY.
		 */
		if (wbc_flush_mode_lazy(wbci))
			mark_inode_dirty(inode);

		/* TODO: batched metadata flushing */
		if (rc == 0)
			rc = sync_inode(inode, &wbc);

		spin_lock(&inode->i_lock);
		ll_i2wbci(inode)->wbci_flags &= ~WBC_STATE_FL_WRITEBACK;
		spin_unlock(&inode->i_lock);
		wbc_inode_writeback_complete(inode);
	}

	RETURN(rc);
}

static inline void wbc_sync_addroot_lockdrop(struct wbc_inode *wbci,
					     struct wbc_dentry *wbcd,
					     struct list_head *fsync_list)
{
	if (wbc_mode_lock_drop(wbci) && wbc_inode_root(wbci)) {
		LASSERT(wbc_inode_has_protected(wbci));
		wbci->wbci_flags |= WBC_STATE_FL_WRITEBACK;
		list_add(&wbcd->wbcd_fsync_item, fsync_list);
	}
}

int wbc_make_inode_sync(struct dentry *dentry)
{
	LIST_HEAD(fsync_list);

	for (;;) {
		struct inode *inode = dentry->d_inode;
		struct wbc_inode *wbci = ll_i2wbci(inode);
		struct wbc_dentry *wbcd = ll_d2wbcd(dentry);

		spin_lock(&inode->i_lock);
		if (wbc_inode_written_out(wbci)) {
			wbc_sync_addroot_lockdrop(wbci, wbcd, &fsync_list);
			spin_unlock(&inode->i_lock);
			break;
		}

		if (wbci->wbci_flags & WBC_STATE_FL_WRITEBACK) {
			__wbc_inode_wait_for_writeback(inode);
			LASSERT(wbc_inode_was_flushed(wbci));
		}

		if (wbc_inode_written_out(wbci)) {
			wbc_sync_addroot_lockdrop(wbci, wbcd, &fsync_list);
			spin_unlock(&inode->i_lock);
			break;
		}

		if (inode->i_state & I_SYNC)
			__inode_wait_for_writeback(inode);

		LASSERT(!(inode->i_state & I_SYNC));

		if (wbc_inode_written_out(wbci)) {
			wbc_sync_addroot_lockdrop(wbci, wbcd, &fsync_list);
			spin_unlock(&inode->i_lock);
			break;
		}

		wbci->wbci_flags |= WBC_STATE_FL_WRITEBACK;
		list_add(&wbcd->wbcd_fsync_item, &fsync_list);
		spin_unlock(&inode->i_lock);
		dentry = dentry->d_parent;
	}

	return wbc_flush_ancestors_topdown(&fsync_list);
}

static int wbc_inode_update_metadata(struct inode *inode,
				     struct ldlm_lock *lock,
				     struct writeback_control_ext *wbcx)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	unsigned int valid = 0;
	long opc = MD_OP_NONE;
	int rc = 0;

	ENTRY;

	LASSERT(wbc_inode_was_flushed(wbci));

	spin_lock(&inode->i_lock);
	/* TODO: hanlde for unsynchornized removed sub files. */
	if (wbc_inode_remove_dirty(wbci)) {
		LASSERT(S_ISDIR(inode->i_mode));
		LASSERT(wbc_mode_lock_keep(wbci));
		wbc_clear_dirty_for_flush(wbci, &valid);
		opc = MD_OP_REMOVE_LOCKLESS;
	} else if (wbc_inode_attr_dirty(wbci)) {
		wbc_clear_dirty_for_flush(wbci, &valid);
		opc = MD_OP_SETATTR_LOCKLESS;
	}
	/* TODO: hardlink. */
	spin_unlock(&inode->i_lock);

	/*
	 * FIXME: if @inode is a directory, it should handle the order of the
	 * metadata attribute updating such as chmod()/chown() and newly file
	 * creation under this directory carefully. Or MDT should ignore the
	 * permission check for newly file creation under the protection of an
	 * WBC EX lock.
	 */
	rc = wbcfs_inode_sync_metadata(opc, inode, valid);
	RETURN(rc);
}

static int wbc_reopen_file_handler(struct inode *inode,
				   struct ldlm_lock *lock,
				   struct writeback_control_ext *wbcx)
{
	struct dentry *dentry;
	int rc = 0;

	ENTRY;

	spin_lock(&inode->i_lock);
	hlist_for_each_entry(dentry, &inode->i_dentry, d_alias) {
		struct wbc_dentry *wbcd = ll_d2wbcd(dentry);
		struct ll_file_data *fd, *tmp;

		dget(dentry);
		spin_unlock(&inode->i_lock);

		/*
		 * Do not need to acquire @wbcd_open_lock spinlock as it is
		 * under the protection of the lock @wbci_rw_sem.
		 */
		list_for_each_entry_safe(fd, tmp, &wbcd->wbcd_open_files,
					 fd_wbc_file.wbcf_open_item) {
			struct file *file = fd->fd_file;

			list_del_init(&fd->fd_wbc_file.wbcf_open_item);
			/* FIXME: Is it safe to switch file operatoins here? */
			if (S_ISDIR(inode->i_mode))
				file->f_op = &ll_dir_operations;
			else if (S_ISREG(inode->i_mode))
				file->f_op = ll_i2sbi(inode)->ll_fop;

			rc = file->f_op->open(inode, file);
			if (rc)
				GOTO(out_dput, rc);

			wbcfs_dcache_dir_close(inode, file);
			dput(dentry); /* Unpin from open in MemFS. */
		}
out_dput:
		dput(dentry);
		if (rc)
			RETURN(rc);
		spin_lock(&inode->i_lock);
	}
	spin_unlock(&inode->i_lock);

	RETURN(rc);
}

static int wbc_flush_regular_file(struct inode *inode, struct ldlm_lock *lock,
				  struct writeback_control_ext *wbcx)
{
	int rc;

	ENTRY;

	rc = wbc_inode_update_metadata(inode, lock, wbcx);
	if (rc)
		RETURN(rc);

	rc = wbcfs_commit_cache_pages(inode);
	if (rc < 0)
		RETURN(rc);

	rc = wbc_reopen_file_handler(inode, lock, wbcx);
	RETURN(rc);
}

static int wbc_flush_dir_children(struct wbc_context *ctx,
				  struct inode *dir,
				  struct list_head *childlist,
				  struct ldlm_lock *lock,
				  struct writeback_control_ext *wbcx)
{
	struct wbc_dentry *wbcd, *tmp;
	int rc = 0;

	ENTRY;

	rc = wbcfs_context_prepare(dir->i_sb, ctx);
	if (rc)
		RETURN(rc);

	list_for_each_entry_safe(wbcd, tmp, childlist, wbcd_flush_item) {
		struct ll_dentry_data *lld;
		struct dentry *dchild;

		lld = container_of(wbcd, struct ll_dentry_data, lld_wbc_dentry);
		dchild = lld->lld_dentry;
		list_del_init(&wbcd->wbcd_flush_item);

		rc = wbcfs_flush_dir_child(ctx, dir, dchild, lock, wbcx);
		/*
		 * Unpin the dentry.
		 * FIXME: race between dirty inode flush and unlink/rmdir().
		 */
		dput(dchild);
		if (rc)
			RETURN(rc);
	}

	rc = wbcfs_context_commit(dir->i_sb, ctx);
	RETURN(rc);
}

static inline bool wbc_dirty_queue_need_unplug(struct wbc_conf *conf,
					       __u32 count)
{
	return conf->wbcc_max_qlen > 0 && count > conf->wbcc_max_qlen;
}

static int wbc_flush_dir(struct inode *dir, struct ldlm_lock *lock,
			 struct writeback_control_ext *wbcx)
{
	struct dentry *dentry, *child, *tmp_subdir;
	LIST_HEAD(dirty_children_list);
	struct wbc_context ctx;
	__u32 count = 0;
	int rc, rc2;

	ENTRY;

	rc = wbc_inode_update_metadata(dir, lock, wbcx);
	if (rc)
		RETURN(rc);

	LASSERT(S_ISDIR(dir->i_mode));

	/*
	 * Usually there is only one dentry in this alias dentry list.
	 * Even if not, It cannot have hardlinks for directories,
	 * so only one will actually have any children entries anyway.
	 */
	dentry = d_find_any_alias(dir);
	if (!dentry)
		RETURN(0);

	rc = wbcfs_context_init(dir->i_sb, &ctx, true, true);
	if (rc)
		RETURN(rc);

	spin_lock(&dentry->d_lock);
	list_for_each_entry_safe(child, tmp_subdir,
				 &dentry->d_subdirs, d_child) {
		struct wbc_inode *wbci;

		/* Negative entry? or being unlinked? Drop it right away */
		if (child->d_inode == NULL || d_unhashed(child))
			continue;

		spin_lock_nested(&child->d_lock, DENTRY_D_LOCK_NESTED);
		if (child->d_inode == NULL || d_unhashed(child)) {
			spin_unlock(&child->d_lock);
			continue;
		}

		/*
		 * The inode will be flushed. Pin it first to avoid be deleted.
		 */
		dget_dlock(child);
		spin_unlock(&child->d_lock);
		count++;

		wbci = ll_i2wbci(child->d_inode);
		LASSERT(wbc_inode_has_protected(ll_i2wbci(child->d_inode)) &&
			ll_d2d(child));
		list_add_tail(&ll_d2wbcd(child)->wbcd_flush_item,
			      &dirty_children_list);

		if (wbc_dirty_queue_need_unplug(ll_i2wbcc(dir), count)) {
			spin_unlock(&dentry->d_lock);
			rc = wbc_flush_dir_children(&ctx, dir,
						    &dirty_children_list,
						    lock, wbcx);
			/* FIXME: error handling... */
			LASSERT(list_empty(&dirty_children_list));
			count = 0;
			cond_resched();
			spin_lock(&dentry->d_lock);
		}
	}
	spin_unlock(&dentry->d_lock);

	rc = wbc_flush_dir_children(&ctx, dir, &dirty_children_list,
				    lock, wbcx);
	mapping_clear_unevictable(dir->i_mapping);
	/* FIXME: error handling when @dirty_children_list is not empty. */
	LASSERT(list_empty(&dirty_children_list));

	if (rc == 0 && !(wbcx->for_decomplete &&
			 wbc_mode_lock_keep(ll_i2wbci(dir))))
		rc = wbc_reopen_file_handler(dir, lock, wbcx);

	rc2 = wbcfs_context_fini(dir->i_sb, &ctx);
	if (rc2 && rc == 0)
		rc = rc2;
	dput(dentry);
	RETURN(rc);
}

static int wbc_inode_flush(struct inode *inode, struct ldlm_lock *lock,
			   struct writeback_control_ext *wbcx)
{
	if (S_ISDIR(inode->i_mode))
		return wbc_flush_dir(inode, lock, wbcx);
	if (S_ISREG(inode->i_mode))
		return wbc_flush_regular_file(inode, lock, wbcx);

	return -ENOTSUPP;
}

static inline int wbc_inode_flush_lockless(struct inode *inode,
					   struct writeback_control_ext *wbcx)
{
	return wbcfs_inode_flush_lockless(inode, wbcx);
}

static inline void wbc_mark_inode_deroot(struct inode *inode)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);

	wbc_super_root_del(inode);
	if (wbc_inode_reserved(wbci))
		wbc_unreserve_inode(inode);

	wbci->wbci_flags = WBC_STATE_FL_NONE;
	wbcfs_inode_operations_switch(inode);
}

int wbc_make_inode_deroot(struct inode *inode, struct ldlm_lock *lock,
			  struct writeback_control_ext *wbcx)
{
	int rc;

	LASSERT(wbc_inode_root(ll_i2wbci(inode)));

	rc = wbc_inode_flush(inode, lock, wbcx);
	spin_lock(&inode->i_lock);
	wbc_mark_inode_deroot(inode);
	spin_unlock(&inode->i_lock);
	return rc;
}

int wbc_make_inode_decomplete(struct inode *inode,
			      unsigned int unrsv_children)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	struct writeback_control_ext wbcx = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0, /* metadata-only */
		.for_decomplete = 1,
		.unrsv_children_decomp = unrsv_children,
	};
	struct ldlm_lock *lock = NULL;
	int rc;

	ENTRY;

	LASSERT(S_ISDIR(inode->i_mode));
	if (wbc_mode_lock_drop(wbci)) {
		lock = ldlm_handle2lock(&wbci->wbci_lock_handle);
		if (lock == NULL) {
			LASSERTF(!wbc_inode_has_protected(wbci),
				 "WBC flags %d\n", wbci->wbci_flags);
			RETURN(0);
		}
	}

	down_write(&wbci->wbci_rw_sem);
	if (wbc_inode_none(wbci) || !wbc_inode_complete(wbci))
		GOTO(up_rwsem, rc = 0);

	rc = wbc_inode_flush(inode, lock, &wbcx);
	/* FIXME: error handling. */

	spin_lock(&inode->i_lock);
	if (wbc_mode_lock_drop(wbci))
		wbc_mark_inode_deroot(inode);
	else if (wbc_mode_lock_keep(wbci))
		wbci->wbci_flags &= ~WBC_STATE_FL_COMPLETE;
	spin_unlock(&inode->i_lock);

up_rwsem:
	up_write(&wbci->wbci_rw_sem);
	if (lock)
		LDLM_LOCK_PUT(lock);

	RETURN(rc);
}

int wbc_make_dir_decomplete(struct inode *dir, struct dentry *parent,
			    unsigned int unrsv_children)
{
	int rc;

	ENTRY;

	LASSERT(parent != NULL && parent->d_inode == dir);

	if (!d_mountpoint(parent)) {
		if (wbc_mode_lock_drop(ll_i2wbci(dir)))
			rc = wbc_make_inode_sync(parent->d_parent);
		else /* lock keep flush mode */
			rc = wbc_make_inode_sync(parent);
		if (rc)
			RETURN(rc);
	}

	rc = wbc_make_inode_decomplete(dir, unrsv_children);
	RETURN(rc);
}

int wbc_make_data_commit(struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct wbc_inode *wbci = ll_i2wbci(inode);
	int rc;

	ENTRY;

	/*
	 * TODO: Reopen the file to support lock drop flush mode.
	 * For Data on PCC (DOP) cache mode , it does not need to flush the
	 * parent to MDT.
	 * The client can create PCC copy stub locally without any interaction
	 * with MDT. After that, I/O can also direct into the local PCC copy.
	 */
	if (!d_mountpoint(dentry->d_parent) &&
	    wbci->wbci_cache_mode == WBC_MODE_MEMFS) {
		if (wbc_mode_lock_drop(ll_i2wbci(inode)))
			rc = wbc_make_inode_sync(dentry->d_parent);
		else /* lock keep flush mode */
			rc = wbc_make_inode_sync(dentry);
		if (rc)
			RETURN(rc);
	}

	down_write(&wbci->wbci_rw_sem);
	rc = wbcfs_commit_cache_pages(inode);
	up_write(&wbci->wbci_rw_sem);

	RETURN(rc);
}

/*
 * Check and eliminate the flush dependency.
 * i.e. When flush a file, it must ensure that its parent has been flushed.
 * This dependency happends when the parent inode is locked for writeback
 * (Marked with I_SYNC), the kernel writeback function @writeback_sb_inodes()
 * will skip and move it to @b_more_io so that writeback can proceed with the
 * other inodes on @s_io.
 * ->writeback_sb_inodes()
 *	...
 *	if ((inode->i_state & I_SYNC) && wbc.sync_mode != WB_SYNC_ALL) {
 *		spin_unlock(&inode->i_lock);
 *		reqeueu_io(inode, wb);
 *		trace_writeback_sb_inodes_requeue(inode);
 *		continue;
 *	}
 *	...
 * However, other inodes in writeback list may have dependency on this inode
 * which is being written back. When writeback other inodes concurrently, it
 * will happend this dependency problem.
 */
static int wbc_flush_dependency_check(struct inode *inode,
				      struct writeback_control_ext *wbcx)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	struct dentry *dentry;
	struct dentry *parent;
	int rc = 0;

	ENTRY;

	/* For WB_SYNC_ALL mode, it wont happen the dependency issues. */
	if (wbcx->sync_mode == WB_SYNC_ALL)
		RETURN(0);

	/* FIXME: hardlinks. */
	dentry = d_find_any_alias(inode);
	if (dentry == NULL)
		RETURN(1);

	parent = dentry->d_parent;
	inode_wait_for_writeback(parent->d_inode);

	if (wbci->wbci_flush_mode == WBC_FLUSH_AGING_KEEP)
		wbc_inode_wait_for_writeback(parent->d_inode, wbcx);

	if (wbci->wbci_flags & WBC_STATE_FL_FREEING) {
		spin_lock(&inode->i_lock);
		inode->i_state &= ~I_SYNC;
		/* Waiters must see I_SYNC cleared before being woken up */
		smp_mb();
		wake_up_bit(&inode->i_state, __I_SYNC);
		spin_unlock(&inode->i_lock);
		rc = 1;
	}

	dput(dentry);
	RETURN(rc);
}

static int wbc_inode_flush_lockdrop(struct inode *inode,
				    struct writeback_control_ext *wbcx)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	struct ldlm_lock *lock;
	int rc = 0;

	lock = ldlm_handle2lock(&wbci->wbci_lock_handle);
	if (lock == NULL) {
		LASSERTF(!wbc_inode_has_protected(wbci),
			 "WBC flags: %d inode %p\n", wbci->wbci_flags, inode);
		RETURN(0);
	}

	down_write(&wbci->wbci_rw_sem);
	if (wbc_inode_has_protected(wbci))
		rc = wbc_make_inode_deroot(inode, lock, wbcx);
	up_write(&wbci->wbci_rw_sem);

	LDLM_LOCK_PUT(lock);

	return rc;
}

void wbc_inode_init(struct inode *inode)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);

	wbci->wbci_flags = WBC_STATE_FL_NONE;
	wbci->wbci_dirty_flags = WBC_DIRTY_FL_NONE;
	INIT_LIST_HEAD(&wbci->wbci_root_list);
	INIT_LIST_HEAD(&wbci->wbci_rsvd_lru);
	INIT_LIST_HEAD(&wbci->wbci_data_lru);
	init_rwsem(&wbci->wbci_rw_sem);

	if (S_ISDIR(inode->i_mode)) {
		spin_lock_init(&wbci->wbci_removed_lock);
		INIT_LIST_HEAD(&wbci->wbci_removed_list);
		wbci->wbci_rmpol = ll_i2wbcc(inode)->wbcc_rmpol;
	}
}

void wbc_dentry_init(struct dentry *dentry)
{
	struct ll_dentry_data *lld;

	lld = ll_d2d(dentry);
	LASSERT(lld);
	lld->lld_dentry = dentry;
	INIT_LIST_HEAD(&lld->lld_wbc_dentry.wbcd_flush_item);
	INIT_LIST_HEAD(&lld->lld_wbc_dentry.wbcd_fsync_item);
	INIT_LIST_HEAD(&lld->lld_wbc_dentry.wbcd_open_files);
	spin_lock_init(&lld->lld_wbc_dentry.wbcd_open_lock);
}

static inline struct wbc_inode *wbc_inode(struct list_head *head)
{
	return list_entry(head, struct wbc_inode, wbci_root_list);
}

static int __wbc_super_shrink_roots(struct wbc_super *super,
				     struct list_head *shrink_list)
{
	struct writeback_control_ext wbcx = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0, /* metadata-only */
		.for_sync = 1,
		.for_callback = 1,
	};
	int rc = 0;

	ENTRY;

	LASSERT(shrink_list == &super->wbcs_lazy_roots ||
		shrink_list == &super->wbcs_roots);

	spin_lock(&super->wbcs_lock);
	while (!list_empty(shrink_list)) {
		struct wbc_inode *wbci = wbc_inode(shrink_list->prev);
		struct inode *inode = ll_wbci2i(wbci);

		LASSERT(wbci->wbci_flags & WBC_STATE_FL_ROOT);
		list_del_init(&wbci->wbci_root_list);
		spin_unlock(&super->wbcs_lock);
		rc = wbc_inode_flush_lockdrop(inode, &wbcx);
		if (rc) {
			CERROR("Failed to flush file: "DFID"\n",
			       PFID(&ll_i2info(inode)->lli_fid));
			RETURN(rc);
		}
		spin_lock(&super->wbcs_lock);
	}
	spin_unlock(&super->wbcs_lock);

	RETURN(rc);
}

int wbc_super_shrink_roots(struct wbc_super *super)
{
	int rc;
	int rc2;

	super->wbcs_conf.wbcc_cache_mode = WBC_MODE_NONE;
	rc = __wbc_super_shrink_roots(super, &super->wbcs_lazy_roots);
	rc2 = __wbc_super_shrink_roots(super, &super->wbcs_roots);
	if (rc)
		rc = rc2;

	if (rc == 0)
		wbc_kill_super(super);

	return rc;
}

int wbc_super_sync_fs(struct wbc_super *super, int wait)
{
	struct wbc_context *ctx = &super->wbcs_context;
	int rc;

	if (!wait)
		return 0;

	rc = __wbc_super_shrink_roots(super, &super->wbcs_lazy_roots);
	if (rc)
		return rc;

	rc = wbc_sync_io_wait(&ctx->ioc_anchor, 0);
	return rc;
}

int wbc_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct writeback_control_ext *wbcx =
		(struct writeback_control_ext *)wbc;
	struct wbc_inode *wbci = ll_i2wbci(inode);
	int rc = 0;

	ENTRY;

	/* The inode was flush to MDT due to LRU lock shrinking? */
	if (!wbc_inode_has_protected(wbci))
		RETURN(0);

	if (wbci->wbci_flush_mode == WBC_FLUSH_AGING_DROP ||
	    wbci->wbci_flush_mode == WBC_FLUSH_AGING_KEEP ||
	    wbcx->for_fsync) {
		rc = wbc_flush_dependency_check(inode, wbcx);
		if (rc == 1)
			RETURN(0);
	}

	/* TODO: Handle WB_SYNC_ALL WB_SYNC_NONE properly. */
	switch (wbci->wbci_flush_mode) {
	case WBC_FLUSH_AGING_DROP:

		rc = wbc_inode_flush_lockdrop(inode, wbcx);
		/* TODO: Convert the EX WBC lock to PR or CR lock. */
		break;
	case WBC_FLUSH_AGING_KEEP:
		rc = wbc_inode_flush_lockless(inode, wbcx);
		break;
	case WBC_FLUSH_LAZY_DROP:
	case WBC_FLUSH_LAZY_KEEP:
		if (wbcx->for_fsync) {
			if (wbc_mode_lock_drop(wbci))
				rc = wbc_inode_flush_lockdrop(inode, wbcx);
			else if (wbc_mode_lock_keep(wbci))
				rc = wbc_inode_flush_lockless(inode, wbcx);
		}
		break;
	default:
		break;
	}

	RETURN(rc);
}

static int wbc_reclaim_inodes_below(struct wbc_super *super, __u32 low)
{
	struct wbc_conf *conf = &super->wbcs_conf;
	int rc = 0;

	ENTRY;

	spin_lock(&super->wbcs_lock);
	while (conf->wbcc_free_inodes < low) {
		struct inode *inode;
		struct wbc_inode *wbci;
		struct ll_inode_info *lli;
		struct dentry *dchild;

		if (list_empty(&super->wbcs_rsvd_inode_lru))
			break;

		wbci = list_entry(super->wbcs_rsvd_inode_lru.next,
				  struct wbc_inode, wbci_rsvd_lru);

		list_del_init(&wbci->wbci_rsvd_lru);
		lli = container_of(wbci, struct ll_inode_info, lli_wbc_inode);
		inode = ll_info2i(lli);
		dchild = d_find_any_alias(inode);
		if (!dchild)
			continue;

		spin_unlock(&super->wbcs_lock);

		rc = wbc_make_dir_decomplete(dchild->d_parent->d_inode,
					     dchild->d_parent, 1);
		dput(dchild);
		if (rc) {
			CERROR("Reclaim inodes failed: rc = %d\n", rc);
			RETURN(rc);
		}

		cond_resched();
		spin_lock(&super->wbcs_lock);
	}
	spin_unlock(&super->wbcs_lock);

	RETURN(rc);
}

static int wbc_reclaim_inodes(struct wbc_super *super)
{
	__u32 low = super->wbcs_conf.wbcc_max_inodes >> 1;

	return wbc_reclaim_inodes_below(super, low);
}

static int wbc_reclaim_pages_count(struct wbc_super *super, __u32 count)
{
	__u32 shrank_count = 0;
	int rc = 0;

	ENTRY;

	spin_lock(&super->wbcs_data_lru_lock);
	while (shrank_count < count) {
		struct inode *inode;
		struct wbc_inode *wbci;
		struct ll_inode_info *lli;
		struct dentry *dentry;

		if (list_empty(&super->wbcs_data_inode_lru))
			break;

		wbci = list_entry(super->wbcs_data_inode_lru.next,
				  struct wbc_inode, wbci_data_lru);

		list_del_init(&wbci->wbci_data_lru);
		lli = container_of(wbci, struct ll_inode_info, lli_wbc_inode);
		inode = ll_info2i(lli);
		dentry = d_find_any_alias(inode);
		if (!dentry)
			continue;

		spin_unlock(&super->wbcs_data_lru_lock);

		rc = wbc_make_data_commit(dentry);
		dput(dentry);
		if (rc < 0) {
			CERROR("Reclaim pages failed: rc = %d\n", rc);
			RETURN(rc);
		}

		shrank_count += rc;
		cond_resched();
		spin_lock(&super->wbcs_data_lru_lock);
	}
	spin_unlock(&super->wbcs_data_lru_lock);

	RETURN(rc);
}

static int wbc_reclaim_pages(struct wbc_super *super)
{
	__u32 count = super->wbcs_conf.wbcc_max_pages >> 1;

	return wbc_reclaim_pages_count(super, count);
}

#ifndef TASK_IDLE
#define TASK_IDLE TASK_INTERRUPTIBLE
#endif

static int ll_wbc_reclaim_main(void *arg)
{
	struct wbc_super *super = arg;

	ENTRY;

	while (({set_current_state(TASK_IDLE);
		 !kthread_should_stop(); })) {
		if (wbc_cache_too_much_inodes(&super->wbcs_conf)) {
			__set_current_state(TASK_RUNNING);
			(void) wbc_reclaim_inodes(super);
			cond_resched();
		} else if (wbc_cache_too_much_pages(&super->wbcs_conf)) {
			__set_current_state(TASK_RUNNING);
			(void) wbc_reclaim_pages(super);
		} else {
			schedule();
		}
	}
	__set_current_state(TASK_RUNNING);

	RETURN(0);
}

static void wbc_super_reset_common_conf(struct wbc_conf *conf)
{
	conf->wbcc_rmpol = WBC_RMPOL_DEFAULT;
	conf->wbcc_readdir_pol = WBC_READDIR_POL_DEFAULT;
	conf->wbcc_flush_pol = WBC_FLUSH_POL_DEFAULT;
	conf->wbcc_max_batch_count = 0;
	conf->wbcc_max_rpcs = WBC_DEFAULT_MAX_RPCS;
	conf->wbcc_max_qlen = WBC_DEFAULT_MAX_QLEN;
	conf->wbcc_max_nrpages_per_file = WBC_DEFAULT_MAX_NRPAGES_PER_FILE;
	conf->wbcc_max_rmfid_count = OBD_MAX_FIDS_IN_ARRAY;
	conf->wbcc_background_async_rpc = 0;
	conf->wbcc_max_inodes = 0;
	conf->wbcc_free_inodes = 0;
	conf->wbcc_max_pages = 0;
	conf->wbcc_hiwm_ratio = WBC_DEFAULT_HIWM_RATIO;
	conf->wbcc_hiwm_inodes_count = 0;
	conf->wbcc_hiwm_pages_count = 0;
	conf->wbcc_active_data_writeback = true;
}

/* called with @wbcs_lock hold. */
static void wbc_super_disable_cache(struct wbc_super *super)
{
	struct wbc_conf *conf = &super->wbcs_conf;

repeat:
	conf->wbcc_cache_mode = WBC_MODE_NONE;
	conf->wbcc_flush_mode = WBC_FLUSH_NONE;

	spin_unlock(&super->wbcs_lock);
	wbc_super_shrink_roots(super);
	spin_lock(&super->wbcs_lock);
	/* The cache mode was changed when shrinking the WBC roots. */
	if (conf->wbcc_cache_mode != WBC_MODE_NONE)
		goto repeat;

	LASSERTF(conf->wbcc_max_inodes == conf->wbcc_free_inodes &&
		 percpu_counter_sum(&conf->wbcc_used_pages) == 0,
		 "max_inodes: %lu free_inodes:%lu\n",
		 conf->wbcc_max_inodes, conf->wbcc_free_inodes);
	wbc_super_reset_common_conf(conf);
}

static void wbc_super_conf_default(struct wbc_conf *conf)
{
	conf->wbcc_cache_mode = WBC_MODE_MEMFS;
	conf->wbcc_flush_mode = WBC_FLUSH_DEFAULT_MODE;
	conf->wbcc_rmpol = WBC_RMPOL_DEFAULT;
	conf->wbcc_max_rpcs = WBC_DEFAULT_MAX_RPCS;
	conf->wbcc_max_qlen = WBC_DEFAULT_MAX_QLEN;
	conf->wbcc_max_nrpages_per_file = WBC_DEFAULT_MAX_NRPAGES_PER_FILE;
	conf->wbcc_max_rmfid_count = OBD_MAX_FIDS_IN_ARRAY;
}

static int wbc_super_conf_update(struct wbc_super *super, struct wbc_conf *conf,
				 struct wbc_cmd *cmd)
{
	/*
	 * Memery limits for inodes/pages are not allowed to be decreased
	 * less then used value in the runtime.
	 */
	if (cmd->wbcc_flags & WBC_CMD_OP_INODES_LIMIT &&
	    (conf->wbcc_max_inodes - conf->wbcc_free_inodes) >
	    cmd->wbcc_conf.wbcc_max_inodes)
		return -EINVAL;

	if (cmd->wbcc_flags & WBC_CMD_OP_PAGES_LIMIT &&
	    percpu_counter_compare(&conf->wbcc_used_pages,
				   cmd->wbcc_conf.wbcc_max_pages) > 0)
		return -EINVAL;

	if (cmd->wbcc_flags & WBC_CMD_OP_INODES_LIMIT) {
		conf->wbcc_free_inodes += (cmd->wbcc_conf.wbcc_max_inodes -
					   conf->wbcc_max_inodes);
		conf->wbcc_max_inodes = cmd->wbcc_conf.wbcc_max_inodes;
	}

	if (cmd->wbcc_flags & WBC_CMD_OP_PAGES_LIMIT)
		conf->wbcc_max_pages = cmd->wbcc_conf.wbcc_max_pages;

	if (cmd->wbcc_flags & WBC_CMD_OP_RECLAIM_RATIO) {
		conf->wbcc_hiwm_ratio = cmd->wbcc_conf.wbcc_hiwm_ratio;
		conf->wbcc_hiwm_inodes_count = conf->wbcc_max_inodes *
					       conf->wbcc_hiwm_ratio / 100;
		conf->wbcc_hiwm_pages_count = conf->wbcc_max_pages *
					      conf->wbcc_hiwm_ratio / 100;
	}

	if (conf->wbcc_cache_mode == WBC_MODE_NONE)
		conf->wbcc_cache_mode = WBC_MODE_DEFAULT;
	if (cmd->wbcc_flags & WBC_CMD_OP_CACHE_MODE)
		conf->wbcc_cache_mode = cmd->wbcc_conf.wbcc_cache_mode;
	if (cmd->wbcc_flags & WBC_CMD_OP_FLUSH_MODE)
		conf->wbcc_flush_mode = cmd->wbcc_conf.wbcc_flush_mode;
	if (cmd->wbcc_flags & WBC_CMD_OP_MAX_RPCS)
		conf->wbcc_max_rpcs = cmd->wbcc_conf.wbcc_max_rpcs;
	if (cmd->wbcc_flags & WBC_CMD_OP_MAX_QLEN)
		conf->wbcc_max_qlen = cmd->wbcc_conf.wbcc_max_qlen;
	if (cmd->wbcc_flags & WBC_CMD_OP_MAX_NRPAGES_PER_FILE)
		conf->wbcc_max_nrpages_per_file =
			cmd->wbcc_conf.wbcc_max_nrpages_per_file;
	if (cmd->wbcc_flags & WBC_CMD_OP_MAX_RMFID_COUNT)
		conf->wbcc_max_rmfid_count =
			cmd->wbcc_conf.wbcc_max_rmfid_count;
	if (cmd->wbcc_flags & WBC_CMD_OP_RMPOL)
		conf->wbcc_rmpol = cmd->wbcc_conf.wbcc_rmpol;
	if (cmd->wbcc_flags & WBC_CMD_OP_READDIR_POL)
		conf->wbcc_readdir_pol = cmd->wbcc_conf.wbcc_readdir_pol;
	if (cmd->wbcc_flags & WBC_CMD_OP_FLUSH_POL)
		conf->wbcc_flush_pol = cmd->wbcc_conf.wbcc_flush_pol;
	if (cmd->wbcc_flags & WBC_CMD_OP_MAX_BATCH_COUNT)
		conf->wbcc_max_batch_count =
				cmd->wbcc_conf.wbcc_max_batch_count;

	if (cmd->wbcc_flags & WBC_CMD_OP_DIRTY_FLUSH_THRESH) {
		struct memfs_writeback *mwb = &super->wbcs_mwb;

		mwb->wb_dirty_flush_thresh =
			cmd->wbcc_conf.wbcc_dirty_flush_thresh;
		conf->wbcc_dirty_flush_thresh =
			cmd->wbcc_conf.wbcc_dirty_flush_thresh;
	}

	if (cmd->wbcc_flags & WBC_CMD_OP_ACTIVE_DATA_WRITEBACK)
		conf->wbcc_active_data_writeback =
			cmd->wbcc_conf.wbcc_active_data_writeback;

	return 0;
}

/* @wbc_wq serves all asynchronous writeback tasks. */
struct workqueue_struct *wbc_wq;

void wbc_kill_super(struct wbc_super *super)
{
	struct memfs_writeback *mwb = &super->wbcs_mwb;

	mod_delayed_work(wbc_wq, &mwb->wb_dwork, 0);
	flush_delayed_work(&mwb->wb_dwork);
	WARN_ON(!list_empty(&mwb->wb_work_list));
	WARN_ON(delayed_work_pending(&mwb->wb_dwork));
}

void wbc_super_fini(struct wbc_super *super)
{
	struct memfs_writeback *mwb = &super->wbcs_mwb;
	int i;

	LASSERT(list_empty(&super->wbcs_rsvd_inode_lru));
	LASSERT(list_empty(&super->wbcs_data_inode_lru));

	if (super->wbcs_reclaim_task) {
		kthread_stop(super->wbcs_reclaim_task);
		super->wbcs_reclaim_task = NULL;
	}

	mod_delayed_work(wbc_wq, &mwb->wb_dwork, 0);
	flush_delayed_work(&mwb->wb_dwork);
	WARN_ON(!list_empty(&mwb->wb_work_list));
	WARN_ON(delayed_work_pending(&mwb->wb_dwork));

	for (i = 0; i < NR_WB_STAT; i++)
		percpu_counter_destroy(&mwb->wb_stat[i]);

	percpu_counter_destroy(&super->wbcs_conf.wbcc_used_pages);
}

static void wbc_workfn(struct work_struct *work);

int wbc_super_init(struct wbc_super *super, struct super_block *sb)
{
	struct memfs_writeback *mwb = &super->wbcs_mwb;
	struct wbc_conf *conf = &super->wbcs_conf;
	int rc;
	int i;

	ENTRY;

#ifdef HAVE_PERCPU_COUNTER_INIT_GFP_FLAG
	rc = percpu_counter_init(&conf->wbcc_used_pages, 0, GFP_KERNEL);
#else
	rc = percpu_counter_init(&conf->wbcc_used_pages, 0);
#endif
	if (rc)
		RETURN(-ENOMEM);

	spin_lock_init(&mwb->wb_work_lock);
	INIT_LIST_HEAD(&mwb->wb_work_list);
	INIT_DELAYED_WORK(&mwb->wb_dwork, wbc_workfn);
	init_waitqueue_head(&mwb->wb_waitq);
	mwb->wb_sb = sb;

	for (i = 0; i < NR_WB_STAT; i++) {
#ifdef HAVE_PERCPU_COUNTER_INIT_GFP_FLAG
		rc = percpu_counter_init(&mwb->wb_stat[i], 0, GFP_KERNEL);
#else
		rc = percpu_counter_init(&mwb->wb_stat[i], 0);
#endif
		if (rc)
			GOTO(out_err, rc);
	}

	conf->wbcc_cache_mode = WBC_MODE_NONE;
	conf->wbcc_flush_mode = WBC_FLUSH_NONE;
	wbc_super_reset_common_conf(conf);
	spin_lock_init(&super->wbcs_lock);
	INIT_LIST_HEAD(&super->wbcs_roots);
	INIT_LIST_HEAD(&super->wbcs_lazy_roots);
	INIT_LIST_HEAD(&super->wbcs_rsvd_inode_lru);
	INIT_LIST_HEAD(&super->wbcs_data_inode_lru);
	spin_lock_init(&super->wbcs_data_lru_lock);

	super->wbcs_context.ioc_anchor_used = 1;
	wbc_sync_io_init(&super->wbcs_context.ioc_anchor, 0);

	super->wbcs_reclaim_task = kthread_run(ll_wbc_reclaim_main, super,
					       "ll_wbc_reclaimer");
	if (IS_ERR(super->wbcs_reclaim_task)) {
		rc = PTR_ERR(super->wbcs_reclaim_task);
		super->wbcs_reclaim_task = NULL;
		CERROR("Cannot start WBC reclaim thread: rc = %d\n", rc);
		GOTO(out_err, rc);
	}

	RETURN(0);
out_err:
	while (i--)
		percpu_counter_destroy(&mwb->wb_stat[i]);
	percpu_counter_destroy(&conf->wbcc_used_pages);
	RETURN(rc);
}

static int wbc_parse_value_pair(struct wbc_cmd *cmd, char *buffer)
{
	struct wbc_conf *conf = &cmd->wbcc_conf;
	char *key, *val, *rest;
	unsigned long num;
	int rc;

	val = buffer;
	key = strsep(&val, "=");
	if (val == NULL || strlen(val) == 0)
		return -EINVAL;

	/* Key of the value pair */
	if (strcmp(key, "cache_mode") == 0) {
		if (strcmp(val, "memfs") == 0)
			conf->wbcc_cache_mode = WBC_MODE_MEMFS;
		else if (strcmp(val, "dop") == 0)
			conf->wbcc_cache_mode = WBC_MODE_DATA_PCC;
		else
			return -EINVAL;

		cmd->wbcc_flags |= WBC_CMD_OP_CACHE_MODE;
	} else if (strcmp(key, "flush_mode") == 0) {
		if (strcmp(val, "lazy") == 0)
			conf->wbcc_flush_mode = WBC_FLUSH_LAZY_DEFAULT;
		else if (strcmp(val, "lazy_drop") == 0)
			conf->wbcc_flush_mode = WBC_FLUSH_LAZY_DROP;
		else if (strcmp(val, "lazy_keep") == 0)
			conf->wbcc_flush_mode = WBC_FLUSH_LAZY_KEEP;
		else if (strcmp(val, "aging_drop") == 0)
			conf->wbcc_flush_mode = WBC_FLUSH_AGING_DROP;
		else if (strcmp(val, "aging_keep") == 0)
			conf->wbcc_flush_mode = WBC_FLUSH_AGING_KEEP;
		else
			return -EINVAL;

		cmd->wbcc_flags |= WBC_CMD_OP_FLUSH_MODE;
	} else if (strcmp(key, "max_rpcs") == 0) {
		rc = kstrtoul(val, 10, &num);
		if (rc)
			return rc;

		conf->wbcc_max_rpcs = num;
		cmd->wbcc_flags |= WBC_CMD_OP_MAX_RPCS;
	} else if (strcmp(key, "max_qlen") == 0) {
		rc = kstrtoul(val, 10, &num);
		if (rc)
			return rc;

		conf->wbcc_max_qlen = num;
		cmd->wbcc_flags |= WBC_CMD_OP_MAX_QLEN;
	} else if (strcmp(key, "max_nrpages_per_file") == 0) {
		rc = kstrtoul(val, 10, &num);
		if (rc)
			return rc;

		conf->wbcc_max_nrpages_per_file = num;
		cmd->wbcc_flags |= WBC_CMD_OP_MAX_NRPAGES_PER_FILE;
	} else if (strcmp(key, "max_rmfid_count") == 0) {
		rc = kstrtoul(val, 10, &num);
		if (rc)
			return rc;

		conf->wbcc_max_rmfid_count = num;
		cmd->wbcc_flags |= WBC_CMD_OP_MAX_RMFID_COUNT;
	} else if (strcmp(key, "rmpol") == 0) {
		if (strcmp(val, "sync") == 0)
			conf->wbcc_rmpol = WBC_RMPOL_SYNC;
		else if (strcmp(val, "delay") == 0)
			conf->wbcc_rmpol = WBC_RMPOL_DELAY;
		else if (strcmp(val, "subtree") == 0)
			conf->wbcc_rmpol = WBC_RMPOL_SUBTREE;
		else
			return -EINVAL;

		cmd->wbcc_flags |= WBC_CMD_OP_RMPOL;
	} else if (strcmp(key, "readdir_pol") == 0) {
		if (strcmp(val, "dcache_compat") == 0)
			conf->wbcc_readdir_pol = WBC_READDIR_DCACHE_COMPAT;
		else if (strcmp(val, "dcache_decomp") == 0)
			conf->wbcc_readdir_pol = WBC_READDIR_DCACHE_DECOMPLETE;
		else
			return -EINVAL;

		cmd->wbcc_flags |= WBC_CMD_OP_READDIR_POL;
	} else if (strcmp(key, "hiwm_ratio") == 0) {
		rc = kstrtoul(val, 10, &num);
		if (rc)
			return rc;

		if (num >= 100)
			return -ERANGE;

		conf->wbcc_hiwm_ratio = num;
		cmd->wbcc_flags |= WBC_CMD_OP_RECLAIM_RATIO;
	} else if (strcmp(key, "max_inodes") == 0) {
		conf->wbcc_max_inodes = memparse(val, &rest);
		if (*rest)
			return -EINVAL;

		cmd->wbcc_flags |= WBC_CMD_OP_INODES_LIMIT;
	} else if (strcmp(key, "max_pages") == 0) {
		conf->wbcc_max_pages = memparse(val, &rest);
		if (*rest)
			return -EINVAL;

		cmd->wbcc_flags |= WBC_CMD_OP_PAGES_LIMIT;
	} else if (strcmp(key, "size") == 0) {
		unsigned long long size;

		size = memparse(val, &rest);
		if (*rest == '%') {
			size <<= PAGE_SHIFT;
			size *= cfs_totalram_pages();
			do_div(size, 100);
			rest++;
		}
		if (*rest)
			return -EINVAL;

		conf->wbcc_max_pages = DIV_ROUND_UP(size, PAGE_SIZE);
		cmd->wbcc_flags |= WBC_CMD_OP_PAGES_LIMIT;
	} else if (strcmp(key, "flush_pol") == 0) {
		if (strcmp(val, "batch") == 0)
			conf->wbcc_flush_pol = WBC_FLUSH_POL_BATCH;
		else if (strcmp(val, "rqset") == 0)
			conf->wbcc_flush_pol = WBC_FLUSH_POL_RQSET;
		else
			return -EINVAL;

		cmd->wbcc_flags |= WBC_CMD_OP_FLUSH_POL;
	} else if (strcmp(key, "max_batch_count") == 0) {
		rc = kstrtoul(val, 10, &num);
		if (rc)
			return rc;

		conf->wbcc_max_batch_count = num;
		cmd->wbcc_flags |= WBC_CMD_OP_MAX_BATCH_COUNT;
	} else if (strcmp(key, "dirty_flush_thresh") == 0) {
		rc = kstrtoul(val, 10, &num);
		if (rc)
			return rc;

		conf->wbcc_dirty_flush_thresh = num;
		cmd->wbcc_flags |= WBC_CMD_OP_DIRTY_FLUSH_THRESH;
	} else if (strcmp(key, "active_data_writeback") == 0 ||
		   strcmp(key, "adw") == 0) {
		bool result;

		rc = kstrtobool(val, &result);
		if (rc)
			return rc;

		conf->wbcc_active_data_writeback = result;
		cmd->wbcc_flags |= WBC_CMD_OP_ACTIVE_DATA_WRITEBACK;
	} else {
		return -EINVAL;
	}

	return 0;
}

static int wbc_parse_value_pairs(struct wbc_cmd *cmd, char *buffer)
{
	char *val;
	char *token;
	int rc;

	val = buffer;
	while (val != NULL && strlen(val) != 0) {
		token = strsep(&val, " ");
		rc = wbc_parse_value_pair(cmd, token);
		if (rc)
			return rc;
	}

	/* TODO: General valid check for the WBC commands. */
	return 0;
}

static struct wbc_cmd *wbc_cmd_parse(char *buffer, unsigned long count)
{
	static struct wbc_cmd *cmd;
	char *token;
	char *val;
	int rc = 0;

	ENTRY;

	OBD_ALLOC_PTR(cmd);
	if (cmd == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	/* Disable WBC on the client, and clear all setting */
	if (strncmp(buffer, "disable", 7) == 0) {
		cmd->wbcc_cmd = WBC_CMD_DISABLE;
		RETURN(cmd);
	}

	if (strncmp(buffer, "enable", 6) == 0) {
		cmd->wbcc_cmd = WBC_CMD_ENABLE;
		RETURN(cmd);
	}

	if (strncmp(buffer, "clear", 5) == 0) {
		cmd->wbcc_cmd = WBC_CMD_CLEAR;
		RETURN(cmd);
	}

	val = buffer;
	token = strsep(&val, " ");
	if (val == NULL || strlen(val) == 0)
		GOTO(out_free_cmd, rc = -EINVAL);

	/* Type of the command */
	if (strcmp(token, "conf") == 0)
		cmd->wbcc_cmd = WBC_CMD_CONFIG;
	else
		GOTO(out_free_cmd, rc = -EINVAL);

	rc = wbc_parse_value_pairs(cmd, val);
	if (rc == 0)
		RETURN(cmd);

out_free_cmd:
	OBD_FREE_PTR(cmd);
	RETURN(ERR_PTR(rc));
}

int wbc_cmd_handle(struct wbc_super *super, struct wbc_cmd *cmd)
{
	struct wbc_conf *conf = &super->wbcs_conf;
	int rc = 0;

	spin_lock(&super->wbcs_lock);
	switch (cmd->wbcc_cmd) {
	case WBC_CMD_DISABLE:
		wbc_super_disable_cache(super);
		super->wbcs_generation++;
		break;
	case WBC_CMD_ENABLE:
		if (conf->wbcc_cache_mode == WBC_MODE_NONE) {
			wbc_super_conf_default(conf);
			super->wbcs_generation++;
		} else {
			rc = -EALREADY;
		}
		break;
	case WBC_CMD_CONFIG:
	case WBC_CMD_CHANGE:
		rc = wbc_super_conf_update(super, conf, cmd);
		if (rc == 0)
			super->wbcs_generation++;
		break;
	case WBC_CMD_CLEAR:
		spin_unlock(&super->wbcs_lock);
		rc = wbc_super_shrink_roots(super);
		spin_lock(&super->wbcs_lock);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	spin_unlock(&super->wbcs_lock);
	return rc;
}

int wbc_cmd_parse_and_handle(char *buffer, unsigned long count,
			     struct wbc_super *super)
{
	int rc = 0;
	struct wbc_cmd *cmd;

	ENTRY;

	cmd = wbc_cmd_parse(buffer, count);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);

	rc = wbc_cmd_handle(super, cmd);
	OBD_FREE_PTR(cmd);
	return rc;
}

static inline bool wbc_over_dirty_threshold(struct memfs_writeback *mwb)
{
	bool over;

	if (mwb->wb_dirty_flush_thresh < 2 * wbc_stat_error())
		over = wbc_stat_sum(mwb, WB_INODE_DIRTY) >
		       mwb->wb_dirty_flush_thresh;
	else
		over = wbc_stat(mwb, WB_INODE_DIRTY) >
		       mwb->wb_dirty_flush_thresh;

	return over;
}

int wbc_workqueue_init(void)
{
	wbc_wq = alloc_workqueue("ll_writeback", WQ_MEM_RECLAIM | WQ_FREEZABLE |
						 WQ_UNBOUND | WQ_HIGHPRI,
						 num_online_cpus());
	if (!wbc_wq)
		return -ENOMEM;

	return 0;
}

void wbc_workqueue_fini(void)
{
	destroy_workqueue(wbc_wq);
}

/* fs-writeback.c */
struct wbc_completion {
	atomic_t		cnt;
};

/*
 * Passed into wb_writeback(), essentially a subset of writeback_control
 */
struct wb_writeback_work {
	long nr_pages;
	struct super_block *sb;
	enum writeback_sync_modes sync_mode;
	unsigned int tagged_writepages:1;
	unsigned int for_kupdate:1;
	unsigned int range_cyclic:1;
	unsigned int for_background:1;
	unsigned int for_sync:1;	/* sync(2) WB_SYNC_ALL writeback */
	unsigned int auto_free:1;	/* free on completion */
	unsigned int for_flush:1;
	unsigned int auto_dput:1;
	enum wb_reason reason;		/* why was writeback initiated? */

	struct list_head list;		/* pending work list */
	struct wbc_completion *done;	/* set if the caller waits */

	struct dentry *parent;		/* parent candidate for batch flush */
};

enum wbc_state {
	/* Flusher is writing out dirty inodes aggressively. */
	WB_FLUSHER_RUNNING,
};

static void finish_writeback_work(struct memfs_writeback *mwb,
				  struct wb_writeback_work *work)
{
	struct wbc_completion *done = work->done;

	if (work->auto_dput) {
		LASSERT(work->parent != NULL);
		dput(work->parent);
	}

	if (work->auto_free)
		OBD_FREE_PTR(work);

	/* Wake up when writeback work item is done. */
	if (done && atomic_dec_and_test(&done->cnt))
		wake_up_all(&mwb->wb_waitq);
}

static void wb_queue_work(struct memfs_writeback *mwb,
			  struct wb_writeback_work *work)
{
	if (work->done)
		atomic_inc(&work->done->cnt);

	spin_lock_bh(&mwb->wb_work_lock);
	list_add_tail(&work->list, &mwb->wb_work_list);
	mod_delayed_work(wbc_wq, &mwb->wb_dwork, 0);
	spin_unlock_bh(&mwb->wb_work_lock);
}

static bool wb_io_lists_populated(struct bdi_writeback *wb)
{
	if (wb_has_dirty_io(wb)) {
		return false;
	} else {
#ifdef HAVE_WB_HAS_DIRTY_IO
		set_bit(WB_has_dirty_io, &wb->state);
		WARN_ON_ONCE(!wb->avg_write_bandwidth);
		atomic_long_add(wb->avg_write_bandwidth,
				&wb->bdi->tot_write_bandwidth);
#endif
		return true;
	}
}

static void wb_io_lists_depopulated(struct bdi_writeback *wb)
{
#ifdef HAVE_WB_HAS_DIRTY_IO
	if (wb_has_dirty_io(wb) && list_empty(&wb->b_dirty) &&
	    list_empty(&wb->b_io) && list_empty(&wb->b_more_io)) {
		clear_bit(WB_has_dirty_io, &wb->state);
		WARN_ON_ONCE(atomic_long_sub_return(wb->avg_write_bandwidth,
					&wb->bdi->tot_write_bandwidth) < 0);
	}
#endif
}

static inline struct inode *wb_inode(struct list_head *head)
{
	return list_entry(head, struct inode, i_io_list);
}

/**
 * inode_io_list_move_locked - move an inode onto a bdi_writeback IO list
 * @inode: inode to be moved
 * @wb: target bdi_writeback
 * @head: one of @wb->b_{dirty|io|more_io|dirty_time}
 *
 * Move @inode->i_io_list to @list of @wb and set %WB_has_dirty_io.
 * Returns %true if @inode is the first occupant of the !dirty_time IO
 * lists; otherwise, %false.
 */
static bool inode_io_list_move_locked(struct inode *inode,
				      struct bdi_writeback *wb,
				      struct list_head *head)
{
	assert_spin_locked(&wb->list_lock);

	list_move(&inode->i_io_list, head);

	/* FIXME: @b_dirty_time doesn't count as dirty_io until expiration */
	return wb_io_lists_populated(wb);
}

/**
 * inode_io_list_del_locked - remove an inode from its bdi_writeback IO list
 * @inode: inode to be removed
 * @wb: bdi_writeback @inode is being removed from
 *
 * Remove @inode which may be on one of @wb->b_{dirty|io|more_io} lists and
 * clear %WB_has_dirty_io if all are empty afterwards.
 */
static void inode_io_list_del_locked(struct inode *inode,
				     struct bdi_writeback *wb)
{
	assert_spin_locked(&wb->list_lock);
	assert_spin_locked(&inode->i_lock);

	inode->i_state &= ~I_SYNC_QUEUED;
	list_del_init(&inode->i_io_list);
	wb_io_lists_depopulated(wb);
}

/*
 * Redirty an inode: set its when-it-was dirtied timestamp and move it to the
 * furthest end of its superblock's dirty-inode list.
 *
 * Before stamping the inode's ->dirtied_when, we check to see whether it is
 * already the most-recently-dirtied inode on the b_dirty list.  If that is
 * the case then the inode must have been redirtied while it was being written
 * out and we don't reset its dirtied_when.
 */
static void redirty_tail_locked(struct inode *inode, struct bdi_writeback *wb)
{
	assert_spin_locked(&inode->i_lock);

	if (!list_empty(&wb->b_dirty)) {
		struct inode *tail;

		tail = wb_inode(wb->b_dirty.next);
		if (time_before(inode->dirtied_when, tail->dirtied_when))
			inode->dirtied_when = jiffies;
	}
	inode_io_list_move_locked(inode, wb, &wb->b_dirty);
	inode->i_state &= ~I_SYNC_QUEUED;
}

static void redirty_tail(struct inode *inode, struct bdi_writeback *wb)
{
	spin_lock(&inode->i_lock);
	redirty_tail_locked(inode, wb);
	spin_unlock(&inode->i_lock);
}

/*
 * requeue inode for re-scanning after bdi->b_io list is exhausted.
 */
static inline void requeue_io(struct inode *inode, struct bdi_writeback *wb)
{
	inode_io_list_move_locked(inode, wb, &wb->b_more_io);

}

static void inode_sync_complete(struct inode *inode)
{
	inode->i_state &= ~I_SYNC;
	/* If inode is clean an unused, put it into LRU now... */
	//inode_add_lru(inode);
	/* Waiters must see I_SYNC cleared before being woken up */
	smp_mb();
	wake_up_bit(&inode->i_state, __I_SYNC);
}

static bool inode_dirtied_after(struct inode *inode, unsigned long t)
{
	bool ret = time_after(inode->dirtied_when, t);
#ifndef CONFIG_64BIT
	/*
	 * For inodes being constantly redirtied, dirtied_when can get stuck.
	 * It _appears_ to be in the future, but is actually in distant past.
	 * This test is necessary to prevent such wrapped-around relative times
	 * from permanently stopping the whole bdi writeback.
	 */
	ret = ret && time_before_eq(inode->dirtied_when, jiffies);
#endif
	return ret;
}

#define EXPIRE_DIRTY_ATIME 0x0001

/*
 * Move expired (dirtied before dirtied_before) dirty inodes from
 * @delaying_queue to @dispatch_queue.
 */
static int move_expired_inodes(struct list_head *delaying_queue,
			       struct list_head *dispatch_queue,
			       unsigned long dirtied_before)
{
	LIST_HEAD(tmp);
	struct list_head *pos, *node;
	struct super_block *sb = NULL;
	struct inode *inode;
	int do_sb_sort = 0;
	int moved = 0;

	while (!list_empty(delaying_queue)) {
		inode = wb_inode(delaying_queue->prev);
		if (inode_dirtied_after(inode, dirtied_before))
			break;
		list_move(&inode->i_io_list, &tmp);
		moved++;
		spin_lock(&inode->i_lock);
		inode->i_state |= I_SYNC_QUEUED;
		spin_unlock(&inode->i_lock);
		if (sb && sb != inode->i_sb)
			do_sb_sort = 1;
		sb = inode->i_sb;
	}

	/* just one sb in list, splice to dispatch_queue and we're done */
	if (!do_sb_sort) {
		list_splice(&tmp, dispatch_queue);
		goto out;
	}

	/* Move inodes from one superblock together */
	while (!list_empty(&tmp)) {
		sb = wb_inode(tmp.prev)->i_sb;
		list_for_each_prev_safe(pos, node, &tmp) {
			inode = wb_inode(pos);
			if (inode->i_sb == sb)
				list_move(&inode->i_io_list, dispatch_queue);
		}
	}
out:
	return moved;
}

/*
 * Queue all expired dirty inodes for io, eldest first.
 * Before
 *         newly dirtied     b_dirty    b_io    b_more_io
 *         =============>    gf         edc     BA
 * After
 *         newly dirtied     b_dirty    b_io    b_more_io
 *         =============>    g          fBAedc
 *                                           |
 *                                           +--> dequeue for IO
 */
static void queue_io(struct bdi_writeback *wb, struct wb_writeback_work *work,
		     unsigned long dirtied_before)
{
	int moved;

	assert_spin_locked(&wb->list_lock);
	list_splice_init(&wb->b_more_io, &wb->b_io);
	moved = move_expired_inodes(&wb->b_dirty, &wb->b_io, dirtied_before);

	/* FIXME: dirty time writeback. */
	if (moved)
		wb_io_lists_populated(wb);
}

/*
 * Sleep until I_SYNC is cleared. This function must be called with i_lock
 * held and drops it. It is aimed for callers not holding any inode reference
 * so once i_lock is dropped, inode can go away.
 */
static void inode_sleep_on_writeback(struct inode *inode)
	__releases(inode->i_lock)
{
	DEFINE_WAIT(wait);
	wait_queue_head_t *wqh = bit_waitqueue(&inode->i_state, __I_SYNC);
	int sleep;

	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	sleep = inode->i_state & I_SYNC;
	spin_unlock(&inode->i_lock);
	if (sleep)
		schedule();
	finish_wait(wqh, &wait);
}

/*
 * Find proper writeback list for the inode depending on its current state and
 * possibly also change of its state while we were doing writeback.  Here we
 * handle things such as livelock prevention or fairness of writeback among
 * inodes. This function can be called only by flusher thread - noone else
 * processes all inodes in writeback lists and requeueing inodes behind flusher
 * thread's back can have unexpected consequences.
 */
static void requeue_inode(struct inode *inode, struct bdi_writeback *wb,
			  struct writeback_control *wbc)
{
	if (inode->i_state & I_FREEING)
		return;

	/*
	 * Sync livelock prevention. Each inode is tagged and synced in one
	 * shot. If still dirty, it will be redirty_tail()'ed below.  Update
	 * the dirty time to prevent enqueue and sync it again.
	 */
	if ((inode->i_state & I_DIRTY) &&
	    (wbc->sync_mode == WB_SYNC_ALL || wbc->tagged_writepages))
		inode->dirtied_when = jiffies;

	if (wbc->pages_skipped) {
		/*
		 * writeback is not making progress due to locked
		 * buffers. Skip this inode for now.
		 */
		redirty_tail_locked(inode, wb);
		return;
	}

#if 0
	if (mapping_tagged(inode->i_mapping, PAGECACHE_TAG_DIRTY)) {
		/*
		 * We didn't write back all the pages.  nfs_writepages()
		 * sometimes bales out without doing anything.
		 */
		if (wbc->nr_to_write <= 0) {
			/* Slice used up. Queue for next turn. */
			requeue_io(inode, wb);
		} else {
			/*
			 * Writeback blocked by something other than
			 * congestion. Delay the inode for some time to
			 * avoid spinning on the CPU (100% iowait)
			 * retrying writeback of the dirty page/inode
			 * that cannot be performed immediately.
			 */
			redirty_tail_locked(inode, wb);
		}
	} else
#endif
	if (inode->i_state & I_DIRTY) {
		/*
		 * Filesystems can dirty the inode during writeback operations,
		 * such as delayed allocation during submission or metadata
		 * updates after data IO completion.
		 */
		redirty_tail_locked(inode, wb);
#ifdef I_DIRTY_TIME
	} else if (inode->i_state & I_DIRTY_TIME) {
		inode->dirtied_when = jiffies;
		inode_io_list_move_locked(inode, wb, &wb->b_dirty_time);
		inode->i_state &= ~I_SYNC_QUEUED;
#endif
	} else {
		/* The inode is clean. Remove from writeback lists. */
		inode_io_list_del_locked(inode, wb);
	}
}

static int write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int ret;

	if (inode->i_sb->s_op->write_inode && !is_bad_inode(inode)) {
		ret = inode->i_sb->s_op->write_inode(inode, wbc);
		return ret;
	}
	return 0;
}

static struct wb_writeback_work *get_next_work_item(struct memfs_writeback *mwb)
{
	struct wb_writeback_work *work = NULL;

	spin_lock_bh(&mwb->wb_work_lock);
	if (!list_empty(&mwb->wb_work_list)) {
		work = list_entry(mwb->wb_work_list.next,
				  struct wb_writeback_work, list);
		list_del_init(&work->list);
	}
	spin_unlock_bh(&mwb->wb_work_lock);
	return work;
}

/*
 * Write out an inode in MemFS.
 */
static int __writeback_single_inode(struct inode *inode,
				    struct writeback_control *wbc)
{
	//struct address_space *mapping = inode->i_mapping;
	unsigned dirty;
	int ret;

	ENTRY;

	WARN_ON(!(inode->i_state & I_SYNC));

	/*
	 * The inode may redirty the inode during the writeback, clear dirty
	 * metadata flags right before write_inode().
	 */
	spin_lock(&inode->i_lock);
	dirty = inode->i_state & I_DIRTY;
	inode->i_state &= ~dirty;

	/*
	 * Paired with smp_mb() in __mark_inode_dirty().  This allows
	 * __mark_inode_dirty() to test i_state without grabbing i_lock -
	 * either they see the I_DIRTY bits cleared or we see the dirtied
	 * inode.
	 *
	 * I_DIRTY_PAGES is always cleared together above even if @mapping
	 * still has dirty pages.  The flag is reinstated after smp_mb() if
	 * necessary.  This guarantees that either __mark_inode_dirty()
	 * sees clear I_DIRTY_PAGES or we see PAGECACHE_TAG_DIRTY.
	 */
	//smp_mb();

	//if (mapping_tagged(mapping, PAGECACHE_TAG_DIRTY))
	//	inode->i_state |= I_DIRTY_PAGES;

	spin_unlock(&inode->i_lock);

	/* Don't write the inode if only I_DIRTY_PAGES was set */
	if (dirty & ~I_DIRTY_PAGES) {
		int err = write_inode(inode, wbc);
		if (ret == 0)
			ret = err;
	}

	return ret;
}

static bool wbc_inode_check_parent(struct inode *inode)
{
	struct dentry *dentry;
	struct dentry *parent;
	struct wbc_inode *wbci;
	bool result;

	dentry = d_find_any_alias(inode);
	if (dentry == NULL)
		return false;

	parent = dentry->d_parent;
	wbci = ll_i2wbci(parent->d_inode);
	if (wbci->wbci_flags == WBC_STATE_FL_NONE)
		result = true;
	else if (wbci->wbci_flags & WBC_STATE_FL_SYNC)
		result = true;

	dput(dentry);
	return result;
}

/*
 * Write a portion of b_io inodes which belong to @sb.
 *
 * Return the number of pages and/or inodes written.
 *
 * NOTE! This is called with wb->list_lock held, and will
 * unlock and relock that for each inode it ends up doing
 * IO for.
 */
static long wbc_writeback_sb_inodes(struct super_block *sb,
				    struct bdi_writeback *wb,
				    struct wb_writeback_work *work,
				    struct writeback_control *wbc)
{
	int rc = 0;

	ENTRY;

	while (!list_empty(&wb->b_io)) {
		struct inode *inode = wb_inode(wb->b_io.prev);

		if (inode->i_sb != sb) {
			if (work->sb) {
				/*
				 * We only want to write back data for this
				 * superblock, move all inodes not belonging
				 * to it back onto the dirty list.
				 */
				redirty_tail(inode, wb);
				continue;
			}

			/*
			 * The inode belongs to a different superblock.
			 * Bounce back to the caller to unpin this and
			 * pin the next superblock.
			 */
			break;
		}

		if (!wbc_inode_check_parent(inode))
			break;

		/*
		 * Don't bother with new inodes or inodes being freed, first
		 * kind does not need periodic writeout yet, and for the latter
		 * kind writeout is handled by the freer.
		 */
		spin_lock(&inode->i_lock);
		if (inode->i_state & (I_NEW | I_FREEING | I_WILL_FREE)) {
			redirty_tail_locked(inode, wb);
			spin_unlock(&inode->i_lock);
			continue;
		}

		if (inode->i_state & I_SYNC ||
		    ll_i2wbci(inode)->wbci_flags & WBC_STATE_FL_WRITEBACK) {
			spin_unlock(&inode->i_lock);
			break;
		}

		spin_unlock(&wb->list_lock);

		if (inode->i_state & I_SYNC) {
			/* Wait for I_SYNC. This function drops i_lock... */
			inode_sleep_on_writeback(inode);
			/* Inode may be gone, */
			spin_lock(&wb->list_lock);
			continue;
		}

		inode->i_state |= I_SYNC;
		spin_unlock(&inode->i_lock);

		/*
		 * We use I_SYNC to pin the inode in memroy. While it is set
		 * evict_inode() will wait so the inode cannot be freed.
		 */
		__writeback_single_inode(inode, wbc);

		if (need_resched())
			cond_resched();

		 /* Requeue @inode if still dirty. */
		spin_lock(&wb->list_lock);
		spin_lock(&inode->i_lock);
		requeue_inode(inode, wb, wbc);
		inode_sync_complete(inode);
		spin_unlock(&inode->i_lock);
	}

	return rc;
}

static int wb_writeback(struct memfs_writeback *mwb,
			struct wb_writeback_work *work,
			struct writeback_control *wbc)
{
	struct super_block *sb = mwb->wb_sb;
	struct backing_dev_info *bdi = sb->s_bdi;
	struct bdi_writeback *wb = &bdi->wb;
	int ret = 0;

	spin_lock(&wb->list_lock);
	for (;;) {
		//if (!wbc_over_dirty_threshold(mwb))
		//	break;

		if (list_empty(&wb->b_io))
			queue_io(wb, work, jiffies);

		ret = wbc_writeback_sb_inodes(sb, wb, work, wbc);
		if (ret < 0)
			break;

		/* No more inodes for IO, bail. */
		if (list_empty(&wb->b_more_io))
			break;
	}
	spin_unlock(&wb->list_lock);

	return ret;
}

static int wb_check_background_flush(struct memfs_writeback *mwb,
				     struct writeback_control *wbc)
{
	struct wb_writeback_work work = {
		.nr_pages	= 0,
		.sync_mode	= WB_SYNC_NONE,
		.for_background	= 1,
		.reason		= WB_REASON_BACKGROUND,
	};

	return wb_writeback(mwb, &work, wbc);
}

static int wb_writeback_parallel(struct memfs_writeback *mwb,
				 struct wb_writeback_work *work,
				 struct writeback_control_ext *wbcx)
{
	struct dentry *parent = work->parent;
	struct dentry *child;
	struct dentry *tmp;
	int rc;

	ENTRY;

	LASSERT(wbcx->has_ioctx);
	LASSERT(parent != NULL);

	spin_lock(&parent->d_lock);
	list_for_each_entry_safe(child, tmp, &parent->d_subdirs, d_child) {
		spin_lock_nested(&child->d_lock, DENTRY_D_LOCK_NESTED);
		if (child->d_inode != NULL && !d_unhashed(child)) {
			dget_dlock(child);
			spin_unlock(&child->d_lock);
			spin_unlock(&parent->d_lock);
			if (!(child->d_inode->i_state & I_SYNC))
				rc = wbcfs_writeback_dir_child(parent->d_inode,
							       child, wbcx);
			dput(child);
			if (rc)
				RETURN(rc);

			cond_resched();
			spin_lock(&parent->d_lock);
		} else {
			spin_unlock(&child->d_lock);
		}
	}
	spin_unlock(&parent->d_lock);

	RETURN(rc);
}

/*
 * Retrieve work items and do the writeback they describe
 */
static int wb_do_writeback(struct memfs_writeback *mwb,
			   struct writeback_control_ext *wbcx)
{
	struct wb_writeback_work *work;
	int ret;

	while ((work = get_next_work_item(mwb)) != NULL) {
		ret = wb_writeback_parallel(mwb, work, wbcx);
		finish_writeback_work(mwb, work);
		if (ret)
			return ret;
	}

	ret = wbcfs_context_commit(mwb->wb_sb, &wbcx->context);
	if (ret)
		return ret;

	ret = wb_check_background_flush(mwb, (struct writeback_control *)wbcx);

	return ret;
}

/*
 * Handle writeback of dirty inodes for the Lustre file system aggressively.
 */
static void wbc_workfn(struct work_struct *work)
{
	struct memfs_writeback *mwb = container_of(to_delayed_work(work),
						   struct memfs_writeback,
						   wb_dwork);
	struct writeback_control_ext wbcx = {
		.nr_to_write		= 0,
		.sync_mode		= WB_SYNC_NONE,
		.for_background		= 1,
		.for_pflush	= 1,
		.has_ioctx		= 1,
	};
	struct super_block *sb = mwb->wb_sb;
	int rc;

	ENTRY;

	set_bit(WB_FLUSHER_RUNNING, &mwb->wb_state);

	rc = wbcfs_context_init(sb, &wbcx.context, false, false);
	if (rc)
		RETURN_EXIT;

	rc = wb_do_writeback(mwb, &wbcx);
	if (rc)
		CDEBUG(D_CACHE, "Writeback failed: rc = %d\n", rc);

	rc = wbcfs_context_fini(sb, &wbcx.context);
	clear_bit(WB_FLUSHER_RUNNING, &mwb->wb_state);

	EXIT;
}

static inline bool wbc_flush_in_progress(struct memfs_writeback *mwb)
{
	return test_bit(WB_FLUSHER_RUNNING, &mwb->wb_state);
}

static void wbc_wakeup(struct memfs_writeback *mwb)
{
	mod_delayed_work(wbc_wq, &mwb->wb_dwork, 0);
}

/* FIXME: Cgroup writeback support. */
static void wbc_start_background_writeback(struct memfs_writeback *mwb)
{
	struct super_block *sb = mwb->wb_sb;
	struct backing_dev_info *bdi = sb->s_bdi;
	struct bdi_writeback *wb = &bdi->wb;

	if (!wb_has_dirty_io(wb)/*|| writeback_in_progress(wb)*/)
		return;

	/*
	 * All caller of this function want to start writeback of dirty inodes.
	 * Places like burst file creation can call this at a very high
	 * frequency, causing pointless allocations of tons of work items and
	 * keeping the flusher threads busy retrieving that work. Ensure that
	 * we only allow one of them pending and inflight at the time.
	 */
	if (test_bit(WB_FLUSHER_RUNNING, &mwb->wb_state) ||
	    test_and_set_bit(WB_FLUSHER_RUNNING, &mwb->wb_state))
		return;

	wbc_wakeup(mwb);
}

void wbc_check_dirty_flush(struct memfs_writeback *mwb)
{
	if (!wbc_cap_flush_parallel(mwb))
		return;

	if (wbc_flush_in_progress(mwb))
		return;

	if (wbc_over_dirty_threshold(mwb))
		wbc_start_background_writeback(mwb);
}

int wbc_queue_writeback_work(struct dentry *parent)
{
	struct memfs_writeback *mwb = ll_i2mwb(parent->d_inode);
	struct wb_writeback_work *work;

	if (!wbc_cap_flush_parallel(mwb))
		return 0;

	OBD_ALLOC_PTR(work);
	if (work == NULL)
		return -ENOMEM;

	work->sync_mode = WB_SYNC_NONE;
	work->for_flush = 1;
	work->auto_free = 1;
	work->auto_dput = 1;
	work->parent = dget(parent);
	wb_queue_work(mwb, work);

	return 0;
}
