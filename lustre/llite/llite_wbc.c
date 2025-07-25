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
 * Copyright (c) 2019, 2021, DDN Storage Corporation.
 */
/*
 * lustre/llite/llite_wbc.c
 *
 * Lustre Metadata Writeback Caching (WBC) Implementation
 *
 * Author: Oleg Drokin <green@whamcloud.com>
 * Author: Qian Yingjin <qian@ddn.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include "llite_internal.h"

static inline int wbc_ioctl_unreserve(struct dentry *dchild,
				      unsigned int unrsv_children)
{
	struct dentry *parent = dchild->d_parent;
	int rc;

	rc = wbc_make_dir_decomplete(parent->d_inode, parent, unrsv_children);
	if (rc == 0 && unrsv_children == 0)
		wbc_inode_unreserve_dput(dchild->d_inode, dchild);

	return rc;
}

#pragma GCC push_options
#pragma GCC optimize ("O1")
long wbc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct wbc_inode *wbci = ll_i2wbci(inode);
	int rc = 0;

	ENTRY;

	switch (cmd) {
	case LL_IOC_WBC_STATE: {
		struct lu_wbc_state __user *ustate =
			(struct lu_wbc_state __user *)arg;
		struct lu_wbc_state *state;

		OBD_ALLOC_PTR(state);
		if (state == NULL)
			RETURN(-ENOMEM);

		state->wbcs_fmode = inode->i_mode;
		state->wbcs_flags = wbci->wbci_flags;
		state->wbcs_cache_mode = wbci->wbci_cache_mode;
		state->wbcs_flush_mode = wbci->wbci_flush_mode;
		state->wbcs_dirty_flags = wbci->wbci_dirty_flags;

		if (copy_to_user(ustate, state, sizeof(*state)))
			GOTO(out_state, rc = -EFAULT);
out_state:
		OBD_FREE_PTR(state);
		RETURN(rc);
	}
	case LL_IOC_WBC_UNRESERVE: {
		struct lu_wbc_unreserve *unrsv;

		if (wbc_inode_none(ll_i2wbci(inode)))
			RETURN(0);

		OBD_ALLOC_PTR(unrsv);
		if (unrsv == NULL)
			RETURN(-ENOMEM);

		if (copy_from_user(unrsv,
				   (const struct lu_wbc_unreserve __user *)arg,
				   sizeof(*unrsv)))
			GOTO(out_unrsv_free, rc = -EFAULT);

		if (!inode_owner_or_capable(inode))
			GOTO(out_unrsv_free, rc = -EPERM);

		rc = wbc_ioctl_unreserve(file->f_path.dentry,
					 unrsv->wbcu_unrsv_siblings);
out_unrsv_free:
		OBD_FREE_PTR(unrsv);
		RETURN(rc);
	}
	case LL_IOC_PCC_STATE:
		RETURN(ll_i2sbi(inode)->ll_fop->unlocked_ioctl(file, cmd, arg));
	case LL_IOC_GET_MDTIDX:
		RETURN(ll_dir_operations.unlocked_ioctl(file, cmd, arg));
	case LL_IOC_LMV_GETSTRIPE:
		if (wbc_inode_was_flushed(wbci))
			RETURN(ll_dir_operations.unlocked_ioctl(file, cmd,
								arg));
		else
			RETURN(-ENODATA);
	default:
		RETURN(-ENOTTY);
	}
}
#pragma GCC pop_options

void wbcfs_inode_operations_switch(struct inode *inode)
{
	if (S_ISDIR(inode->i_mode)) {
		inode->i_fop = &ll_dir_operations;
		inode->i_op = &ll_dir_inode_operations;
	} else if (S_ISREG(inode->i_mode)) {
		struct ll_sb_info *sbi = ll_i2sbi(inode);

		inode->i_op = &ll_file_inode_operations;
		inode->i_fop = sbi->ll_fop;
		inode->i_mapping->a_ops = &ll_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &ll_fast_symlink_inode_operations;
	} else {
		inode->i_op = &ll_special_inode_operations;
		init_special_inode(inode, inode->i_mode,
				   inode->i_rdev);
	}
}

/*
 * Same as ll_d_init(), but init with valid flags.
 */
int wbcfs_d_init(struct dentry *de)
{
	struct ll_dentry_data *lld;

	ENTRY;

	LASSERT(de != NULL);

	CDEBUG(D_DENTRY, "ldd on dentry %.*s (%p) parent %p inode %p refc %d\n",
		de->d_name.len, de->d_name.name, de, de->d_parent, de->d_inode,
		ll_d_count(de));

	if (de->d_fsdata == NULL) {
		OBD_ALLOC_PTR(lld);
		if (likely(lld != NULL)) {
			spin_lock(&de->d_lock);
			if (likely(de->d_fsdata == NULL)) {
				de->d_fsdata = lld;
				wbc_dentry_init(de);
			} else {
				OBD_FREE_PTR(lld);
			}
			spin_unlock(&de->d_lock);
		} else {
			RETURN(-ENOMEM);
		}
	} else {
		/* Maybe no necessary, but for check and safety. */
		spin_lock(&de->d_lock);
		lld = ll_d2d(de);
		LASSERT(lld != NULL && lld->lld_invalid == 0);
		spin_unlock(&de->d_lock);
	}
	LASSERT(de->d_op == &ll_d_ops);

	RETURN(0);
}

static inline void wbc_context_note(struct wbc_context *ctx, int ioret)
{
	if (ctx && ctx->ioc_anchor_used)
		wbc_sync_io_note(&ctx->ioc_anchor, ioret);
}

static inline bool wbc_inode_should_pccrw(struct inode *inode)
{
	return S_ISREG(inode->i_mode) && wbc_cache_mode_dop(ll_i2wbci(inode));
}

static int wbc_pccrw_md_set(struct inode *inode, struct md_op_data *op_data,
			    __u64 *cr_flags, struct lov_user_md **md)
{
	struct pcc_dataset *dataset;
	struct lov_user_md *lum;
	int rc = 0;

	ENTRY;

	/* @rwid = 0 means that select the first available PCC dataset. */
	dataset = pcc_dataset_get(ll_i2pccs(inode), LU_PCC_READWRITE, 0);
	if (dataset == NULL)
		/* TODO: fallback WBC_MODE_MEMFS cache mode. */
		RETURN(-ENOENT);

	OBD_ALLOC_PTR(lum);
	if (lum == NULL)
		GOTO(out, rc = -ENOMEM);

	lum->lmm_magic = LOV_USER_MAGIC_V1;
	lum->lmm_pattern = LOV_PATTERN_F_RELEASED | LOV_PATTERN_RAID0;
	op_data->op_data = lum;
	op_data->op_data_size = sizeof(*lum);
	op_data->op_archive_id = dataset->pccd_rwid;
	ll_i2wbci(inode)->wbci_archive_id = dataset->pccd_rwid;
	*cr_flags |= MDS_OPEN_PCC;
	if (md)
		*md = lum;
out:
	pcc_dataset_put(dataset);
	RETURN(rc);
}

static void wbc_fini_op_item(struct md_op_item *item, int ioret)
{
	struct md_op_data *op_data = &item->mop_data;

	ll_unlock_md_op_lsm(op_data);
	security_release_secctx(op_data->op_file_secctx,
				op_data->op_file_secctx_size);

	if (item->mop_it.it_flags & MDS_OPEN_PCC) {
		struct lov_user_md *lum = op_data->op_data;

		LASSERT(op_data->op_data_size == sizeof(*lum));
		OBD_FREE_PTR(lum);
	}

	wbc_context_note(item->mop_cbdata, ioret);
	OBD_FREE_PTR(item);
}

static inline void wbc_prep_exlock_common(struct md_op_item *item, int it_op)
{
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;

	item->mop_it.it_op = it_op;
	einfo->ei_type = LDLM_IBITS;
	einfo->ei_mode = it_to_lock_mode(&item->mop_it);
	einfo->ei_cb_bl = ll_md_blocking_ast;
	einfo->ei_cb_cp = ldlm_completion_ast;
	einfo->ei_cb_gl = NULL;
	einfo->ei_cbdata = NULL;
	item->mop_lock_flags = LDLM_FL_INTENT_PARENT_LOCKED |
			       LDLM_FL_INTENT_EXLOCK_UPDATE;
}

static int wbc_create_exlock_cb(struct req_capsule *pill,
			       struct md_op_item *item, int rc)
{
	struct lookup_intent *it = &item->mop_it;
	struct dentry *dentry = item->mop_dentry;
	struct inode *inode = dentry->d_inode;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct wbc_inode *wbci = &lli->lli_wbc_inode;

	ENTRY;

	/*
	 * TODO: handling for error return.
	 * Discard the cached data, remove the whole subtree?
	 * Or notify the user via console messages.
	 */
	if (rc) {
		CERROR("Failed to async create: rc = %d!\n", rc);
		mapping_clear_unevictable(inode->i_mapping);
		GOTO(out_dput, rc);
	}

	/* Must return with EX lock. */
	if (it->it_lock_mode != LCK_EX)
		GOTO(out_dput, rc = -EPROTO);

	ll_set_lock_data(ll_i2mdexp(inode), inode, it, NULL);

	/*
	 * TODO: Currently the file layout for a regular file Must be
	 * instantiated when create the metadata file object.
	 * However, this can be optimized that instantiate the layout in need
	 * when the file is being written.
	 */
	rc = ll_prep_inode(&inode, pill, dentry->d_sb, it);
	if (rc)
		GOTO(out_it, rc);

