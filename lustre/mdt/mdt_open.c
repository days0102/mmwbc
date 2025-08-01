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
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * lustre/mdt/mdt_open.c
 *
 * Lustre Metadata Target (mdt) open/close file handling
 *
 * Author: Huang Hua <huanghua@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <lustre_acl.h>
#include <lustre_mds.h>
#include <lustre_swab.h>
#include "mdt_internal.h"
#include <lustre_nodemap.h>

static const char mfd_open_handle_owner[] = "mdt";

/* Create a new mdt_file_data struct, initialize it,
 * and insert it to global hash table */
struct mdt_file_data *mdt_mfd_new(const struct mdt_export_data *med)
{
	struct mdt_file_data *mfd;
	ENTRY;

	OBD_ALLOC_PTR(mfd);
	if (mfd != NULL) {
		refcount_set(&mfd->mfd_open_handle.h_ref, 1);
		INIT_HLIST_NODE(&mfd->mfd_open_handle.h_link);
		mfd->mfd_owner = med;
		INIT_LIST_HEAD(&mfd->mfd_list);
		class_handle_hash(&mfd->mfd_open_handle, mfd_open_handle_owner);
	}

	RETURN(mfd);
}

/*
 * Find the mfd pointed to by handle in global hash table.
 * In case of replay the handle is obsoleted
 * but mfd can be found in mfd list by that handle.
 * Callers need to be holding med_open_lock.
 */
struct mdt_file_data *mdt_open_handle2mfd(struct mdt_export_data *med,
					const struct lustre_handle *open_handle,
					bool is_replay_or_resent)
{
	struct mdt_file_data   *mfd;
	ENTRY;

	LASSERT(open_handle != NULL);
	mfd = class_handle2object(open_handle->cookie, mfd_open_handle_owner);
	if (mfd)
		refcount_dec(&mfd->mfd_open_handle.h_ref);

	/* during dw/setattr replay the mfd can be found by old handle */
	if ((!mfd || mfd->mfd_owner != med) && is_replay_or_resent) {
		list_for_each_entry(mfd, &med->med_open_head, mfd_list) {
			if (mfd->mfd_open_handle_old.cookie ==
			    open_handle->cookie)
				RETURN(mfd);
		}
		mfd = NULL;
	}

	RETURN(mfd);
}

/* free mfd */
void mdt_mfd_free(struct mdt_file_data *mfd)
{
	LASSERT(refcount_read(&mfd->mfd_open_handle.h_ref) == 1);
	LASSERT(list_empty(&mfd->mfd_list));
	OBD_FREE_PRE(mfd, sizeof(*mfd), "rcu");
	kfree_rcu(mfd, mfd_open_handle.h_rcu);
}

static int mdt_create_data(struct mdt_thread_info *info,
			   struct mdt_object *p, struct mdt_object *o)
{
	struct md_op_spec     *spec = &info->mti_spec;
	struct md_attr        *ma   = &info->mti_attr;
	int                    rc   = 0;
	ENTRY;

	if (!md_should_create(spec->sp_cr_flags))
		RETURN(0);

	ma->ma_need = MA_INODE | MA_LOV;
	ma->ma_valid = 0;
	mutex_lock(&o->mot_lov_mutex);
	if (!o->mot_lov_created) {
		rc = mdo_create_data(info->mti_env,
				     p ? mdt_object_child(p) : NULL,
				     mdt_object_child(o), spec, ma);
		if (rc == 0)
			rc = mdt_attr_get_complex(info, o, ma);

		if (rc == 0 && ma->ma_valid & MA_LOV)
			o->mot_lov_created = 1;
	}

	mutex_unlock(&o->mot_lov_mutex);
	RETURN(rc);
}

int mdt_write_read(struct mdt_object *o)
{
	int rc = 0;
	ENTRY;
	spin_lock(&o->mot_write_lock);
	rc = o->mot_write_count;
	spin_unlock(&o->mot_write_lock);
	RETURN(rc);
}

int mdt_write_get(struct mdt_object *o)
{
	int rc = 0;
	ENTRY;
	spin_lock(&o->mot_write_lock);
	if (o->mot_write_count < 0)
		rc = -ETXTBSY;
	else
		o->mot_write_count++;
	spin_unlock(&o->mot_write_lock);

	RETURN(rc);
}

void mdt_write_put(struct mdt_object *o)
{
	ENTRY;
	spin_lock(&o->mot_write_lock);
	o->mot_write_count--;
	spin_unlock(&o->mot_write_lock);
	EXIT;
}

static int mdt_write_deny(struct mdt_object *o)
{
	int rc = 0;
	ENTRY;
	spin_lock(&o->mot_write_lock);
	if (o->mot_write_count > 0)
		rc = -ETXTBSY;
	else
		o->mot_write_count--;
	spin_unlock(&o->mot_write_lock);
	RETURN(rc);
}

static void mdt_write_allow(struct mdt_object *o)
{
	ENTRY;
	spin_lock(&o->mot_write_lock);
	o->mot_write_count++;
	spin_unlock(&o->mot_write_lock);
	EXIT;
}

/* there can be no real transaction so prepare the fake one */
static void mdt_empty_transno(struct mdt_thread_info *info, int rc)
{
        struct mdt_device      *mdt = info->mti_mdt;
        struct ptlrpc_request  *req = mdt_info_req(info);
        struct tg_export_data  *ted;
        struct lsd_client_data *lcd;
        ENTRY;

	if (mdt_rdonly(req->rq_export))
		RETURN_EXIT;

        /* transaction has occurred already */
        if (lustre_msg_get_transno(req->rq_repmsg) != 0)
                RETURN_EXIT;

	if (tgt_is_multimodrpcs_client(req->rq_export)) {
		struct thandle	       *th;

		/* generate an empty transaction to get a transno
		 * and reply data */
		th = dt_trans_create(info->mti_env, mdt->mdt_bottom);
		if (!IS_ERR(th)) {
			rc = dt_trans_start(info->mti_env, mdt->mdt_bottom, th);
			dt_trans_stop(info->mti_env, mdt->mdt_bottom, th);
		}
		RETURN_EXIT;
	}

	spin_lock(&mdt->mdt_lut.lut_translock);
	if (rc != 0) {
		if (info->mti_transno != 0) {
			struct obd_export *exp = req->rq_export;

			CERROR("%s: replay trans %llu NID %s: rc = %d\n",
			       mdt_obd_name(mdt), info->mti_transno,
			       obd_export_nid2str(exp), rc);
			spin_unlock(&mdt->mdt_lut.lut_translock);
			RETURN_EXIT;
		}
	} else if (info->mti_transno == 0) {
		info->mti_transno = ++mdt->mdt_lut.lut_last_transno;
	} else {
		/* should be replay */
		if (info->mti_transno > mdt->mdt_lut.lut_last_transno)
			mdt->mdt_lut.lut_last_transno = info->mti_transno;
	}
	spin_unlock(&mdt->mdt_lut.lut_translock);

	CDEBUG(D_INODE, "transno = %llu, last_committed = %llu\n",
	       info->mti_transno,
	       req->rq_export->exp_obd->obd_last_committed);

	req->rq_transno = info->mti_transno;
	lustre_msg_set_transno(req->rq_repmsg, info->mti_transno);

	/* update lcd in memory only for resent cases */
	ted = &req->rq_export->exp_target_data;
	LASSERT(ted);
	mutex_lock(&ted->ted_lcd_lock);
	lcd = ted->ted_lcd;
	if (info->mti_transno < lcd->lcd_last_transno &&
	    info->mti_transno != 0) {
		/* This should happen during replay. Do not update
		 * last rcvd info if replay req transno < last transno,
		 * otherwise the following resend(after replay) can not
		 * be checked correctly by xid */
		mutex_unlock(&ted->ted_lcd_lock);
		CDEBUG(D_HA, "%s: transno = %llu < last_transno = %llu\n",
		       mdt_obd_name(mdt), info->mti_transno,
		       lcd->lcd_last_transno);
		RETURN_EXIT;
	}

	if (lustre_msg_get_opc(req->rq_reqmsg) == MDS_CLOSE) {
		if (info->mti_transno != 0)
			lcd->lcd_last_close_transno = info->mti_transno;
                lcd->lcd_last_close_xid = req->rq_xid;
                lcd->lcd_last_close_result = rc;
        } else {
                /* VBR: save versions in last_rcvd for reconstruct. */
                __u64 *pre_versions = lustre_msg_get_versions(req->rq_repmsg);
                if (pre_versions) {
                        lcd->lcd_pre_versions[0] = pre_versions[0];
                        lcd->lcd_pre_versions[1] = pre_versions[1];
                        lcd->lcd_pre_versions[2] = pre_versions[2];
                        lcd->lcd_pre_versions[3] = pre_versions[3];
                }
		if (info->mti_transno != 0)
			lcd->lcd_last_transno = info->mti_transno;

		lcd->lcd_last_xid = req->rq_xid;
                lcd->lcd_last_result = rc;
                lcd->lcd_last_data = info->mti_opdata;
        }
	mutex_unlock(&ted->ted_lcd_lock);

        EXIT;
}

void mdt_mfd_set_mode(struct mdt_file_data *mfd, u64 open_flags)
{
	LASSERT(mfd != NULL);

	CDEBUG(D_DENTRY, DFID " Change mfd open_flags %#llo -> %#llo.\n",
	       PFID(mdt_object_fid(mfd->mfd_object)), mfd->mfd_open_flags,
	       open_flags);

	mfd->mfd_open_flags = open_flags;
}

/**
 * prep ma_lmm/ma_lmv for md_attr from reply
 */
void mdt_prep_ma_buf_from_rep(struct mdt_thread_info *info,
			      struct mdt_object *obj,
			      struct md_attr *ma)
{
	if (ma->ma_lmv || ma->ma_lmm) {
		CDEBUG(D_INFO, DFID " %s already set.\n",
		       PFID(mdt_object_fid(obj)),
		       ma->ma_lmv ? (ma->ma_lmm ? "ma_lmv and ma_lmm"
						: "ma_lmv")
				  : "ma_lmm");
		return;
	}

	if (S_ISDIR(obj->mot_header.loh_attr)) {
		ma->ma_lmv = req_capsule_server_get(info->mti_pill,
						    &RMF_MDT_MD);
		ma->ma_lmv_size = req_capsule_get_size(info->mti_pill,
						       &RMF_MDT_MD,
						       RCL_SERVER);
		if (ma->ma_lmv_size > 0)
			ma->ma_need |= MA_LMV;

		if (req_capsule_has_field(info->mti_pill, &RMF_DEFAULT_MDT_MD,
					  RCL_SERVER)) {
			ma->ma_default_lmv =
				req_capsule_server_get(info->mti_pill,
						       &RMF_DEFAULT_MDT_MD);
			ma->ma_default_lmv_size =
				req_capsule_get_size(info->mti_pill,
						     &RMF_DEFAULT_MDT_MD,
						     RCL_SERVER);
			if (ma->ma_default_lmv_size > 0)
				ma->ma_need |= MA_LMV_DEF;
		}
	} else {
		ma->ma_lmm = req_capsule_server_get(info->mti_pill,
						    &RMF_MDT_MD);
		ma->ma_lmm_size = req_capsule_get_size(info->mti_pill,
						       &RMF_MDT_MD,
						       RCL_SERVER);
		if (ma->ma_lmm_size > 0)
			ma->ma_need |= MA_LOV;
	}
}

static int mdt_mfd_open(struct mdt_thread_info *info, struct mdt_object *p,
			struct mdt_object *o, u64 open_flags, int created,
			struct ldlm_reply *rep)
{
	struct ptlrpc_request *req = mdt_info_req(info);
	struct mdt_export_data *med = &req->rq_export->exp_mdt_data;
	struct mdt_file_data *mfd;
	struct md_attr *ma  = &info->mti_attr;
	struct lu_attr *la  = &ma->ma_attr;
	struct mdt_body *repbody;
	bool isdir, isreg;
	int rc = 0;

