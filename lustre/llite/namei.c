/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/quotaops.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/user_namespace.h>
#include <linux/uidgid.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <lustre_fid.h>
#include <lustre_dlm.h>
#include "llite_internal.h"

static int ll_create_it(struct inode *dir, struct dentry *dentry,
			struct lookup_intent *it,
			void *secctx, __u32 secctxlen, bool encrypt,
			void *encctx, __u32 encctxlen);

/* called from iget5_locked->find_inode() under inode_lock spinlock */
static int ll_test_inode(struct inode *inode, void *opaque)
{
	struct ll_inode_info	*lli = ll_i2info(inode);
	struct lustre_md	*md = opaque;

	if (unlikely(!(md->body->mbo_valid & OBD_MD_FLID))) {
		CERROR("MDS body missing FID\n");
		return 0;
	}

	if (!lu_fid_eq(&lli->lli_fid, &md->body->mbo_fid1))
		return 0;

	return 1;
}

static int ll_set_inode(struct inode *inode, void *opaque)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct mdt_body *body = ((struct lustre_md *)opaque)->body;

	if (unlikely(!(body->mbo_valid & OBD_MD_FLID))) {
		CERROR("MDS body missing FID\n");
		return -EINVAL;
	}

	lli->lli_fid = body->mbo_fid1;
	if (unlikely(!(body->mbo_valid & OBD_MD_FLTYPE))) {
		CERROR("Can not initialize inode "DFID" without object type: "
		       "valid = %#llx\n",
		       PFID(&lli->lli_fid), body->mbo_valid);
		return -EINVAL;
	}

	inode->i_mode = (inode->i_mode & ~S_IFMT) | (body->mbo_mode & S_IFMT);
	if (unlikely(inode->i_mode == 0)) {
		CERROR("Invalid inode "DFID" type\n", PFID(&lli->lli_fid));
		return -EINVAL;
	}

	ll_lli_init(lli);

	return 0;
}


/**
 * Get an inode by inode number(@hash), which is already instantiated by
 * the intent lookup).
 */
struct inode *ll_iget(struct super_block *sb, ino_t hash,
                      struct lustre_md *md)
{
	struct inode	*inode;
	int		rc = 0;

	ENTRY;