	LASSERT(!wbc_decomplete_lock_keep(wbci, item) &&
		wbc_inode_has_protected(wbci) && wbc_inode_complete(wbci));
	spin_lock(&inode->i_lock);
	wbci->wbci_flags |= WBC_STATE_FL_ROOT | WBC_STATE_FL_SYNC;
	wbc_super_root_add(inode);
	wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
	spin_unlock(&inode->i_lock);

	wbci->wbci_lock_handle.cookie = it->it_lock_handle;
out_it:
	ll_intent_release(it);

out_dput:
	wbc_inode_unreserve_dput(inode, dentry);
	wbc_inode_writeback_complete(inode);
	wbc_fini_op_item(item, rc);

	RETURN(rc);
}

static int wbc_fill_create_common(struct inode *dir,
				  struct dentry *dchild,
				  struct md_op_item *item)
{
	struct md_op_data *op_data = &item->mop_data;
	struct lookup_intent *it = &item->mop_it;
	struct inode *inode = dchild->d_inode;
	int opc;

	ENTRY;

	switch (inode->i_mode & S_IFMT) {
	case S_IFDIR:
		opc = LUSTRE_OPC_MKDIR;
		break;
	case S_IFLNK:
		opc = LUSTRE_OPC_SYMLINK;
		break;
	case S_IFREG:
	default:
		opc = LUSTRE_OPC_CREATE;
	}

	op_data = ll_prep_md_op_data(op_data, dir, inode,
				     dchild->d_name.name, dchild->d_name.len,
				     inode->i_mode, opc, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	/*
	 * TODO: Set the file attributes such as timestamps, uid/gid, mode for
	 * the file correctly (If the file attributes was modified after
	 * created in MemFS).
	 */
	op_data->op_fsuid = from_kuid(&init_user_ns, inode->i_uid);
	op_data->op_fsgid = from_kgid(&init_user_ns, inode->i_gid);
	op_data->op_cap = cfs_curproc_cap_pack();

	if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
		op_data->op_rdev = old_encode_dev(inode->i_rdev);

	if (opc == LUSTRE_OPC_SYMLINK) {
		struct ll_inode_info *lli = ll_i2info(inode);

		LASSERT(lli->lli_symlink_name);

		op_data->op_data = lli->lli_symlink_name;
		op_data->op_data_size = strlen(lli->lli_symlink_name) + 1;
	} else if (wbc_inode_should_pccrw(inode)) {
		int rc;

		rc = wbc_pccrw_md_set(inode, op_data, &it->it_flags, NULL);
		if (rc)
			RETURN(rc);
	}

	/* Tell lmv we got the child fid under control */
	it->it_flags |= MDS_OPEN_BY_FID;

	if (!IS_POSIXACL(dir))
		it->it_create_mode &= ~current_umask();

	/* FIXME: Set create mode correctly. */
	it->it_create_mode = inode->i_mode & S_IALLUGO;
	if (S_ISDIR(inode->i_mode)) {
		it->it_create_mode = (inode->i_mode & (S_IRWXUGO | S_ISVTX)) |
				     S_IFDIR;
	} else if (S_ISREG(inode->i_mode)) {
		it->it_create_mode = (inode->i_mode & S_IALLUGO) | S_IFREG;
		/*
		 * With the create intent flags MDS_FMODE_WRITE, the data
		 * objects for this regular file will be created, and the layout
		 * will be instantiated and return to the client.
		 * TODO:
		 * - Send the current size of the file to MDT, then it can
		 *   determine how to allocate data object more intelligent
		 *   accordingly.
		 * - Delay to allocate data objects and instantiate the layout
		 *   for this regular file unless it is necessary if file size
		 *   is zero.
		 */
		it->it_flags |= MDS_FMODE_WRITE;
	} else if (S_ISLNK(inode->i_mode)) {
		it->it_create_mode = S_IFLNK | S_IRWXUGO;
	}

	RETURN(0);
}

/*
 * Prepare arguments for async create RPC.
 */
static struct md_op_item *
wbc_prep_create_exlock(struct inode *dir, struct dentry *dchild,
		       unsigned int valid)
{
	struct md_op_item *item;
	int rc;

	OBD_ALLOC_PTR(item);
	if (item == NULL)
		return ERR_PTR(-ENOMEM);

	rc = wbc_fill_create_common(dir, dchild, item);
	if (rc) {
		wbc_fini_op_item(item, rc);
		return ERR_PTR(rc);
	}

	wbc_prep_exlock_common(item, IT_CREAT);

	return item;
}

static int wbc_setattr_exlock_cb(struct req_capsule *pill,
				struct md_op_item *item, int rc)
{
	struct lookup_intent *it = &item->mop_it;
	struct dentry *dentry = item->mop_dentry;
	struct inode *inode = dentry->d_inode;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct wbc_inode *wbci = &lli->lli_wbc_inode;
	__u64 bits;

	ENTRY;

	if (rc) {
		CERROR("Failed to do WBC setattr: rc = %d!\n", rc);
		GOTO(out_dput, rc);
	}

	/* Must return with EX lock. */
	if (it->it_lock_mode != LCK_EX)
		GOTO(out_dput, rc = -EPROTO);

	ll_set_lock_data(ll_i2mdexp(inode), inode, it, &bits);
	LASSERT(bits & (MDS_INODELOCK_UPDATE | MDS_INODELOCK_LOOKUP));
	LASSERT(wbci->wbci_flags & WBC_STATE_FL_PROTECTED &&
		wbci->wbci_flags & WBC_STATE_FL_SYNC);
	LASSERT(!wbc_decomplete_lock_keep(wbci, item));
	spin_lock(&inode->i_lock);
	wbci->wbci_flags |= WBC_STATE_FL_ROOT;
	wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
	wbc_super_root_add(inode);
	spin_unlock(&inode->i_lock);

	wbci->wbci_lock_handle.cookie = it->it_lock_handle;
	ll_intent_release(it);

out_dput:
	wbc_inode_unreserve_dput(inode, dentry);
	wbc_inode_writeback_complete(inode);
	wbc_fini_op_item(item, rc);

	RETURN(rc);
}

static inline void wbc_generic_fillattr(struct inode *inode, struct iattr *attr)
{
	attr->ia_uid = inode->i_uid;
	attr->ia_gid = inode->i_gid;
	attr->ia_size = inode->i_size;
	attr->ia_atime = inode->i_atime;
	attr->ia_mtime = inode->i_mtime;
	attr->ia_ctime = inode->i_ctime;
	attr->ia_mode = inode->i_mode;
}

static void memfs_iattr_from_inode(struct inode *inode, unsigned int valid,
				   struct iattr *attr, enum op_xvalid *xvalid)
{
	attr->ia_valid = valid;
	wbc_generic_fillattr(inode, attr);

	if (valid & ATTR_SIZE)
		*xvalid |= OP_XVALID_OWNEROVERRIDE;
	if (valid & ATTR_CTIME)
		*xvalid |= OP_XVALID_CTIME_SET;
}

static void wbc_iattr_from_inode(struct inode *inode, unsigned int valid,
				 struct iattr *attr, enum op_xvalid *xvalid)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);

	switch (wbci->wbci_cache_mode) {
	case WBC_MODE_MEMFS:
		memfs_iattr_from_inode(inode, valid, attr, xvalid);
		break;
	default:
		break;
	}
}

/*
 * Prepare data for async setattr RPC.
 */
static struct md_op_item *
wbc_prep_setattr_exlock(struct inode *dir, struct dentry *dchild,
			unsigned int valid)
{
	struct md_op_item *item;
	struct inode *inode = dchild->d_inode;
	struct md_op_data *op_data;

	ENTRY;

	OBD_ALLOC_PTR(item);
	if (item == NULL)
		return ERR_PTR(-ENOMEM);

	op_data = ll_prep_md_op_data(&item->mop_data, inode, NULL, NULL, 0, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data)) {
		OBD_FREE_PTR(item);
		return (struct md_op_item *)op_data;
	}

	wbc_iattr_from_inode(inode, valid, &op_data->op_attr,
			     &op_data->op_xvalid);
	wbc_prep_exlock_common(item, IT_SETATTR);

	return item;
}

static int wbc_exlock_only_cb(struct req_capsule *pill,
			       struct md_op_item *item, int rc)
{
	struct lookup_intent *it = &item->mop_it;
	struct dentry *dentry = item->mop_dentry;
	struct inode *inode = dentry->d_inode;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct wbc_inode *wbci = &lli->lli_wbc_inode;
	__u64 bits;

	ENTRY;

	if (rc) {
		CERROR("Failed to acquire EX lock: rc = %d!\n", rc);
		GOTO(out_dput, rc);
	}

	/* Must return with EX lock. */
	if (it->it_lock_mode != LCK_EX)
		GOTO(out_dput, rc = -EPROTO);

	LASSERT(!wbc_decomplete_lock_keep(wbci, item));

	ll_set_lock_data(ll_i2mdexp(inode), inode, it, &bits);
	LASSERT(bits & (MDS_INODELOCK_UPDATE | MDS_INODELOCK_LOOKUP));
	LASSERT(wbci->wbci_flags & WBC_STATE_FL_PROTECTED &&
		wbci->wbci_flags & WBC_STATE_FL_SYNC);

	spin_lock(&inode->i_lock);
	wbci->wbci_flags |= WBC_STATE_FL_ROOT;
	wbc_super_root_add(inode);
	spin_unlock(&inode->i_lock);

	wbci->wbci_lock_handle.cookie = it->it_lock_handle;
	ll_intent_release(it);

out_dput:
	wbc_inode_unreserve_dput(inode, dentry);
	wbc_inode_writeback_complete(inode);
	wbc_fini_op_item(item, rc);

	RETURN(rc);
}