	ENTRY;
	repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);

	isreg = S_ISREG(la->la_mode);
	isdir = S_ISDIR(la->la_mode);
	if (isreg && !(ma->ma_valid & MA_LOV) &&
	    !(open_flags & MDS_OPEN_RELEASE)) {
		/*
		 * No EA, check whether it is will set regEA and dirEA since in
		 * above attr get, these size might be zero, so reset it, to
		 * retrieve the MD after create obj.
		 */
		ma->ma_lmm_size = req_capsule_get_size(info->mti_pill,
						       &RMF_MDT_MD,
						       RCL_SERVER);
		/* in replay case, p == NULL */
		rc = mdt_create_data(info, p, o);
		if (rc)
			RETURN(rc);

		if (exp_connect_flags(req->rq_export) & OBD_CONNECT_DISP_STRIPE)
			mdt_set_disposition(info, rep, DISP_OPEN_STRIPE);
	}

	CDEBUG(D_INODE, "after open, ma_valid bit = %#llx lmm_size = %d\n",
	       ma->ma_valid, ma->ma_lmm_size);

	if (ma->ma_valid & MA_LOV) {
		LASSERT(ma->ma_lmm_size != 0);
		repbody->mbo_eadatasize = ma->ma_lmm_size;
		if (isdir)
			repbody->mbo_valid |= OBD_MD_FLDIREA;
		else
			repbody->mbo_valid |= OBD_MD_FLEASIZE;
	}

	if (ma->ma_valid & MA_LMV) {
		LASSERT(ma->ma_lmv_size != 0);
		repbody->mbo_eadatasize = ma->ma_lmv_size;
		LASSERT(isdir);
		repbody->mbo_valid |= OBD_MD_FLDIREA | OBD_MD_MEA;
	}

	if (open_flags & MDS_FMODE_WRITE)
		rc = mdt_write_get(o);
	else if (open_flags & MDS_FMODE_EXEC)
		rc = mdt_write_deny(o);

	if (rc)
		RETURN(rc);

	rc = mo_open(info->mti_env, mdt_object_child(o),
		     created ? open_flags | MDS_OPEN_CREATED : open_flags,
		     &info->mti_spec);
	if (rc != 0) {
		/* If we allow the client to chgrp (CFS_SETGRP_PERM), but the
		 * client does not know which suppgid should be sent to the MDS,
		 * or some other(s) changed the target file's GID after this RPC
		 * sent to the MDS with the suppgid as the original GID, then we
		 * give the client another chance to send the right suppgid. */
		if (rc == -EACCES &&
		    allow_client_chgrp(info, lu_ucred(info->mti_env)))
			mdt_set_disposition(info, rep, DISP_OPEN_DENY);

		GOTO(err_out, rc);
	}

	mfd = mdt_mfd_new(med);
	if (mfd == NULL)
		GOTO(err_out, rc = -ENOMEM);

	/*
	 * Keep a reference on this object for this open, and is
	 * released by mdt_mfd_close().
	 */
	mdt_object_get(info->mti_env, o);
	mfd->mfd_object = o;
	mfd->mfd_xid = req->rq_xid;

	/*
	 * @open_flags is always not zero. At least it should be FMODE_READ,
	 * FMODE_WRITE or MDS_FMODE_EXEC.
	 */
	LASSERT(open_flags != 0);

	/* Open handling. */
	mdt_mfd_set_mode(mfd, open_flags);

	atomic_inc(&o->mot_open_count);
	if (open_flags & MDS_OPEN_LEASE)
		atomic_inc(&o->mot_lease_count);

	/* replay handle */
	if (req_is_replay(req)) {
		struct mdt_file_data *old_mfd;
		/* Check wheather old cookie already exist in
		 * the list, becasue when do recovery, client
		 * might be disconnected from server, and
		 * restart replay, so there maybe some orphan
		 * mfd here, we should remove them */
		LASSERT(info->mti_rr.rr_open_handle != NULL);
		spin_lock(&med->med_open_lock);
		old_mfd = mdt_open_handle2mfd(med, info->mti_rr.rr_open_handle,
					      true);
		if (old_mfd != NULL) {
			CDEBUG(D_HA, "delete orphan mfd = %p, fid = "DFID", "
			       "cookie = %#llx\n", mfd,
			       PFID(mdt_object_fid(mfd->mfd_object)),
			       info->mti_rr.rr_open_handle->cookie);
			class_handle_unhash(&old_mfd->mfd_open_handle);
			list_del_init(&old_mfd->mfd_list);
			spin_unlock(&med->med_open_lock);
			/* no attr update for that close */
			la->la_valid = 0;
			ma->ma_valid |= MA_FLAGS;
			ma->ma_attr_flags |= MDS_RECOV_OPEN;
			mdt_mfd_close(info, old_mfd);
			ma->ma_attr_flags &= ~MDS_RECOV_OPEN;
			ma->ma_valid &= ~MA_FLAGS;
		} else {
			spin_unlock(&med->med_open_lock);
			CDEBUG(D_HA, "orphan mfd not found, fid = "DFID", "
			       "cookie = %#llx\n",
			       PFID(mdt_object_fid(mfd->mfd_object)),
			       info->mti_rr.rr_open_handle->cookie);
		}

		CDEBUG(D_HA, "Store old cookie %#llx in new mfd\n",
		       info->mti_rr.rr_open_handle->cookie);

		mfd->mfd_open_handle_old = *info->mti_rr.rr_open_handle;
	}

	repbody->mbo_open_handle.cookie = mfd->mfd_open_handle.h_cookie;

	if (req->rq_export->exp_disconnected) {
		spin_lock(&med->med_open_lock);
		class_handle_unhash(&mfd->mfd_open_handle);
		list_del_init(&mfd->mfd_list);
		spin_unlock(&med->med_open_lock);
		mdt_mfd_close(info, mfd);
	} else {
		spin_lock(&med->med_open_lock);
		list_add_tail(&mfd->mfd_list, &med->med_open_head);
		spin_unlock(&med->med_open_lock);
	}

	mdt_empty_transno(info, rc);

	RETURN(rc);

err_out:
	if (open_flags & MDS_FMODE_WRITE)
		mdt_write_put(o);
	else if (open_flags & MDS_FMODE_EXEC)
		mdt_write_allow(o);

	return rc;
}

static int mdt_finish_open(struct mdt_thread_info *info,
			   struct mdt_object *p, struct mdt_object *o,
			   u64 open_flags, int created,
			   struct ldlm_reply *rep)
{
	struct ptlrpc_request	*req = mdt_info_req(info);
	struct obd_export	*exp = req->rq_export;
	struct mdt_export_data	*med = &req->rq_export->exp_mdt_data;
	struct md_attr		*ma  = &info->mti_attr;
	struct lu_attr		*la  = &ma->ma_attr;
	struct mdt_file_data	*mfd;
	struct mdt_body		*repbody;
	int			 rc = 0;
	int			 isreg, isdir, islnk;
	struct list_head	*t;
	ENTRY;

        LASSERT(ma->ma_valid & MA_INODE);

        repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);

        isreg = S_ISREG(la->la_mode);
        isdir = S_ISDIR(la->la_mode);
        islnk = S_ISLNK(la->la_mode);
        mdt_pack_attr2body(info, repbody, la, mdt_object_fid(o));

	/* compatibility check for 2.10 clients when it tries to open mirrored
	 * files. 2.10 clients don't verify overlapping components so they
	 * would read and write mirrored files just as if they were normal
	 * PFL files, which will cause the problem that sycned mirrors actually
	 * contain different data.
	 * Older clients are not a concern here because they don't even
	 * understand PFL layout. */
	if (isreg && !exp_connect_flr(exp) && ma->ma_valid & MA_LOV &&
	    mdt_lmm_is_flr(ma->ma_lmm)) {
		/* LU-10286: for simplicity clients who don't understand
		 * mirrored layout(with connect flag OBD_CONNECT2_FLR) won't
		 * be able to open mirrored files */
		RETURN(-EOPNOTSUPP);
	}

	/* Overstriped files can crash older clients */
	if (isreg && !exp_connect_overstriping(exp) &&
	    mdt_lmm_is_overstriping(ma->ma_lmm))
		RETURN(-EOPNOTSUPP);

	/* LU-2275, simulate broken behaviour (esp. prevalent in
	 * pre-2.4 servers where a very strange reply is sent on error
	 * that looks like it was actually almost successful and a
	 * failure at the same time.) */
	if (OBD_FAIL_CHECK(OBD_FAIL_MDS_NEGATIVE_POSITIVE)) {
		mdt_set_disposition(info, rep, DISP_OPEN_OPEN |
					       DISP_LOOKUP_NEG |
					       DISP_LOOKUP_POS);

		if (open_flags & MDS_OPEN_LOCK)
			mdt_set_disposition(info, rep, DISP_OPEN_LOCK);

		RETURN(-ENOENT);
	}

#ifdef CONFIG_LUSTRE_FS_POSIX_ACL
	if (exp_connect_flags(exp) & OBD_CONNECT_ACL) {
		struct lu_nodemap *nodemap = nodemap_get_from_exp(exp);
		if (IS_ERR(nodemap))
			RETURN(PTR_ERR(nodemap));

		rc = mdt_pack_acl2body(info, repbody, o, nodemap);
		nodemap_putref(nodemap);
		if (rc)
			RETURN(rc);
	}
#endif

	/*
	 * If we are following a symlink, don't open; and do not return open
	 * handle for special nodes as client required.
	 */
	if (islnk || (!isreg && !isdir &&
	    (exp_connect_flags(req->rq_export) & OBD_CONNECT_NODEVOH))) {
		lustre_msg_set_transno(req->rq_repmsg, 0);
		RETURN(0);
	}

	/*
	 * We need to return the existing object's fid back, so it is done here,
	 * after preparing the reply.
	 */
	if (!created && (open_flags & MDS_OPEN_EXCL) &&
	    (open_flags & MDS_OPEN_CREAT))
		RETURN(-EEXIST);

	/* This can't be done earlier, we need to return reply body */
	if (isdir) {
		if (open_flags & (MDS_OPEN_CREAT | MDS_FMODE_WRITE)) {
			/* We are trying to create or write an existing dir. */
			RETURN(-EISDIR);
		}
	} else if (open_flags & MDS_OPEN_DIRECTORY)
		RETURN(-ENOTDIR);

	if (OBD_FAIL_CHECK_RESET(OBD_FAIL_MDS_OPEN_CREATE,
				 OBD_FAIL_MDS_LDLM_REPLY_NET | OBD_FAIL_ONCE))
		RETURN(-EAGAIN);

	mfd = NULL;
	if (lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT) {
		spin_lock(&med->med_open_lock);
		list_for_each(t, &med->med_open_head) {
			mfd = list_entry(t, struct mdt_file_data, mfd_list);
			if (mfd->mfd_xid == req->rq_xid)
				break;
			mfd = NULL;
		}
		spin_unlock(&med->med_open_lock);

		if (mfd != NULL) {
			repbody->mbo_open_handle.cookie =
				mfd->mfd_open_handle.h_cookie;
			/* set repbody->ea_size for resent case */
			if (ma->ma_valid & MA_LOV) {
				LASSERT(ma->ma_lmm_size != 0);
				repbody->mbo_eadatasize = ma->ma_lmm_size;
				if (isdir)
					repbody->mbo_valid |= OBD_MD_FLDIREA;
				else
					repbody->mbo_valid |= OBD_MD_FLEASIZE;
                        }
			mdt_set_disposition(info, rep, DISP_OPEN_OPEN);
			RETURN(0);
		}
	}

	rc = mdt_mfd_open(info, p, o, open_flags, created, rep);
	if (!rc)
		mdt_set_disposition(info, rep, DISP_OPEN_OPEN);

	RETURN(rc);
}

void mdt_reconstruct_open(struct mdt_thread_info *info,
			  struct mdt_lock_handle *lhc)
{
	const struct lu_env *env = info->mti_env;
	struct mdt_device *mdt = info->mti_mdt;
	struct req_capsule *pill = info->mti_pill;
	struct ptlrpc_request *req = mdt_info_req(info);
	struct md_attr *ma = &info->mti_attr;
	struct mdt_reint_record *rr = &info->mti_rr;
	u64 open_flags = info->mti_spec.sp_cr_flags;
	struct ldlm_reply *ldlm_rep;
	struct mdt_object *parent;
	struct mdt_object *child;
	struct mdt_body *repbody;
	u64 opdata;
	int rc;
	ENTRY;

	LASSERT(pill->rc_fmt == &RQF_LDLM_INTENT_OPEN);
	ldlm_rep = req_capsule_server_get(pill, &RMF_DLM_REP);
	repbody = req_capsule_server_get(pill, &RMF_MDT_BODY);

	ma->ma_need = MA_INODE | MA_HSM;
	ma->ma_valid = 0;

	opdata = mdt_req_from_lrd(req, info->mti_reply_data);
	mdt_set_disposition(info, ldlm_rep, opdata);

	CDEBUG(D_INODE, "This is reconstruct open: disp=%#llx, result=%d\n",
               ldlm_rep->lock_policy_res1, req->rq_status);