        LASSERT(hash != 0);
        inode = iget5_locked(sb, hash, ll_test_inode, ll_set_inode, md);
	if (inode == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	if (inode->i_state & I_NEW) {
		rc = ll_read_inode2(inode, md);
		if (rc == 0 && S_ISREG(inode->i_mode) &&
		    ll_i2info(inode)->lli_clob == NULL)
			rc = cl_file_inode_init(inode, md);

		if (rc != 0) {
			/* Let's clear directory lsm here, otherwise
			 * make_bad_inode() will reset the inode mode
			 * to regular, then ll_clear_inode will not
			 * be able to clear lsm_md */
			if (S_ISDIR(inode->i_mode))
				ll_dir_clear_lsm_md(inode);
			make_bad_inode(inode);
			unlock_new_inode(inode);
			iput(inode);
			inode = ERR_PTR(rc);
		} else {
			inode_has_no_xattr(inode);
			unlock_new_inode(inode);
		}
	} else if (is_bad_inode(inode)) {
		iput(inode);
		inode = ERR_PTR(-ESTALE);
	} else if (!(inode->i_state & (I_FREEING | I_CLEAR))) {
		rc = ll_update_inode(inode, md);
		CDEBUG(D_VFSTRACE, "got inode: "DFID"(%p): rc = %d\n",
		       PFID(&md->body->mbo_fid1), inode, rc);
		if (rc != 0) {
			if (S_ISDIR(inode->i_mode))
				ll_dir_clear_lsm_md(inode);
			iput(inode);
			inode = ERR_PTR(rc);
		}
	}

        RETURN(inode);
}

/* mark negative sub file dentries invalid and prune unused dentries */
static void ll_prune_negative_children(struct inode *dir)
{
	struct dentry *dentry;
	struct dentry *child;

	ENTRY;

restart:
	spin_lock(&dir->i_lock);
	hlist_for_each_entry(dentry, &dir->i_dentry, d_alias) {
		spin_lock(&dentry->d_lock);
		list_for_each_entry(child, &dentry->d_subdirs, d_child) {
			if (child->d_inode)
				continue;

			spin_lock_nested(&child->d_lock, DENTRY_D_LOCK_NESTED);
			__d_lustre_invalidate(child);
			if (!ll_d_count(child)) {
				dget_dlock(child);
				__d_drop(child);
				spin_unlock(&child->d_lock);
				spin_unlock(&dentry->d_lock);
				spin_unlock(&dir->i_lock);

				CDEBUG(D_DENTRY, "prune negative dentry %pd\n",
				       child);

				dput(child);
				goto restart;
			}
			spin_unlock(&child->d_lock);
		}
		spin_unlock(&dentry->d_lock);
	}
	spin_unlock(&dir->i_lock);

	EXIT;
}

int ll_test_inode_by_fid(struct inode *inode, void *opaque)
{
	return lu_fid_eq(&ll_i2info(inode)->lli_fid, opaque);
}

static int ll_dom_lock_cancel(struct inode *inode, struct ldlm_lock *lock)
{
	struct lu_env *env;
	struct ll_inode_info *lli = ll_i2info(inode);
	__u16 refcheck;
	int rc;
	ENTRY;

	env = cl_env_get(&refcheck);
	if (IS_ERR(env))
		RETURN(PTR_ERR(env));

	OBD_FAIL_TIMEOUT(OBD_FAIL_LDLM_REPLAY_PAUSE, cfs_fail_val);

	/* reach MDC layer to flush data under  the DoM ldlm lock */
	rc = cl_object_flush(env, lli->lli_clob, lock);
	if (rc == -ENODATA) {
		CDEBUG(D_INODE, "inode "DFID" layout has no DoM stripe\n",
		       PFID(ll_inode2fid(inode)));
		/* most likely result of layout change, do nothing */
		rc = 0;
	}

	cl_env_put(env, &refcheck);
	RETURN(rc);
}

static void ll_lock_cancel_bits(struct ldlm_lock *lock, __u64 to_cancel)
{
	struct inode *inode = ll_inode_from_resource_lock(lock);
	struct ll_inode_info *lli;
	__u64 bits = to_cancel;
	int rc;

	ENTRY;

	if (!inode) {
		/* That means the inode is evicted most likely and may cause
		 * the skipping of lock cleanups below, so print the message
		 * about that in log.
		 */
		if (lock->l_resource->lr_lvb_inode)
			LDLM_DEBUG(lock,
				   "can't take inode for the lock (%sevicted)\n",
				   lock->l_resource->lr_lvb_inode->i_state &
				   I_FREEING ? "" : "not ");
		RETURN_EXIT;
	}

	if (!fid_res_name_eq(ll_inode2fid(inode),
			     &lock->l_resource->lr_name)) {
		LDLM_ERROR(lock, "data mismatch with object "DFID"(%p)",
			   PFID(ll_inode2fid(inode)), inode);
		LBUG();
	}


	if (bits & MDS_INODELOCK_XATTR) {
		ll_xattr_cache_destroy(inode);
		bits &= ~MDS_INODELOCK_XATTR;
	}

	/* For OPEN locks we differentiate between lock modes
	 * LCK_CR, LCK_CW, LCK_PR - bug 22891 */
	if (bits & MDS_INODELOCK_OPEN)
		ll_have_md_lock(inode, &bits, lock->l_req_mode);

	if (bits & MDS_INODELOCK_OPEN) {
		fmode_t fmode;

		switch (lock->l_req_mode) {
		case LCK_CW:
			fmode = FMODE_WRITE;
			break;
		case LCK_PR:
			fmode = FMODE_EXEC;
			break;
		case LCK_CR:
			fmode = FMODE_READ;
			break;
		default:
			LDLM_ERROR(lock, "bad lock mode for OPEN lock");
			LBUG();
		}

		ll_md_real_close(inode, fmode);

		bits &= ~MDS_INODELOCK_OPEN;
	}

	if (bits & (MDS_INODELOCK_LOOKUP | MDS_INODELOCK_UPDATE |
		    MDS_INODELOCK_LAYOUT | MDS_INODELOCK_PERM |
		    MDS_INODELOCK_DOM))
		ll_have_md_lock(inode, &bits, LCK_MINMODE);

	/* root WBC EX lock */
	if (lock->l_req_mode == LCK_EX && bits & MDS_INODELOCK_UPDATE) {
		bool cached;

		wbc_inode_lock_callback(inode, lock, &cached);

		/*
		 * Invalidate the alias dentries of this inode.
		 * TODO: downgrade the lock mode or drop ibits properly.
		 */
		if (cached && !(bits & MDS_INODELOCK_LOOKUP) &&
		    inode->i_sb->s_root != NULL &&
		    inode != inode->i_sb->s_root->d_inode)
			ll_prune_aliases(inode);
	}

	if (bits & MDS_INODELOCK_DOM) {
		rc =  ll_dom_lock_cancel(inode, lock);
		if (rc < 0)
			CDEBUG(D_INODE, "cannot flush DoM data "
			       DFID": rc = %d\n",
			       PFID(ll_inode2fid(inode)), rc);
	}

	if (bits & MDS_INODELOCK_LAYOUT) {
		struct cl_object_conf conf = {
			.coc_opc = OBJECT_CONF_INVALIDATE,
			.coc_inode = inode,
		};

		rc = ll_layout_conf(inode, &conf);
		if (rc < 0)
			CDEBUG(D_INODE, "cannot invalidate layout of "
			       DFID": rc = %d\n",
			       PFID(ll_inode2fid(inode)), rc);
	}

	lli = ll_i2info(inode);

	if (bits & MDS_INODELOCK_UPDATE)
		set_bit(LLIF_UPDATE_ATIME, &lli->lli_flags);

	if ((bits & MDS_INODELOCK_UPDATE) && S_ISDIR(inode->i_mode)) {
		CDEBUG(D_INODE, "invalidating inode "DFID" lli = %p, "
		       "pfid  = "DFID"\n", PFID(ll_inode2fid(inode)),
		       lli, PFID(&lli->lli_pfid));
		truncate_inode_pages(inode->i_mapping, 0);

		if (unlikely(!fid_is_zero(&lli->lli_pfid))) {
			struct inode *master_inode = NULL;
			unsigned long hash;

			/* This is slave inode, since all of the child dentry
			 * is connected on the master inode, so we have to
			 * invalidate the negative children on master inode */
			CDEBUG(D_INODE, "Invalidate s"DFID" m"DFID"\n",
			       PFID(ll_inode2fid(inode)), PFID(&lli->lli_pfid));

			hash = cl_fid_build_ino(&lli->lli_pfid,
					ll_need_32bit_api(ll_i2sbi(inode)));

			/* Do not lookup the inode with ilookup5, otherwise
			 * it will cause dead lock,
			 * 1. Client1 send chmod req to the MDT0, then on MDT0,
			 * it enqueues master and all of its slaves lock,
			 * (mdt_attr_set() -> mdt_lock_slaves()), after gets
			 * master and stripe0 lock, it will send the enqueue
			 * req (for stripe1) to MDT1, then MDT1 finds the lock
			 * has been granted to client2. Then MDT1 sends blocking
			 * ast to client2.
			 * 2. At the same time, client2 tries to unlink
			 * the striped dir (rm -rf striped_dir), and during
			 * lookup, it will hold the master inode of the striped
			 * directory, whose inode state is NEW, then tries to
			 * revalidate all of its slaves, (ll_prep_inode()->
			 * ll_iget()->ll_read_inode2()-> ll_update_inode().).
			 * And it will be blocked on the server side because
			 * of 1.
			 * 3. Then the client get the blocking_ast req, cancel
			 * the lock, but being blocked if using ->ilookup5()),
			 * because master inode state is NEW. */
			master_inode = ilookup5_nowait(inode->i_sb, hash,
							ll_test_inode_by_fid,
							(void *)&lli->lli_pfid);
			if (master_inode) {
				ll_prune_negative_children(master_inode);
				iput(master_inode);
			}
		} else {
			ll_prune_negative_children(inode);
		}
	}

	if ((bits & (MDS_INODELOCK_LOOKUP | MDS_INODELOCK_PERM)) &&
	    !is_root_inode(inode))
		ll_prune_aliases(inode);

	if (bits & (MDS_INODELOCK_LOOKUP | MDS_INODELOCK_PERM))
		forget_all_cached_acls(inode);

	iput(inode);
	RETURN_EXIT;
}

/* Check if the given lock may be downgraded instead of canceling and
 * that convert is really needed. */
int ll_md_need_convert(struct ldlm_lock *lock)
{
	struct ldlm_namespace *ns = ldlm_lock_to_ns(lock);
	struct inode *inode;
	__u64 wanted = lock->l_policy_data.l_inodebits.cancel_bits;
	__u64 bits = lock->l_policy_data.l_inodebits.bits & ~wanted;
	enum ldlm_mode mode = LCK_MINMODE;

	if (!lock->l_conn_export ||
	    !exp_connect_lock_convert(lock->l_conn_export))
		return 0;

	if (!wanted || !bits || ldlm_is_cancel(lock))
		return 0;

	/* do not convert locks other than DOM for now */
	if (!((bits | wanted) & MDS_INODELOCK_DOM))
		return 0;

	/* We may have already remaining bits in some other lock so
	 * lock convert will leave us just extra lock for the same bit.
	 * Check if client has other lock with the same bits and the same
	 * or lower mode and don't convert if any.
	 */
	switch (lock->l_req_mode) {
	case LCK_PR:
		mode = LCK_PR;
		/* fallthrough */
	case LCK_PW:
		mode |= LCK_CR;
		break;
	case LCK_CW:
		mode = LCK_CW;
		/* fallthrough */
	case LCK_CR:
		mode |= LCK_CR;
		break;
	default:
		/* do not convert other modes */
		return 0;
	}

	/* is lock is too old to be converted? */
	lock_res_and_lock(lock);
	if (ktime_after(ktime_get(),
			ktime_add(lock->l_last_used, ns->ns_dirty_age_limit))) {
		unlock_res_and_lock(lock);
		return 0;
	}
	unlock_res_and_lock(lock);

	inode = ll_inode_from_resource_lock(lock);
	ll_have_md_lock(inode, &bits, mode);
	iput(inode);
	return !!(bits);
}

int ll_md_blocking_ast(struct ldlm_lock *lock, struct ldlm_lock_desc *ld,
		       void *data, int flag)
{
	struct lustre_handle lockh;
	int rc;

	ENTRY;

	switch (flag) {
	case LDLM_CB_BLOCKING:
	{
		__u64 cancel_flags = LCF_ASYNC;

		/* if lock convert is not needed then still have to
		 * pass lock via ldlm_cli_convert() to keep all states
		 * correct, set cancel_bits to full lock bits to cause
		 * full cancel to happen.
		 */
		if (!ll_md_need_convert(lock)) {
			lock_res_and_lock(lock);
			lock->l_policy_data.l_inodebits.cancel_bits =
					lock->l_policy_data.l_inodebits.bits;
			unlock_res_and_lock(lock);
		}
		rc = ldlm_cli_convert(lock, cancel_flags);
		if (!rc)
			RETURN(0);
		/* continue with cancel otherwise */
		ldlm_lock2handle(lock, &lockh);
		rc = ldlm_cli_cancel(&lockh, cancel_flags);
		if (rc < 0) {
			CDEBUG(D_INODE, "ldlm_cli_cancel: rc = %d\n", rc);
			RETURN(rc);
		}
		break;
	}
	case LDLM_CB_CANCELING:
	{
		__u64 to_cancel = lock->l_policy_data.l_inodebits.bits;

		/* Nothing to do for non-granted locks */
		if (!ldlm_is_granted(lock))
			break;

		/* If 'ld' is supplied then bits to be cancelled are passed
		 * implicitly by lock converting and cancel_bits from 'ld'
		 * should be used. Otherwise full cancel is being performed
		 * and lock inodebits are used.
		 *
		 * Note: we cannot rely on cancel_bits in lock itself at this
		 * moment because they can be changed by concurrent thread,
		 * so ldlm_cli_inodebits_convert() pass cancel bits implicitly
		 * in 'ld' parameter.
		 */
		if (ld) {
			/* partial bits cancel allowed only during convert */
			LASSERT(ldlm_is_converting(lock));
			/* mask cancel bits by lock bits so only no any unused
			 * bits are passed to ll_lock_cancel_bits()
			 */
			to_cancel &= ld->l_policy_data.l_inodebits.cancel_bits;
		}
		ll_lock_cancel_bits(lock, to_cancel);
		break;
	}
	default:
		LBUG();
	}

	RETURN(0);
}

__u32 ll_i2suppgid(struct inode *i)
{
	if (in_group_p(i->i_gid))
		return (__u32)from_kgid(&init_user_ns, i->i_gid);
	else
		return (__u32) __kgid_val(INVALID_GID);
}

/* Pack the required supplementary groups into the supplied groups array.
 * If we don't need to use the groups from the target inode(s) then we
 * instead pack one or more groups from the user's supplementary group
 * array in case it might be useful.  Not needed if doing an MDS-side upcall. */
void ll_i2gids(__u32 *suppgids, struct inode *i1, struct inode *i2)
{
	LASSERT(i1 != NULL);
	LASSERT(suppgids != NULL);

	suppgids[0] = ll_i2suppgid(i1);

	if (i2)
		suppgids[1] = ll_i2suppgid(i2);
	else
		suppgids[1] = -1;
}

/*
 * try to reuse three types of dentry:
 * 1. unhashed alias, this one is unhashed by d_invalidate (but it may be valid
 *    by concurrent .revalidate).
 * 2. INVALID alias (common case for no valid ldlm lock held, but this flag may
 *    be cleared by others calling d_lustre_revalidate).
 * 3. DISCONNECTED alias.
 */
static struct dentry *ll_find_alias(struct inode *inode, struct dentry *dentry)
{
	struct dentry *alias, *discon_alias, *invalid_alias;

	if (hlist_empty(&inode->i_dentry))
		return NULL;

	discon_alias = invalid_alias = NULL;