/*
 * Prepare data for WBC EX lock.
 * If the @inode is flushed to MDT in the state SYNC(S), during the time of
 * dropping the root WBC lock of the parent directory @dir level by level, the
 * client needs to acquire the WBC EX locks back on the children file @inode.
 */
static struct md_op_item *
wbc_prep_exlock_only(struct inode *dir, struct dentry *dchild,
		     unsigned int valid)
{
	struct md_op_item *item;
	struct inode *inode = dchild->d_inode;
	struct md_op_data *op_data;

	OBD_ALLOC_PTR(item);
	if (item == NULL)
		return ERR_PTR(-ENOMEM);

	op_data = ll_prep_md_op_data(&item->mop_data, inode, NULL, NULL, 0, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data)) {
		OBD_FREE_PTR(item);
		return (struct md_op_item *)op_data;
	}

	wbc_prep_exlock_common(item, IT_WBC_EXLOCK);
	return item;
}

static inline void wbc_prep_lockless_common(struct md_op_item *item, int it_op)
{
	item->mop_it.it_op = it_op;
	item->mop_data.op_bias |= MDS_WBC_LOCKLESS;
}

static inline bool wbc_active_data_writeback(struct inode *inode)
{
	return ll_i2wbcc(inode)->wbcc_active_data_writeback;
}

static int wbc_create_lockless_cb(struct req_capsule *pill,
				  struct md_op_item *item, int rc)
{
	struct dentry *dchild = item->mop_dentry;
	struct dentry *parent = dchild->d_parent;
	struct inode *inode = dchild->d_inode;
	struct wbc_inode *wbci = ll_i2wbci(inode);

	ENTRY;

	LASSERTF(wbc_inode_has_protected(wbci) &&
		 wbc_inode_reserved(wbci) &&
		 wbc_inode_complete(ll_i2wbci(parent->d_inode)),
		 "Create lockless %pd with state %08X\n",
		 dchild, wbci->wbci_flags);

	if (rc) {
		CERROR("Failed to create %pd with lockless IO, rc = %d\n",
		       dchild, rc);
		GOTO(out_fini, rc);
	}

	rc = ll_prep_inode(&inode, pill, dchild->d_sb, NULL);
	if (rc)
		GOTO(out_fini, rc);

	spin_lock(&inode->i_lock);
	wbci->wbci_flags |= WBC_STATE_FL_SYNC;
	wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
	spin_unlock(&inode->i_lock);
	if (item->mop_flags & WBC_FL_UNRSV_CHILDREN) {
		LASSERT(item->mop_flags & WBC_FL_DECOMPLETE);
		wbc_inode_unreserve_dput(inode, dchild);
	}

	LASSERT(wbc_mode_lock_keep(wbci));
	if (S_ISREG(inode->i_mode) && item->mop_flags & WBC_FL_SYNC_NONE &&
	    wbc_active_data_writeback(inode)) {
		/*
		 * Mark the inode as dirty, and the kernel flusher thread will
		 * delay to do the assimilation work to commit the cache pages
		 * form MemFS to Lustre.
		 */
		mark_inode_dirty(inode);
	} else if (S_ISDIR(inode->i_mode)) {
		if (ll_d2wbcd(dchild)->wbcd_dirent_num > 2) {
			/* Queue the directory for parallel flush. */
			rc = wbc_queue_writeback_work(dchild);
			if (rc)
				CERROR("Queue work for %pd failed: rc = %d\n",
				       dchild, rc);
		}
	}

out_fini:
	wbc_inode_writeback_complete(inode);
	wbc_fini_op_item(item, rc);

	RETURN(rc);
}

static struct md_op_item *
wbc_prep_create_lockless(struct inode *dir, struct dentry *dchild,
			 unsigned int valid)
{
	struct md_op_item *item;
	int rc;

	OBD_ALLOC_PTR(item);
	if (item == NULL)
		return ERR_PTR(-ENOMEM);

	rc = wbc_fill_create_common(dir, dchild, item);
	if (rc) {
		OBD_FREE_PTR(item);
		return ERR_PTR(rc);
	}

	wbc_prep_lockless_common(item, IT_CREAT);
	return item;
}

static int wbc_setattr_lockless_cb(struct req_capsule *pill,
				   struct md_op_item *item, int rc)
{
	struct dentry *dentry = item->mop_dentry;
	struct inode *inode = dentry->d_inode;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct wbc_inode *wbci = &lli->lli_wbc_inode;

	ENTRY;

	if (rc) {
		CERROR("Failed to async setattr: rc = %d!\n", rc);
		GOTO(out_dput, rc);
	}

	LASSERT(wbci->wbci_flags & WBC_STATE_FL_PROTECTED &&
		wbci->wbci_flags & WBC_STATE_FL_SYNC);
	LASSERT(!wbc_decomplete_lock_keep(wbci, item));
	spin_lock(&inode->i_lock);
	wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
	spin_unlock(&inode->i_lock);

out_dput:
	wbc_inode_writeback_complete(inode);
	wbc_fini_op_item(item, rc);

	RETURN(rc);
}

static struct md_op_item *
wbc_prep_setattr_lockless(struct inode *dir, struct dentry *dchild,
			  unsigned int valid)
{
	struct md_op_item *item;
	struct inode *inode = dchild->d_inode;
	struct md_op_data *op_data;

	OBD_ALLOC_PTR(item);
	if (item == NULL)
		return ERR_PTR(-ENOMEM);

	op_data = ll_prep_md_op_data(&item->mop_data, inode, NULL, NULL, 0, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data)) {
		OBD_FREE_PTR(item);
		return (struct md_op_item *)op_data;
	}

	wbc_iattr_from_inode(inode, valid, &op_data->op_attr,
			     &op_data->op_xvalid);
	wbc_prep_lockless_common(item, IT_SETATTR);

	return item;
}

typedef struct md_op_item *(*md_prep_op_item_t)(struct inode *dir,
						struct dentry *dchild,
						unsigned int valid);

md_prep_op_item_t wbc_op_item_preps[MD_OP_MAX] = {
	[MD_OP_CREATE_LOCKLESS]		= wbc_prep_create_lockless,
	[MD_OP_CREATE_EXLOCK]		= wbc_prep_create_exlock,
	[MD_OP_SETATTR_LOCKLESS]	= wbc_prep_setattr_lockless,
	[MD_OP_SETATTR_EXLOCK]		= wbc_prep_setattr_exlock,
	[MD_OP_EXLOCK_ONLY]		= wbc_prep_exlock_only,
};

md_op_item_cb_t wbc_op_item_cbs[MD_OP_MAX] = {
	[MD_OP_CREATE_LOCKLESS]		= wbc_create_lockless_cb,
	[MD_OP_CREATE_EXLOCK]		= wbc_create_exlock_cb,
	[MD_OP_SETATTR_LOCKLESS]	= wbc_setattr_lockless_cb,
	[MD_OP_SETATTR_EXLOCK]		= wbc_setattr_exlock_cb,
	[MD_OP_EXLOCK_ONLY]		= wbc_exlock_only_cb,
};

static struct md_op_item *
wbc_prep_op_item(enum md_opcode opc, struct inode *dir,
		 struct dentry *dchild, struct ldlm_lock *lock,
		 struct writeback_control_ext *wbcx,
		 unsigned int valid)
{
	struct md_op_item *item;

	if (opc >= MD_OP_MAX || wbc_op_item_preps[opc] == NULL ||
	    wbc_op_item_cbs[opc] == NULL)
		return ERR_PTR(-EINVAL);

	item = wbc_op_item_preps[opc](dir, dchild, valid);
	if (IS_ERR(item))
		return item;

	item->mop_opc = opc;
	item->mop_dentry = dchild;
	item->mop_cb = wbc_op_item_cbs[opc];

	if (wbcx->sync_mode == WB_SYNC_NONE)
		item->mop_flags |= WBC_FL_SYNC_NONE;
	if (wbcx->for_decomplete)
		item->mop_flags |= WBC_FL_DECOMPLETE;
	if (wbcx->unrsv_children_decomp)
		item->mop_flags |= WBC_FL_UNRSV_CHILDREN;

	if (lock)
		item->mop_data.op_open_handle = lock->l_remote_handle;

	return item;
}