        if (mdt_get_disposition(ldlm_rep, DISP_OPEN_CREATE) &&
            req->rq_status != 0)
                /* We did not create successfully, return error to client. */
                GOTO(out, rc = req->rq_status);

        if (mdt_get_disposition(ldlm_rep, DISP_OPEN_CREATE)) {
                struct obd_export *exp = req->rq_export;
                /*
                 * We failed after creation, but we do not know in which step
                 * we failed. So try to check the child object.
                 */
                parent = mdt_object_find(env, mdt, rr->rr_fid1);
                if (IS_ERR(parent)) {
                        rc = PTR_ERR(parent);
                        LCONSOLE_WARN("Parent "DFID" lookup error %d."
                                      " Evicting client %s with export %s.\n",
                                      PFID(rr->rr_fid1), rc,
                                      obd_uuid2str(&exp->exp_client_uuid),
                                      obd_export_nid2str(exp));
                        mdt_export_evict(exp);
                        RETURN_EXIT;
                }

		child = mdt_object_find(env, mdt, rr->rr_fid2);
		if (IS_ERR(child)) {
			rc = PTR_ERR(child);
			LCONSOLE_WARN("cannot lookup child "DFID": rc = %d; "
				      "evicting client %s with export %s\n",
				      PFID(rr->rr_fid2), rc,
				      obd_uuid2str(&exp->exp_client_uuid),
				      obd_export_nid2str(exp));
			mdt_object_put(env, parent);
			mdt_export_evict(exp);
			RETURN_EXIT;
		}

		if (unlikely(mdt_object_remote(child))) {
			mdt_object_put(env, parent);
			mdt_object_put(env, child);
			/* the child object was created on remote server */
			if (!mdt_is_dne_client(exp))
				/* Return -EIO for old client */
				GOTO(out, rc = -EIO);
			repbody->mbo_fid1 = *rr->rr_fid2;
			repbody->mbo_valid |= (OBD_MD_FLID | OBD_MD_MDS);
			GOTO(out, rc = 0);
		}
		if (mdt_object_exists(child)) {
			mdt_prep_ma_buf_from_rep(info, child, ma);
			rc = mdt_attr_get_complex(info, child, ma);
			if (!rc)
				rc = mdt_finish_open(info, parent, child,
						     open_flags, 1, ldlm_rep);
			mdt_object_put(env, parent);
			mdt_object_put(env, child);
			if (!rc)
				mdt_pack_size2body(info, rr->rr_fid2,
						   &lhc->mlh_reg_lh);
			GOTO(out, rc);
		}
		/* the child does not exist, we should do regular open */
		mdt_object_put(env, parent);
		mdt_object_put(env, child);
	}
	/* We did not try to create, so we are a pure open */
	rc = mdt_reint_open(info, lhc);
	EXIT;
out:
	req->rq_status = rc;
	lustre_msg_set_status(req->rq_repmsg, req->rq_status);
	LASSERT(ergo(rc < 0, lustre_msg_get_transno(req->rq_repmsg) == 0));
}

static int mdt_open_by_fid(struct mdt_thread_info *info, struct ldlm_reply *rep)
{
	u64 open_flags = info->mti_spec.sp_cr_flags;
	struct mdt_reint_record *rr = &info->mti_rr;
	struct md_attr *ma = &info->mti_attr;
	struct mdt_object *o;
	int rc;

	ENTRY;
	o = mdt_object_find(info->mti_env, info->mti_mdt, rr->rr_fid2);
	if (IS_ERR(o))
		RETURN(rc = PTR_ERR(o));

	if (unlikely(mdt_object_remote(o))) {
		/* the child object was created on remote server */
		struct mdt_body *repbody;

		mdt_set_disposition(info, rep, (DISP_IT_EXECD |
						DISP_LOOKUP_EXECD |
						DISP_LOOKUP_POS));
		repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);
		repbody->mbo_fid1 = *rr->rr_fid2;
		repbody->mbo_valid |= (OBD_MD_FLID | OBD_MD_MDS);
		rc = 0;
	} else {
		if (mdt_object_exists(o)) {
			mdt_set_disposition(info, rep, (DISP_IT_EXECD |
							DISP_LOOKUP_EXECD |
							DISP_LOOKUP_POS));
			mdt_prep_ma_buf_from_rep(info, o, ma);
			rc = mdt_attr_get_complex(info, o, ma);
			if (rc == 0)
				rc = mdt_finish_open(info, NULL, o, open_flags,
						     0, rep);
		} else {
			rc = -ENOENT;
		}
	}

	mdt_object_put(info->mti_env, o);
	RETURN(rc);
}

/* lock object for open */
static int mdt_object_open_lock(struct mdt_thread_info *info,
				struct mdt_object *obj,
				struct mdt_lock_handle *lhc,
				__u64 *ibits)
{
	struct md_attr *ma = &info->mti_attr;
	__u64 open_flags = info->mti_spec.sp_cr_flags;
	__u64 trybits = 0;
	enum ldlm_mode lm = LCK_CR;
	bool acq_lease = !!(open_flags & MDS_OPEN_LEASE);
	bool try_layout = false;
	bool create_layout = false;
	int rc = 0;
	__u32 dom_stripe = 0;
	unsigned int dom_only = 0;
	unsigned int dom_lock = 0;

	ENTRY;

	*ibits = 0;
	mdt_lock_handle_init(lhc);

	if (req_is_replay(mdt_info_req(info)))
		RETURN(0);

	if (S_ISREG(lu_object_attr(&obj->mot_obj))) {
		if (ma->ma_need & MA_LOV && !(ma->ma_valid & MA_LOV) &&
		    md_should_create(open_flags))
			create_layout = true;
		if (exp_connect_layout(info->mti_exp) && !create_layout &&
		    ma->ma_need & MA_LOV)
			try_layout = true;

		/* DoM files can take IO lock at OPEN when it makes sense,
		 * check if file has DoM stripe and ask for lock if client
		 * no lock on that resource yet.
		 */
		if (ma->ma_valid & MA_LOV && ma->ma_lmm != NULL)
			dom_stripe = mdt_lmm_dom_entry_check(ma->ma_lmm,
							     &dom_only);
		/* If only DOM stripe is being used then we can expect IO
		 * to it after OPEN and will return corresponding DOM ibit
		 * using default strategy from mdt_opts.mo_dom_lock.
		 * Otherwise trylock mode is used always and DOM ibit will
		 * be returned optionally.
		 */
		if (dom_stripe &&
		    !mdt_dom_client_has_lock(info, mdt_object_fid(obj)))
			dom_lock = !dom_only ? TRYLOCK_DOM_ON_OPEN :
				   info->mti_mdt->mdt_opts.mo_dom_lock;
	}

	if (acq_lease) {
		/* lease open, acquire write mode of open sem */
		down_write(&obj->mot_open_sem);

		/* Lease exists and ask for new lease */
		if (atomic_read(&obj->mot_lease_count) > 0) {
			/* only exclusive open is supported, so lease
			 * are conflicted to each other */
			GOTO(out, rc = -EBUSY);
		}

		/* Lease must be with open lock */
		if (!(open_flags & MDS_OPEN_LOCK)) {
			CERROR("%s: Request lease for file:"DFID ", but open lock "
			       "is missed, open_flags = %#llo : rc = %d\n",
			       mdt_obd_name(info->mti_mdt),
			       PFID(mdt_object_fid(obj)), open_flags, -EPROTO);
			GOTO(out, rc = -EPROTO);
		}

		/* XXX: only exclusive open is supported. */
		lm = LCK_EX;
		*ibits = MDS_INODELOCK_OPEN;

		/* never grant LCK_EX layout lock to client */
		try_layout = false;
	} else { /* normal open */
		/* normal open holds read mode of open sem */
		down_read(&obj->mot_open_sem);

		if (open_flags & MDS_OPEN_LOCK) {
			if (open_flags & MDS_FMODE_WRITE)
				lm = LCK_CW;
			else if (open_flags & MDS_FMODE_EXEC)
				lm = LCK_PR;
			else
				lm = LCK_CR;

			*ibits = MDS_INODELOCK_LOOKUP | MDS_INODELOCK_OPEN;
		} else if (atomic_read(&obj->mot_lease_count) > 0) {
			if (open_flags & MDS_FMODE_WRITE)
				lm = LCK_CW;
			else
				lm = LCK_CR;

			/* revoke lease */
			*ibits = MDS_INODELOCK_OPEN;
			try_layout = false;

			lhc = &info->mti_lh[MDT_LH_LOCAL];
		} else if (dom_lock) {
			lm = (open_flags & MDS_FMODE_WRITE) ? LCK_PW : LCK_PR;
			trybits |= MDS_INODELOCK_DOM | MDS_INODELOCK_LAYOUT;
		}

		CDEBUG(D_INODE, "normal open:"DFID" lease count: %d, lm: %d\n",
			PFID(mdt_object_fid(obj)),
			atomic_read(&obj->mot_lease_count), lm);
	}

	mdt_lock_reg_init(lhc, lm);

	/* Return lookup lock to validate inode at the client side.
	 * This is pretty important otherwise MDT will return layout
	 * lock for each open.
	 * However this is a double-edged sword because changing
	 * permission will revoke a huge number of LOOKUP locks.
	 */
	if (!OBD_FAIL_CHECK(OBD_FAIL_MDS_NO_LL_OPEN) && try_layout) {
		if (!(*ibits & MDS_INODELOCK_LOOKUP))
			trybits |= MDS_INODELOCK_LOOKUP;
		trybits |= MDS_INODELOCK_LAYOUT;
	}

	if (*ibits | trybits)
		rc = mdt_object_lock_try(info, obj, lhc, ibits, trybits, false);

	CDEBUG(D_INODE, "%s: Requested bits lock:"DFID ", ibits = %#llx/%#llx"
	       ", open_flags = %#llo, try_layout = %d : rc = %d\n",
	       mdt_obd_name(info->mti_mdt), PFID(mdt_object_fid(obj)),
	       *ibits, trybits, open_flags, try_layout, rc);

	/* will change layout, revoke layout locks by enqueuing EX lock. */
	if (rc == 0 && create_layout) {
		struct mdt_lock_handle *ll = &info->mti_lh[MDT_LH_LAYOUT];

		CDEBUG(D_INODE, "Will create layout, get EX layout lock:"DFID
			", open_flags = %#llo\n",
			PFID(mdt_object_fid(obj)), open_flags);

		/* We cannot enqueue another lock for the same resource we
		 * already have a lock for, due to mechanics of waiting list
		 * iterating in ldlm, see LU-3601.
		 * As such we'll drop the open lock we just got above here,
		 * it's ok not to have this open lock as it's main purpose is to
		 * flush unused cached client open handles. */
		if (lustre_handle_is_used(&lhc->mlh_reg_lh))
			mdt_object_unlock(info, obj, lhc, 1);

		LASSERT(!try_layout);
		mdt_lock_handle_init(ll);
		mdt_lock_reg_init(ll, LCK_EX);
		rc = mdt_object_lock(info, obj, ll, MDS_INODELOCK_LAYOUT);

		OBD_FAIL_TIMEOUT(OBD_FAIL_MDS_LL_BLOCK, 2);
	}

	/* Check if there is any other open handles after acquiring
	 * open lock. At this point, caching open handles have been revoked
	 * by open lock.
	 * XXX: Now only exclusive open is supported. Need to check the
	 * type of open for generic lease support. */
	if (rc == 0 && acq_lease) {
		struct ptlrpc_request *req = mdt_info_req(info);
		struct mdt_export_data *med = &req->rq_export->exp_mdt_data;
		struct mdt_file_data *mfd;
		bool is_replay_or_resent;
		int open_count = 0;

		/* For lease: application can open a file and then apply lease,
		 * @handle contains original open handle in that case.
		 * In recovery, open REQ will be replayed and the lease REQ may
		 * be resent that means the open handle is already stale, so we
		 * need to fix it up here by finding new handle. */
		is_replay_or_resent = req_is_replay(req) ||
			lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT;

		/* if the request is _not_ a replay request, rr_open_handle
		 * may be used to hold an open file handle which is issuing the
		 * lease request, so that this openhandle doesn't count. */
		mfd = mdt_open_handle2mfd(med, info->mti_rr.rr_open_handle,
					  is_replay_or_resent);
		if (mfd != NULL)
			++open_count;

		CDEBUG(D_INODE, "acq_lease "DFID": openers: %d, want: %d\n",
			PFID(mdt_object_fid(obj)),
			atomic_read(&obj->mot_open_count), open_count);

		if (atomic_read(&obj->mot_open_count) > open_count)
			GOTO(out, rc = -EBUSY);
	}
	GOTO(out, rc);

out:
	RETURN(rc);
}