	spin_lock(&inode->i_lock);
	hlist_for_each_entry(alias, &inode->i_dentry, d_alias) {
		LASSERT(alias != dentry);

		spin_lock(&alias->d_lock);
		if ((alias->d_flags & DCACHE_DISCONNECTED) &&
		    S_ISDIR(inode->i_mode))
			/* LASSERT(last_discon == NULL); LU-405, bz 20055 */
			discon_alias = alias;
		else if (alias->d_parent == dentry->d_parent             &&
			 alias->d_name.hash == dentry->d_name.hash       &&
			 alias->d_name.len == dentry->d_name.len         &&
			 memcmp(alias->d_name.name, dentry->d_name.name,
				dentry->d_name.len) == 0)
			invalid_alias = alias;
		spin_unlock(&alias->d_lock);

		if (invalid_alias)
			break;
	}
	alias = invalid_alias ?: discon_alias ?: NULL;
	if (alias) {
		spin_lock(&alias->d_lock);
		dget_dlock(alias);
		spin_unlock(&alias->d_lock);
	}
	spin_unlock(&inode->i_lock);

	return alias;
}

/*
 * Similar to d_splice_alias(), but lustre treats invalid alias
 * similar to DCACHE_DISCONNECTED, and tries to use it anyway.
 */
struct dentry *ll_splice_alias(struct inode *inode, struct dentry *de)
{
	struct dentry *new;
	int rc;

	if (inode) {
		new = ll_find_alias(inode, de);
		if (new) {
			rc = ll_d_init(new);
			if (rc < 0) {
				dput(new);
				return ERR_PTR(rc);
			}
			d_move(new, de);
			iput(inode);
			CDEBUG(D_DENTRY,
			       "Reuse dentry %p inode %p refc %d flags %#x\n",
			      new, new->d_inode, ll_d_count(new), new->d_flags);
			return new;
		}
	}
	rc = ll_d_init(de);
	if (rc < 0)
		return ERR_PTR(rc);
	d_add(de, inode);

	/* this needs only to be done for foreign symlink dirs as
	 * DCACHE_SYMLINK_TYPE is already set by d_flags_for_inode()
	 * kernel routine for files with symlink ops (ie, real symlink)
	 */
	if (inode && S_ISDIR(inode->i_mode) &&
	    ll_sbi_has_foreign_symlink(ll_i2sbi(inode)) &&
#ifdef HAVE_IOP_GET_LINK
	    inode->i_op->get_link) {
#else
	    inode->i_op->follow_link) {
#endif
		CDEBUG(D_INFO, "%s: inode "DFID": faking foreign dir as a symlink\n",
		       ll_i2sbi(inode)->ll_fsname, PFID(ll_inode2fid(inode)));
		spin_lock(&de->d_lock);
		/* like d_flags_for_inode() already does for files */
		de->d_flags = (de->d_flags & ~DCACHE_ENTRY_TYPE) |
			      DCACHE_SYMLINK_TYPE;
		spin_unlock(&de->d_lock);
	}