int wbc_do_setattr(struct inode *inode, unsigned int valid)
{
	struct ptlrpc_request *request = NULL;
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct md_op_data *op_data;
	int mode = inode->i_mode;
	struct iattr *attr;
	int rc;

	ENTRY;

	OBD_ALLOC_PTR(op_data);
	if (op_data == NULL)
		RETURN(-ENOMEM);

	attr = &op_data->op_attr;
	attr->ia_valid = valid;
	wbc_generic_fillattr(inode, attr);

	if ((valid & (ATTR_CTIME | ATTR_SIZE | ATTR_MODE)) ==
	    (ATTR_CTIME | ATTR_SIZE | ATTR_MODE))
		op_data->op_xvalid |= OP_XVALID_OWNEROVERRIDE;

	if (((valid & (ATTR_MODE|ATTR_FORCE|ATTR_SIZE)) ==
	     (ATTR_SIZE|ATTR_MODE)) &&
	    ((mode & (S_ISGID|S_IXGRP)) == (S_ISGID|S_IXGRP)))
		attr->ia_valid |= ATTR_FORCE;

	if (valid & ATTR_SIZE)
		attr->ia_valid |= ATTR_MTIME | ATTR_CTIME;

	/* POSIX: check before ATTR_*TIME_SET set (from inode_change_ok) */
	if (attr->ia_valid & TIMES_SET_FLAGS) {
		if ((!uid_eq(current_fsuid(), inode->i_uid)) &&
		    !capable(CAP_FOWNER))
			RETURN(-EPERM);
	}

	/* We mark all of the fields "set" so MDS/OST does not re-set them */
	if (valid & ATTR_CTIME)
		op_data->op_xvalid |= OP_XVALID_CTIME_SET;

	if (valid & (ATTR_MTIME | ATTR_CTIME))
		CDEBUG(D_INODE, "setting mtime %lld, ctime %lld, now = %lld\n",
		       (s64)attr->ia_mtime.tv_sec, (s64)attr->ia_ctime.tv_sec,
		       ktime_get_real_seconds());

	attr->ia_valid &= ~ATTR_FILE;
	/*
	if (attr->ia_valid & ATTR_FILE) {
		struct ll_file_data *fd = attr->ia_file->private_data;

		if (fd->fd_lease_och)
			op_data->op_bias |= MDS_TRUNC_KEEP_LEASE;
	}
	*/

	ll_prep_md_op_data(op_data, inode, NULL, NULL, 0, 0,
			   LUSTRE_OPC_ANY, NULL);
	op_data->op_bias |= MDS_WBC_LOCKLESS;

	rc = md_setattr(sbi->ll_md_exp, op_data, NULL, 0, &request);
	if (rc) {
		if (rc == -ENOENT)
			clear_nlink(inode);
		else if (rc != -EPERM && rc != -EACCES && rc != -ETXTBSY)
			CERROR("md_setattr fails: rc = %d\n", rc);
	}

	ptlrpc_req_finished(request);
	if (rc)
		GOTO(out, rc);

	if ((attr->ia_valid & (ATTR_SIZE | ATTR_ATIME | ATTR_ATIME_SET |
			       ATTR_MTIME | ATTR_MTIME_SET | ATTR_CTIME) ||
	     op_data->op_xvalid & OP_XVALID_CTIME_SET) &&
	     wbc_inode_data_committed(ll_i2wbci(inode))) {
		struct ll_inode_info *lli = ll_i2info(inode);

		LASSERT(lli->lli_clob);
		rc = cl_setattr_ost(lli->lli_clob, attr, op_data->op_xvalid, 0);
	}

out:
	ll_finish_md_op_data(op_data);

	RETURN(rc);
}

static inline int wbc_make_inode_assimilated(struct inode *inode)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	int rc;

	if (!S_ISREG(inode->i_mode))
		return 0;

	if (wbc_inode_assimilated(wbci) || wbc_inode_none(wbci) ||
	    wbci->wbci_flags & WBC_STATE_FL_FREEING)
		return 0;

	down_write(&wbci->wbci_rw_sem);
	rc = wbcfs_commit_cache_pages(inode);
	up_write(&wbci->wbci_rw_sem);

	return rc > 0 ? 0 : rc;
}

static int wbc_sync_create(struct inode *inode, struct dentry *dchild)
{
	struct inode *dir = dchild->d_parent->d_inode;
	struct ptlrpc_request *request = NULL;
	struct ll_sb_info *sbi = ll_i2sbi(dir);
	struct lov_user_md *lum = NULL;
	struct md_op_data *op_data;
	const void *data = NULL;
	size_t datalen = 0;
	umode_t mode = 0;
	__u64 cr_flags = 0;
	int rdev = 0;
	int opc;
	int rc;

	ENTRY;

	switch (inode->i_mode & S_IFMT) {
	case S_IFDIR:
		opc = LUSTRE_OPC_MKDIR;
		mode = (inode->i_mode & (S_IRWXUGO | S_ISVTX)) | S_IFDIR;
		break;
	case S_IFLNK:
		opc = LUSTRE_OPC_SYMLINK;
		mode = S_IFLNK | S_IRWXUGO;
		data = ll_i2info(inode)->lli_symlink_name;
		datalen = strlen(ll_i2info(inode)->lli_symlink_name) + 1;
		break;
	case S_IFREG:
		mode = (inode->i_mode & S_IALLUGO) | S_IFREG;
		cr_flags |= MDS_FMODE_WRITE;
		/* fallthrough */
	default:
		rdev = old_encode_dev(inode->i_rdev);
		opc = LUSTRE_OPC_CREATE;
		break;
	}

	op_data = ll_prep_md_op_data(NULL, dir, inode,
				     dchild->d_name.name, dchild->d_name.len,
				     0, opc, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	if (wbc_inode_should_pccrw(inode)) {
		rc = wbc_pccrw_md_set(inode, op_data, &cr_flags, &lum);
		if (rc)
			GOTO(out, rc);

		data = op_data->op_data;
		datalen = op_data->op_data_size;
	}

	/* TODO: Set the timstamps for the inode correctly. */
	op_data->op_bias |= MDS_WBC_LOCKLESS;
	rc = md_create(sbi->ll_md_exp, op_data, data, datalen, mode,
		       from_kuid(&init_user_ns, inode->i_uid),
		       from_kgid(&init_user_ns, inode->i_gid),
		       cfs_curproc_cap_pack(), rdev, cr_flags, &request);
	if (rc)
		GOTO(out, rc);

	ll_update_times(request, dir);

	rc = ll_prep_inode(&inode, &request->rq_pill, dchild->d_sb, NULL);
	if (S_ISREG(inode->i_mode))
		LASSERTF(ll_i2info(inode)->lli_clob, "%pd %p, rc = %d\n",
			 dchild, inode, rc);

	ptlrpc_req_finished(request);
out:
	if (lum != NULL)
		OBD_FREE_PTR(lum);
	ll_finish_md_op_data(op_data);
	RETURN(rc);
}

static int wbc_do_create(struct inode *inode)
{
	struct dentry *dentry;
	int rc;

	ENTRY;

	/* TODO: Hardlink for a non-directory file. */
	dentry = d_find_any_alias(inode);
	if (!dentry)
		RETURN(0);

	rc = wbc_sync_create(inode, dentry);
	/*
	 * TODO: for WB_SYNC_NONE mode, do async create and data flush on
	 * background.
	 */
	if (rc == 0)
		rc = wbc_make_inode_assimilated(inode);

	dput(dentry);
	RETURN(rc);
}

/* TODO: if @valid != 0, still need to set attributes for the file. */
static int wbc_do_rmfid(struct inode *dir, unsigned int valid)
{
	__u32 max = ll_i2wbcc(dir)->wbcc_max_rmfid_count;
	struct wbc_inode *wbci = ll_i2wbci(dir);
	struct wbc_removed_item *item, *tmp;
	struct fid_array *fa = NULL;
	int *rcs = NULL;
	LIST_HEAD(head);
	unsigned int nr;
	__u32 total = 0;
	__u32 count;
	__u64 flags;
	size_t size;
	int rc;

	ENTRY;

	spin_lock(&wbci->wbci_removed_lock);
	count = wbci->wbci_removed_count;
	wbci->wbci_removed_count = 0;
	list_splice_init(&wbci->wbci_removed_list, &head);
	spin_unlock(&wbci->wbci_removed_lock);

	flags = OBD_FL_LOCKLESS;
	if (wbci->wbci_rmpol == WBC_RMPOL_SUBTREE)
		flags |= OBD_FL_SUBTREE_RM;

	nr = min_t(__u32, max, count);
	size = offsetof(struct fid_array, fa_fids[nr]);
	OBD_ALLOC(fa, size);
	if (!fa)
		RETURN(-ENOMEM);
	OBD_ALLOC_PTR_ARRAY(rcs, nr);
	if (!rcs)
		GOTO(free_fa, rc = -ENOMEM);

	list_for_each_entry_safe(item, tmp, &head, wbvi_item) {
		list_del_init(&item->wbvi_item);
		fa->fa_fids[fa->fa_nr] = item->wbvi_fid;
		fa->fa_nr++;
		total++;
		OBD_FREE_PTR(item);
		if (fa->fa_nr == max) {
			rc = md_rmfid(ll_i2mdexp(dir), fa, rcs, flags, NULL);
			if (rc)
				GOTO(free_rcs, rc);
			fa->fa_nr = 0;
		}
	}

	LASSERT(total == count);
	if (fa->fa_nr)
		rc = md_rmfid(ll_i2mdexp(dir), fa, rcs, flags, NULL);

free_rcs:
	OBD_FREE_PTR_ARRAY(rcs, nr);
free_fa:
	OBD_FREE(fa, size);
	RETURN(rc);
}

int wbcfs_inode_sync_metadata(long opc, struct inode *inode, unsigned int valid)
{
	switch (opc) {
	case MD_OP_SETATTR_LOCKLESS:
		RETURN(wbc_do_setattr(inode, valid));
	case MD_OP_REMOVE_LOCKLESS:
		RETURN(wbc_do_rmfid(inode, valid));
	case MD_OP_NONE:
		RETURN(0);
	default:
		RETURN(-EOPNOTSUPP);
	}
}

int wbcfs_setattr_data_object(struct inode *inode, struct iattr *attr)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	bool cached;
	int rc;

	ENTRY;

	inode_unlock(inode);
	if (wbc_cache_mode_dop(wbci)) {
		rc = pcc_inode_setattr(inode, attr, &cached);
		if (cached) {
			if (rc)
				CERROR("%s: PCC inode "DFID" setattr failed: "
				       "rc = %d\n", ll_i2sbi(inode)->ll_fsname,
				       PFID(ll_inode2fid(inode)), rc);
			GOTO(out, rc);
		}
	}

	rc = cl_setattr_ost(ll_i2info(inode)->lli_clob, attr,
			    OP_XVALID_OWNEROVERRIDE, 0);
out:
	inode_lock(inode);
	inode_dio_wait(inode);
	inode_has_no_xattr(inode);

	RETURN(rc);
}