static void mdt_object_open_unlock(struct mdt_thread_info *info,
				   struct mdt_object *obj,
				   struct mdt_lock_handle *lhc,
				   __u64 ibits, int rc)
{
	__u64 open_flags = info->mti_spec.sp_cr_flags;
	struct mdt_lock_handle *ll = &info->mti_lh[MDT_LH_LOCAL];
	ENTRY;

	if (req_is_replay(mdt_info_req(info)))
		RETURN_EXIT;

	/* Release local lock - the lock put in MDT_LH_LOCAL will never
	 * return to client side. */
	if (lustre_handle_is_used(&ll->mlh_reg_lh))
		mdt_object_unlock(info, obj, ll, 1);

	ll = &info->mti_lh[MDT_LH_LAYOUT];
	/* Release local layout lock, layout was created */
	if (lustre_handle_is_used(&ll->mlh_reg_lh)) {
		LASSERT(!(ibits & MDS_INODELOCK_LAYOUT));
		mdt_object_unlock(info, obj, ll, 1);
	}

	if (open_flags & MDS_OPEN_LEASE)
		up_write(&obj->mot_open_sem);
	else
		up_read(&obj->mot_open_sem);

	/* Cross-ref case, the lock should be returned to the client */
	if (ibits == 0 || rc == -MDT_EREMOTE_OPEN)
		RETURN_EXIT;

	if (!(open_flags & MDS_OPEN_LOCK) && !(ibits & MDS_INODELOCK_LAYOUT) &&
	    !(ibits & MDS_INODELOCK_DOM)) {
		/* for the open request, the lock will only return to client
		 * if open or layout lock is granted. */
		rc = 1;
	}

	if (rc != 0 || !lustre_handle_is_used(&lhc->mlh_reg_lh)) {
		struct ldlm_reply       *ldlm_rep;

		LASSERT(!info->mti_parent_locked);

		ldlm_rep = req_capsule_server_get(info->mti_pill, &RMF_DLM_REP);
		mdt_clear_disposition(info, ldlm_rep, DISP_OPEN_LOCK);
		if (lustre_handle_is_used(&lhc->mlh_reg_lh))
			mdt_object_unlock(info, obj, lhc, 1);
	}
	RETURN_EXIT;
}

/**
 * Check release is permitted for the current HSM flags.
 */
static bool mdt_hsm_release_allow(const struct md_attr *ma)
{
	if (!(ma->ma_valid & MA_HSM))
		return false;

	if (ma->ma_hsm.mh_flags & (HS_DIRTY|HS_NORELEASE|HS_LOST))
		return false;

	if (!(ma->ma_hsm.mh_flags & HS_ARCHIVED))
		return false;

	return true;
}

static int mdt_open_by_fid_lock(struct mdt_thread_info *info,
				struct ldlm_reply *rep,
				struct mdt_lock_handle *lhc)
{
	const struct lu_env *env = info->mti_env;
	struct mdt_device *mdt = info->mti_mdt;
	u64 open_flags = info->mti_spec.sp_cr_flags;
	struct mdt_reint_record *rr = &info->mti_rr;
	struct md_attr *ma = &info->mti_attr;
	struct mdt_object *parent = NULL;
	struct mdt_object *o;
	bool object_locked = false;
	u64 ibits = 0;
	int rc;

	ENTRY;
	if (md_should_create(open_flags)) {
		if (!lu_fid_eq(rr->rr_fid1, rr->rr_fid2)) {
			parent = mdt_object_find(env, mdt, rr->rr_fid1);
			if (IS_ERR(parent)) {
				CDEBUG(D_INODE, "Fail to find parent "DFID
				       " for anonymous created %ld, try to"
				       " use server-side parent.\n",
				       PFID(rr->rr_fid1), PTR_ERR(parent));
				parent = NULL;
			}
		}
		if (parent == NULL)
			ma->ma_need |= MA_PFID;
	}

	o = mdt_object_find(env, mdt, rr->rr_fid2);
	if (IS_ERR(o))
		GOTO(out_parent_put, rc = PTR_ERR(o));

	if (mdt_object_remote(o)) {
		CDEBUG(D_INFO, "%s: "DFID" is on remote MDT.\n",
		       mdt_obd_name(info->mti_mdt),
		       PFID(rr->rr_fid2));
		GOTO(out, rc = -EREMOTE);
	} else if (!mdt_object_exists(o)) {
		mdt_set_disposition(info, rep,
				    DISP_IT_EXECD |
				    DISP_LOOKUP_EXECD |
				    DISP_LOOKUP_NEG);
		GOTO(out, rc = -ENOENT);
	}

	mdt_set_disposition(info, rep, (DISP_IT_EXECD | DISP_LOOKUP_EXECD));

	mdt_prep_ma_buf_from_rep(info, o, ma);
	if (open_flags & MDS_OPEN_RELEASE)
		ma->ma_need |= MA_HSM;
	rc = mdt_attr_get_complex(info, o, ma);
	if (rc)
		GOTO(out, rc);

	/* We should not change file's existing LOV EA */
	if (S_ISREG(lu_object_attr(&o->mot_obj)) &&
	    open_flags & MDS_OPEN_HAS_EA && ma->ma_valid & MA_LOV)
		GOTO(out, rc = -EEXIST);

	/* If a release request, check file open flags are fine and ask for an
	 * exclusive open access. */
	if (open_flags & MDS_OPEN_RELEASE && !mdt_hsm_release_allow(ma))
		GOTO(out, rc = -EPERM);

	rc = mdt_check_resent_lock(info, o, lhc);
	if (rc < 0) {
		GOTO(out, rc);
	} else if (rc > 0) {
		rc = mdt_object_open_lock(info, o, lhc, &ibits);
		object_locked = true;
		if (rc)
			GOTO(out_unlock, rc);
	}

	if (ma->ma_valid & MA_PFID) {
		parent = mdt_object_find(env, mdt, &ma->ma_pfid);
		if (IS_ERR(parent)) {
			CDEBUG(D_INODE, "Fail to find parent "DFID
			       " for anonymous created %ld, try to"
			       " use system default.\n",
			       PFID(&ma->ma_pfid), PTR_ERR(parent));
			parent = NULL;
		}
	}

	rc = mdt_finish_open(info, parent, o, open_flags, 0, rep);
	if (!rc) {
		mdt_set_disposition(info, rep, DISP_LOOKUP_POS);
		if (open_flags & MDS_OPEN_LOCK)
			mdt_set_disposition(info, rep, DISP_OPEN_LOCK);
		if (open_flags & MDS_OPEN_LEASE)
			mdt_set_disposition(info, rep, DISP_OPEN_LEASE);
	}
	GOTO(out_unlock, rc);

out_unlock:
	if (object_locked)
		mdt_object_open_unlock(info, o, lhc, ibits, rc);
out:
	mdt_object_put(env, o);
	if (rc == 0)
		mdt_pack_size2body(info, rr->rr_fid2, &lhc->mlh_reg_lh);
out_parent_put:
	if (parent != NULL)
		mdt_object_put(env, parent);
	return rc;
}

/* Cross-ref request. Currently it can only be a pure open (w/o create) */
static int mdt_cross_open(struct mdt_thread_info *info,
			  const struct lu_fid *parent_fid,
			  const struct lu_fid *fid,
			  struct ldlm_reply *rep, u64 open_flags)
{
	struct md_attr *ma = &info->mti_attr;
	struct mdt_object *o;
	int rc;
	ENTRY;

	o = mdt_object_find(info->mti_env, info->mti_mdt, fid);
	if (IS_ERR(o))
		RETURN(rc = PTR_ERR(o));

	if (mdt_object_remote(o)) {
		/* Something is wrong here, the object is on another MDS! */
		CERROR("%s: "DFID" isn't on this server!: rc = %d\n",
		       mdt_obd_name(info->mti_mdt), PFID(fid), -EFAULT);
		LU_OBJECT_DEBUG(D_WARNING, info->mti_env,
				&o->mot_obj,
				"Object isn't on this server! FLD error?");
		rc = -EFAULT;
	} else {
		if (mdt_object_exists(o)) {
			int mask;

			/* Do permission check for cross-open after converting
			 * MDS_OPEN_* flags to MAY_* permission mask.
			 */
			mask = mds_accmode(open_flags);

			rc = mo_permission(info->mti_env, NULL,
					   mdt_object_child(o), NULL, mask);
			if (rc)
				goto out;

			mdt_prep_ma_buf_from_rep(info, o, ma);
			rc = mdt_attr_get_complex(info, o, ma);
			if (rc != 0)
				GOTO(out, rc);

			rc = mdt_pack_secctx_in_reply(info, o);
			if (unlikely(rc))
				GOTO(out, rc);

			rc = mdt_pack_encctx_in_reply(info, o);
			if (unlikely(rc))
				GOTO(out, rc);

			rc = mdt_finish_open(info, NULL, o, open_flags, 0, rep);
		} else {
			/*
			 * Something is wrong here. lookup was positive but
			 * there is no object!
			 */
			CERROR("%s: "DFID" doesn't exist!: rc = %d\n",
			      mdt_obd_name(info->mti_mdt), PFID(fid), -EFAULT);
			rc = -EFAULT;
		}
	}
out:
	mdt_object_put(info->mti_env, o);

	RETURN(rc);
}

/*
 * find root object and take its xattr lock if it's on remote MDT, later create
 * may use fs default striping (which is stored in root xattr).
 */
static int mdt_lock_root_xattr(struct mdt_thread_info *info,
			       struct mdt_device *mdt)
{
	struct mdt_object *md_root = mdt->mdt_md_root;
	struct lustre_handle lhroot;
	int rc;

	if (md_root == NULL) {
		lu_root_fid(&info->mti_tmp_fid1);
		md_root = mdt_object_find(info->mti_env, mdt,
					  &info->mti_tmp_fid1);
		if (IS_ERR(md_root))
			return PTR_ERR(md_root);

		spin_lock(&mdt->mdt_lock);
		if (mdt->mdt_md_root != NULL) {
			spin_unlock(&mdt->mdt_lock);

			LASSERTF(mdt->mdt_md_root == md_root,
				 "Different root object ("
				 DFID") instances, %p, %p\n",
				 PFID(&info->mti_tmp_fid1),
				 mdt->mdt_md_root, md_root);
			LASSERT(atomic_read(
				&md_root->mot_obj.lo_header->loh_ref) > 1);

			mdt_object_put(info->mti_env, md_root);
		} else {
			mdt->mdt_md_root = md_root;
			spin_unlock(&mdt->mdt_lock);
		}
	}

	if (md_root->mot_cache_attr || !mdt_object_remote(md_root))
		return 0;

	rc = mdt_remote_object_lock(info, md_root, mdt_object_fid(md_root),
				    &lhroot, LCK_PR, MDS_INODELOCK_XATTR,
				    true);
	if (rc < 0)
		return rc;

	md_root->mot_cache_attr = 1;

	/* don't cancel this lock, so that we know the cached xattr is valid. */
	ldlm_lock_decref(&lhroot, LCK_PR);

	return 0;
}