	CDEBUG(D_DENTRY, "Add dentry %p inode %p refc %d flags %#x\n",
	       de, de->d_inode, ll_d_count(de), de->d_flags);
        return de;
}

static int ll_lookup_it_finish(struct ptlrpc_request *request,
			       struct lookup_intent *it,
			       struct inode *parent, struct dentry **de,
			       void *secctx, __u32 secctxlen,
			       void *encctx, __u32 encctxlen,
			       ktime_t kstart, bool encrypt)
{
	struct inode		 *inode = NULL;
	__u64			  bits = 0;
	int			  rc;
	struct dentry *alias;

	ENTRY;

	/* NB 1 request reference will be taken away by ll_intent_lock()
	 * when I return */
	CDEBUG(D_DENTRY, "it %p it_disposition %x\n", it,
	       it->it_disposition);
	if (!it_disposition(it, DISP_LOOKUP_NEG)) {
		struct req_capsule *pill = &request->rq_pill;
		struct mdt_body *body = req_capsule_server_get(pill,
							       &RMF_MDT_BODY);

		rc = ll_prep_inode(&inode, &request->rq_pill, (*de)->d_sb, it);
		if (rc)
			RETURN(rc);

		/* If encryption context was returned by MDT, put it in
		 * inode now to save an extra getxattr and avoid deadlock.
		 */
		if (body->mbo_valid & OBD_MD_ENCCTX) {
			encctx = req_capsule_server_get(pill, &RMF_FILE_ENCCTX);
			encctxlen = req_capsule_get_size(pill,
							 &RMF_FILE_ENCCTX,
							 RCL_SERVER);

			if (encctxlen) {
				CDEBUG(D_SEC,
				       "server returned encryption ctx for "DFID"\n",
				       PFID(ll_inode2fid(inode)));
				rc = ll_xattr_cache_insert(inode,
					       LL_XATTR_NAME_ENCRYPTION_CONTEXT,
							   encctx, encctxlen);
				if (rc)
					CWARN("%s: cannot set enc ctx for "DFID": rc = %d\n",
					      ll_i2sbi(inode)->ll_fsname,
					      PFID(ll_inode2fid(inode)), rc);
				else if (encrypt) {
					rc = llcrypt_get_encryption_info(inode);
					if (rc)
						CDEBUG(D_SEC,
						 "cannot get enc info for "DFID": rc = %d\n",
						 PFID(ll_inode2fid(inode)), rc);
				}
			}
		}

		ll_intent_inode_init(parent, inode, it);

		ll_set_lock_data(ll_i2sbi(parent)->ll_md_exp, inode, it, &bits);
		/* OPEN can return data if lock has DoM+LAYOUT bits set */
		if (it->it_op & IT_OPEN &&
		    bits & MDS_INODELOCK_DOM && bits & MDS_INODELOCK_LAYOUT)
			ll_dom_finish_open(inode, request);

		/* We used to query real size from OSTs here, but actually
		 * this is not needed. For stat() calls size would be updated
		 * from subsequent do_revalidate()->ll_inode_revalidate_it() in
		 * 2.4 and
		 * vfs_getattr_it->ll_getattr()->ll_inode_revalidate_it() in 2.6
		 * Everybody else who needs correct file size would call
		 * ll_glimpse_size or some equivalent themselves anyway.
		 * Also see bug 7198.
		 */

		/* If security context was returned by MDT, put it in
		 * inode now to save an extra getxattr from security hooks,
		 * and avoid deadlock.
		 */
		if (body->mbo_valid & OBD_MD_SECCTX) {
			secctx = req_capsule_server_get(pill, &RMF_FILE_SECCTX);
			secctxlen = req_capsule_get_size(pill,
							   &RMF_FILE_SECCTX,
							   RCL_SERVER);

			if (secctxlen)
				CDEBUG(D_SEC, "server returned security context"
				       " for "DFID"\n",
				       PFID(ll_inode2fid(inode)));
		}

		if (secctx != NULL && secctxlen != 0) {
			/* no need to protect selinux_inode_setsecurity() by
			 * inode_lock. Taking it would lead to a client deadlock
			 * LU-13617
			 */
			rc = security_inode_notifysecctx(inode, secctx,
							 secctxlen);
			if (rc)
				CWARN("%s: cannot set security context for "DFID": rc = %d\n",
				      ll_i2sbi(inode)->ll_fsname,
				      PFID(ll_inode2fid(inode)),
				      rc);
		}
	}

	/* Only hash *de if it is unhashed (new dentry).
	 * Atoimc_open may passin hashed dentries for open.
	 */
	alias = ll_splice_alias(inode, *de);
	if (IS_ERR(alias))
		GOTO(out, rc = PTR_ERR(alias));

	*de = alias;

	if (!it_disposition(it, DISP_LOOKUP_NEG)) {
		/*
		 * we have lookup look - unhide dentry, or protected by
		 * a root WBC EX lock.
		 */
		if (bits & MDS_INODELOCK_LOOKUP ||
		    wbc_inode_has_protected(ll_i2wbci(parent)))
			d_lustre_revalidate(*de);

		if (encrypt) {
			rc = llcrypt_get_encryption_info(inode);
			if (rc)
				GOTO(out, rc);
			if (!llcrypt_has_encryption_key(inode))
				GOTO(out, rc = -ENOKEY);
		}
	} else if (!it_disposition(it, DISP_OPEN_CREATE)) {
		/*
		 * If file was created on the server, the dentry is revalidated
		 * in ll_create_it if the lock allows for it.
		 */
		/* Check that parent has UPDATE lock. */
		struct lookup_intent parent_it = {
					.it_op = IT_GETATTR,
					.it_lock_handle = 0 };
		struct lu_fid	fid = ll_i2info(parent)->lli_fid;

		/* If it is striped directory, get the real stripe parent */
		if (unlikely(ll_dir_striped(parent))) {
			rc = md_get_fid_from_lsm(ll_i2mdexp(parent),
						 ll_i2info(parent)->lli_lsm_md,
						 (*de)->d_name.name,
						 (*de)->d_name.len, &fid);
			if (rc != 0)
				GOTO(out, rc);
		}

		/*
		 * TODO: revalidate the dentry if @parent is under the
		 * protection of the root WBC EX lock for caching the negative
		 * dentries.
		 */
		if (md_revalidate_lock(ll_i2mdexp(parent), &parent_it, &fid,
				       NULL)) {
			d_lustre_revalidate(*de);
			ll_intent_release(&parent_it);
		}
	}

	if (it_disposition(it, DISP_OPEN_CREATE)) {
		ll_stats_ops_tally(ll_i2sbi(parent), LPROC_LL_MKNOD,
				   ktime_us_delta(ktime_get(), kstart));
	}

	GOTO(out, rc = 0);

out:
	if (rc != 0 && it->it_op & IT_OPEN) {
		ll_intent_drop_lock(it);
		ll_open_cleanup((*de)->d_sb, &request->rq_pill);
	}

	return rc;
}

static struct dentry *ll_lookup_it(struct inode *parent, struct dentry *dentry,
				   struct lookup_intent *it,
				   void **secctx, __u32 *secctxlen,
				   struct pcc_create_attach *pca,
				   bool encrypt,
				   void **encctx, __u32 *encctxlen)
{
	ktime_t kstart = ktime_get();
	struct lookup_intent lookup_it = { .it_op = IT_LOOKUP };
	struct dentry *save = dentry, *retval;
	struct ptlrpc_request *req = NULL;
	struct md_op_data *op_data = NULL;
	struct lov_user_md *lum = NULL;
	__u32 opc;
	int rc;
	char secctx_name[XATTR_NAME_MAX + 1];
	__u64 extra_lock_flags;

	ENTRY;

	if (dentry->d_name.len > ll_i2sbi(parent)->ll_namelen)
		RETURN(ERR_PTR(-ENAMETOOLONG));

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p), intent=%s\n",
	       dentry, PFID(ll_inode2fid(parent)), parent, LL_IT2STR(it));

	if (d_mountpoint(dentry))
		CERROR("Tell Peter, lookup on mtpt, it %s\n", LL_IT2STR(it));

	if (it == NULL || it->it_op == IT_GETXATTR)
		it = &lookup_it;

	if (it->it_op == IT_GETATTR && dentry_may_statahead(parent, dentry)) {
		rc = ll_revalidate_statahead(parent, &dentry, 0);
		if (rc == 1)
			RETURN(dentry == save ? NULL : dentry);
	}

	if (it->it_op & IT_OPEN && it->it_flags & FMODE_WRITE &&
	    dentry->d_sb->s_flags & SB_RDONLY)
		RETURN(ERR_PTR(-EROFS));

	if (it->it_op & IT_CREAT)
		opc = LUSTRE_OPC_CREATE;
	else
		opc = LUSTRE_OPC_ANY;

	op_data = ll_prep_md_op_data(NULL, parent, NULL, dentry->d_name.name,
				     dentry->d_name.len, 0, opc, NULL);
	if (IS_ERR(op_data))
		GOTO(out, retval = ERR_CAST(op_data));

	/* enforce umask if acl disabled or MDS doesn't support umask */
	if (!IS_POSIXACL(parent) || !exp_connect_umask(ll_i2mdexp(parent)))
		it->it_create_mode &= ~current_umask();

	if (it->it_op & IT_CREAT &&
	    ll_i2sbi(parent)->ll_flags & LL_SBI_FILE_SECCTX) {
		rc = ll_dentry_init_security(dentry, it->it_create_mode,
					     &dentry->d_name,
					     &op_data->op_file_secctx_name,
					     &op_data->op_file_secctx,
					     &op_data->op_file_secctx_size);
		if (rc < 0)
			GOTO(out, retval = ERR_PTR(rc));
		if (secctx != NULL)
			*secctx = op_data->op_file_secctx;
		if (secctxlen != NULL)
			*secctxlen = op_data->op_file_secctx_size;
	} else {
		if (secctx != NULL)
			*secctx = NULL;
		if (secctxlen != NULL)
			*secctxlen = 0;
	}
	if (it->it_op & IT_CREAT && encrypt) {
		rc = llcrypt_inherit_context(parent, NULL, op_data, false);
		if (rc)
			GOTO(out, retval = ERR_PTR(rc));
		if (encctx != NULL)
			*encctx = op_data->op_file_encctx;
		if (encctxlen != NULL)
			*encctxlen = op_data->op_file_encctx_size;
	} else {
		if (encctx != NULL)
			*encctx = NULL;
		if (encctxlen != NULL)
			*encctxlen = 0;
	}

	/* ask for security context upon intent */
	if (it->it_op & (IT_LOOKUP | IT_GETATTR | IT_OPEN)) {
		/* get name of security xattr to request to server */
		rc = ll_listsecurity(parent, secctx_name,
				     sizeof(secctx_name));
		if (rc < 0) {
			CDEBUG(D_SEC, "cannot get security xattr name for "
			       DFID": rc = %d\n",
			       PFID(ll_inode2fid(parent)), rc);
		} else if (rc > 0) {
			op_data->op_file_secctx_name = secctx_name;
			op_data->op_file_secctx_name_size = rc;
			CDEBUG(D_SEC, "'%.*s' is security xattr for "DFID"\n",
			       rc, secctx_name, PFID(ll_inode2fid(parent)));
		}
	}

	if (pca && pca->pca_dataset) {
		OBD_ALLOC_PTR(lum);
		if (lum == NULL)
			GOTO(out, retval = ERR_PTR(-ENOMEM));

		lum->lmm_magic = LOV_USER_MAGIC_V1;
		lum->lmm_pattern = LOV_PATTERN_F_RELEASED | LOV_PATTERN_RAID0;
		op_data->op_data = lum;
		op_data->op_data_size = sizeof(*lum);
		op_data->op_archive_id = pca->pca_dataset->pccd_rwid;
		it->it_flags |= MDS_OPEN_PCC;
	}

	extra_lock_flags = wbc_intent_lock_flags(ll_i2wbci(parent), it);
	rc = md_intent_lock(ll_i2mdexp(parent), op_data, it, &req,
			    &ll_md_blocking_ast, extra_lock_flags);
	/* If the MDS allows the client to chgrp (CFS_SETGRP_PERM), but the
	 * client does not know which suppgid should be sent to the MDS, or
	 * some other(s) changed the target file's GID after this RPC sent
	 * to the MDS with the suppgid as the original GID, then we should
	 * try again with right suppgid. */
	if (rc == -EACCES && it->it_op & IT_OPEN &&
	    it_disposition(it, DISP_OPEN_DENY)) {
		struct mdt_body *body;

		LASSERT(req != NULL);

		body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
		if (op_data->op_suppgids[0] == body->mbo_gid ||
		    op_data->op_suppgids[1] == body->mbo_gid ||
		    !in_group_p(make_kgid(&init_user_ns, body->mbo_gid)))
			GOTO(out, retval = ERR_PTR(-EACCES));

		fid_zero(&op_data->op_fid2);
		op_data->op_suppgids[1] = body->mbo_gid;
		ptlrpc_req_finished(req);
		req = NULL;
		ll_intent_release(it);
		rc = md_intent_lock(ll_i2mdexp(parent), op_data, it, &req,
				    &ll_md_blocking_ast, extra_lock_flags);
	}

	if (rc < 0)
		GOTO(out, retval = ERR_PTR(rc));

	if (pca && pca->pca_dataset) {
		rc = pcc_inode_create(parent->i_sb, pca->pca_dataset,
				      &op_data->op_fid2,
				      &pca->pca_dentry);
		if (rc)
			GOTO(out, retval = ERR_PTR(rc));
	}

	/* dir layout may change */
	ll_unlock_md_op_lsm(op_data);
	rc = ll_lookup_it_finish(req, it, parent, &dentry,
				 secctx != NULL ? *secctx : NULL,
				 secctxlen != NULL ? *secctxlen : 0,
				 encctx != NULL ? *encctx : NULL,
				 encctxlen != NULL ? *encctxlen : 0,
				 kstart, encrypt);
	if (rc != 0) {
		ll_intent_release(it);
		GOTO(out, retval = ERR_PTR(rc));
	}

	if ((it->it_op & IT_OPEN) && dentry->d_inode &&
	    !S_ISREG(dentry->d_inode->i_mode) &&
	    !S_ISDIR(dentry->d_inode->i_mode)) {
		ll_release_openhandle(dentry, it);
	}
	ll_lookup_finish_locks(it, dentry);

	GOTO(out, retval = (dentry == save) ? NULL : dentry);

out:
	if (op_data != NULL && !IS_ERR(op_data)) {
		if (secctx != NULL && secctxlen != NULL) {
			/* caller needs sec ctx info, so reset it in op_data to
			 * prevent it from being freed */
			op_data->op_file_secctx = NULL;
			op_data->op_file_secctx_size = 0;
		}
		if (encctx != NULL && encctxlen != NULL &&
		    it->it_op & IT_CREAT && encrypt) {
			/* caller needs enc ctx info, so reset it in op_data to
			 * prevent it from being freed
			 */
			op_data->op_file_encctx = NULL;
			op_data->op_file_encctx_size = 0;
		}
		ll_finish_md_op_data(op_data);
	}

	if (lum != NULL)
		OBD_FREE_PTR(lum);

	ptlrpc_req_finished(req);
	return retval;
}