static int wbc_reopen_pcc_file(struct inode *inode)
{
	struct dentry *dentry;
	struct ll_file_data *fd;
	struct wbc_dentry *wbcd;
	int rc = 0;

	ENTRY;

	/* FIXME: hardlink. */
	dentry = d_find_any_alias(inode);
	if (!dentry)
		RETURN(0);

	wbcd = ll_d2wbcd(dentry);
	/*
	 * It does not need to take @wbcd_open_lock as it is under the
	 * protection of the lock @wbci_rw_sem.
	 */
	list_for_each_entry(fd, &wbcd->wbcd_open_files,
			    fd_wbc_file.wbcf_open_item) {
		rc = pcc_file_open(inode, fd->fd_file);
		if (rc)
			break;
	}

	dput(dentry);
	RETURN(rc);
}

static int wbc_commit_data_pcc(struct inode *inode)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	struct address_space *mapping = inode->i_mapping;
	int nr_pages = mapping->nrpages;
	int rc;

	ENTRY;

	/*
	 * The file data has already assimilated from MemFS into Lustre
	 * or PCC.
	 */
	if (wbc_inode_data_committed(wbci) || wbc_inode_none(wbci))
		RETURN(0);

	rc = pcc_wbc_commit_data(inode, wbci->wbci_archive_id);
	if (rc < 0)
		RETURN(rc);

	rc = wbc_reopen_pcc_file(inode);
	/* XXX failure handling. */
	wbc_inode_data_lru_del(inode);
	spin_lock(&inode->i_lock);
	wbci->wbci_flags |= WBC_STATE_FL_DATA_COMMITTED;
	wbc_inode_unacct_pages(inode, mapping->nrpages);
	mapping->a_ops = &ll_aops;
	spin_unlock(&inode->i_lock);
	if (rc == 0)
		rc = nr_pages;
	mapping_clear_unevictable(mapping);

	RETURN(rc);
}

/*
 * Write the cached data in MemFS into Lustre clio for an inode.
 * TODO: It need to ensure that generic IO must be blocked in this phase until
 * finished data assimilation.
 */
static int wbc_commit_data_lustre(struct inode *inode)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	struct address_space *mapping = inode->i_mapping;
	struct lu_env *env = NULL;
	struct cl_io *io = NULL;
	struct cl_lock *lock = NULL;
	struct cl_lock_descr *descr;
	struct cl_page_list queue;
	struct cl_page *page = NULL;
	struct pagevec pvec;
	pgoff_t index = 0;
	__u16 refcheck;
	loff_t isize;
	int nr_pages;
	unsigned int to;
	int rc;

	ENTRY;

	/* The file data has already assimilated from MemFS into Lustre. */
	if (wbc_inode_data_committed(wbci) || wbc_inode_none(wbci))
		RETURN(0);

	isize = i_size_read(inode);
	LASSERTF(ll_i2info(inode)->lli_clob, "inode %p\n", inode);

	/* Get IO environment */
	rc = cl_io_get(inode, &env, &io, &refcheck);
	if (rc <= 0)
		GOTO(out, rc);

	/*
	 * It is still under the protection of root WBC EX lock where
	 * the granted cached ibits lock is MDS_INODELOCK_UPDATE |
	 * MDS_INODELOCK_LAYOUT at the time of the lock revocation.
	 */
	io->ci_ignore_layout = 1;
	rc = cl_io_init(env, io, CIT_MISC, io->ci_obj);
	if (rc) {
		/*
		 * If rc > 0, it means that the layout of the file is not yet
		 * instantiated. Nothing to do for this IO, return immediately.
		 */
		rc = io->ci_result;
		GOTO(out_env_put, rc);
	}

	lock = vvp_env_lock(env);
	descr = &lock->cll_descr;
	memset(descr, 0, sizeof(*descr)); /* XXX ??? */
	descr->cld_obj = io->ci_obj;
	descr->cld_mode = CLM_WRITE;
	descr->cld_start = 0;
	descr->cld_end = CL_PAGE_EOF; /* Could be cl_index(io->ci_obj, isize) */
	descr->cld_enq_flags = /*CEF_GLIMPSE parallel|*/ CEF_MUST;

	rc = cl_lock_request(env, io, lock);
	if (rc < 0)
		GOTO(out_io_fini, rc);

	/*
	 * Now the client has acquired all locks for page cache in MemFS.
	 * It is the time to assimilate these pages in MemFS into Lustre
	 * page cache via clio engine.
	 * XXX Eventually we actually want to do it all in parallel as the
	 * individual sub locks complete, but it's too much hassle now.
	 */

	/*
	 * Here it is safe to release WBC EX lock that guards the layout,
	 * since IO has already acquired all the extent locks for data IO.
	 * Or It does not need to acquire any exent locks for data IO, and
	 * could hold WBC EX lock until finished data assimulation, sync
	 * the data to OSTs and discard the cached data.
	 */


	cl_page_list_init(&queue);
	ll_pagevec_init(&pvec, 0);
	do {
		int i;
		struct page *vmpage = NULL;

#ifdef HAVE_PAGEVEC_LOOKUP_THREE_PARAM
		nr_pages = pagevec_lookup(&pvec, mapping, &index);
#else
		nr_pages = pagevec_lookup(&pvec, mapping, index, PAGEVEC_SIZE);
#endif

		for (i = 0 ; i < nr_pages ; i++) {
			vmpage = pvec.pages[i];
			/* XXX lock the page? */
			lock_page(vmpage);

			/* Cannot have any clean pages here! */
			LASSERT(PageDirty(vmpage));

			/*
			 * We are 100% sure that this page is not in the Lustre
			 * clio lists yet.
			 */
			page = cl_page_alloc(env, io->ci_obj, vmpage->index,
					    vmpage, CPT_CACHEABLE);
			if (IS_ERR(page)) {
				unlock_page(vmpage);
				pagevec_release(&pvec);
				GOTO(out_page_discard, rc = PTR_ERR(page));
			}

			lu_ref_add(&page->cp_reference, "cl_io", io);
			cl_page_assume(env, io, page);
			cl_page_list_add(&queue, page);
			/*
			 * TODO: Once accumulated one full RPC, commit it
			 * immediately.
			 */
		}

		/* Update the index for the next search */
		if (nr_pages > 0)
			index = vmpage->index + 1;

		pagevec_release(&pvec);
		cond_resched(); /* because why not? */
	} while (nr_pages > 0);

	if (queue.pl_nr == 0)
		GOTO(out_page_discard, rc);

	to = isize & (PAGE_SIZE - 1);
	if (to == 0)
		to = PAGE_SIZE;
	else if (page)
		/* Update the size accordingly for the last page. */
		cl_page_clip(env, page, 0, to);

	rc = cl_io_commit_async(env, io, &queue, 0, to, write_commit_callback);

	/*
	 * Now the pages in queue were failed to commit, discard them unless
	 * they were dirtied before.
	 */
out_page_discard:
	while (queue.pl_nr > 0) {
		page = cl_page_list_first(&queue);
		cl_page_list_del(env, &queue, page);

		if (!PageDirty(cl_page_vmpage(page)))
			cl_page_discard(env, io, page);

		cl_page_disown(env, io, page);
		lu_ref_del(&page->cp_reference, "cl_io", io);
		cl_page_put(env, page);
	}

	cl_page_list_fini(env, &queue);
	cl_lock_release(env, lock);
out_io_fini:
	cl_io_fini(env, io);
out_env_put:
	cl_env_put(env, &refcheck);

out:
	if (rc < 0)
		CERROR("Failed to WBC data for inode %lu, discard data: %d\n",
		       inode->i_ino, rc);

	/* XXX failure handling. */
	wbc_inode_data_lru_del(inode);
	spin_lock(&inode->i_lock);
	wbci->wbci_flags |= WBC_STATE_FL_DATA_COMMITTED;
	wbc_inode_unacct_pages(inode, mapping->nrpages);
	mapping->a_ops = &ll_aops;
	spin_unlock(&inode->i_lock);

	if (rc == 0)
		rc = mapping->nrpages;
	mapping_clear_unevictable(mapping);

	RETURN(rc);
}

int wbcfs_commit_cache_pages(struct inode *inode)
{
	switch (ll_i2wbci(inode)->wbci_cache_mode) {
	case WBC_MODE_MEMFS:
		return wbc_commit_data_lustre(inode);
	case WBC_MODE_DATA_PCC:
		return wbc_commit_data_pcc(inode);
	default:
		return -EOPNOTSUPP;
	}
}

void wbc_free_inode_pages_final(struct inode *inode,
				struct address_space *mapping)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);

	if (wbc_inode_has_protected(wbci)) {
		if (wbc_inode_data_committed(wbci)) {
			LASSERT(wbc_cache_mode_dop(wbci));
		}

		if (inode->i_nlink) {
			(void) wbcfs_commit_cache_pages(inode);
			cl_sync_file_range(inode, 0, OBD_OBJECT_EOF,
					   CL_FSYNC_LOCAL, 1);
		} else {
			wbc_inode_unacct_pages(inode, mapping->nrpages);
		}
	}
}

int wbcfs_writeback_dir_child(struct inode *dir, struct dentry *child,
			      struct writeback_control_ext *wbcx)
{
	struct inode *inode = child->d_inode;
	struct wbc_inode *wbci = ll_i2wbci(inode);
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct wbc_context *ctx;
	struct md_op_item *item;
	unsigned int valid = 0;
	long opc;
	int rc = 0;

	ENTRY;