int mdt_reint_open(struct mdt_thread_info *info, struct mdt_lock_handle *lhc)
{
	struct mdt_device *mdt = info->mti_mdt;
	struct ptlrpc_request *req = mdt_info_req(info);
	struct mdt_object *parent;
	struct mdt_object *child;
	struct mdt_lock_handle *lh;
	struct ldlm_reply *ldlm_rep;
	struct mdt_body *repbody;
	struct lu_fid *child_fid = &info->mti_tmp_fid1;
	struct md_attr *ma = &info->mti_attr;
	u64 open_flags = info->mti_spec.sp_cr_flags;
	u64 ibits = 0;
	struct mdt_reint_record *rr = &info->mti_rr;
	int result, rc;
	int created = 0;
	int object_locked = 0;
	enum ldlm_mode lock_mode = LCK_PR;
	u32 msg_flags;
	ktime_t kstart = ktime_get();

	ENTRY;
	OBD_FAIL_TIMEOUT_ORSET(OBD_FAIL_MDS_PAUSE_OPEN, OBD_FAIL_ONCE,
			       (obd_timeout + 1) / 4);

	repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);

	ma->ma_need = MA_INODE;
	ma->ma_valid = 0;

	LASSERT(info->mti_pill->rc_fmt == &RQF_LDLM_INTENT_OPEN &&
		!info->mti_parent_locked);
	ldlm_rep = req_capsule_server_get(info->mti_pill, &RMF_DLM_REP);

	if (unlikely(open_flags & MDS_OPEN_JOIN_FILE)) {
		CERROR("file join is not supported anymore.\n");
		GOTO(out, result = err_serious(-EOPNOTSUPP));
	}
	msg_flags = lustre_msg_get_flags(req->rq_reqmsg);

	if ((open_flags & (MDS_OPEN_HAS_EA | MDS_OPEN_HAS_OBJS)) &&
	    info->mti_spec.u.sp_ea.eadata == NULL)
		GOTO(out, result = err_serious(-EINVAL));

	if (open_flags & MDS_FMODE_WRITE &&
	    exp_connect_flags(req->rq_export) & OBD_CONNECT_RDONLY)
		GOTO(out, result = -EROFS);

	CDEBUG(D_INODE, "I am going to open "DFID"/("DNAME"->"DFID") "
	       "cr_flag=%#llo mode=0%06o msg_flag=0x%x\n",
	       PFID(rr->rr_fid1), PNAME(&rr->rr_name), PFID(rr->rr_fid2),
	       open_flags, ma->ma_attr.la_mode, msg_flags);

	if (info->mti_cross_ref) {
		/* This is cross-ref open */
		mdt_set_disposition(info, ldlm_rep,
			    (DISP_IT_EXECD | DISP_LOOKUP_EXECD |
			     DISP_LOOKUP_POS));
		result = mdt_cross_open(info, rr->rr_fid2, rr->rr_fid1,
					ldlm_rep, open_flags);
		GOTO(out, result);
	} else if (req_is_replay(req)) {
		result = mdt_open_by_fid(info, ldlm_rep);

		if (result != -ENOENT)
			GOTO(out, result);

		/* We didn't find the correct object, so we need to re-create it
		 * via a regular replay. */
		if (!(open_flags & MDS_OPEN_CREAT)) {
			DEBUG_REQ(D_ERROR, req,
				  "OPEN & CREAT not in open replay/by_fid");
			GOTO(out, result = -EFAULT);
		}
		CDEBUG(D_INFO, "No object(1), continue as regular open.\n");
	} else if ((open_flags & MDS_OPEN_BY_FID)) {
		result = mdt_open_by_fid_lock(info, ldlm_rep, lhc);
		if (result < 0)
			CDEBUG(D_INFO, "no object for "DFID": %d\n",
			       PFID(rr->rr_fid2), result);
		GOTO(out, result);
	}

	if (OBD_FAIL_CHECK(OBD_FAIL_MDS_OPEN_PACK))
		GOTO(out, result = err_serious(-ENOMEM));

	mdt_set_disposition(info, ldlm_rep,
			    (DISP_IT_EXECD | DISP_LOOKUP_EXECD));

	if (!lu_name_is_valid(&rr->rr_name))
		GOTO(out, result = -EPROTO);

	result = mdt_lock_root_xattr(info, mdt);
	if (result < 0)
		GOTO(out, result);

	parent = mdt_object_find(info->mti_env, mdt, rr->rr_fid1);
	if (IS_ERR(parent))
		GOTO(out, result = PTR_ERR(parent));

	/* get and check version of parent */
	result = mdt_version_get_check(info, parent, 0);
	if (result) {
		mdt_object_put(info->mti_env, parent);
		GOTO(out, result);
	}

	OBD_RACE(OBD_FAIL_MDS_REINT_OPEN);
again_pw:
	lh = &info->mti_lh[MDT_LH_PARENT];
	mdt_lock_pdo_init(lh, lock_mode, &rr->rr_name);

	result = mdt_object_lock(info, parent, lh, MDS_INODELOCK_UPDATE);
	if (result != 0) {
		mdt_object_put(info->mti_env, parent);
		GOTO(out, result);
	}
	fid_zero(child_fid);

	result = -ENOENT;
	if ((open_flags & MDS_OPEN_VOLATILE) == 0)
		result = mdo_lookup(info->mti_env, mdt_object_child(parent),
				    &rr->rr_name, child_fid, &info->mti_spec);

	LASSERTF(ergo(result == 0, fid_is_sane(child_fid)),
		 "looking for "DFID"/"DNAME", found FID = "DFID"\n",
		 PFID(mdt_object_fid(parent)), PNAME(&rr->rr_name),
		 PFID(child_fid));

	if (result != 0 && result != -ENOENT)
		GOTO(out_parent, result);

	OBD_RACE(OBD_FAIL_MDS_REINT_OPEN2);

	if (result == -ENOENT) {
		mdt_set_disposition(info, ldlm_rep, DISP_LOOKUP_NEG);
		if (!(open_flags & MDS_OPEN_CREAT))
			GOTO(out_parent, result);
		if (mdt_rdonly(req->rq_export))
			GOTO(out_parent, result = -EROFS);

		if (lock_mode == LCK_PR) {
			/* first pass: get write lock and restart */
			mdt_object_unlock(info, parent, lh, 1);
			mdt_clear_disposition(info, ldlm_rep, DISP_LOOKUP_NEG);
			mdt_lock_handle_init(lh);
			lock_mode = LCK_PW;
			goto again_pw;
		}

		*child_fid = *info->mti_rr.rr_fid2;
		LASSERTF(fid_is_sane(child_fid), "fid="DFID"\n",
			 PFID(child_fid));
		/* In the function below, .hs_keycmp resolves to
		 * lu_obj_hop_keycmp() */
		/* coverity[overrun-buffer-val] */
		child = mdt_object_new(info->mti_env, mdt, child_fid);
	} else {
		/*
		 * Check for O_EXCL is moved to the mdt_finish_open(),
		 * we need to return FID back in that case.
		 */
		mdt_set_disposition(info, ldlm_rep, DISP_LOOKUP_POS);
		child = mdt_object_find(info->mti_env, mdt, child_fid);
	}
	if (IS_ERR(child))
		GOTO(out_parent, result = PTR_ERR(child));

	/** check version of child  */
	rc = mdt_version_get_check(info, child, 1);
	if (rc)
		GOTO(out_child, result = rc);

	if (result == -ENOENT) {
		/* Create under OBF and .lustre is not permitted */
		if (!fid_is_md_operative(rr->rr_fid1) &&
		    (open_flags & MDS_OPEN_VOLATILE) == 0)
			GOTO(out_child, result = -EPERM);

		/* save versions in reply */
		mdt_version_get_save(info, parent, 0);
		mdt_version_get_save(info, child, 1);

		/* version of child will be changed */
		tgt_vbr_obj_set(info->mti_env, mdt_obj2dt(child));

		/* Not found and with MDS_OPEN_CREAT: let's create it. */
		mdt_set_disposition(info, ldlm_rep, DISP_OPEN_CREATE);

		/* Let lower layers know what is lock mode on directory. */
		info->mti_spec.sp_cr_mode =
			mdt_dlm_mode2mdl_mode(lh->mlh_pdo_mode);

		/* Don't do lookup sanity check. We know name doesn't exist. */
		info->mti_spec.sp_cr_lookup = 0;
		info->mti_spec.sp_feat = &dt_directory_features;

		result = mdo_create(info->mti_env, mdt_object_child(parent),
				    &rr->rr_name, mdt_object_child(child),
				    &info->mti_spec, &info->mti_attr);
		if (result == -ERESTART) {
			mdt_clear_disposition(info, ldlm_rep, DISP_OPEN_CREATE);
			GOTO(out_child, result);
		} else {
			mdt_prep_ma_buf_from_rep(info, child, ma);
			/* XXX: we should call this once, see few lines below */
			if (result == 0)
				result = mdt_attr_get_complex(info, child, ma);

                        if (result != 0)
                                GOTO(out_child, result);
                }
		created = 1;
		mdt_counter_incr(req, LPROC_MDT_MKNOD,
				 ktime_us_delta(ktime_get(), kstart));
        } else {
                /*
                 * The object is on remote node, return its FID for remote open.
                 */
		if (mdt_object_remote(child)) {
                        /*
                         * Check if this lock already was sent to client and
                         * this is resent case. For resent case do not take lock
                         * again, use what is already granted.
                         */
                        LASSERT(lhc != NULL);

			rc = mdt_check_resent_lock(info, child, lhc);
			if (rc < 0) {
				GOTO(out_child, result = rc);
			} else if (rc > 0) {
				mdt_lock_handle_init(lhc);
				mdt_lock_reg_init(lhc, LCK_PR);

				rc = mdt_object_lock(info, child, lhc,
						     MDS_INODELOCK_LOOKUP);
			}
			repbody->mbo_fid1 = *mdt_object_fid(child);
			repbody->mbo_valid |= (OBD_MD_FLID | OBD_MD_MDS);
                        if (rc != 0)
                                result = rc;
			else
				result = -MDT_EREMOTE_OPEN;
                        GOTO(out_child, result);
		} else if (mdt_object_exists(child)) {
			/* Check early for MDS_OPEN_DIRECTORY/O_DIRECTORY to
			 * avoid opening regular files from lfs getstripe
			 * since doing so breaks the leases used by lfs
			 * mirror. See LU-13693. */
			if (open_flags & MDS_OPEN_DIRECTORY &&
			    S_ISREG(lu_object_attr(&child->mot_obj)))
				GOTO(out_child, result = -ENOTDIR);

			/* We have to get attr & LOV EA & HSM for this
			 * object. */
			mdt_prep_ma_buf_from_rep(info, child, ma);
			ma->ma_need |= MA_HSM;
			result = mdt_attr_get_complex(info, child, ma);
			if (result != 0)
				GOTO(out_child, result);
		} else {
			/* Object does not exist. Likely FS corruption. */
			CERROR("%s: name '"DNAME"' present, but FID "
			       DFID" is invalid\n", mdt_obd_name(info->mti_mdt),
			       PNAME(&rr->rr_name), PFID(child_fid));
			GOTO(out_child, result = -EIO);
		}
	}

	repbody->mbo_max_mdsize = info->mti_mdt->mdt_max_mdsize;
	repbody->mbo_valid |= OBD_MD_FLMODEASIZE;

	rc = mdt_pack_secctx_in_reply(info, child);
	if (unlikely(rc))
		GOTO(out_child, result = rc);

	rc = mdt_pack_encctx_in_reply(info, child);
	if (unlikely(rc))
		GOTO(out_child, result = rc);

	rc = mdt_check_resent_lock(info, child, lhc);
	if (rc < 0) {
		GOTO(out_child, result = rc);
	} else if (rc == 0) {
		/* the open lock might already be gotten in
		 * ldlm_handle_enqueue() */
		LASSERT(lustre_msg_get_flags(req->rq_reqmsg) & MSG_RESENT);
		if (open_flags & MDS_OPEN_LOCK)
			mdt_set_disposition(info, ldlm_rep, DISP_OPEN_LOCK);
	} else {
		/* get openlock if this isn't replay and client requested it */
		if (!req_is_replay(req)) {
			rc = mdt_object_open_lock(info, child, lhc, &ibits);
			object_locked = 1;
			if (rc != 0)
				GOTO(out_child_unlock, result = rc);
			else if (open_flags & MDS_OPEN_LOCK)
				mdt_set_disposition(info, ldlm_rep,
						    DISP_OPEN_LOCK);
		}
	}
	/* Try to open it now. */
	rc = mdt_finish_open(info, parent, child, open_flags,
			     created, ldlm_rep);
	if (rc) {
		result = rc;
		/* openlock will be released if mdt_finish_open() failed */
		mdt_clear_disposition(info, ldlm_rep, DISP_OPEN_LOCK);

		if (created && (open_flags & MDS_OPEN_VOLATILE)) {
			CERROR("%s: cannot open volatile file "DFID", orphan "
			       "file will be left in PENDING directory until "
			       "next reboot, rc = %d\n", mdt_obd_name(mdt),
			       PFID(mdt_object_fid(child)), rc);
			GOTO(out_child_unlock, result);
		}

		if (created) {
			ma->ma_need = 0;
			ma->ma_valid = 0;
			rc = mdo_unlink(info->mti_env,
					mdt_object_child(parent),
					mdt_object_child(child),
					&rr->rr_name,
					&info->mti_attr, 0);
			if (rc != 0)
				CERROR("%s: "DFID" cleanup of open: rc = %d\n",
				       mdt_obd_name(info->mti_mdt),
				       PFID(mdt_object_fid(child)), rc);
			mdt_clear_disposition(info, ldlm_rep, DISP_OPEN_CREATE);
		}
	}

	mdt_counter_incr(req, LPROC_MDT_OPEN,
			 ktime_us_delta(ktime_get(), kstart));

	EXIT;
out_child_unlock:
	if (object_locked)
		mdt_object_open_unlock(info, child, lhc, ibits, result);