static struct dentry *ll_lookup_nd(struct inode *parent, struct dentry *dentry,
				   unsigned int flags)
{
	struct lookup_intent *itp, it = { .it_op = IT_GETATTR };
	struct dentry *de;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p), flags=%u\n",
	       dentry, PFID(ll_inode2fid(parent)), parent, flags);

	/*
	 * Optimize away (CREATE && !OPEN). Let .create handle the race.
	 * but only if we have write permissions there, otherwise we need
	 * to proceed with lookup. LU-4185
	 */
	if ((flags & LOOKUP_CREATE) && !(flags & LOOKUP_OPEN) &&
	    (inode_permission(parent, MAY_WRITE | MAY_EXEC) == 0))
		return NULL;

	if (flags & (LOOKUP_PARENT|LOOKUP_OPEN|LOOKUP_CREATE))
		itp = NULL;
	else
		itp = &it;
	de = ll_lookup_it(parent, dentry, itp, NULL, NULL, NULL, false,
			  NULL, NULL);

	if (itp != NULL)
		ll_intent_release(itp);

	if (IS_ERR(de) && PTR_ERR(de) == -ENOENT && (flags & LOOKUP_CREATE))
		de = NULL;

	return de;
}

/*
 * For cached negative dentry and new dentry, handle lookup/create/open
 * together.
 */
static int ll_atomic_open(struct inode *dir, struct dentry *dentry,
			  struct file *file, unsigned open_flags,
			  umode_t mode ll_last_arg)
{
	struct lookup_intent *it;
	struct dentry *de;
	long long lookup_flags = LOOKUP_OPEN;
	void *secctx = NULL;
	__u32 secctxlen = 0;
	void *encctx = NULL;
	__u32 encctxlen = 0;
	struct ll_sb_info *sbi = NULL;
	struct pcc_create_attach pca = { NULL, NULL };
	bool encrypt = false;
	int rc = 0;

	ENTRY;

	CDEBUG(D_VFSTRACE,
	       "VFS Op:name=%pd, dir="DFID"(%p), file %p, open_flags %x, mode %x opened %d\n",
	       dentry, PFID(ll_inode2fid(dir)), dir, file, open_flags, mode,
	       ll_is_opened(opened, file));

	/* Only negative dentries enter here */
	LASSERT(dentry->d_inode == NULL);

	if (!d_unhashed(dentry)) {
		/* A valid negative dentry that just passed revalidation,
		 * there's little point to try and open it server-side,
		 * even though there's a minuscule chance it might succeed.
		 * Either way it's a valid race to just return -ENOENT here.
		 */
		if (!(open_flags & O_CREAT))
			return -ENOENT;

		/* Otherwise we just unhash it to be rehashed afresh via
		 * lookup if necessary
		 */
		d_drop(dentry);
	}

	OBD_ALLOC(it, sizeof(*it));
	if (!it)
		RETURN(-ENOMEM);

	it->it_op = IT_OPEN;
	if (open_flags & O_CREAT) {
		it->it_op |= IT_CREAT;
		lookup_flags |= LOOKUP_CREATE;
		sbi = ll_i2sbi(dir);
		/* Volatile file is used for HSM restore, so do not use PCC */
		if (!filename_is_volatile(dentry->d_name.name,
					  dentry->d_name.len, NULL)) {
			struct pcc_matcher item;
			struct pcc_dataset *dataset;

			item.pm_uid = from_kuid(&init_user_ns, current_uid());
			item.pm_gid = from_kgid(&init_user_ns, current_gid());
			item.pm_projid = ll_i2info(dir)->lli_projid;
			item.pm_name = &dentry->d_name;
			dataset = pcc_dataset_match_get(&sbi->ll_pcc_super,
							&item);
			pca.pca_dataset = dataset;
		}
	}
	it->it_create_mode = (mode & S_IALLUGO) | S_IFREG;
	it->it_flags = (open_flags & ~O_ACCMODE) | OPEN_FMODE(open_flags);
	it->it_flags &= ~MDS_OPEN_FL_INTERNAL;

	if (ll_sbi_has_encrypt(ll_i2sbi(dir)) && IS_ENCRYPTED(dir)) {
		/* in case of create, this is going to be a regular file because
		 * we set S_IFREG bit on it->it_create_mode above
		 */
		rc = llcrypt_get_encryption_info(dir);
		if (rc)
			GOTO(out_release, rc);
		if (open_flags & O_CREAT) {
			if (!llcrypt_has_encryption_key(dir))
				GOTO(out_release, rc = -ENOKEY);
			encrypt = true;
		}
	}

	OBD_FAIL_TIMEOUT(OBD_FAIL_LLITE_CREATE_FILE_PAUSE2, cfs_fail_val);

	/* We can only arrive at this path when we have no inode, so
	 * we only need to request open lock if it was requested
	 * for every open
	 */
	if (ll_i2sbi(dir)->ll_oc_thrsh_count == 1)
		it->it_flags |= MDS_OPEN_LOCK;

	/* Dentry added to dcache tree in ll_lookup_it */
	de = ll_lookup_it(dir, dentry, it, &secctx, &secctxlen, &pca, encrypt,
			  &encctx, &encctxlen);
	if (IS_ERR(de))
		rc = PTR_ERR(de);
	else if (de != NULL)
		dentry = de;

	CFS_FAIL_TIMEOUT(OBD_FAIL_LLITE_CREATE_FILE_PAUSE, cfs_fail_val);

	if (!rc) {
		if (it_disposition(it, DISP_OPEN_CREATE)) {
			/* Dentry instantiated in ll_create_it. */
			rc = ll_create_it(dir, dentry, it, secctx, secctxlen,
					  encrypt, encctx, encctxlen);
			ll_security_release_secctx(secctx, secctxlen);
			llcrypt_free_ctx(encctx, encctxlen);
			if (rc) {
				/* We dget in ll_splice_alias. */
				if (de != NULL)
					dput(de);
				goto out_release;
			}

			rc = pcc_inode_create_fini(dentry->d_inode, &pca);
			if (rc) {
				if (de != NULL)
					dput(de);
				GOTO(out_release, rc);
			}

			ll_set_created(opened, file);
		} else {
			/* Open the file with O_CREAT, but the file already
			 * existed on MDT. This may happend in the case that
			 * the LOOKUP ibits lock is revoked and the
			 * corresponding dentry cache is deleted.
			 * i.e. In the current Lustre, the truncate operation
			 * will revoke the LOOKUP ibits lock, and the file
			 * dentry cache will be invalidated. The following open
			 * with O_CREAT flag will call into ->atomic_open, the
			 * file was wrongly though as newly created file and
			 * try to auto cache the file. So after client knows it
			 * is not a DISP_OPEN_CREATE, it should cleanup the
			 * already created PCC copy.
			 */
			pcc_create_attach_cleanup(dir->i_sb, &pca);

			if (open_flags & O_CREAT && encrypt &&
			    dentry->d_inode) {
				rc = ll_set_encflags(dentry->d_inode, encctx,
						     encctxlen, true);
				llcrypt_free_ctx(encctx, encctxlen);
				if (rc)
					GOTO(out_release, rc);
			}
		}

		/* check also if a foreign file is openable */
		if (dentry->d_inode && it_disposition(it, DISP_OPEN_OPEN) &&
		    ll_foreign_is_openable(dentry, open_flags)) {
			/* Open dentry. */
			if (S_ISFIFO(dentry->d_inode->i_mode)) {
				/* We cannot call open here as it might
				 * deadlock. This case is unreachable in
				 * practice because of OBD_CONNECT_NODEVOH. */
				rc = finish_no_open(file, de);
			} else {
				file->private_data = it;
				rc = ll_finish_open(file, dentry, opened);
				/* We dget in ll_splice_alias. finish_open takes
				 * care of dget for fd open.
				 */
				if (de != NULL)
					dput(de);
			}
		} else {
			rc = finish_no_open(file, de);
		}
	} else {
		pcc_create_attach_cleanup(dir->i_sb, &pca);
	}

out_release:
	ll_intent_release(it);
	OBD_FREE(it, sizeof(*it));

	RETURN(rc);
}

/* We depend on "mode" being set with the proper file type/umask by now */
static struct inode *ll_create_node(struct inode *dir, struct lookup_intent *it)
{
        struct inode *inode = NULL;
        struct ptlrpc_request *request = NULL;
        struct ll_sb_info *sbi = ll_i2sbi(dir);
        int rc;
        ENTRY;

	LASSERT(it && it->it_disposition);

	LASSERT(it_disposition(it, DISP_ENQ_CREATE_REF));
	request = it->it_request;
	it_clear_disposition(it, DISP_ENQ_CREATE_REF);
	rc = ll_prep_inode(&inode, &request->rq_pill, dir->i_sb, it);
	if (rc)
		GOTO(out, inode = ERR_PTR(rc));

	/* Pause to allow for a race with concurrent access by fid */
	OBD_FAIL_TIMEOUT(OBD_FAIL_LLITE_CREATE_NODE_PAUSE, cfs_fail_val);

        /* We asked for a lock on the directory, but were granted a
         * lock on the inode.  Since we finally have an inode pointer,
         * stuff it in the lock. */
	CDEBUG(D_DLMTRACE, "setting l_ast_data to inode "DFID"(%p)\n",
	       PFID(ll_inode2fid(inode)), inode);
        ll_set_lock_data(sbi->ll_md_exp, inode, it, NULL);
        EXIT;
 out:
        ptlrpc_req_finished(request);
        return inode;
}