	opc = wbc_flush_opcode_get(inode, NULL, wbcx, &valid);
	if (opc == MD_OP_NONE) {
		if (wbcx->for_pflush)
			RETURN(0);

		if (wbc_active_data_writeback(inode))
			rc = wbc_make_inode_assimilated(inode);
		RETURN(rc);
	} else if (opc == MD_OP_REMOVE_LOCKLESS) {
		rc = wbc_do_rmfid(inode, valid);
		spin_lock(&inode->i_lock);
		LASSERT(wbci->wbci_flags & WBC_STATE_FL_WRITEBACK &&
			wbci->wbci_dirty_flags & WBC_DIRTY_FL_FLUSHING);
		wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
		spin_unlock(&inode->i_lock);
		wbc_inode_writeback_complete(inode);
		RETURN(rc);
	} else if (opc == MD_OP_SETATTR_LOCKLESS) {
		rc = wbc_do_setattr(inode, valid);
		spin_lock(&inode->i_lock);
		LASSERT(wbci->wbci_flags & WBC_STATE_FL_WRITEBACK &&
			wbci->wbci_dirty_flags & WBC_DIRTY_FL_FLUSHING);
		wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
		spin_unlock(&inode->i_lock);
		wbc_inode_writeback_complete(inode);
		RETURN(rc);
	}

	LASSERT(opc == MD_OP_CREATE_LOCKLESS);
	item = wbc_prep_op_item(opc, dir, child, NULL, wbcx, valid);
	if (IS_ERR(item))
		RETURN(PTR_ERR(item));

	ctx = &sbi->ll_wbc_super.wbcs_context;
	atomic_inc(&ctx->ioc_anchor.wsi_sync_nr);
	item->mop_cbdata = ctx;
	if (wbcx->has_ioctx) {
		struct wbc_context *ctx = &wbcx->context;

		switch (ctx->ioc_pol) {
		case WBC_FLUSH_POL_PTLRPCD:
			LASSERT(ctx->ioc_rqset == NULL);
			rc = md_reint_async(sbi->ll_md_exp, item, NULL);
			break;
		case WBC_FLUSH_POL_BATCH:
			rc = md_batch_add(sbi->ll_md_exp, ctx->ioc_batch, item);
			break;
		default:
			rc = -EOPNOTSUPP;
			break;
		}
	} else {
		item->mop_flags |= WBC_FL_FLOW_CONTROL;
		rc = md_reint_async(sbi->ll_md_exp, item, NULL);
	}

	if (rc) {
		CERROR("Failed to flush file %pd\n", child);
		wbc_fini_op_item(item, rc);
	}

	RETURN(rc);
}

static int wbc_inode_flush_lockless_async(struct inode *inode,
					  struct writeback_control_ext *wbcx)
{
	struct dentry *dentry;
	struct inode *dir;
	int rc = 0;

	ENTRY;

	dentry = d_find_any_alias(inode);
	if (!dentry)
		RETURN(0);

	dir = dentry->d_parent->d_inode;
	rc = wbcfs_writeback_dir_child(dir, dentry, wbcx);
	dput(dentry);
	RETURN(rc);
}

static int wbc_inode_flush_lockless_sync(struct inode *inode,
					 struct writeback_control_ext *wbcx)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	unsigned int valid = 0;
	int rc = 0;
	long opc;

	ENTRY;

	opc = wbc_flush_opcode_get(inode, NULL, wbcx, &valid);
	switch (opc) {
	case MD_OP_NONE:
		rc = wbc_make_inode_assimilated(inode);
		break;
	case MD_OP_CREATE_LOCKLESS:
		rc = wbc_do_create(inode);
		spin_lock(&inode->i_lock);
		LASSERT(wbci->wbci_flags & WBC_STATE_FL_WRITEBACK &&
			wbci->wbci_dirty_flags & WBC_DIRTY_FL_FLUSHING);
		wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
		if (rc == 0)
			wbci->wbci_flags |= WBC_STATE_FL_SYNC;
		spin_unlock(&inode->i_lock);
		if (!wbcx->for_fsync)
			wbc_inode_writeback_complete(inode);
		break;
	case MD_OP_SETATTR_LOCKLESS: {
		rc = wbc_do_setattr(inode, valid);
		spin_lock(&inode->i_lock);
		LASSERT(wbci->wbci_flags & WBC_STATE_FL_WRITEBACK &&
			wbci->wbci_dirty_flags & WBC_DIRTY_FL_FLUSHING);
		wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
		spin_unlock(&inode->i_lock);
		wbc_inode_writeback_complete(inode);
		break;
	}
	case MD_OP_REMOVE_LOCKLESS: {
		rc = wbc_do_rmfid(inode, valid);
		spin_lock(&inode->i_lock);
		LASSERT(wbci->wbci_flags & WBC_STATE_FL_WRITEBACK &&
			wbci->wbci_dirty_flags & WBC_DIRTY_FL_FLUSHING);
		wbci->wbci_dirty_flags &= ~WBC_DIRTY_FL_FLUSHING;
		spin_unlock(&inode->i_lock);
		wbc_inode_writeback_complete(inode);
		break;
	}
	default:
		LBUG();
	}

	/* TODO: error handling - redirty and requeue the inode? */

	RETURN(rc);
}

int wbcfs_inode_flush_lockless(struct inode *inode,
			       struct writeback_control_ext *wbcx)
{
	if (wbcx->sync_mode == WB_SYNC_ALL)
		return wbc_inode_flush_lockless_sync(inode, wbcx);
	else
		return wbc_inode_flush_lockless_async(inode, wbcx);
}

int wbcfs_context_init(struct super_block *sb, struct wbc_context *ctx,
		       bool lazy_init, bool anchor_used)
{
	struct wbc_super *super = ll_s2wbcs(sb);
	struct wbc_conf *conf = &super->wbcs_conf;

	ENTRY;

	if (lazy_init) {
		memset(ctx, 0, sizeof(*ctx));
		ctx->ioc_anchor_used = anchor_used;
		RETURN(0);
	}

	ctx->ioc_pol = conf->wbcc_flush_pol;
	ctx->ioc_anchor_used = anchor_used;
	switch (ctx->ioc_pol) {
	case WBC_FLUSH_POL_RQSET:
		ctx->ioc_rqset = ptlrpc_prep_set();
		if (ctx->ioc_rqset == NULL)
			RETURN(-ENOMEM);
		break;
	case WBC_FLUSH_POL_BATCH:
		ctx->ioc_batch = md_batch_create(ll_s2sbi(sb)->ll_md_exp, 0,
						 conf->wbcc_max_batch_count);
		if (IS_ERR(ctx->ioc_batch))
			RETURN(PTR_ERR(ctx->ioc_batch));
		/* fall through */
	case WBC_FLUSH_POL_PTLRPCD:
		/*
		 * Hold one ref so that it won't be released until every sub
		 * update request is added.
		 */
		if (ctx->ioc_anchor_used)
			wbc_sync_io_init(&ctx->ioc_anchor, 1);
		break;
	default:
		RETURN(-ENOTSUPP);
	}

	ctx->ioc_inited = 1;
	RETURN(0);
}

int wbcfs_context_fini(struct super_block *sb, struct wbc_context *ctx)
{
	int rc = 0, rc2;

	ENTRY;

	if (!ctx->ioc_inited)
		RETURN(0);

	switch (ctx->ioc_pol) {
	case WBC_FLUSH_POL_RQSET:
		rc = ptlrpc_set_wait(NULL, ctx->ioc_rqset);
		ptlrpc_set_destroy(ctx->ioc_rqset);
		RETURN(rc);
	case WBC_FLUSH_POL_BATCH:
		rc = md_batch_stop(ll_s2sbi(sb)->ll_md_exp, ctx->ioc_batch);
		/* fall through */
	case WBC_FLUSH_POL_PTLRPCD:
		/*
		 * @anchor was inited as 1 to prevent it to be released before
		 * we add all sub requests for IO, so drop one extra reference
		 * to make sure we could wait count to be zero.
		 */
		if (ctx->ioc_anchor_used) {
			wbc_sync_io_note(&ctx->ioc_anchor, rc);
			rc2 = wbc_sync_io_wait(&ctx->ioc_anchor, 0);
			if (rc2 < 0 && rc == 0)
				rc = rc2;
		}
		break;
	default:
		RETURN(-EOPNOTSUPP);
	}

	RETURN(rc);
}

int wbcfs_context_prepare(struct super_block *sb, struct wbc_context *ctx)
{
	if (ctx->ioc_inited)
		return 0;

	return wbcfs_context_init(sb, ctx, false, ctx->ioc_anchor_used);
}

int wbcfs_context_commit(struct super_block *sb, struct wbc_context *ctx)
{
	struct ll_sb_info *sbi = ll_s2sbi(sb);
	int rc = 0, rc2;

	//if (!ctx->ioc_sync)
	//	return 0;

	switch (ctx->ioc_pol) {
	case WBC_FLUSH_POL_RQSET:
		rc = ptlrpc_set_wait(NULL, ctx->ioc_rqset);
		ptlrpc_set_destroy(ctx->ioc_rqset);
		if (rc)
			RETURN(rc);

		ctx->ioc_rqset = ptlrpc_prep_set();
		if (ctx->ioc_rqset == NULL)
			RETURN(-ENOMEM);
		break;
	case WBC_FLUSH_POL_BATCH:
		rc = md_batch_flush(sbi->ll_md_exp, ctx->ioc_batch, false);
		if (rc)
			RETURN(rc);
		/* fall through */
	case WBC_FLUSH_POL_PTLRPCD:
		if (ctx->ioc_anchor_used) {
			wbc_sync_io_note(&ctx->ioc_anchor, 0);
			rc2 = wbc_sync_io_wait(&ctx->ioc_anchor, 0);
			if (rc == 0 && rc2)
				rc = rc2;
			/*
			 * One extra reference again, as if @anchor is
			 * reused we assume it as 1 before using.
			 */
			atomic_add(1, &ctx->ioc_anchor.wsi_sync_nr);
		}
		break;
	default:
		RETURN(-EOPNOTSUPP);
	}

	RETURN(rc);
}