out_child:
	mdt_object_put(info->mti_env, child);
	if (result == 0)
		mdt_pack_size2body(info, child_fid, &lhc->mlh_reg_lh);
out_parent:
	mdt_object_unlock_put(info, parent, lh, result || !created);
out:
	if (result)
		lustre_msg_set_transno(req->rq_repmsg, 0);
	return result;
}

/**
 * Create an orphan object use local root.
 */
static struct mdt_object *mdt_orphan_open(struct mdt_thread_info *info,
					  struct mdt_device *mdt,
					  const struct lu_fid *fid,
					  struct md_attr *attr, fmode_t fmode)
{
	const struct lu_env *env = info->mti_env;
	struct md_op_spec *spec = &info->mti_spec;
	struct lu_fid *local_root_fid = &info->mti_tmp_fid1;
	struct mdt_object *obj = NULL;
	struct mdt_object *local_root;
	static const struct lu_name lname = {
		.ln_name = "i_am_nobody",
		.ln_namelen = sizeof("i_am_nobody") - 1,
	};
	struct lu_ucred *uc;
	cfs_cap_t uc_cap_save;
	int rc;
	ENTRY;

	rc = dt_root_get(env, mdt->mdt_bottom, local_root_fid);
	if (rc != 0)
		RETURN(ERR_PTR(rc));

	local_root = mdt_object_find(env, mdt, local_root_fid);
	if (IS_ERR(local_root))
		RETURN(local_root);

	obj = mdt_object_new(env, mdt, fid);
	if (IS_ERR(obj))
		GOTO(out, rc = PTR_ERR(obj));

	spec->sp_cr_lookup = 0;
	spec->sp_feat = &dt_directory_features;
	spec->sp_cr_mode = MDL_MINMODE; /* no lock */
	spec->sp_cr_flags = MDS_OPEN_VOLATILE | fmode;
	if (attr->ma_valid & MA_LOV) {
		spec->u.sp_ea.eadata = attr->ma_lmm;
		spec->u.sp_ea.eadatalen = attr->ma_lmm_size;
		spec->sp_cr_flags |= MDS_OPEN_HAS_EA;
	} else {
		spec->sp_cr_flags |= MDS_OPEN_DELAY_CREATE;
	}

	uc = lu_ucred(env);
	uc_cap_save = uc->uc_cap;
	uc->uc_cap |= BIT(CAP_DAC_OVERRIDE);
	rc = mdo_create(env, mdt_object_child(local_root), &lname,
			mdt_object_child(obj), spec, attr);
	uc->uc_cap = uc_cap_save;
	if (rc < 0) {
		CERROR("%s: cannot create volatile file "DFID": rc = %d\n",
		       mdt_obd_name(mdt), PFID(fid), rc);
		GOTO(out, rc);
	}

	rc = mo_open(env, mdt_object_child(obj), MDS_OPEN_CREATED, spec);
	if (rc < 0)
		CERROR("%s: cannot open volatile file "DFID", orphan "
		       "file will be left in PENDING directory until "
		       "next reboot, rc = %d\n", mdt_obd_name(mdt),
		       PFID(fid), rc);
	GOTO(out, rc);

out:
	if (rc < 0) {
		if (!IS_ERR(obj))
			mdt_object_put(env, obj);
		obj = ERR_PTR(rc);
	}
	mdt_object_put(env, local_root);
	return obj;
}

/* XXX Look into layout in MDT layer. */
static inline int mdt_hsm_set_released(struct lov_mds_md *lmm)
{
	struct lov_comp_md_v1	*comp_v1;
	struct lov_mds_md	*v1;
	__u32	off;
	int	i;

	if (lmm->lmm_magic == cpu_to_le32(LOV_MAGIC_COMP_V1_DEFINED)) {
		comp_v1 = (struct lov_comp_md_v1 *)lmm;

		if (comp_v1->lcm_entry_count == 0)
			return -EINVAL;

		for (i = 0; i < le16_to_cpu(comp_v1->lcm_entry_count); i++) {
			off = le32_to_cpu(comp_v1->lcm_entries[i].lcme_offset);
			v1 = (struct lov_mds_md *)((char *)comp_v1 + off);
			v1->lmm_pattern |= cpu_to_le32(LOV_PATTERN_F_RELEASED);
		}
	} else {
		lmm->lmm_pattern |= cpu_to_le32(LOV_PATTERN_F_RELEASED);
	}
	return 0;
}

static inline int mdt_get_lmm_gen(struct lov_mds_md *lmm, __u32 *gen)
{
	struct lov_comp_md_v1 *comp_v1;

	if (le32_to_cpu(lmm->lmm_magic == LOV_MAGIC_COMP_V1)) {
		comp_v1 = (struct lov_comp_md_v1 *)lmm;
		*gen = le32_to_cpu(comp_v1->lcm_layout_gen);
	} else if (le32_to_cpu(lmm->lmm_magic) == LOV_MAGIC_V1 ||
		   le32_to_cpu(lmm->lmm_magic) == LOV_MAGIC_V3) {
		*gen = le16_to_cpu(lmm->lmm_layout_gen);
	} else {
		return -EINVAL;
	}
	return 0;
}

static int mdt_hsm_release(struct mdt_thread_info *info, struct mdt_object *o,
			   struct md_attr *ma)
{
	struct mdt_lock_handle *lh = &info->mti_lh[MDT_LH_LAYOUT];
	struct lu_ucred	       *uc = mdt_ucred(info);
	struct close_data      *data;
	struct ldlm_lock       *lease;
	struct mdt_object      *orphan;
	struct md_attr         *orp_ma;
	struct lu_buf          *buf;
	cfs_cap_t		cap;
	bool			lease_broken;
	int                     rc;
	int                     rc2;
	ENTRY;

	if (mdt_rdonly(info->mti_exp))
		RETURN(-EROFS);

	data = req_capsule_client_get(info->mti_pill, &RMF_CLOSE_DATA);
	if (data == NULL)
		RETURN(-EPROTO);

	lease = ldlm_handle2lock(&data->cd_handle);
	if (lease == NULL)
		RETURN(-ESTALE);

	/* try to hold open_sem so that nobody else can open the file */
	if (!down_write_trylock(&o->mot_open_sem)) {
		ldlm_lock_cancel(lease);
		GOTO(out_reprocess, rc = -EBUSY);
	}

	/* Check if the lease open lease has already canceled */
	lock_res_and_lock(lease);
	lease_broken = ldlm_is_cancel(lease);
	unlock_res_and_lock(lease);

	LDLM_DEBUG(lease, DFID " lease broken? %d",
		   PFID(mdt_object_fid(o)), lease_broken);

	/* Cancel server side lease. Client side counterpart should
	 * have been cancelled. It's okay to cancel it now as we've
	 * held mot_open_sem. */
	ldlm_lock_cancel(lease);

	if (lease_broken) /* don't perform release task */
		GOTO(out_unlock, rc = -ESTALE);

	if (fid_is_zero(&data->cd_fid) || !fid_is_sane(&data->cd_fid))
		GOTO(out_unlock, rc = -EINVAL);

	/* ma_need was set before but it seems fine to change it in order to
	 * avoid modifying the one from RPC */
	ma->ma_need = MA_HSM;
	rc = mdt_attr_get_complex(info, o, ma);
	if (rc != 0)
		GOTO(out_unlock, rc);

	if (ma->ma_attr_flags & MDS_PCC_ATTACH) {
		if (ma->ma_valid & MA_HSM) {
			if (ma->ma_hsm.mh_flags & HS_RELEASED)
				GOTO(out_unlock, rc = -EALREADY);

			if (ma->ma_hsm.mh_arch_id != data->cd_archive_id)
				CDEBUG(D_CACHE,
				       DFID" archive id diff: %llu:%u\n",
				       PFID(mdt_object_fid(o)),
				       ma->ma_hsm.mh_arch_id,
				       data->cd_archive_id);

			if (!(ma->ma_hsm.mh_flags & HS_DIRTY) &&
			    ma->ma_hsm.mh_arch_ver == data->cd_data_version) {
				CDEBUG(D_CACHE,
				       DFID" data version matches: packed=%llu "
				       "and on-disk=%llu\n",
				       PFID(mdt_object_fid(o)),
				       data->cd_data_version,
				       ma->ma_hsm.mh_arch_ver);
				ma->ma_hsm.mh_flags = HS_ARCHIVED | HS_EXISTS;
			}

			if (ma->ma_hsm.mh_flags & HS_DIRTY)
				ma->ma_hsm.mh_flags = HS_ARCHIVED | HS_EXISTS;
		} else {
			/* Set up HSM attribte for PCC archived object */
			BUILD_BUG_ON(sizeof(struct hsm_attrs) >
				     sizeof(info->mti_xattr_buf));
			buf = &info->mti_buf;
			buf->lb_buf = info->mti_xattr_buf;
			buf->lb_len = sizeof(struct hsm_attrs);
			memset(&ma->ma_hsm, 0, sizeof(ma->ma_hsm));
			ma->ma_hsm.mh_flags = HS_ARCHIVED | HS_EXISTS;
			ma->ma_hsm.mh_arch_id = data->cd_archive_id;
			ma->ma_hsm.mh_arch_ver = data->cd_data_version;
			lustre_hsm2buf(buf->lb_buf, &ma->ma_hsm);

			rc = mo_xattr_set(info->mti_env, mdt_object_child(o),
					  buf, XATTR_NAME_HSM, 0);
			if (rc)
				GOTO(out_unlock, rc);
		}
	} else {
		if (!mdt_hsm_release_allow(ma))
			GOTO(out_unlock, rc = -EPERM);

		/* already released? */
		if (ma->ma_hsm.mh_flags & HS_RELEASED)
			GOTO(out_unlock, rc = 0);

		/* Compare on-disk and packed data_version */
		if (data->cd_data_version != ma->ma_hsm.mh_arch_ver) {
			CDEBUG(D_HSM, DFID" data_version mismatches: "
			       "packed=%llu and on-disk=%llu\n",
			       PFID(mdt_object_fid(o)),
			       data->cd_data_version,
			       ma->ma_hsm.mh_arch_ver);
			GOTO(out_unlock, rc = -EPERM);
		}
	}

	ma->ma_valid = MA_INODE;
	ma->ma_attr.la_valid &= LA_ATIME | LA_MTIME | LA_CTIME | LA_SIZE;
	rc = mo_attr_set(info->mti_env, mdt_object_child(o), ma);
	if (rc < 0)
		GOTO(out_unlock, rc);

	mutex_lock(&o->mot_som_mutex);
	rc2 = mdt_set_som(info, o, SOM_FL_STRICT, ma->ma_attr.la_size,
			   ma->ma_attr.la_blocks);
	mutex_unlock(&o->mot_som_mutex);
	if (rc2 < 0)
		CDEBUG(D_INODE,
		       "%s: File "DFID" SOM update failed: rc = %d\n",
		       mdt_obd_name(info->mti_mdt),
		       PFID(mdt_object_fid(o)), rc2);


	ma->ma_need = MA_INODE | MA_LOV;
	rc = mdt_attr_get_complex(info, o, ma);
	if (rc < 0)
		GOTO(out_unlock, rc);

	if (!(ma->ma_valid & MA_LOV)) {
		/* Even empty file are released */
		memset(ma->ma_lmm, 0, sizeof(*ma->ma_lmm));
		ma->ma_lmm->lmm_magic = cpu_to_le32(LOV_MAGIC_V1_DEFINED);
		ma->ma_lmm->lmm_pattern = cpu_to_le32(LOV_PATTERN_RAID0);
		ma->ma_lmm->lmm_stripe_size = cpu_to_le32(LOV_MIN_STRIPE_SIZE);
		ma->ma_lmm_size = sizeof(*ma->ma_lmm);
	} else {
		/* Magic must be LOV_MAGIC_*_DEFINED or LOD will interpret
		 * ma_lmm as lov_user_md, then it will be confused by union of
		 * layout_gen and stripe_offset. */
		if ((le32_to_cpu(ma->ma_lmm->lmm_magic) & LOV_MAGIC_MASK) ==
		    LOV_MAGIC_MAGIC)
			ma->ma_lmm->lmm_magic |= cpu_to_le32(LOV_MAGIC_DEFINED);
		else
			GOTO(out_unlock, rc = -EINVAL);
	}

	/* Set file as released. */
	rc = mdt_hsm_set_released(ma->ma_lmm);
	if (rc)
		GOTO(out_unlock, rc);