/*
 * By the time this is called, we already have created the directory cache
 * entry for the new file, but it is so far negative - it has no inode.
 *
 * We defer creating the OBD object(s) until open, to keep the intent and
 * non-intent code paths similar, and also because we do not have the MDS
 * inode number before calling ll_create_node() (which is needed for LOV),
 * so we would need to do yet another RPC to the MDS to store the LOV EA
 * data on the MDS.  If needed, we would pass the PACKED lmm as data and
 * lmm_size in datalen (the MDS still has code which will handle that).
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int ll_create_it(struct inode *dir, struct dentry *dentry,
			struct lookup_intent *it,
			void *secctx, __u32 secctxlen, bool encrypt,
			void *encctx, __u32 encctxlen)
{
	struct inode *inode;
	__u64 bits = 0;
	int rc = 0;
	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p), intent=%s\n",
	       dentry, PFID(ll_inode2fid(dir)), dir, LL_IT2STR(it));

	rc = it_open_error(DISP_OPEN_CREATE, it);
	if (rc)
		RETURN(rc);

	inode = ll_create_node(dir, it);
	if (IS_ERR(inode))
		RETURN(PTR_ERR(inode));

	if ((ll_i2sbi(inode)->ll_flags & LL_SBI_FILE_SECCTX) &&
	    secctx != NULL) {
		/* must be done before d_instantiate, because it calls
		 * security_d_instantiate, which means a getxattr if security
		 * context is not set yet */
		/* no need to protect selinux_inode_setsecurity() by
		 * inode_lock. Taking it would lead to a client deadlock
		 * LU-13617
		 */
		rc = security_inode_notifysecctx(inode, secctx, secctxlen);
		if (rc)
			RETURN(rc);
	}

	d_instantiate(dentry, inode);

	if (encrypt) {
		rc = ll_set_encflags(inode, encctx, encctxlen, true);
		if (rc)
			RETURN(rc);
	}

	if (!(ll_i2sbi(inode)->ll_flags & LL_SBI_FILE_SECCTX)) {
		rc = ll_inode_init_security(dentry, inode, dir);
		if (rc)
			RETURN(rc);
	}

	ll_set_lock_data(ll_i2sbi(dir)->ll_md_exp, inode, it, &bits);
	if (bits & MDS_INODELOCK_LOOKUP)
		d_lustre_revalidate(dentry);

	RETURN(0);
}

void ll_update_times(struct ptlrpc_request *request, struct inode *inode)
{
	struct mdt_body *body = req_capsule_server_get(&request->rq_pill,
						       &RMF_MDT_BODY);

	LASSERT(body);
	if (body->mbo_valid & OBD_MD_FLMTIME &&
	    body->mbo_mtime > inode->i_mtime.tv_sec) {
		CDEBUG(D_INODE,
		       "setting fid " DFID " mtime from %lld to %llu\n",
		       PFID(ll_inode2fid(inode)),
		       (s64)inode->i_mtime.tv_sec, body->mbo_mtime);
		inode->i_mtime.tv_sec = body->mbo_mtime;
	}

	if (body->mbo_valid & OBD_MD_FLCTIME &&
	    body->mbo_ctime > inode->i_ctime.tv_sec)
		inode->i_ctime.tv_sec = body->mbo_ctime;
}

static int ll_new_node(struct inode *dir, struct dentry *dchild,
		       const char *tgt, umode_t mode, int rdev, __u32 opc)
{
	struct qstr *name = &dchild->d_name;
	struct ptlrpc_request *request = NULL;
	struct md_op_data *op_data = NULL;
	struct inode *inode = NULL;
	struct ll_sb_info *sbi = ll_i2sbi(dir);
	int tgt_len = 0;
	bool encrypt = false;
	int err;

	ENTRY;
	if (unlikely(tgt != NULL))
		tgt_len = strlen(tgt) + 1;

again:
	op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name,
				     name->len, 0, opc, NULL);
	if (IS_ERR(op_data))
		GOTO(err_exit, err = PTR_ERR(op_data));

	if (sbi->ll_flags & LL_SBI_FILE_SECCTX) {
		err = ll_dentry_init_security(dchild, mode, &dchild->d_name,
					      &op_data->op_file_secctx_name,
					      &op_data->op_file_secctx,
					      &op_data->op_file_secctx_size);
		if (err < 0)
			GOTO(err_exit, err);
	}

	if (ll_sbi_has_encrypt(sbi) &&
	    ((IS_ENCRYPTED(dir) &&
	    (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode))) ||
	    (unlikely(llcrypt_dummy_context_enabled(dir)) && S_ISDIR(mode)))) {
		err = llcrypt_get_encryption_info(dir);
		if (err)
			GOTO(err_exit, err);
		if (!llcrypt_has_encryption_key(dir))
			GOTO(err_exit, err = -ENOKEY);
		encrypt = true;
	}

	if (encrypt) {
		err = llcrypt_inherit_context(dir, NULL, op_data, false);
		if (err)
			GOTO(err_exit, err);
	}

	op_data->op_bias |= wbc_md_op_bias(ll_i2wbci(dir));
	err = md_create(sbi->ll_md_exp, op_data, tgt, tgt_len, mode,
			from_kuid(&init_user_ns, current_fsuid()),
			from_kgid(&init_user_ns, current_fsgid()),
			cfs_curproc_cap_pack(), rdev, 0, &request);
#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(2, 14, 58, 0)
	/*
	 * server < 2.12.58 doesn't pack default LMV in intent_getattr reply,
	 * fetch default LMV here.
	 */
	if (unlikely(err == -EREMOTE)) {
		struct ll_inode_info	*lli = ll_i2info(dir);
		struct lmv_user_md	*lum;
		int			lumsize;
		int			err2;

		ptlrpc_req_finished(request);
		request = NULL;

		err2 = ll_dir_getstripe(dir, (void **)&lum, &lumsize, &request,
					OBD_MD_DEFAULT_MEA);
		ll_finish_md_op_data(op_data);
		op_data = NULL;
		if (err2 == 0) {
			struct lustre_md md = { NULL };

			md.body = req_capsule_server_get(&request->rq_pill,
							 &RMF_MDT_BODY);
			if (!md.body)
				GOTO(err_exit, err = -EPROTO);

			OBD_ALLOC_PTR(md.default_lmv);
			if (!md.default_lmv)
				GOTO(err_exit, err = -ENOMEM);

			md.default_lmv->lsm_md_magic = lum->lum_magic;
			md.default_lmv->lsm_md_stripe_count =
				lum->lum_stripe_count;
			md.default_lmv->lsm_md_master_mdt_index =
				lum->lum_stripe_offset;
			md.default_lmv->lsm_md_hash_type = lum->lum_hash_type;
			md.default_lmv->lsm_md_max_inherit =
				lum->lum_max_inherit;
			md.default_lmv->lsm_md_max_inherit_rr =
				lum->lum_max_inherit_rr;

			err = ll_update_inode(dir, &md);
			md_free_lustre_md(sbi->ll_md_exp, &md);
			if (err)
				GOTO(err_exit, err);
		} else if (err2 == -ENODATA && lli->lli_default_lsm_md) {
			/*
			 * If there are no default stripe EA on the MDT, but the
			 * client has default stripe, then it probably means
			 * default stripe EA has just been deleted.
			 */
			down_write(&lli->lli_lsm_sem);
			if (lli->lli_default_lsm_md)
				OBD_FREE_PTR(lli->lli_default_lsm_md);
			lli->lli_default_lsm_md = NULL;
			up_write(&lli->lli_lsm_sem);
		} else {
			GOTO(err_exit, err);
		}

		ptlrpc_req_finished(request);
		request = NULL;
		goto again;
	}
#endif

	if (err < 0)
		GOTO(err_exit, err);

	ll_update_times(request, dir);

	CFS_FAIL_TIMEOUT(OBD_FAIL_LLITE_NEWNODE_PAUSE, cfs_fail_val);

	err = ll_prep_inode(&inode, &request->rq_pill, dchild->d_sb, NULL);
	if (err)
		GOTO(err_exit, err);

	if (sbi->ll_flags & LL_SBI_FILE_SECCTX) {
		/* must be done before d_instantiate, because it calls
		 * security_d_instantiate, which means a getxattr if security
		 * context is not set yet */
		/* no need to protect selinux_inode_setsecurity() by
		 * inode_lock. Taking it would lead to a client deadlock
		 * LU-13617
		 */
		err = security_inode_notifysecctx(inode,
						  op_data->op_file_secctx,
						  op_data->op_file_secctx_size);
		if (err)
			GOTO(err_exit, err);
	}

	err = ll_new_inode_init(dir, dchild, inode);
	if (err)
		GOTO(err_exit, err);

	if (encrypt) {
		err = ll_set_encflags(inode, op_data->op_file_encctx,
				      op_data->op_file_encctx_size, true);
		if (err)
			GOTO(err_exit, err);
	}

	if (!(sbi->ll_flags & LL_SBI_FILE_SECCTX)) {
		err = ll_inode_init_security(dchild, inode, dir);
		if (err)
			GOTO(err_exit, err);
	}

	EXIT;
err_exit:
	if (request != NULL)
		ptlrpc_req_finished(request);

	if (!IS_ERR_OR_NULL(op_data))
		ll_finish_md_op_data(op_data);

	RETURN(err);
}