static int wbc_flush_dir_child(struct wbc_context *ctx, struct inode *dir,
			       struct writeback_control_ext *wbcx,
			       struct md_op_item *item)
{
	struct ll_sb_info *sbi = ll_i2sbi(dir);
	int rc;

	switch (ctx->ioc_pol) {
	case WBC_FLUSH_POL_PTLRPCD:
		LASSERT(ctx->ioc_rqset == NULL);
		/* fall through */
	case WBC_FLUSH_POL_RQSET:
		if (wbc_decomplete_lock_keep(ll_i2wbci(dir), item)) {
			LASSERT(item->mop_opc == MD_OP_CREATE_LOCKLESS);
			rc = md_reint_async(sbi->ll_md_exp, item,
					    ctx->ioc_rqset);
		} else {
			LASSERT(item->mop_opc == MD_OP_CREATE_EXLOCK ||
				item->mop_opc == MD_OP_SETATTR_EXLOCK ||
				item->mop_opc == MD_OP_EXLOCK_ONLY);
			rc = md_intent_lock_async(sbi->ll_md_exp, item,
						  ctx->ioc_rqset);
		}
		break;
	case WBC_FLUSH_POL_BATCH:
		rc = md_batch_add(sbi->ll_md_exp, ctx->ioc_batch, item);
		break;
	default:
		rc = -ENOTSUPP;
		break;
	}

	RETURN(rc);
}

int wbcfs_flush_dir_child(struct wbc_context *ctx, struct inode *dir,
			  struct dentry *dchild, struct ldlm_lock *lock,
			  struct writeback_control_ext *wbcx)
{
	struct md_op_item *item;
	unsigned int valid = 0;
	long opc;
	int rc;

	ENTRY;

	opc = wbc_flush_opcode_get(dchild->d_inode, dchild, wbcx, &valid);
	if (opc == MD_OP_NONE)
		RETURN(0);

	item = wbc_prep_op_item(opc, dir, dchild, lock, wbcx, valid);
	if (IS_ERR(item)) {
		CERROR("prepare op item failed: rc = %ld\n", PTR_ERR(item));
		RETURN(PTR_ERR(item));
	}

	if (md_opcode_need_exlock(opc))
		item->mop_einfo.ei_mode = LCK_EX;

	if (ctx->ioc_anchor_used) {
		atomic_inc(&ctx->ioc_anchor.wsi_sync_nr);
		item->mop_cbdata = ctx;

	}

	OBD_FAIL_TIMEOUT(OBD_FAIL_LLITE_WBC_FLUSH_PAUSE, cfs_fail_val);
	rc = wbc_flush_dir_child(ctx, dir, wbcx, item);
	if (rc) {
		CERROR("failed to flush dchild(%pd:%p): opc = %X, rc = %d\n",
		       item->mop_dentry, item->mop_dentry, item->mop_opc, rc);
		wbc_fini_op_item(item, rc);
	}

	RETURN(rc);
}

int wbcfs_file_open_local(struct inode *inode, struct file *file)
{
	struct ll_file_data *fd;
	struct dentry *dentry = file_dentry(file);
	struct wbc_dentry *wbcd = ll_d2wbcd(dentry);
	__u64 flags = file->f_flags;
	int rc = 0;

	ENTRY;

	fd = ll_file_data_get();
	if (fd == NULL)
		RETURN(-ENOMEM);

	fd->fd_file = file;
	file->private_data = fd;
	ll_readahead_init(inode, &fd->fd_ras);

	if ((flags + 1)  & O_ACCMODE)
		flags++;
	if (file->f_flags & O_TRUNC)
		flags |= FMODE_WRITE;
	fd->fd_omode = flags & (FMODE_READ | FMODE_WRITE | FMODE_EXEC);

	/* ll_cl_context intiialize */
	rwlock_init(&fd->fd_lock);
	INIT_LIST_HEAD(&fd->fd_lccs);
	INIT_LIST_HEAD(&fd->fd_wbc_file.wbcf_open_item);

	/* Pin dentry, thus it will keep in MemFS until unlink. */
	dget(dentry);
	spin_lock(&wbcd->wbcd_open_lock);
	list_add(&fd->fd_wbc_file.wbcf_open_item, &wbcd->wbcd_open_files);
	spin_unlock(&wbcd->wbcd_open_lock);

	fd->fd_wbc_file.wbcf_readdir_pol = ll_i2wbcc(inode)->wbcc_readdir_pol;

	if (wbc_cache_mode_dop(ll_i2wbci(inode))) {
		rc = pcc_file_open(inode, file);
		if (rc) {
			ll_file_data_put(fd);
			file->private_data = NULL;
		}
	}

	RETURN(rc);
}

void wbcfs_file_release_local(struct inode *inode, struct file *file)
{
	struct ll_file_data *fd;
	struct dentry *dentry = file_dentry(file);
	struct wbc_dentry *wbcd = ll_d2wbcd(dentry);

	if (wbc_cache_mode_dop(ll_i2wbci(inode)))
		pcc_file_release(inode, file);

	fd = file->private_data;
	spin_lock(&wbcd->wbcd_open_lock);
	list_del_init(&fd->fd_wbc_file.wbcf_open_item);
	spin_unlock(&wbcd->wbcd_open_lock);
	ll_file_data_put(fd);
	file->private_data = NULL;
}

int wbcfs_dcache_dir_open(struct inode *inode, struct file *file)
{
	struct ll_file_data *fd = file->private_data;
	int rc;

	rc = dcache_dir_open(inode, file);
	fd->fd_wbc_file.wbcf_private_data = file->private_data;
	file->private_data = fd;
	return rc;
}

int wbcfs_dcache_dir_close(struct inode *inode, struct file *file)
{
	struct ll_file_data *fd = file->private_data;

	dput(fd->fd_wbc_file.wbcf_private_data);
	return 0;
}

void wbc_inode_lock_callback(struct inode *inode, struct ldlm_lock *lock,
			     bool *cached)
{
	struct wbc_inode *wbci = ll_i2wbci(inode);
	struct writeback_control_ext wbcx = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0, /* metadata-only */
		.for_callback = 1,
	};

	ENTRY;

	*cached = wbc_inode_has_protected(wbci);
	if (!*cached)
		RETURN_EXIT;

	down_write(&wbci->wbci_rw_sem);
	*cached = wbc_inode_has_protected(wbci);
	if (!*cached) {
		up_write(&wbci->wbci_rw_sem);
		RETURN_EXIT;
	}

	(void) wbc_make_inode_deroot(inode, lock, &wbcx);
	up_write(&wbci->wbci_rw_sem);
	RETURN_EXIT;
}

/*
 * TODO: Set cache policy for the root WBC directory according to
 * the predefined customized caching rules.
 */
void wbc_intent_inode_init(struct inode *dir, struct inode *inode,
			   struct lookup_intent *it)
{
	struct wbc_inode *dwbci = ll_i2wbci(dir);
	struct wbc_inode *wbci = ll_i2wbci(inode);

	ENTRY;

	LASSERT(it->it_op == IT_CREAT || it->it_op == IT_LOOKUP);
	spin_lock(&inode->i_lock);
	if (it->it_lock_mode == LCK_EX) {
		struct wbc_super *super = ll_i2wbcs(dir);

		LASSERT(!wbc_inode_has_protected(dwbci));
		wbci->wbci_cache_mode = super->wbcs_conf.wbcc_cache_mode;
		wbci->wbci_flush_mode = super->wbcs_conf.wbcc_flush_mode;
		/*
		 * Set this newly created WBC directory with the state of
		 * Protected(P) | Sync(S) | Root(R) | Complete(C).
		 */
		wbci->wbci_flags = WBC_STATE_FL_ROOT | WBC_STATE_FL_PROTECTED |
				   WBC_STATE_FL_SYNC | WBC_STATE_FL_COMPLETE;
		wbc_super_root_add(inode);
		wbci->wbci_lock_handle.cookie = it->it_lock_handle;
	} else {
		LASSERT(it->it_lock_mode == 0 &&
			wbc_inode_has_protected(dwbci));
		wbci->wbci_cache_mode = dwbci->wbci_cache_mode;
		wbci->wbci_flush_mode = dwbci->wbci_flush_mode;
		wbci->wbci_flags = WBC_STATE_FL_PROTECTED | WBC_STATE_FL_SYNC;
	}

	spin_unlock(&inode->i_lock);
	wbc_inode_operations_set(inode, inode->i_mode, inode->i_rdev);
	RETURN_EXIT;
}

/*
 * TODO: Customizable rule based auto WBC.
 * Define various auto caching rule for WBC on a client similar to TBF or PCC.
 * When a newly creating directory meets the rule condition, it can try to
 * obtain EX WBC lock from MDS and keep exclusive access on the directory
 * under the protection of the EX lock on the client.
 * The rule can be combination of uid/gid/projid/fname or jobid.
 * Moerover, it would better to return the customized WBC cache specification
 * here to set the cache mode and flush mode for the newly created directory.
 */
bool wbc_may_exclusive_cache(struct inode *dir, struct dentry *dentry,
			     umode_t mode, __u64 *extra_lock_flags)
{
	struct wbc_conf *conf = &ll_i2wbcs(dir)->wbcs_conf;
	struct wbc_inode *wbci = ll_i2wbci(dir);

	if (conf->wbcc_cache_mode == WBC_MODE_NONE) {
		*extra_lock_flags = 0;
		return false;
	}

	if (wbc_inode_has_protected(wbci)) {
		LASSERT(!wbc_inode_complete(wbci));
		*extra_lock_flags = LDLM_FL_INTENT_PARENT_LOCKED;
	} else {
		LASSERT(wbc_inode_none(wbci));
		*extra_lock_flags = LDLM_FL_INTENT_EXLOCK_UPDATE;
	}

	return true;
}