	orp_ma = &info->mti_u.hsm.attr;
	orp_ma->ma_attr.la_mode = S_IFREG | S_IWUSR;
	/* We use root ownership to bypass potential quota
	 * restrictions on the user and group of the file. */
	orp_ma->ma_attr.la_uid = 0;
	orp_ma->ma_attr.la_gid = 0;
	orp_ma->ma_attr.la_valid = LA_MODE | LA_UID | LA_GID;
	orp_ma->ma_lmm = ma->ma_lmm;
	orp_ma->ma_lmm_size = ma->ma_lmm_size;
	orp_ma->ma_valid = MA_INODE | MA_LOV;
	orphan = mdt_orphan_open(info, info->mti_mdt, &data->cd_fid, orp_ma,
				 MDS_FMODE_WRITE);
	if (IS_ERR(orphan)) {
		CERROR("%s: cannot open orphan file "DFID": rc = %ld\n",
		       mdt_obd_name(info->mti_mdt), PFID(&data->cd_fid),
		       PTR_ERR(orphan));
		GOTO(out_unlock, rc = PTR_ERR(orphan));
	}

	/* Set up HSM attribute for orphan object */
	BUILD_BUG_ON(sizeof(struct hsm_attrs) > sizeof(info->mti_xattr_buf));
	buf = &info->mti_buf;
	buf->lb_buf = info->mti_xattr_buf;
	buf->lb_len = sizeof(struct hsm_attrs);
	ma->ma_hsm.mh_flags |= HS_RELEASED;
	lustre_hsm2buf(buf->lb_buf, &ma->ma_hsm);
	ma->ma_hsm.mh_flags &= ~HS_RELEASED;

	mdt_lock_reg_init(lh, LCK_EX);
	rc = mdt_object_lock(info, o, lh, MDS_INODELOCK_LAYOUT |
			     MDS_INODELOCK_XATTR);
	if (rc != 0)
		GOTO(out_close, rc);

	/* The orphan has root ownership so we need to raise
	 * CAP_FOWNER to set the HSM attributes. */
	cap = uc->uc_cap;
	uc->uc_cap |= MD_CAP_TO_MASK(CAP_FOWNER);
	rc = mo_xattr_set(info->mti_env, mdt_object_child(orphan), buf,
			  XATTR_NAME_HSM, 0);
	uc->uc_cap = cap;
	if (rc != 0)
		GOTO(out_layout_lock, rc);

	/* Swap layout with orphan objects. */
	rc = mo_swap_layouts(info->mti_env, mdt_object_child(o),
			     mdt_object_child(orphan),
			     SWAP_LAYOUTS_MDS_HSM);

	if (!rc && ma->ma_attr_flags & MDS_PCC_ATTACH) {
		ma->ma_need = MA_LOV;
		rc = mdt_attr_get_complex(info, o, ma);
	}

	EXIT;

out_layout_lock:
	/* Release exclusive LL */
	mdt_object_unlock(info, o, lh, 1);
out_close:
	/* Close orphan object anyway */
	rc2 = mo_close(info->mti_env, mdt_object_child(orphan), orp_ma,
		       MDS_FMODE_WRITE);
	if (rc2 < 0)
		CERROR("%s: error closing volatile file "DFID": rc = %d\n",
		       mdt_obd_name(info->mti_mdt), PFID(&data->cd_fid), rc2);
	LU_OBJECT_DEBUG(D_HSM, info->mti_env, &orphan->mot_obj,
			"object closed");
	mdt_object_put(info->mti_env, orphan);

out_unlock:
	up_write(&o->mot_open_sem);

	/* already released */
	if (rc == 0) {
		struct mdt_body *repbody;

		repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);
		LASSERT(repbody != NULL);
		repbody->mbo_valid |= OBD_MD_CLOSE_INTENT_EXECED;
		if (ma->ma_attr_flags & MDS_PCC_ATTACH) {
			LASSERT(ma->ma_valid & MA_LOV);
			rc = mdt_get_lmm_gen(ma->ma_lmm,
					     &repbody->mbo_layout_gen);
			if (!rc)
				repbody->mbo_valid |= OBD_MD_LAYOUT_VERSION;
		}
	}

out_reprocess:
	ldlm_reprocess_all(lease->l_resource, lease);
	LDLM_LOCK_PUT(lease);

	ma->ma_valid = 0;
	ma->ma_need = 0;

	return rc;
}

int mdt_close_handle_layouts(struct mdt_thread_info *info,
			     struct mdt_object *o, struct md_attr *ma)
{
	struct mdt_lock_handle	*lh1 = &info->mti_lh[MDT_LH_NEW];
	struct mdt_lock_handle	*lh2 = &info->mti_lh[MDT_LH_OLD];
	struct close_data	*data;
	struct ldlm_lock	*lease;
	struct mdt_object	*o1 = o, *o2 = NULL;
	bool			 lease_broken;
	bool			 swap_objects = false;
	int			 rc;
	ENTRY;

	if (exp_connect_flags(info->mti_exp) & OBD_CONNECT_RDONLY)
		RETURN(-EROFS);

	if (!S_ISREG(lu_object_attr(&o1->mot_obj)))
		RETURN(-EINVAL);

	data = req_capsule_client_get(info->mti_pill, &RMF_CLOSE_DATA);
	if (data == NULL)
		RETURN(-EPROTO);

	if (fid_is_zero(&data->cd_fid) || !fid_is_sane(&data->cd_fid))
		RETURN(-EINVAL);

	rc = lu_fid_cmp(&data->cd_fid, mdt_object_fid(o));
	if (rc == 0) {
		/**
		 * only MDS_CLOSE_LAYOUT_SPLIT use the same fid to indicate
		 * mirror deletion, so we'd zero cd_fid, and keeps o2 be NULL.
		 */
		if (!(ma->ma_attr_flags & MDS_CLOSE_LAYOUT_SPLIT))
			RETURN(-EINVAL);

		/* zero cd_fid to keeps o2 be NULL */
		fid_zero(&data->cd_fid);
	} else if (rc < 0) {
		/* Exchange o1 and o2, to enforce locking order */
		swap_objects = true;
	}

	lease = ldlm_handle2lock(&data->cd_handle);
	if (lease == NULL)
		RETURN(-ESTALE);

	if (!fid_is_zero(&data->cd_fid)) {
		o2 = mdt_object_find(info->mti_env, info->mti_mdt,
				     &data->cd_fid);
		if (IS_ERR(o2))
			GOTO(out_lease, rc = PTR_ERR(o2));

		if (!mdt_object_exists(o2))
			GOTO(out_obj, rc = -ENOENT);

		if (!S_ISREG(lu_object_attr(&o2->mot_obj)))
			GOTO(out_obj, rc = -EINVAL);

		if (swap_objects)
			swap(o1, o2);
	}

	rc = mo_permission(info->mti_env, NULL, mdt_object_child(o1), NULL,
			   MAY_WRITE);
	if (rc < 0)
		GOTO(out_obj, rc);

	if (o2) {
		rc = mo_permission(info->mti_env, NULL, mdt_object_child(o2),
				   NULL, MAY_WRITE);
		if (rc < 0)
			GOTO(out_obj, rc);
	}

	/* try to hold open_sem so that nobody else can open the file */
	if (!down_write_trylock(&o->mot_open_sem)) {
		ldlm_lock_cancel(lease);
		GOTO(out_obj, rc = -EBUSY);
	}

	/* Check if the lease open lease has already canceled */
	lock_res_and_lock(lease);
	lease_broken = ldlm_is_cancel(lease);
	unlock_res_and_lock(lease);

	LDLM_DEBUG(lease, DFID " lease broken? %d",
		   PFID(mdt_object_fid(o)), lease_broken);

	/* Cancel server side lease. Client side counterpart should
	 * have been cancelled. It's okay to cancel it now as we've
	 * held mot_open_sem. */
	ldlm_lock_cancel(lease);

	if (lease_broken)
		GOTO(out_unlock_sem, rc = -ESTALE);

	mdt_lock_reg_init(lh1, LCK_EX);
	rc = mdt_object_lock(info, o1, lh1, MDS_INODELOCK_LAYOUT |
			     MDS_INODELOCK_XATTR);
	if (rc < 0)
		GOTO(out_unlock_sem, rc);

	if (o2) {
		mdt_lock_reg_init(lh2, LCK_EX);
		rc = mdt_object_lock(info, o2, lh2, MDS_INODELOCK_LAYOUT |
				     MDS_INODELOCK_XATTR);
		if (rc < 0)
			GOTO(out_unlock1, rc);
	}

	/* Swap layout with orphan object */
	if (ma->ma_attr_flags & MDS_CLOSE_LAYOUT_SWAP) {
		rc = mo_swap_layouts(info->mti_env, mdt_object_child(o1),
				     mdt_object_child(o2), 0);
	} else if (ma->ma_attr_flags & MDS_CLOSE_LAYOUT_MERGE ||
		   ma->ma_attr_flags & MDS_CLOSE_LAYOUT_SPLIT) {
		struct lu_buf *buf = &info->mti_buf;
		struct md_rejig_data mrd;

		if (o2) {
			mrd.mrd_obj = mdt_object_child(o == o1 ? o2 : o1);
		} else {
			if (!(ma->ma_attr_flags & MDS_CLOSE_LAYOUT_SPLIT)) {
				/* paranoid check again */
				CERROR(DFID
				  ":only mirror split support NULL o2 object\n",
					PFID(mdt_object_fid(o)));
				GOTO(out_unlock1, rc = -EINVAL);
			}

			/* set NULL mrd_obj for deleting mirror objects */
			mrd.mrd_obj = NULL;
		}

		if (ma->ma_attr_flags & MDS_CLOSE_LAYOUT_SPLIT) {
			mrd.mrd_mirror_id = data->cd_mirror_id;
			/* set a small enough blocks in the SoM */
			ma->ma_attr.la_blocks >>= 1;
		}

		buf->lb_len = sizeof(mrd);
		buf->lb_buf = &mrd;
		rc = mo_xattr_set(info->mti_env, mdt_object_child(o), buf,
				  XATTR_LUSTRE_LOV,
				  ma->ma_attr_flags & MDS_CLOSE_LAYOUT_SPLIT ?
				  LU_XATTR_SPLIT : LU_XATTR_MERGE);
		if (rc == 0 && ma->ma_attr.la_valid & (LA_SIZE | LA_BLOCKS |
						       LA_LSIZE | LA_LBLOCKS)) {
			int rc2;
			enum lustre_som_flags lsf;

			if (ma->ma_attr.la_valid & (LA_SIZE | LA_BLOCKS))
				lsf = SOM_FL_STRICT;
			else
				lsf = SOM_FL_LAZY;

			mutex_lock(&o->mot_som_mutex);
			rc2 = mdt_set_som(info, o, lsf,
					  ma->ma_attr.la_size,
					  ma->ma_attr.la_blocks);
			mutex_unlock(&o->mot_som_mutex);
			if (rc2 < 0)
				CERROR(DFID": Setting i_blocks error: %d, "
				       "i_blocks will be reported wrongly and "
				       "can only be fixed in next resync\n",
				       PFID(mdt_object_fid(o)), rc2);
		}
	}
	if (rc < 0)
		GOTO(out_unlock2, rc);

	EXIT;

out_unlock2:
	/* Release exclusive LL */
	if (o2)
		mdt_object_unlock(info, o2, lh2, 1);

out_unlock1:
	mdt_object_unlock(info, o1, lh1, 1);

out_unlock_sem:
	up_write(&o->mot_open_sem);

	/* already swapped */
	if (rc == 0) {
		struct mdt_body *repbody;

		repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);
		LASSERT(repbody != NULL);
		repbody->mbo_valid |= OBD_MD_CLOSE_INTENT_EXECED;
	}

out_obj:
	if (o1 != o)
		/* the 2nd object has been used, and swapped to o1 */
		mdt_object_put(info->mti_env, o1);
	else if (o2)
		/* the 2nd object has been used, and not swapped */
		mdt_object_put(info->mti_env, o2);

	ldlm_reprocess_all(lease->l_resource, lease);

out_lease:
	LDLM_LOCK_PUT(lease);

	if (ma != NULL) {
		ma->ma_valid = 0;
		ma->ma_need = 0;
	}

	return rc;
}

static int mdt_close_resync_done(struct mdt_thread_info *info,
				 struct mdt_object *o, struct md_attr *ma)
{
	struct mdt_lock_handle *lhc = &info->mti_lh[MDT_LH_LOCAL];
	struct close_data *data;
	struct ldlm_lock *lease;
	struct md_layout_change layout = { 0 };
	__u32 *resync_ids = NULL;
	size_t resync_count = 0;
	bool lease_broken;
	int rc;