static int ll_mknod(struct inode *dir, struct dentry *dchild, umode_t mode,
		    dev_t rdev)
{
	ktime_t kstart = ktime_get();
	int err;
	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p) mode %o dev %x\n",
	       dchild, PFID(ll_inode2fid(dir)), dir, mode, rdev);

	if (!IS_POSIXACL(dir) || !exp_connect_umask(ll_i2mdexp(dir)))
		mode &= ~current_umask();

	switch (mode & S_IFMT) {
	case 0:
		mode |= S_IFREG;
		/* fallthrough */
	case S_IFREG:
	case S_IFCHR:
	case S_IFBLK:
	case S_IFIFO:
	case S_IFSOCK:
		err = ll_new_node(dir, dchild, NULL, mode, old_encode_dev(rdev),
				  LUSTRE_OPC_MKNOD);
		break;
	case S_IFDIR:
		err = -EPERM;
		break;
	default:
		err = -EINVAL;
	}

	if (!err)
		ll_stats_ops_tally(ll_i2sbi(dir), LPROC_LL_MKNOD,
				   ktime_us_delta(ktime_get(), kstart));

	RETURN(err);
}

/*
 * Plain create. Intent create is handled in atomic_open.
 */
static int ll_create_nd(struct inode *dir, struct dentry *dentry,
			umode_t mode, bool want_excl)
{
	ktime_t kstart = ktime_get();
	int rc;

	CFS_FAIL_TIMEOUT(OBD_FAIL_LLITE_CREATE_FILE_PAUSE, cfs_fail_val);

	CDEBUG(D_VFSTRACE,
	       "VFS Op:name=%pd, dir="DFID"(%p), flags=%u, excl=%d\n",
	       dentry, PFID(ll_inode2fid(dir)), dir, mode, want_excl);

	/* Using mknod(2) to create a regular file is designed to not recognize
	 * volatile file name, so we use ll_mknod() here. */
	rc = ll_mknod(dir, dentry, mode, 0);

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, unhashed %d\n",
	       dentry, d_unhashed(dentry));

	if (!rc)
		ll_stats_ops_tally(ll_i2sbi(dir), LPROC_LL_CREATE,
				   ktime_us_delta(ktime_get(), kstart));

	return rc;
}

static int ll_symlink(struct inode *dir, struct dentry *dchild,
		      const char *oldpath)
{
	ktime_t kstart = ktime_get();
	int err;

	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p), target=%.*s\n",
	       dchild, PFID(ll_inode2fid(dir)), dir, 3000, oldpath);

	err = ll_new_node(dir, dchild, oldpath, S_IFLNK | S_IRWXUGO, 0,
			  LUSTRE_OPC_SYMLINK);
	if (!err)
		ll_stats_ops_tally(ll_i2sbi(dir), LPROC_LL_SYMLINK,
				   ktime_us_delta(ktime_get(), kstart));
	RETURN(err);
}

static int ll_link(struct dentry *old_dentry, struct inode *dir,
		   struct dentry *new_dentry)
{
	struct inode *src = old_dentry->d_inode;
	struct qstr *name = &new_dentry->d_name;
	struct ll_sb_info *sbi = ll_i2sbi(dir);
	struct ptlrpc_request *request = NULL;
	struct md_op_data *op_data;
	ktime_t kstart = ktime_get();
	int err;

	ENTRY;
	CDEBUG(D_VFSTRACE,
	       "VFS Op: inode="DFID"(%p), dir="DFID"(%p), target=%pd\n",
	       PFID(ll_inode2fid(src)), src,
	       PFID(ll_inode2fid(dir)), dir, new_dentry);

	err = llcrypt_prepare_link(old_dentry, dir, new_dentry);
	if (err)
		RETURN(err);

	op_data = ll_prep_md_op_data(NULL, src, dir, name->name, name->len,
				     0, LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	err = md_link(sbi->ll_md_exp, op_data, &request);
	ll_finish_md_op_data(op_data);
	if (err)
		GOTO(out, err);

	ll_update_times(request, dir);
	ll_stats_ops_tally(sbi, LPROC_LL_LINK,
			   ktime_us_delta(ktime_get(), kstart));
	EXIT;
out:
	ptlrpc_req_finished(request);
	RETURN(err);
}

static int ll_mkdir(struct inode *dir, struct dentry *dchild, umode_t mode)
{
	struct lookup_intent mkdir_it = { .it_op = IT_CREAT };
	struct ll_sb_info *sbi = ll_i2sbi(dir);
	struct ptlrpc_request *request = NULL;
	struct md_op_data *op_data;
	struct inode *inode = NULL;
	ktime_t kstart = ktime_get();
	bool excl_cache = false;
	__u64 extra_lock_flags;
	int rc;

	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p)\n",
	       dchild, PFID(ll_inode2fid(dir)), dir);

	if (!IS_POSIXACL(dir) || !exp_connect_umask(ll_i2mdexp(dir)))
		mode &= ~current_umask();

	/*
	 * TODO: If not want the granted WBC EX lock to be canceled due to
	 * aging, the lock should not be put into the LRU list via the flag
	 * LDLM_FL_NO_LRU.
	 */
	excl_cache = wbc_may_exclusive_cache(dir, dchild, mode,
					     &extra_lock_flags);

	if (!excl_cache) {
		mode = (mode & (S_IRWXUGO | S_ISVTX)) | S_IFDIR;
		rc = ll_new_node(dir, dchild, NULL, mode, 0, LUSTRE_OPC_MKDIR);
		GOTO(out_stats, rc);
	}

	mkdir_it.it_create_mode = (mode & (S_IRWXUGO | S_ISVTX)) | S_IFDIR;

	op_data = ll_prep_md_op_data(NULL, dir, NULL, dchild->d_name.name,
				     dchild->d_name.len, mode, LUSTRE_OPC_MKDIR,
				     NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	if (extra_lock_flags & LDLM_FL_INTENT_PARENT_LOCKED)
		op_data->op_bias |= MDS_WBC_LOCKLESS;

	if ((sbi->ll_flags & LL_SBI_FILE_SECCTX) && !excl_cache) {
		rc = ll_dentry_init_security(dchild, mode, &dchild->d_name,
					     &op_data->op_file_secctx_name,
					     &op_data->op_file_secctx,
					     &op_data->op_file_secctx_size);
		if (rc < 0)
			GOTO(out_fini, rc);
	}


	rc = md_intent_lock(sbi->ll_md_exp, op_data, &mkdir_it,
			    &request, &ll_md_blocking_ast, extra_lock_flags);
	if (rc)
		GOTO(out_fini, rc);

	/* dir layout may change */
	ll_unlock_md_op_lsm(op_data);

	ll_update_times(request, dir);

	CFS_FAIL_TIMEOUT(OBD_FAIL_LLITE_NEWNODE_PAUSE, cfs_fail_val);

	rc = ll_prep_inode(&inode, &request->rq_pill, dchild->d_sb, &mkdir_it);
	if (rc)
		GOTO(out_fini, rc);

	if (excl_cache)
		wbc_intent_inode_init(dir, inode, &mkdir_it);

	if (sbi->ll_flags & LL_SBI_FILE_SECCTX && !excl_cache) {
		inode_lock(inode);
		/* must be done before d_instantiate, because it calls
		 * security_d_instantiate, which means a getxattr if security
		 * context is not set yet
		 */
		rc = security_inode_notifysecctx(inode,
						 op_data->op_file_secctx,
						 op_data->op_file_secctx_size);
		inode_unlock(inode);
		if (rc)
			GOTO(out_fini, rc);
	}

	rc = ll_d_init(dchild);
	if (rc)
		GOTO(out_fini, rc);

	if (d_unhashed(dchild))
		d_add(dchild, inode);
	else
		d_instantiate(dchild, inode);

	if (!(sbi->ll_flags & LL_SBI_FILE_SECCTX) && !excl_cache) {
		rc = ll_inode_init_security(dchild, inode, dir);
		if (rc)
			GOTO(out_fini, rc);
	}


	if (mkdir_it.it_lock_mode || excl_cache) {
		__u64 bits = 0;

		LASSERT(it_disposition(&mkdir_it, DISP_LOOKUP_NEG));
		ll_set_lock_data(sbi->ll_md_exp, inode, &mkdir_it, &bits);
		if (bits & MDS_INODELOCK_LOOKUP || excl_cache)
			d_lustre_revalidate(dchild);
	}

out_fini:
	ll_finish_md_op_data(op_data);
	ll_intent_release(&mkdir_it);
	ptlrpc_req_finished(request);
out_stats:
	if (rc == 0)
		ll_stats_ops_tally(sbi, LPROC_LL_MKDIR,
				   ktime_us_delta(ktime_get(), kstart));

	RETURN(rc);
}

/*
 * TODO: Same optimization to unlink() if the inode of @dchild is a root
 * WBC inode.
 */