int ll_new_inode_init(struct inode *dir, struct dentry *dchild,
		      struct inode *inode)
{
	struct wbc_inode *dwbci = ll_i2wbci(dir);
	int rc = 0;

	if (wbc_inode_has_protected(dwbci)) {
		struct wbc_inode *wbci = ll_i2wbci(inode);

		LASSERT(!wbc_inode_complete(dwbci));
		spin_lock(&inode->i_lock);
		wbci->wbci_cache_mode = dwbci->wbci_cache_mode;
		wbci->wbci_flush_mode = dwbci->wbci_flush_mode;
		wbci->wbci_flags = WBC_STATE_FL_PROTECTED | WBC_STATE_FL_SYNC;
		spin_unlock(&inode->i_lock);
		wbc_inode_operations_set(inode, inode->i_mode, inode->i_rdev);

		rc = ll_d_init(dchild);
		if (rc == 0) {
			if (d_unhashed(dchild))
				d_add(dchild, inode);
			else
				d_instantiate(dchild, inode);

			d_lustre_revalidate(dchild);
		}
	} else {
		d_instantiate(dchild, inode);
	}

	return rc;
}

void ll_intent_inode_init(struct inode *dir, struct inode *inode,
			  struct lookup_intent *it)
{
	if (wbc_inode_has_protected(ll_i2wbci(dir)))
		wbc_intent_inode_init(dir, inode, it);
}

static int wbc_conf_seq_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct wbc_conf *conf = ll_s2wbcc(sb);

	seq_printf(m, "cache_mode: %s\n",
		   wbc_cachemode2string(conf->wbcc_cache_mode));
	seq_printf(m, "flush_mode: %s\n",
		   wbc_flushmode2string(conf->wbcc_flush_mode));
	seq_printf(m, "max_rpcs: %u\n", conf->wbcc_max_rpcs);
	seq_printf(m, "flush_pol: %s\n",
		   wbc_flushpol2string(conf->wbcc_flush_pol));
	seq_printf(m, "max_batch_count: %u\n", conf->wbcc_max_batch_count);
	seq_printf(m, "readdir_pol: %s\n",
		   wbc_readdir_pol2string(conf->wbcc_readdir_pol));
	seq_printf(m, "remove_pol: %s\n", wbc_rmpol2string(conf->wbcc_rmpol));
	seq_printf(m, "hiwm_ratio: %d\n", conf->wbcc_hiwm_ratio);
	seq_printf(m, "inodes_max: %lu\n", conf->wbcc_max_inodes);
	seq_printf(m, "inodes_free: %lu\n", conf->wbcc_free_inodes);
	seq_printf(m, "inodes_hiwm: %u\n", conf->wbcc_hiwm_inodes_count);
	seq_printf(m, "pages_max: %lu\n", conf->wbcc_max_pages);
	seq_printf(m, "pages_free: %lu\n",
		   (unsigned long)(conf->wbcc_max_pages -
		   percpu_counter_sum(&conf->wbcc_used_pages)));
	seq_printf(m, "pages_hiwm: %u\n", conf->wbcc_hiwm_pages_count);
	seq_printf(m, "dirty_flush_thresh: %lu\n",
		   conf->wbcc_dirty_flush_thresh);
	seq_printf(m, "inodes_dirty: %lu\n",
		   (unsigned long)wbc_stat_sum(ll_s2mwb(sb), WB_INODE_DIRTY));
	seq_printf(m, "max_nrpages_per_file: %lu\n",
		   conf->wbcc_max_nrpages_per_file);
	seq_printf(m, "active_data_writeback: %d\n",
		   conf->wbcc_active_data_writeback);
	return 0;
}

static ssize_t wbc_conf_seq_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct super_block *sb = m->private;
	char *kernbuf;
	int rc;

	if (count >= LPROCFS_WR_WBC_MAX_CMD)
		return -EINVAL;

	/*
	 * TODO: Check for the WBC support via the connection flag.
	 */
	OBD_ALLOC(kernbuf, count + 1);
	if (kernbuf == NULL)
		return -ENOMEM;

	if (copy_from_user(kernbuf, buffer, count))
		GOTO(out_free_kernbuff, rc = -EFAULT);

	rc = wbc_cmd_parse_and_handle(kernbuf, count, ll_s2wbcs(sb));
out_free_kernbuff:
	OBD_FREE(kernbuf, count + 1);
	return rc ? rc : count;
}
LDEBUGFS_SEQ_FOPS(wbc_conf);

static int wbc_flush_mode_seq_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct wbc_conf *conf = ll_s2wbcc(sb);

	seq_printf(m, "%s\n", wbc_flushmode2string(conf->wbcc_flush_mode));

	return 0;
}

static ssize_t wbc_flush_mode_seq_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct super_block *sb = m->private;
	struct wbc_cmd cmd;
	char kernbuf[128];
	int rc;

	ENTRY;

	if (count >= sizeof(kernbuf))
		RETURN(-EINVAL);

	if (copy_from_user(kernbuf, buffer, count))
		RETURN(-EFAULT);

	kernbuf[count] = 0;
	memset(&cmd, 0, sizeof(cmd));
	if (strncmp(kernbuf, "lazy_drop", 9) == 0)
		cmd.wbcc_conf.wbcc_flush_mode = WBC_FLUSH_LAZY_DROP;
	else if (strncmp(kernbuf, "lazy_keep", 10) == 0)
		cmd.wbcc_conf.wbcc_flush_mode = WBC_FLUSH_LAZY_KEEP;
	else if (strncmp(kernbuf, "aging_drop", 10) == 0)
		cmd.wbcc_conf.wbcc_flush_mode = WBC_FLUSH_AGING_DROP;
	else if (strncmp(kernbuf, "aging_keep", 10) == 0)
		cmd.wbcc_conf.wbcc_flush_mode = WBC_FLUSH_AGING_KEEP;
	else
		RETURN(-EINVAL);

	cmd.wbcc_flags |= WBC_CMD_OP_FLUSH_MODE;
	rc = wbc_cmd_handle(ll_s2wbcs(sb), &cmd);
	RETURN(rc ? rc : count);
}

LDEBUGFS_SEQ_FOPS(wbc_flush_mode);

static int wbc_max_rpcs_seq_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct wbc_conf *conf = ll_s2wbcc(sb);

	seq_printf(m, "%u\n", conf->wbcc_max_rpcs);

	return 0;
}

static ssize_t wbc_max_rpcs_seq_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct super_block *sb = m->private;
	struct wbc_cmd cmd;
	unsigned int val;
	int rc;

	rc = kstrtouint_from_user(buffer, count, 0, &val);
	if (rc)
		return rc;

	memset(&cmd, 0, sizeof(cmd));
	cmd.wbcc_conf.wbcc_max_rpcs = val;
	cmd.wbcc_flags |= WBC_CMD_OP_MAX_RPCS;

	rc = wbc_cmd_handle(ll_s2wbcs(sb), &cmd);
	return rc ? rc : count;
}
LDEBUGFS_SEQ_FOPS(wbc_max_rpcs);

static int wbc_rmpol_seq_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct wbc_conf *conf = ll_s2wbcc(sb);

	seq_printf(m, "%s\n", wbc_rmpol2string(conf->wbcc_rmpol));

	return 0;
}

static ssize_t wbc_rmpol_seq_write(struct file *file,
				   const char __user *buffer,
				   size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct super_block *sb = m->private;
	struct wbc_cmd cmd;
	char kernbuf[128];
	int rc;

	if (count >= sizeof(kernbuf))
		RETURN(-EINVAL);

	if (copy_from_user(kernbuf, buffer, count))
		RETURN(-EFAULT);

	kernbuf[count] = 0;
	memset(&cmd, 0, sizeof(cmd));
	if (strncmp(kernbuf, "sync", 4) == 0)
		cmd.wbcc_conf.wbcc_rmpol = WBC_RMPOL_SYNC;
	else
		return -EINVAL;

	cmd.wbcc_flags |= WBC_CMD_OP_RMPOL;
	rc = wbc_cmd_handle(ll_s2wbcs(sb), &cmd);
	return rc ? rc : count;
}
LDEBUGFS_SEQ_FOPS(wbc_rmpol);

struct ldebugfs_vars ldebugfs_llite_wbc_vars[] = {
	{ .name =	"conf",
	  .fops =	&wbc_conf_fops		},
	{ .name =	"flush_mode",
	  .fops =	&wbc_flush_mode_fops	},
	{ .name =	"max_rpcs",
	  .fops =	&wbc_max_rpcs_fops,	},
	{ .name =	"rmpol",
	  .fops =	&wbc_rmpol_fops,	},
	{ NULL }
};

/* Create sysfs/debugfs entries for WBC. */
void wbc_tunables_init(struct super_block *sb)
{
	struct ll_sb_info *sbi = ll_s2sbi(sb);
	struct wbc_super *wbcs = ll_s2wbcs(sb);

	wbcs->wbcs_debugfs_dir = debugfs_create_dir("wbc",
						    sbi->ll_debugfs_entry);
	ldebugfs_add_vars(wbcs->wbcs_debugfs_dir, ldebugfs_llite_wbc_vars, sb);
}

void wbc_tunables_fini(struct super_block *sb)
{
	struct wbc_super *wbcs = ll_s2wbcs(sb);

	debugfs_remove_recursive(wbcs->wbcs_debugfs_dir);
	wbcs->wbcs_debugfs_dir = NULL;
}