	ENTRY;

	if (exp_connect_flags(info->mti_exp) & OBD_CONNECT_RDONLY)
		RETURN(-EROFS);

	if (!S_ISREG(lu_object_attr(&o->mot_obj)))
		RETURN(-EINVAL);

	data = req_capsule_client_get(info->mti_pill, &RMF_CLOSE_DATA);
	if (data == NULL)
		RETURN(-EPROTO);

	if (req_capsule_req_need_swab(info->mti_pill))
		lustre_swab_close_data_resync_done(&data->cd_resync);

	if (!fid_is_zero(&data->cd_fid))
		RETURN(-EPROTO);

	lease = ldlm_handle2lock(&data->cd_handle);
	if (lease == NULL)
		RETURN(-ESTALE);

	/* try to hold open_sem so that nobody else can open the file */
	if (!down_write_trylock(&o->mot_open_sem)) {
		ldlm_lock_cancel(lease);
		GOTO(out_reprocess, rc = -EBUSY);
	}

	/* Check if the lease open lease has already canceled */
	lock_res_and_lock(lease);
	lease_broken = ldlm_is_cancel(lease);
	unlock_res_and_lock(lease);

	LDLM_DEBUG(lease, DFID " lease broken? %d\n",
		   PFID(mdt_object_fid(o)), lease_broken);

	/* Cancel server side lease. Client side counterpart should
	 * have been cancelled. It's okay to cancel it now as we've
	 * held mot_open_sem. */
	ldlm_lock_cancel(lease);

	if (lease_broken) /* don't perform release task */
		GOTO(out_unlock, rc = -ESTALE);

	resync_count = data->cd_resync.resync_count;

	if (resync_count > INLINE_RESYNC_ARRAY_SIZE) {
		void *data;

		if (!req_capsule_has_field(info->mti_pill, &RMF_U32,
					   RCL_CLIENT))
			GOTO(out_unlock, rc = -EPROTO);

		OBD_ALLOC_PTR_ARRAY(resync_ids, resync_count);
		if (!resync_ids)
			GOTO(out_unlock, rc = -ENOMEM);

		data = req_capsule_client_get(info->mti_pill, &RMF_U32);
		memcpy(resync_ids, data, resync_count * sizeof(__u32));

		layout.mlc_resync_ids = resync_ids;
	} else {
		layout.mlc_resync_ids = data->cd_resync.resync_ids_inline;
	}

	layout.mlc_opc = MD_LAYOUT_RESYNC_DONE;
	layout.mlc_resync_count = resync_count;
	if (ma->ma_attr.la_valid & (LA_SIZE | LA_BLOCKS)) {
		layout.mlc_som.lsa_valid = SOM_FL_STRICT;
		layout.mlc_som.lsa_size = ma->ma_attr.la_size;
		layout.mlc_som.lsa_blocks = ma->ma_attr.la_blocks;
	}
	rc = mdt_layout_change(info, o, lhc, &layout);
	if (rc)
		GOTO(out_unlock, rc);

	mdt_object_unlock(info, o, lhc, 0);

	EXIT;

out_unlock:
	up_write(&o->mot_open_sem);

	/* already released */
	if (rc == 0) {
		struct mdt_body *repbody;

		repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);
		LASSERT(repbody != NULL);
		repbody->mbo_valid |= OBD_MD_CLOSE_INTENT_EXECED;
	}

	if (resync_ids)
		OBD_FREE_PTR_ARRAY(resync_ids, resync_count);

out_reprocess:
	ldlm_reprocess_all(lease->l_resource, lease);
	LDLM_LOCK_PUT(lease);

	ma->ma_valid = 0;
	ma->ma_need = 0;

	return rc;
}

#define MFD_CLOSED(open_flags) ((open_flags) == MDS_FMODE_CLOSED)

static int mdt_mfd_closed(struct mdt_file_data *mfd)
{
	return ((mfd == NULL) || MFD_CLOSED(mfd->mfd_open_flags));
}

int mdt_mfd_close(struct mdt_thread_info *info, struct mdt_file_data *mfd)
{
	struct mdt_object *o = mfd->mfd_object;
	struct md_object *next = mdt_object_child(o);
	struct md_attr *ma = &info->mti_attr;
	struct lu_fid *ofid = &info->mti_tmp_fid1;
	int rc = 0;
	u64 open_flags;
	u64 intent;

	ENTRY;

	open_flags = mfd->mfd_open_flags;
	intent = ma->ma_attr_flags & MDS_CLOSE_INTENT;
	*ofid = *mdt_object_fid(o);

	/* the below message is checked in replay-single.sh test_46 */
	CDEBUG(D_INODE, "%s: %sclosing file handle "DFID" with intent: %llx\n",
	       mdt_obd_name(info->mti_mdt),
	       ma->ma_valid & MA_FORCE_LOG ? "force " : "", PFID(ofid), intent);

	switch (intent) {
	case MDS_HSM_RELEASE: {
		rc = mdt_hsm_release(info, o, ma);
		if (rc < 0) {
			CDEBUG(D_HSM, "%s: File " DFID " release failed: %d\n",
			       mdt_obd_name(info->mti_mdt),
			       PFID(ofid), rc);
			/* continue to close even error occurred. */
		}
		break;
	}
	case MDS_CLOSE_LAYOUT_MERGE:
	case MDS_CLOSE_LAYOUT_SPLIT:
	case MDS_CLOSE_LAYOUT_SWAP: {
		rc = mdt_close_handle_layouts(info, o, ma);
		if (rc < 0) {
			CDEBUG(D_INODE,
			       "%s: cannot swap layout of "DFID": rc = %d\n",
			       mdt_obd_name(info->mti_mdt),
			       PFID(ofid), rc);
			/* continue to close even if error occurred. */
		}
		break;
	}
	case MDS_CLOSE_RESYNC_DONE:
		rc = mdt_close_resync_done(info, o, ma);
		break;
	default:
		/* nothing */
		break;
	}

	if (S_ISREG(lu_object_attr(&o->mot_obj)) &&
	    ma->ma_attr.la_valid & (LA_LSIZE | LA_LBLOCKS)) {
		int rc2;

		rc2 = mdt_lsom_update(info, o, false);
		if (rc2 < 0)
			CDEBUG(D_INODE,
			       "%s: File " DFID " LSOM failed: rc = %d\n",
			       mdt_obd_name(info->mti_mdt),
			       PFID(ofid), rc2);
			/* continue to close even if error occured. */
	}

	if (open_flags & MDS_FMODE_WRITE)
		mdt_write_put(o);
	else if (open_flags & MDS_FMODE_EXEC)
		mdt_write_allow(o);

	/* Update atime|mtime|ctime on close. */
	if ((open_flags & MDS_FMODE_EXEC || open_flags & MDS_FMODE_READ ||
	     open_flags & MDS_FMODE_WRITE) && (ma->ma_valid & MA_INODE) &&
	    (ma->ma_attr.la_valid & LA_ATIME ||
	     ma->ma_attr.la_valid & LA_MTIME ||
	     ma->ma_attr.la_valid & LA_CTIME)) {
		ma->ma_valid = MA_INODE;
		ma->ma_attr_flags |= MDS_CLOSE_UPDATE_TIMES;
		ma->ma_attr.la_valid &= (LA_ATIME | LA_MTIME | LA_CTIME);

		if (ma->ma_attr.la_valid & LA_MTIME) {
			rc = mdt_attr_get_pfid(info, o, &ma->ma_pfid);
			if (!rc)
				ma->ma_valid |= MA_PFID;
		}

		rc = mo_attr_set(info->mti_env, next, ma);
	}

	/* If file data is modified, add the dirty flag. */
	if (ma->ma_attr_flags & MDS_DATA_MODIFIED)
		rc = mdt_add_dirty_flag(info, o, ma);

        ma->ma_need |= MA_INODE;
        ma->ma_valid &= ~MA_INODE;

	LASSERT(atomic_read(&o->mot_open_count) > 0);
	atomic_dec(&o->mot_open_count);
	mdt_handle_last_unlink(info, o, ma);

	if (!MFD_CLOSED(open_flags)) {
		rc = mo_close(info->mti_env, next, ma, open_flags);
		if (mdt_dom_check_for_discard(info, o))
			mdt_dom_discard_data(info, o);
	}

	/* adjust open and lease count */
	if (open_flags & MDS_OPEN_LEASE) {
		LASSERT(atomic_read(&o->mot_lease_count) > 0);
		atomic_dec(&o->mot_lease_count);
	}

	mdt_mfd_free(mfd);
	mdt_object_put(info->mti_env, o);

	RETURN(rc);
}

int mdt_close_internal(struct mdt_thread_info *info, struct ptlrpc_request *req,
		       struct mdt_body *repbody)
{
	struct mdt_export_data *med;
	struct mdt_file_data   *mfd;
	int			ret = 0;
	int			rc = 0;
	ENTRY;

	med = &req->rq_export->exp_mdt_data;
	spin_lock(&med->med_open_lock);
	mfd = mdt_open_handle2mfd(med, &info->mti_open_handle,
				  req_is_replay(req));
	if (mdt_mfd_closed(mfd)) {
		spin_unlock(&med->med_open_lock);
		CDEBUG(D_INODE, "no handle for file close: fid = "DFID
		       ": cookie = %#llx\n", PFID(info->mti_rr.rr_fid1),
		       info->mti_open_handle.cookie);
		/** not serious error since bug 3633 */
		rc = -ESTALE;
	} else {
		class_handle_unhash(&mfd->mfd_open_handle);
		list_del_init(&mfd->mfd_list);
		spin_unlock(&med->med_open_lock);
		ret = mdt_mfd_close(info, mfd);
	}

	RETURN(rc ? rc : ret);
}

int mdt_close(struct tgt_session_info *tsi)
{
	struct mdt_thread_info	*info = tsi2mdt_info(tsi);
	struct ptlrpc_request	*req = tgt_ses_req(tsi);
        struct md_attr         *ma = &info->mti_attr;
        struct mdt_body        *repbody = NULL;
	ktime_t			kstart = ktime_get();
        int rc, ret = 0;
        ENTRY;

	/* Close may come with the Size-on-MDS update. Unpack it. */
	rc = mdt_close_unpack(info);
	if (rc)
		GOTO(out, rc = err_serious(rc));

	/* These fields are no longer used and are left for compatibility.
	 * size is always zero */
        req_capsule_set_size(info->mti_pill, &RMF_MDT_MD, RCL_SERVER,
			     0);
        req_capsule_set_size(info->mti_pill, &RMF_LOGCOOKIES, RCL_SERVER,
			     0);
        rc = req_capsule_server_pack(info->mti_pill);
        if (mdt_check_resent(info, mdt_reconstruct_generic, NULL)) {
                mdt_client_compatibility(info);
                if (rc == 0)
                        mdt_fix_reply(info);
		mdt_exit_ucred(info);
		GOTO(out, rc = lustre_msg_get_status(req->rq_repmsg));
        }

        /* Continue to close handle even if we can not pack reply */
        if (rc == 0) {
                repbody = req_capsule_server_get(info->mti_pill,
                                                 &RMF_MDT_BODY);
                ma->ma_lmm = req_capsule_server_get(info->mti_pill,
                                                    &RMF_MDT_MD);
                ma->ma_lmm_size = req_capsule_get_size(info->mti_pill,
                                                       &RMF_MDT_MD,
                                                       RCL_SERVER);
		ma->ma_need = MA_INODE | MA_LOV;
		repbody->mbo_eadatasize = 0;
		repbody->mbo_aclsize = 0;
	} else {
		rc = err_serious(rc);
	}

	rc = mdt_close_internal(info, req, repbody);
	if (rc != -ESTALE)
		mdt_empty_transno(info, rc);

        if (repbody != NULL) {
                mdt_client_compatibility(info);
                rc = mdt_fix_reply(info);
        }

	mdt_exit_ucred(info);
	if (OBD_FAIL_CHECK(OBD_FAIL_MDS_CLOSE_PACK))
		GOTO(out, rc = err_serious(-ENOMEM));

	if (OBD_FAIL_CHECK_RESET(OBD_FAIL_MDS_CLOSE_NET_REP,
				 OBD_FAIL_MDS_CLOSE_NET_REP))
		tsi->tsi_reply_fail_id = OBD_FAIL_MDS_CLOSE_NET_REP;
out:
	mdt_thread_info_fini(info);
	if (rc == 0)
		mdt_counter_incr(req, LPROC_MDT_CLOSE,
				 ktime_us_delta(ktime_get(), kstart));
	RETURN(rc ? rc : ret);
}