static int ll_rmdir(struct inode *dir, struct dentry *dchild)
{
	struct qstr *name = &dchild->d_name;
	struct ptlrpc_request *request = NULL;
	struct md_op_data *op_data;
	ktime_t kstart = ktime_get();
	int rc;

	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p)\n",
	       dchild, PFID(ll_inode2fid(dir)), dir);

	if (unlikely(d_mountpoint(dchild)))
                RETURN(-EBUSY);

	/* some foreign dir may not be allowed to be removed */
	if (!ll_foreign_is_removable(dchild, false))
		RETURN(-EPERM);

	op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name, name->len,
				     S_IFDIR, LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	if (dchild->d_inode != NULL)
		op_data->op_fid3 = *ll_inode2fid(dchild->d_inode);

	op_data->op_fid2 = op_data->op_fid3;
	op_data->op_bias |= wbc_md_op_bias(ll_i2wbci(dir));
	rc = md_unlink(ll_i2sbi(dir)->ll_md_exp, op_data, &request);
	ll_finish_md_op_data(op_data);
	if (!rc) {
		struct mdt_body *body;

		ll_update_times(request, dir);
		ll_stats_ops_tally(ll_i2sbi(dir), LPROC_LL_RMDIR,
				   ktime_us_delta(ktime_get(), kstart));

		/*
		 * The server puts attributes in on the last unlink, use them
		 * to update the link count so the inode can be freed
		 * immediately.
		 */
		if (!wbc_inode_reserved(ll_i2wbci(dchild->d_inode))) {
			body = req_capsule_server_get(&request->rq_pill,
						      &RMF_MDT_BODY);
			if (body->mbo_valid & OBD_MD_FLNLINK)
				set_nlink(dchild->d_inode, body->mbo_nlink);
		}
	}

	ptlrpc_req_finished(request);

	RETURN(rc);
}

/**
 * Remove dir entry
 **/
int ll_rmdir_entry(struct inode *dir, char *name, int namelen)
{
	struct ptlrpc_request *request = NULL;
	struct md_op_data *op_data;
	ktime_t kstart = ktime_get();
	int rc;
	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%.*s, dir="DFID"(%p)\n",
	       namelen, name, PFID(ll_inode2fid(dir)), dir);

	op_data = ll_prep_md_op_data(NULL, dir, NULL, name, strlen(name),
				     S_IFDIR, LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));
	op_data->op_cli_flags |= CLI_RM_ENTRY;
	rc = md_unlink(ll_i2sbi(dir)->ll_md_exp, op_data, &request);
	ll_finish_md_op_data(op_data);
	if (!rc)
		ll_update_times(request, dir);

	ptlrpc_req_finished(request);
	if (!rc)
		ll_stats_ops_tally(ll_i2sbi(dir), LPROC_LL_RMDIR,
				   ktime_us_delta(ktime_get(), kstart));
	RETURN(rc);
}

/*
 * TODO: If the inode of @dchild is a root WBC inode, it can be optimized as
 * follows:
 * As the inode of @dchild is under the protection of the root WBC EX lock,
 * thus it can delete the inode from icache and discard all cache pages
 * directly upon the last unlink, and then cancel the root WBC EX lock via
 * early lock cancelation (ELC) when send unlink request to MDT; Otherwise,
 * the blocking callback of the root WBC EX lock will flush all dirty data
 * to OSTs unnecessaryly.
 */
static int ll_unlink(struct inode *dir, struct dentry *dchild)
{
	struct qstr *name = &dchild->d_name;
	struct ptlrpc_request *request = NULL;
	struct md_op_data *op_data;
	struct mdt_body *body;
	ktime_t kstart = ktime_get();
	int rc;

	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:name=%pd, dir="DFID"(%p)\n",
	       dchild, PFID(ll_inode2fid(dir)), dir);

	/*
	 * XXX: unlink bind mountpoint maybe call to here,
	 * just check it as vfs_unlink does.
	 */
	if (unlikely(d_mountpoint(dchild)))
		RETURN(-EBUSY);

	/* some foreign file/dir may not be allowed to be unlinked */
	if (!ll_foreign_is_removable(dchild, false))
		RETURN(-EPERM);

	op_data = ll_prep_md_op_data(NULL, dir, NULL, name->name, name->len, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	op_data->op_fid3 = *ll_inode2fid(dchild->d_inode);
	/* notify lower layer if inode has dirty pages */
	if (S_ISREG(dchild->d_inode->i_mode) &&
	    ll_i2info(dchild->d_inode)->lli_clob &&
	    dirty_cnt(dchild->d_inode))
		op_data->op_cli_flags |= CLI_DIRTY_DATA;
	op_data->op_fid2 = op_data->op_fid3;
	op_data->op_bias |= wbc_md_op_bias(ll_i2wbci(dir));
	rc = md_unlink(ll_i2sbi(dir)->ll_md_exp, op_data, &request);
	ll_finish_md_op_data(op_data);
	if (rc)
		GOTO(out, rc);

	if (!wbc_inode_reserved(ll_i2wbci(dchild->d_inode))) {
		/*
		 * The server puts attributes in on the last unlink, use them
		 * to update the link count so the inode can be freed
		 * immediately.
		 */
		body = req_capsule_server_get(&request->rq_pill, &RMF_MDT_BODY);
		if (body->mbo_valid & OBD_MD_FLNLINK)
			set_nlink(dchild->d_inode, body->mbo_nlink);
	}

	ll_update_times(request, dir);

out:
	ptlrpc_req_finished(request);
	if (!rc)
		ll_stats_ops_tally(ll_i2sbi(dir), LPROC_LL_UNLINK,
				   ktime_us_delta(ktime_get(), kstart));
	RETURN(rc);
}

static int ll_rename(struct inode *src, struct dentry *src_dchild,
		     struct inode *tgt, struct dentry *tgt_dchild
#ifdef HAVE_IOPS_RENAME_WITH_FLAGS
		     , unsigned int flags
#endif
		     )
{
	struct qstr *src_name = &src_dchild->d_name;
	struct qstr *tgt_name = &tgt_dchild->d_name;
	struct ptlrpc_request *request = NULL;
	struct ll_sb_info *sbi = ll_i2sbi(src);
	struct md_op_data *op_data;
	ktime_t kstart = ktime_get();
	umode_t mode = 0;
	int err;

	ENTRY;

#ifdef HAVE_IOPS_RENAME_WITH_FLAGS
	if (flags)
		return -EINVAL;
#endif

	CDEBUG(D_VFSTRACE,
	       "VFS Op:oldname=%pd, src_dir="DFID"(%p), newname=%pd, tgt_dir="DFID"(%p)\n",
	       src_dchild, PFID(ll_inode2fid(src)), src,
	       tgt_dchild, PFID(ll_inode2fid(tgt)), tgt);

	if (unlikely(d_mountpoint(src_dchild) || d_mountpoint(tgt_dchild)))
		RETURN(-EBUSY);

#ifdef HAVE_IOPS_RENAME_WITH_FLAGS
	err = llcrypt_prepare_rename(src, src_dchild, tgt, tgt_dchild, flags);
#else
	err = llcrypt_prepare_rename(src, src_dchild, tgt, tgt_dchild, 0);
#endif
	if (err)
		RETURN(err);

	if (src_dchild->d_inode)
		mode = src_dchild->d_inode->i_mode;

	if (tgt_dchild->d_inode)
		mode = tgt_dchild->d_inode->i_mode;

	op_data = ll_prep_md_op_data(NULL, src, tgt, NULL, 0, mode,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	if (src_dchild->d_inode)
		op_data->op_fid3 = *ll_inode2fid(src_dchild->d_inode);

	if (tgt_dchild->d_inode)
		op_data->op_fid4 = *ll_inode2fid(tgt_dchild->d_inode);

	err = md_rename(sbi->ll_md_exp, op_data,
			src_name->name, src_name->len,
			tgt_name->name, tgt_name->len, &request);
	ll_finish_md_op_data(op_data);
	if (!err) {
		ll_update_times(request, src);
		ll_update_times(request, tgt);
	}

	ptlrpc_req_finished(request);

	if (!err) {
		d_move(src_dchild, tgt_dchild);
		ll_stats_ops_tally(sbi, LPROC_LL_RENAME,
				   ktime_us_delta(ktime_get(), kstart));
	}

	RETURN(err);
}

const struct inode_operations ll_dir_inode_operations = {
	.mknod		= ll_mknod,
	.atomic_open	= ll_atomic_open,
	.lookup		= ll_lookup_nd,
	.create		= ll_create_nd,
	/* We need all these non-raw things for NFSD, to not patch it. */
	.unlink		= ll_unlink,
	.mkdir		= ll_mkdir,
	.rmdir		= ll_rmdir,
	.symlink	= ll_symlink,
	.link		= ll_link,
	.rename		= ll_rename,
	.setattr	= ll_setattr,
	.getattr	= ll_getattr,
	.permission	= ll_inode_permission,
#ifdef HAVE_IOP_XATTR
	.setxattr	= ll_setxattr,
	.getxattr	= ll_getxattr,
	.removexattr	= ll_removexattr,
#endif
	.listxattr	= ll_listxattr,
	.get_acl	= ll_get_acl,
#ifdef HAVE_IOP_SET_ACL
	.set_acl	= ll_set_acl,
#endif
};

const struct inode_operations ll_special_inode_operations = {
	.setattr        = ll_setattr,
	.getattr        = ll_getattr,
	.permission     = ll_inode_permission,
#ifdef HAVE_IOP_XATTR
	.setxattr	= ll_setxattr,
	.getxattr	= ll_getxattr,
	.removexattr    = ll_removexattr,
#endif
	.listxattr      = ll_listxattr,
	.get_acl	= ll_get_acl,
#ifdef HAVE_IOP_SET_ACL
	.set_acl	= ll_set_acl,
#endif
};
