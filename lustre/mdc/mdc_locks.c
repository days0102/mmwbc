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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#define DEBUG_SUBSYSTEM S_MDC

#include <linux/module.h>

#include <obd.h>
#include <obd_class.h>
#include <lustre_dlm.h>
#include <lustre_fid.h>
#include <lustre_intent.h>
#include <lustre_mdc.h>
#include <lustre_net.h>
#include <lustre_req_layout.h>
#include <lustre_swab.h>
#include <lustre_acl.h>

#include "mdc_internal.h"

struct mdc_getattr_args {
	struct obd_export	*ga_exp;
	struct md_op_item	*ga_item;
};

int it_open_error(int phase, struct lookup_intent *it)
{
	if (it_disposition(it, DISP_OPEN_LEASE)) {
		if (phase >= DISP_OPEN_LEASE)
			return it->it_status;
		else
			return 0;
	}
	if (it_disposition(it, DISP_OPEN_OPEN)) {
		if (phase >= DISP_OPEN_OPEN)
			return it->it_status;
		else
			return 0;
	}

	if (it_disposition(it, DISP_OPEN_CREATE)) {
		if (phase >= DISP_OPEN_CREATE)
			return it->it_status;
		else
			return 0;
	}

	if (it_disposition(it, DISP_LOOKUP_EXECD)) {
		if (phase >= DISP_LOOKUP_EXECD)
			return it->it_status;
		else
			return 0;
	}

	if (it_disposition(it, DISP_IT_EXECD)) {
		if (phase >= DISP_IT_EXECD)
			return it->it_status;
		else
			return 0;
	}

	CERROR("it disp: %X, status: %d\n", it->it_disposition, it->it_status);
	LBUG();

	return 0;
}
EXPORT_SYMBOL(it_open_error);

/* this must be called on a lockh that is known to have a referenced lock */
int mdc_set_lock_data(struct obd_export *exp, const struct lustre_handle *lockh,
		      void *data, __u64 *bits)
{
	struct ldlm_lock *lock;
	struct inode *new_inode = data;

	ENTRY;
	if (bits)
		*bits = 0;

	if (!lustre_handle_is_used(lockh))
		RETURN(0);

	lock = ldlm_handle2lock(lockh);

	LASSERT(lock != NULL);
	lock_res_and_lock(lock);
	if (lock->l_resource->lr_lvb_inode &&
	    lock->l_resource->lr_lvb_inode != data) {
		struct inode *old_inode = lock->l_resource->lr_lvb_inode;

		LASSERTF(old_inode->i_state & I_FREEING,
			 "Found existing inode %p/%lu/%u state %lu in lock: setting data to %p/%lu/%u\n",
			 old_inode, old_inode->i_ino, old_inode->i_generation,
			 old_inode->i_state,
			 new_inode, new_inode->i_ino, new_inode->i_generation);
	}
	lock->l_resource->lr_lvb_inode = new_inode;
	if (bits)
		*bits = lock->l_policy_data.l_inodebits.bits;

	unlock_res_and_lock(lock);
	LDLM_LOCK_PUT(lock);

	RETURN(0);
}

enum ldlm_mode mdc_lock_match(struct obd_export *exp, __u64 flags,
			      const struct lu_fid *fid, enum ldlm_type type,
			      union ldlm_policy_data *policy,
			      enum ldlm_mode mode, struct lustre_handle *lockh)
{
	struct ldlm_res_id res_id;
	enum ldlm_mode rc;

	ENTRY;
	fid_build_reg_res_name(fid, &res_id);
	/* LU-4405: Clear bits not supported by server */
	policy->l_inodebits.bits &= exp_connect_ibits(exp);
	rc = ldlm_lock_match(class_exp2obd(exp)->obd_namespace, flags,
			     &res_id, type, policy, mode, lockh);
	RETURN(rc);
}

int mdc_cancel_unused(struct obd_export *exp, const struct lu_fid *fid,
		      union ldlm_policy_data *policy, enum ldlm_mode mode,
		      enum ldlm_cancel_flags flags, void *opaque)
{
	struct obd_device *obd = class_exp2obd(exp);
	struct ldlm_res_id res_id;
	int rc;

	ENTRY;
	fid_build_reg_res_name(fid, &res_id);
	rc = ldlm_cli_cancel_unused_resource(obd->obd_namespace, &res_id,
					     policy, mode, flags, opaque);
	RETURN(rc);
}

int mdc_null_inode(struct obd_export *exp,
		   const struct lu_fid *fid)
{
	struct ldlm_res_id res_id;
	struct ldlm_resource *res;
	struct ldlm_namespace *ns = class_exp2obd(exp)->obd_namespace;

	ENTRY;
	LASSERTF(ns != NULL, "no namespace passed\n");

	fid_build_reg_res_name(fid, &res_id);

	res = ldlm_resource_get(ns, NULL, &res_id, 0, 0);
	if (IS_ERR(res))
		RETURN(0);

	lock_res(res);
	res->lr_lvb_inode = NULL;
	unlock_res(res);

	ldlm_resource_putref(res);
	RETURN(0);
}

static inline void mdc_clear_replay_flag(struct ptlrpc_request *req, int rc)
{
	/* Don't hold error requests for replay. */
	if (req->rq_replay) {
		spin_lock(&req->rq_lock);
		req->rq_replay = 0;
		spin_unlock(&req->rq_lock);
	}
	if (rc && req->rq_transno != 0) {
		DEBUG_REQ(D_ERROR, req, "transno returned on error: rc = %d",
			  rc);
		LBUG();
	}
}

/**
 * Save a large LOV EA into the request buffer so that it is available
 * for replay.  We don't do this in the initial request because the
 * original request doesn't need this buffer (at most it sends just the
 * lov_mds_md) and it is a waste of RAM/bandwidth to send the empty
 * buffer and may also be difficult to allocate and save a very large
 * request buffer for each open. (b=5707)
 *
 * OOM here may cause recovery failure if lmm is needed (only for the
 * original open if the MDS crashed just when this client also OOM'd)
 * but this is incredibly unlikely, and questionable whether the client
 * could do MDS recovery under OOM anyways...
 */
static int mdc_save_lovea(struct ptlrpc_request *req, void *data, u32 size)
{
	struct req_capsule *pill = &req->rq_pill;
	void *lovea;
	int rc = 0;

	if (req_capsule_get_size(pill, &RMF_EADATA, RCL_CLIENT) < size) {
		rc = sptlrpc_cli_enlarge_reqbuf(req, &RMF_EADATA, size);
		if (rc) {
			CERROR("%s: Can't enlarge ea size to %d: rc = %d\n",
			       req->rq_export->exp_obd->obd_name,
			       size, rc);
			return rc;
		}
	} else {
		req_capsule_shrink(pill, &RMF_EADATA, size, RCL_CLIENT);
	}

	req_capsule_set_size(pill, &RMF_EADATA, RCL_CLIENT, size);
	lovea = req_capsule_client_get(pill, &RMF_EADATA);
	if (lovea) {
		memcpy(lovea, data, size);
		lov_fix_ea_for_replay(lovea);
	}

	return rc;
}

static struct ptlrpc_request *
mdc_intent_open_pack(struct obd_export *exp, struct lookup_intent *it,
		     struct md_op_data *op_data, __u32 acl_bufsize)
{
	struct ptlrpc_request *req;
	struct obd_device *obd = class_exp2obd(exp);
	struct ldlm_intent *lit;
	const void *lmm = op_data->op_data;
	__u32 lmmsize = op_data->op_data_size;
	__u32  mdt_md_capsule_size;
	LIST_HEAD(cancels);
	int count = 0;
	enum ldlm_mode mode;
	int repsize, repsize_estimate;
	int rc;

	ENTRY;

	mdt_md_capsule_size = obd->u.cli.cl_default_mds_easize;

	it->it_create_mode = (it->it_create_mode & ~S_IFMT) | S_IFREG;

	/* XXX: openlock is not cancelled for cross-refs. */
	/* If inode is known, cancel conflicting OPEN locks. */
	if (fid_is_sane(&op_data->op_fid2)) {
		if (it->it_flags & MDS_OPEN_LEASE) { /* try to get lease */
			if (it->it_flags & MDS_FMODE_WRITE)
				mode = LCK_EX;
			else
				mode = LCK_PR;
		} else {
			if (it->it_flags & (MDS_FMODE_WRITE | MDS_OPEN_TRUNC))
				mode = LCK_CW;
#ifdef FMODE_EXEC
			else if (it->it_flags & FMODE_EXEC)
				mode = LCK_PR;
#endif
			else
				mode = LCK_CR;
		}
		count = mdc_resource_get_unused(exp, &op_data->op_fid2,
						&cancels, mode,
						MDS_INODELOCK_OPEN);
	}

	/* If CREATE, cancel parent's UPDATE lock. */
	if (it->it_op & IT_CREAT)
		mode = LCK_EX;
	else
		mode = LCK_CR;
	count += mdc_resource_get_unused(exp, &op_data->op_fid1,
					 &cancels, mode,
					 MDS_INODELOCK_UPDATE);

	req = ptlrpc_request_alloc(class_exp2cliimp(exp),
				   &RQF_LDLM_INTENT_OPEN);
	if (req == NULL) {
		ldlm_lock_list_put(&cancels, l_bl_ast, count);
		RETURN(ERR_PTR(-ENOMEM));
	}

	req_capsule_set_size(&req->rq_pill, &RMF_NAME, RCL_CLIENT,
			     op_data->op_namelen + 1);
	if (cl_is_lov_delay_create(it->it_flags)) {
		/* open(O_LOV_DELAY_CREATE) won't pack lmm */
		LASSERT(lmmsize == 0);
		req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_CLIENT, 0);
	} else {
		req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_CLIENT,
			     max(lmmsize, obd->u.cli.cl_default_mds_easize));
	}

	req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX_NAME,
			     RCL_CLIENT, op_data->op_file_secctx_name != NULL ?
			     op_data->op_file_secctx_name_size : 0);

	req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX, RCL_CLIENT,
			     op_data->op_file_secctx_size);

	req_capsule_set_size(&req->rq_pill, &RMF_FILE_ENCCTX, RCL_CLIENT,
			     op_data->op_file_encctx_size);

	/* get SELinux policy info if any */
	rc = sptlrpc_get_sepol(req);
	if (rc < 0) {
		ldlm_lock_list_put(&cancels, l_bl_ast, count);
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}
	req_capsule_set_size(&req->rq_pill, &RMF_SELINUX_POL, RCL_CLIENT,
			     strlen(req->rq_sepol) ?
			     strlen(req->rq_sepol) + 1 : 0);

	rc = ldlm_prep_enqueue_req(exp, req, &cancels, count);
	if (rc < 0) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	spin_lock(&req->rq_lock);
	req->rq_replay = req->rq_import->imp_replayable;
	spin_unlock(&req->rq_lock);

	/* pack the intent */
	lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
	lit->opc = (__u64)it->it_op;

	/* pack the intended request */
	mdc_open_pack(&req->rq_pill, op_data, it->it_create_mode, 0,
		      it->it_flags, lmm, lmmsize);

	req_capsule_set_size(&req->rq_pill, &RMF_MDT_MD, RCL_SERVER,
			     mdt_md_capsule_size);
	req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER, acl_bufsize);

	if (!(it->it_op & IT_CREAT) && it->it_op & IT_OPEN &&
	    req_capsule_has_field(&req->rq_pill, &RMF_FILE_SECCTX_NAME,
				  RCL_CLIENT) &&
	    op_data->op_file_secctx_name_size > 0 &&
	    op_data->op_file_secctx_name != NULL) {
		char *secctx_name;

		secctx_name = req_capsule_client_get(&req->rq_pill,
						     &RMF_FILE_SECCTX_NAME);
		memcpy(secctx_name, op_data->op_file_secctx_name,
		       op_data->op_file_secctx_name_size);
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX,
				     RCL_SERVER,
				     obd->u.cli.cl_max_mds_easize);

		CDEBUG(D_SEC, "packed '%.*s' as security xattr name\n",
		       op_data->op_file_secctx_name_size,
		       op_data->op_file_secctx_name);

	} else {
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX,
				     RCL_SERVER, 0);
	}

	if (exp_connect_encrypt(exp) && !(it->it_op & IT_CREAT) &&
	    it->it_op & IT_OPEN)
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_ENCCTX,
				     RCL_SERVER,
				     obd->u.cli.cl_max_mds_easize);
	else
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_ENCCTX,
				     RCL_SERVER, 0);

	/**
	 * Inline buffer for possible data from Data-on-MDT files.
	 */
	req_capsule_set_size(&req->rq_pill, &RMF_NIOBUF_INLINE, RCL_SERVER,
			     sizeof(struct niobuf_remote));
	ptlrpc_request_set_replen(req);

	/* Get real repbuf allocated size as rounded up power of 2 */
	repsize = size_roundup_power2(req->rq_replen +
				      lustre_msg_early_size());
	/* Estimate free space for DoM files in repbuf */
	repsize_estimate = repsize - (req->rq_replen -
			   mdt_md_capsule_size +
			   sizeof(struct lov_comp_md_v1) +
			   sizeof(struct lov_comp_md_entry_v1) +
			   lov_mds_md_size(0, LOV_MAGIC_V3));

	if (repsize_estimate < obd->u.cli.cl_dom_min_inline_repsize) {
		repsize = obd->u.cli.cl_dom_min_inline_repsize -
			  repsize_estimate + sizeof(struct niobuf_remote);
		req_capsule_set_size(&req->rq_pill, &RMF_NIOBUF_INLINE,
				     RCL_SERVER,
				     sizeof(struct niobuf_remote) + repsize);
		ptlrpc_request_set_replen(req);
		CDEBUG(D_INFO, "Increase repbuf by %d bytes, total: %d\n",
		       repsize, req->rq_replen);
		repsize = size_roundup_power2(req->rq_replen +
					      lustre_msg_early_size());
	}
	/* The only way to report real allocated repbuf size to the server
	 * is the lm_repsize but it must be set prior buffer allocation itself
	 * due to security reasons - it is part of buffer used in signature
	 * calculation (see LU-11414). Therefore the saved size is predicted
	 * value as rq_replen rounded to the next higher power of 2.
	 * Such estimation is safe. Though the final allocated buffer might
	 * be even larger, it is not possible to know that at this point.
	 */
	req->rq_reqmsg->lm_repsize = repsize;
	RETURN(req);
}

static struct ptlrpc_request *
mdc_intent_create_pack(struct obd_export *exp, struct lookup_intent *it,
		       struct md_op_data *op_data, __u32 acl_bufsize,
		       __u64 extra_lock_flags)
{
	LIST_HEAD(cancels);
	struct ptlrpc_request *req;
	struct obd_device *obd = class_exp2obd(exp);
	bool parent_locked = extra_lock_flags & LDLM_FL_INTENT_PARENT_LOCKED;
	int canceloff = LDLM_ENQUEUE_CANCEL_OFF;
	struct ldlm_intent *lit;
	int lmm_size = 0;
	int count = 0;
	int rc;

	ENTRY;

	if (parent_locked)
		canceloff += 1;
	else if (fid_is_sane(&op_data->op_fid1))
		/* cancel parent's UPDATE lock. */
		count = mdc_resource_get_unused(exp, &op_data->op_fid1,
						&cancels, LCK_EX,
						MDS_INODELOCK_UPDATE);

	req = ptlrpc_request_alloc(class_exp2cliimp(exp),
				   &RQF_LDLM_INTENT_CREATE);
	if (req == NULL) {
		ldlm_lock_list_put(&cancels, l_bl_ast, count);
		RETURN(ERR_PTR(-ENOMEM));
	}

	req_capsule_set_size(&req->rq_pill, &RMF_NAME, RCL_CLIENT,
			     op_data->op_namelen + 1);
	req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX_NAME,
			     RCL_CLIENT, op_data->op_file_secctx_name != NULL ?
			     strlen(op_data->op_file_secctx_name) + 1 : 0);
	req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX, RCL_CLIENT,
			     op_data->op_file_secctx_size);

	if ((it->it_flags & MDS_OPEN_PCC || S_ISLNK(it->it_create_mode)) &&
	    op_data->op_data && op_data->op_data_size)
		lmm_size = op_data->op_data_size;

	req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_CLIENT, lmm_size);
	req_capsule_set_size(&req->rq_pill, &RMF_FILE_ENCCTX, RCL_CLIENT,
			     op_data->op_file_encctx_size);

	/* get SELinux policy info if any */
	rc = sptlrpc_get_sepol(req);
	if (rc < 0) {
		ldlm_lock_list_put(&cancels, l_bl_ast, count);
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}
	req_capsule_set_size(&req->rq_pill, &RMF_SELINUX_POL, RCL_CLIENT,
			     strlen(req->rq_sepol) ?
			     strlen(req->rq_sepol) + 1 : 0);

	rc = ldlm_prep_elc_req(exp, req, LUSTRE_DLM_VERSION, LDLM_ENQUEUE,
			       canceloff, &cancels, count);
	if (rc < 0) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	if (parent_locked) {
		struct ldlm_request *dlm;

		dlm = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
		dlm->lock_count++;
		dlm->lock_handle[1] = op_data->op_open_handle;
	}

	/* Pack the intent */
	lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
	lit->opc = (__u64)it->it_op;

	/* Pack the intent request. */
	mdc_create_pack(&req->rq_pill, op_data, op_data->op_data,
			op_data->op_data_size, it->it_create_mode,
			op_data->op_fsuid, op_data->op_fsgid,
			op_data->op_cap, op_data->op_rdev, it->it_flags);

	req_capsule_set_size(&req->rq_pill, &RMF_MDT_MD, RCL_SERVER,
			     obd->u.cli.cl_default_mds_easize);
	req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER, acl_bufsize);
	req_capsule_set_size(&req->rq_pill, &RMF_DEFAULT_MDT_MD, RCL_SERVER,
			     sizeof(struct lmv_user_md));
	req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX,
			     RCL_SERVER, 0);
	req_capsule_set_size(&req->rq_pill, &RMF_FILE_ENCCTX, RCL_SERVER, 0);

	ptlrpc_request_set_replen(req);
	RETURN(req);
}

static struct ptlrpc_request *
mdc_intent_setattr_pack(struct obd_export *exp,
			struct lookup_intent *it,
			struct md_op_data *op_data,
			__u64 extra_lock_flags)
{
	bool parent_locked = extra_lock_flags & LDLM_FL_INTENT_PARENT_LOCKED;
	int canceloff = LDLM_ENQUEUE_CANCEL_OFF;
	struct ptlrpc_request *req;
	struct ldlm_intent *lit;
	LIST_HEAD(cancels);
	int count = 0;
	int rc;

	ENTRY;

	LASSERT(op_data != NULL);

	if (parent_locked) {
		canceloff += 1;
	} else {
		__u64 bits;

		bits = MDS_INODELOCK_UPDATE;
		if (op_data->op_attr.ia_valid & (ATTR_MODE|ATTR_UID|ATTR_GID))
			bits |= MDS_INODELOCK_LOOKUP;
		if ((op_data->op_flags & MF_MDC_CANCEL_FID1) &&
		    (fid_is_sane(&op_data->op_fid1)))
			count = mdc_resource_get_unused(exp, &op_data->op_fid1,
							&cancels, LCK_EX, bits);
	}

	req = ptlrpc_request_alloc(class_exp2cliimp(exp),
				   &RQF_LDLM_INTENT_SETATTR);
	if (req == NULL) {
		ldlm_lock_list_put(&cancels, l_bl_ast, count);
		RETURN(ERR_PTR(-ENOMEM));
	}

	req_capsule_set_size(&req->rq_pill, &RMF_MDT_EPOCH, RCL_CLIENT, 0);
	req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_CLIENT, 0);
	req_capsule_set_size(&req->rq_pill, &RMF_LOGCOOKIES, RCL_CLIENT, 0);

	rc = ldlm_prep_elc_req(exp, req, LUSTRE_DLM_VERSION, LDLM_ENQUEUE,
			       canceloff, &cancels, count);
	if (rc) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	if (parent_locked) {
		struct ldlm_request *dlm;

		dlm = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
		dlm->lock_count++;
		dlm->lock_handle[1] = op_data->op_open_handle;
	}

	/* Pack the intent */
	lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
	lit->opc = (__u64)it->it_op;

	if (op_data->op_attr.ia_valid & (ATTR_MTIME | ATTR_CTIME))
		CDEBUG(D_INODE, "setting mtime %lld, ctime %lld\n",
		       (s64)op_data->op_attr.ia_mtime.tv_sec,
		       (s64)op_data->op_attr.ia_ctime.tv_sec);
	/* TODO: EA data support. */
	mdc_setattr_pack(&req->rq_pill, op_data, NULL, 0);
	req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER, 0);

	ptlrpc_request_set_replen(req);
	RETURN(req);
}

#define GA_DEFAULT_EA_NAME_LEN	 20
#define GA_DEFAULT_EA_VAL_LEN	250
#define GA_DEFAULT_EA_NUM	 10

static struct ptlrpc_request *
mdc_intent_getxattr_pack(struct obd_export *exp, struct lookup_intent *it,
			 struct md_op_data *op_data)
{
	struct ptlrpc_request *req;
	struct ldlm_intent *lit;
	int rc, count = 0;
	LIST_HEAD(cancels);
	u32 ea_vals_buf_size = GA_DEFAULT_EA_VAL_LEN * GA_DEFAULT_EA_NUM;

	ENTRY;
	req = ptlrpc_request_alloc(class_exp2cliimp(exp),
					&RQF_LDLM_INTENT_GETXATTR);
	if (req == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	/* get SELinux policy info if any */
	rc = sptlrpc_get_sepol(req);
	if (rc < 0) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}
	req_capsule_set_size(&req->rq_pill, &RMF_SELINUX_POL, RCL_CLIENT,
			     strlen(req->rq_sepol) ?
			     strlen(req->rq_sepol) + 1 : 0);

	rc = ldlm_prep_enqueue_req(exp, req, &cancels, count);
	if (rc) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	/* pack the intent */
	lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
	lit->opc = IT_GETXATTR;
	/* Message below is checked in sanity-selinux test_20d
	 * and sanity-sec test_49
	 */
	CDEBUG(D_INFO, "%s: get xattrs for "DFID"\n",
	       exp->exp_obd->obd_name, PFID(&op_data->op_fid1));

#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(3, 0, 53, 0)
	/* If the supplied buffer is too small then the server will return
	 * -ERANGE and llite will fallback to using non cached xattr
	 * operations. On servers before 2.10.1 a (non-cached) listxattr RPC
	 * for an orphan or dead file causes an oops. So let's try to avoid
	 * sending too small a buffer to too old a server. This is effectively
	 * undoing the memory conservation of LU-9417 when it would be *more*
	 * likely to crash the server. See LU-9856.
	 */
	if (exp->exp_connect_data.ocd_version < OBD_OCD_VERSION(2, 10, 1, 0))
		ea_vals_buf_size = max_t(u32, ea_vals_buf_size,
					 exp->exp_connect_data.ocd_max_easize);
#endif

	/* pack the intended request */
	mdc_pack_body(&req->rq_pill, &op_data->op_fid1, op_data->op_valid,
		      ea_vals_buf_size, -1, 0);

	/* get SELinux policy info if any */
	mdc_file_sepol_pack(&req->rq_pill);

	req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_SERVER,
			     GA_DEFAULT_EA_NAME_LEN * GA_DEFAULT_EA_NUM);

	req_capsule_set_size(&req->rq_pill, &RMF_EAVALS, RCL_SERVER,
			     ea_vals_buf_size);

	req_capsule_set_size(&req->rq_pill, &RMF_EAVALS_LENS, RCL_SERVER,
			     sizeof(u32) * GA_DEFAULT_EA_NUM);

	req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER, 0);

	ptlrpc_request_set_replen(req);

	RETURN(req);
}

static struct ptlrpc_request *
mdc_intent_getattr_pack(struct obd_export *exp, struct lookup_intent *it,
			struct md_op_data *op_data, __u32 acl_bufsize)
{
	struct ptlrpc_request *req;
	struct obd_device *obd = class_exp2obd(exp);
	u64 valid = OBD_MD_FLGETATTR | OBD_MD_FLEASIZE | OBD_MD_FLMODEASIZE |
		    OBD_MD_FLDIREA | OBD_MD_MEA | OBD_MD_FLACL |
		    OBD_MD_DEFAULT_MEA;
	struct ldlm_intent *lit;
	__u32 easize;
	bool have_secctx = false;
	int rc;

	ENTRY;
	req = ptlrpc_request_alloc(class_exp2cliimp(exp),
				   &RQF_LDLM_INTENT_GETATTR);
	if (req == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	/* send name of security xattr to get upon intent */
	if (it->it_op & (IT_LOOKUP | IT_GETATTR) &&
	    req_capsule_has_field(&req->rq_pill, &RMF_FILE_SECCTX_NAME,
				  RCL_CLIENT) &&
	    op_data->op_file_secctx_name_size > 0 &&
	    op_data->op_file_secctx_name != NULL) {
		have_secctx = true;
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX_NAME,
				     RCL_CLIENT,
				     op_data->op_file_secctx_name_size);
	}

	req_capsule_set_size(&req->rq_pill, &RMF_NAME, RCL_CLIENT,
			     op_data->op_namelen + 1);

	rc = ldlm_prep_enqueue_req(exp, req, NULL, 0);
	if (rc) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	/* pack the intent */
	lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
	lit->opc = (__u64)it->it_op;

	if (obd->u.cli.cl_default_mds_easize > 0)
		easize = obd->u.cli.cl_default_mds_easize;
	else
		easize = obd->u.cli.cl_max_mds_easize;

	/* pack the intended request */
	mdc_getattr_pack(&req->rq_pill, valid, it->it_flags, op_data, easize);

	req_capsule_set_size(&req->rq_pill, &RMF_MDT_MD, RCL_SERVER, easize);
	req_capsule_set_size(&req->rq_pill, &RMF_ACL, RCL_SERVER, acl_bufsize);
	req_capsule_set_size(&req->rq_pill, &RMF_DEFAULT_MDT_MD, RCL_SERVER,
			     sizeof(struct lmv_user_md));

	if (have_secctx) {
		char *secctx_name;

		secctx_name = req_capsule_client_get(&req->rq_pill,
						     &RMF_FILE_SECCTX_NAME);
		memcpy(secctx_name, op_data->op_file_secctx_name,
		       op_data->op_file_secctx_name_size);

		req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX,
				     RCL_SERVER, easize);

		CDEBUG(D_SEC, "packed '%.*s' as security xattr name\n",
		       op_data->op_file_secctx_name_size,
		       op_data->op_file_secctx_name);
	} else {
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_SECCTX,
				     RCL_SERVER, 0);
	}

	if (exp_connect_encrypt(exp) && it->it_op & (IT_LOOKUP | IT_GETATTR))
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_ENCCTX,
				     RCL_SERVER, easize);
	else
		req_capsule_set_size(&req->rq_pill, &RMF_FILE_ENCCTX,
				     RCL_SERVER, 0);

	ptlrpc_request_set_replen(req);
	RETURN(req);
}

static struct ptlrpc_request *mdc_intent_layout_pack(struct obd_export *exp,
						     struct lookup_intent *it,
						     struct md_op_data *op_data)
{
	struct obd_device *obd = class_exp2obd(exp);
	struct ptlrpc_request *req;
	struct ldlm_intent *lit;
	struct layout_intent *layout;
	LIST_HEAD(cancels);
	int count = 0, rc;

	ENTRY;
	req = ptlrpc_request_alloc(class_exp2cliimp(exp),
				&RQF_LDLM_INTENT_LAYOUT);
	if (req == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	if (fid_is_sane(&op_data->op_fid2) && (it->it_op & IT_LAYOUT) &&
	    (it->it_flags & FMODE_WRITE)) {
		count = mdc_resource_get_unused(exp, &op_data->op_fid2,
						&cancels, LCK_EX,
						MDS_INODELOCK_LAYOUT);
	}

	req_capsule_set_size(&req->rq_pill, &RMF_EADATA, RCL_CLIENT, 0);
	rc = ldlm_prep_enqueue_req(exp, req, &cancels, count);
	if (rc) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	/* pack the intent */
	lit = req_capsule_client_get(&req->rq_pill, &RMF_LDLM_INTENT);
	lit->opc = (__u64)it->it_op;

	/* pack the layout intent request */
	layout = req_capsule_client_get(&req->rq_pill, &RMF_LAYOUT_INTENT);
	LASSERT(op_data->op_data != NULL);
	LASSERT(op_data->op_data_size == sizeof(*layout));
	memcpy(layout, op_data->op_data, sizeof(*layout));

	req_capsule_set_size(&req->rq_pill, &RMF_DLM_LVB, RCL_SERVER,
			     obd->u.cli.cl_default_mds_easize);
	ptlrpc_request_set_replen(req);
	RETURN(req);
}

static struct ptlrpc_request *mdc_enqueue_pack(struct obd_export *exp,
					       int lvb_len)
{
	struct ptlrpc_request *req;
	int rc;

	ENTRY;

	req = ptlrpc_request_alloc(class_exp2cliimp(exp), &RQF_LDLM_ENQUEUE);
	if (req == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	rc = ldlm_prep_enqueue_req(exp, req, NULL, 0);
	if (rc) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	req_capsule_set_size(&req->rq_pill, &RMF_DLM_LVB, RCL_SERVER, lvb_len);
	ptlrpc_request_set_replen(req);
	RETURN(req);
}

static struct ptlrpc_request *
mdc_wbc_exlock_pack(struct obd_export *exp, struct md_op_data *op_data,
		    __u64 extra_lock_flags)
{
	bool parent_locked = extra_lock_flags & LDLM_FL_INTENT_PARENT_LOCKED;
	int canceloff = LDLM_ENQUEUE_CANCEL_OFF;
	struct ptlrpc_request *req;
	LIST_HEAD(cancels);
	int rc;

	ENTRY;

	req = ptlrpc_request_alloc(class_exp2cliimp(exp), &RQF_LDLM_ENQUEUE);
	if (req == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	if (parent_locked)
		canceloff += 1;

	rc = ldlm_prep_elc_req(exp, req, LUSTRE_DLM_VERSION, LDLM_ENQUEUE,
			       canceloff, NULL, 0);
	if (rc) {
		ptlrpc_request_free(req);
		RETURN(ERR_PTR(rc));
	}

	if (parent_locked) {
		struct ldlm_request *dlm;

		dlm = req_capsule_client_get(&req->rq_pill, &RMF_DLM_REQ);
		dlm->lock_count++;
		dlm->lock_handle[1] = op_data->op_open_handle;
	}

	req_capsule_set_size(&req->rq_pill, &RMF_DLM_LVB, RCL_SERVER, 0);
	ptlrpc_request_set_replen(req);
	RETURN(req);
}

int mdc_finish_enqueue(struct obd_export *exp,
		       struct req_capsule *pill,
		       struct ldlm_enqueue_info *einfo,
		       struct lookup_intent *it,
		       struct lustre_handle *lockh, int rc)
{
	struct ptlrpc_request *req = NULL;
	struct ldlm_request *lockreq;
	struct ldlm_reply *lockrep;
	struct ldlm_lock *lock;
	struct mdt_body *body = NULL;
	void *lvb_data = NULL;
	__u32 lvb_len = 0;

	ENTRY;

	LASSERT(rc >= 0);

	if (req_capsule_ptlreq(pill))
		req = pill->rc_req;
	/* Similarly, if we're going to replay this request, we don't want to
	 * actually get a lock, just perform the intent.
	 */
	if (req && (req->rq_transno || req->rq_replay)) {
		lockreq = req_capsule_client_get(pill, &RMF_DLM_REQ);
		lockreq->lock_flags |= ldlm_flags_to_wire(LDLM_FL_INTENT_ONLY);
	}

	if (rc == ELDLM_LOCK_ABORTED) {
		einfo->ei_mode = 0;
		memset(lockh, 0, sizeof(*lockh));
		rc = 0;
	} else { /* rc = 0 */
		lock = ldlm_handle2lock(lockh);
		LASSERT(lock != NULL);

		/* If server returned a different lock mode, fix up variables */
		if (lock->l_req_mode != einfo->ei_mode) {
			ldlm_lock_addref(lockh, lock->l_req_mode);
			ldlm_lock_decref(lockh, einfo->ei_mode);
			einfo->ei_mode = lock->l_req_mode;
		}
		LDLM_LOCK_PUT(lock);
	}

	lockrep = req_capsule_server_get(pill, &RMF_DLM_REP);
	LASSERT(lockrep != NULL); /* checked by ldlm_cli_enqueue() */

	it->it_disposition = (int)lockrep->lock_policy_res1;
	it->it_status = (int)lockrep->lock_policy_res2;
	it->it_lock_mode = einfo->ei_mode;
	it->it_lock_handle = lockh->cookie;
	it->it_request = req;

	/* Technically speaking rq_transno must already be zero if
	 * it_status is in error, so the check is a bit redundant.
	 */
	if (req && (!req->rq_transno || it->it_status < 0) && req->rq_replay)
		mdc_clear_replay_flag(req, it->it_status);

	/* If we're doing an IT_OPEN which did not result in an actual
	 * successful open, then we need to remove the bit which saves
	 * this request for unconditional replay.
	 *
	 * It's important that we do this first!  Otherwise we might exit the
	 * function without doing so, and try to replay a failed create.
	 * (b=3440)
	 */
	if (it->it_op & IT_OPEN && req && req->rq_replay &&
	    (!it_disposition(it, DISP_OPEN_OPEN) || it->it_status != 0))
		mdc_clear_replay_flag(req, it->it_status);

	DEBUG_REQ(D_RPCTRACE, pill->rc_req, "op=%x disposition=%x, status=%d",
		  it->it_op, it->it_disposition, it->it_status);

	/* We know what to expect, so we do any byte flipping required here */
	if (it_has_reply_body(it)) {
		body = req_capsule_server_get(pill, &RMF_MDT_BODY);
		if (body == NULL) {
			rc = -EPROTO;
			CERROR("%s: cannot swab mdt_body: rc = %d\n",
			       exp->exp_obd->obd_name, rc);
			RETURN(rc);
		}

		if (it_disposition(it, DISP_OPEN_OPEN) &&
		    !it_open_error(DISP_OPEN_OPEN, it)) {
			/*
			 * If this is a successful OPEN request, we need to set
			 * replay handler and data early, so that if replay
			 * happens immediately after swabbing below, new reply
			 * is swabbed by that handler correctly.
			 */
			mdc_set_open_replay_data(NULL, NULL, it);
		}

		if (it_disposition(it, DISP_OPEN_CREATE) &&
		    !it_open_error(DISP_OPEN_CREATE, it)) {
			lprocfs_counter_incr(exp->exp_obd->obd_md_stats,
					     LPROC_MD_CREATE);
		}

		if (body->mbo_valid & (OBD_MD_FLDIREA | OBD_MD_FLEASIZE)) {
			void *eadata;

			mdc_update_max_ea_from_body(exp, body);

			/*
			 * The eadata is opaque; just check that it is there.
			 * Eventually, obd_unpackmd() will check the contents.
			 */
			eadata = req_capsule_server_sized_get(pill, &RMF_MDT_MD,
							body->mbo_eadatasize);
			if (eadata == NULL)
				RETURN(-EPROTO);

			/* save LVB data and length if for layout lock */
			lvb_data = eadata;
			lvb_len = body->mbo_eadatasize;

			/*
			 * We save the reply LOV EA in case we have to replay a
			 * create for recovery.  If we didn't allocate a large
			 * enough request buffer above we need to reallocate it
			 * here to hold the actual LOV EA.
			 *
			 * To not save LOV EA if request is not going to replay
			 * (for example error one).
			 */
			if ((it->it_op & IT_OPEN) && req && req->rq_replay) {
				rc = mdc_save_lovea(req, eadata,
						    body->mbo_eadatasize);
				if (rc) {
					body->mbo_valid &= ~OBD_MD_FLEASIZE;
					body->mbo_eadatasize = 0;
					rc = 0;
				}
			}
		}
	} else if (it->it_op & IT_LAYOUT) {
		/* maybe the lock was granted right away and layout
		 * is packed into RMF_DLM_LVB of req
		 */
		LASSERT(req != NULL);
		lvb_len = req_capsule_get_size(pill, &RMF_DLM_LVB, RCL_SERVER);
		CDEBUG(D_INFO, "%s: layout return lvb %d transno %lld\n",
		       class_exp2obd(exp)->obd_name, lvb_len, req->rq_transno);
		if (lvb_len > 0) {
			lvb_data = req_capsule_server_sized_get(pill,
							&RMF_DLM_LVB, lvb_len);
			if (lvb_data == NULL)
				RETURN(-EPROTO);

			/**
			 * save replied layout data to the request buffer for
			 * recovery consideration (lest MDS reinitialize
			 * another set of OST objects).
			 */
			if (req->rq_transno)
				(void)mdc_save_lovea(req, lvb_data, lvb_len);
		}
	}

	/* fill in stripe data for layout lock.
	 * LU-6581: trust layout data only if layout lock is granted. The MDT
	 * has stopped sending layout unless the layout lock is granted. The
	 * client still does this checking in case it's talking with an old
	 * server. - Jinshan
	 */
	lock = ldlm_handle2lock(lockh);
	if (lock == NULL)
		RETURN(rc);

	if (ldlm_has_layout(lock) && lvb_data != NULL &&
	    !(lockrep->lock_flags & LDLM_FL_BLOCKED_MASK)) {
		void *lmm;

		LDLM_DEBUG(lock, "layout lock returned by: %s, lvb_len: %d",
			ldlm_it2str(it->it_op), lvb_len);

		OBD_ALLOC_LARGE(lmm, lvb_len);
		if (lmm == NULL)
			GOTO(out_lock, rc = -ENOMEM);

		memcpy(lmm, lvb_data, lvb_len);

		/* install lvb_data */
		lock_res_and_lock(lock);
		if (lock->l_lvb_data == NULL) {
			lock->l_lvb_type = LVB_T_LAYOUT;
			lock->l_lvb_data = lmm;
			lock->l_lvb_len = lvb_len;
			lmm = NULL;
		}
		unlock_res_and_lock(lock);
		if (lmm != NULL)
			OBD_FREE_LARGE(lmm, lvb_len);
	}

	if (ldlm_has_dom(lock)) {
		LASSERT(lock->l_glimpse_ast == mdc_ldlm_glimpse_ast);

		body = req_capsule_server_get(pill, &RMF_MDT_BODY);
		if (!(body->mbo_valid & OBD_MD_DOM_SIZE)) {
			LDLM_ERROR(lock, "%s: DoM lock without size.",
				   exp->exp_obd->obd_name);
			GOTO(out_lock, rc = -EPROTO);
		}

		LDLM_DEBUG(lock, "DoM lock is returned by: %s, size: %llu",
			   ldlm_it2str(it->it_op), body->mbo_dom_size);

		lock_res_and_lock(lock);
		mdc_body2lvb(body, &lock->l_ost_lvb);
		ldlm_lock_allow_match_locked(lock);
		unlock_res_and_lock(lock);
	}
out_lock:
	LDLM_LOCK_PUT(lock);

	RETURN(rc);
}

static inline bool mdc_skip_mod_rpc_slot(const struct lookup_intent *it)
{
	if (it != NULL &&
	    (it->it_op == IT_GETATTR || it->it_op == IT_LOOKUP ||
	     it->it_op == IT_READDIR ||
	     (it->it_op == IT_LAYOUT && !(it->it_flags & MDS_FMODE_WRITE))))
		return true;
	return false;
}

/* We always reserve enough space in the reply packet for a stripe MD, because
 * we don't know in advance the file type.
 */
static int mdc_enqueue_base(struct obd_export *exp,
			    struct ldlm_enqueue_info *einfo,
			    const union ldlm_policy_data *policy,
			    struct lookup_intent *it,
			    struct md_op_data *op_data,
			    struct lustre_handle *lockh,
			    __u64 extra_lock_flags,
			    int async)
{
	struct obd_device *obd = class_exp2obd(exp);
	struct ptlrpc_request *req;
	__u64 flags, saved_flags = extra_lock_flags;
	struct ldlm_res_id res_id;
	static const union ldlm_policy_data lookup_policy = {
				  .l_inodebits = { MDS_INODELOCK_LOOKUP } };
	static const union ldlm_policy_data update_policy = {
				  .l_inodebits = { MDS_INODELOCK_UPDATE } };
	static const union ldlm_policy_data layout_policy = {
				  .l_inodebits = { MDS_INODELOCK_LAYOUT } };
	static const union ldlm_policy_data getxattr_policy = {
				  .l_inodebits = { MDS_INODELOCK_XATTR } };
	int generation, resends = 0;
	struct ldlm_reply *lockrep;
	struct obd_import *imp = class_exp2cliimp(exp);
	__u32 acl_bufsize;
	enum lvb_type lvb_type = 0;
	int rc;

	ENTRY;
	LASSERTF(!it || einfo->ei_type == LDLM_IBITS, "lock type %d\n",
		 einfo->ei_type);
	fid_build_reg_res_name(&op_data->op_fid1, &res_id);

	if (it != NULL) {
		LASSERT(policy == NULL);

		saved_flags |= LDLM_FL_HAS_INTENT;
		if (it->it_op & (IT_GETATTR | IT_READDIR |
				 IT_CREAT | IT_WBC_EXLOCK))
			policy = &update_policy;
		else if (it->it_op & IT_LAYOUT)
			policy = &layout_policy;
		else if (it->it_op & IT_GETXATTR)
			policy = &getxattr_policy;
		else
			policy = &lookup_policy;
	}

	generation = obd->u.cli.cl_import->imp_generation;
	if (!it || (it->it_op & (IT_OPEN | IT_CREAT)))
		acl_bufsize = min_t(__u32, imp->imp_connect_data.ocd_max_easize,
				    XATTR_SIZE_MAX);
	else
		acl_bufsize = LUSTRE_POSIX_ACL_MAX_SIZE_OLD;

resend:
	flags = saved_flags;
	if (it == NULL) {
		/* The only way right now is FLOCK. */
		LASSERTF(einfo->ei_type == LDLM_FLOCK, "lock type %d\n",
			 einfo->ei_type);
		res_id.name[3] = LDLM_FLOCK;
		req = ldlm_enqueue_pack(exp, 0);
	} else if (it->it_op & IT_OPEN) {
		req = mdc_intent_open_pack(exp, it, op_data, acl_bufsize);
	} else if (it->it_op & (IT_GETATTR | IT_LOOKUP)) {
		req = mdc_intent_getattr_pack(exp, it, op_data, acl_bufsize);
	} else if (it->it_op & IT_READDIR) {
		req = mdc_enqueue_pack(exp, 0);
	} else if (it->it_op & IT_LAYOUT) {
		if (!imp_connect_lvb_type(imp))
			RETURN(-EOPNOTSUPP);
		req = mdc_intent_layout_pack(exp, it, op_data);
		lvb_type = LVB_T_LAYOUT;
	} else if (it->it_op & IT_GETXATTR) {
		req = mdc_intent_getxattr_pack(exp, it, op_data);
	} else if (it->it_op == IT_CREAT) {
		req = mdc_intent_create_pack(exp, it, op_data, acl_bufsize,
					     extra_lock_flags);
	} else if (it->it_op == IT_SETATTR) {
		req = mdc_intent_setattr_pack(exp, it, op_data,
					      extra_lock_flags);
	} else if (it->it_op == IT_WBC_EXLOCK) {
		LASSERT(extra_lock_flags & LDLM_FL_INTENT_EXLOCK_UPDATE);
		/* Enqueue lock only, no intent. */
		flags &= ~LDLM_FL_HAS_INTENT;
		req = mdc_wbc_exlock_pack(exp, op_data, extra_lock_flags);
	} else {
		LBUG();
		RETURN(-EINVAL);
	}

	if (IS_ERR(req))
		RETURN(PTR_ERR(req));

	if (resends) {
		req->rq_generation_set = 1;
		req->rq_import_generation = generation;
		req->rq_sent = ktime_get_real_seconds() + resends;
	}

	einfo->ei_enq_slot = !(mdc_skip_mod_rpc_slot(it) || async);

	/* With Data-on-MDT the glimpse callback is needed too.
	 * It is set here in advance but not in mdc_finish_enqueue()
	 * to avoid possible races. It is safe to have glimpse handler
	 * for non-DOM locks and costs nothing.
	 */
	if (einfo->ei_cb_gl == NULL)
		einfo->ei_cb_gl = mdc_ldlm_glimpse_ast;

	rc = ldlm_cli_enqueue(exp, &req, einfo, &res_id, policy, &flags, NULL,
			      0, lvb_type, lockh, async);
	if (!it) {
		/* For flock requests we immediatelly return without further
		 * delay and let caller deal with the rest, since rest of
		 * this function metadata processing makes no sense for flock
		 * requests anyway. But in case of problem during comms with
		 * server (-ETIMEDOUT) or any signal/kill attempt (-EINTR),
		 * we cannot rely on caller and this mainly for F_UNLCKs
		 * (explicits or automatically generated by kernel to clean
		 * current flocks upon exit) that can't be trashed.
		 */
		ptlrpc_req_finished(req);
		if (((rc == -EINTR) || (rc == -ETIMEDOUT)) &&
		    (einfo->ei_type == LDLM_FLOCK) &&
		    (einfo->ei_mode == LCK_NL))
			goto resend;
		RETURN(rc);
	}

	if (async) {
		it->it_request = req;
		RETURN(rc);
	}

	if (rc < 0) {
		CDEBUG(D_INFO,
		      "%s: ldlm_cli_enqueue "DFID":"DFID"=%s failed: rc = %d\n",
		      obd->obd_name, PFID(&op_data->op_fid1),
		      PFID(&op_data->op_fid2), op_data->op_name ?: "", rc);

		mdc_clear_replay_flag(req, rc);
		ptlrpc_req_finished(req);
		RETURN(rc);
	}

	lockrep = req_capsule_server_get(&req->rq_pill, &RMF_DLM_REP);
	LASSERT(lockrep != NULL);

	lockrep->lock_policy_res2 =
		ptlrpc_status_ntoh(lockrep->lock_policy_res2);

	/* Retry infinitely when the server returns -EINPROGRESS for the
	 * intent operation, when server returns -EINPROGRESS for acquiring
	 * intent lock, we'll retry in after_reply().
	 */
	if (it && (int)lockrep->lock_policy_res2 == -EINPROGRESS) {
		mdc_clear_replay_flag(req, rc);
		ptlrpc_req_finished(req);
		if (generation == obd->u.cli.cl_import->imp_generation) {
			if (signal_pending(current))
				RETURN(-EINTR);

			resends++;
			CDEBUG(D_HA, "%s: resend:%d op:%d "DFID"/"DFID"\n",
			       obd->obd_name, resends, it->it_op,
			       PFID(&op_data->op_fid1),
			       PFID(&op_data->op_fid2));
			goto resend;
		} else {
			CDEBUG(D_HA, "resend cross eviction\n");
			RETURN(-EIO);
		}
	}

	if ((int)lockrep->lock_policy_res2 == -ERANGE &&
	    it->it_op & (IT_OPEN | IT_GETATTR | IT_LOOKUP) &&
	    acl_bufsize == LUSTRE_POSIX_ACL_MAX_SIZE_OLD) {
		mdc_clear_replay_flag(req, -ERANGE);
		ptlrpc_req_finished(req);
		acl_bufsize = min_t(__u32, imp->imp_connect_data.ocd_max_easize,
				    XATTR_SIZE_MAX);
		goto resend;
	}

	rc = mdc_finish_enqueue(exp, &req->rq_pill, einfo, it, lockh, rc);
	if (rc < 0) {
		if (lustre_handle_is_used(lockh)) {
			ldlm_lock_decref(lockh, einfo->ei_mode);
			memset(lockh, 0, sizeof(*lockh));
		}
		ptlrpc_req_finished(req);

		it->it_lock_handle = 0;
		it->it_lock_mode = 0;
		it->it_request = NULL;
	}

	RETURN(rc);
}

int mdc_enqueue(struct obd_export *exp, struct ldlm_enqueue_info *einfo,
		const union ldlm_policy_data *policy,
		struct md_op_data *op_data,
		struct lustre_handle *lockh, __u64 extra_lock_flags)
{
	return mdc_enqueue_base(exp, einfo, policy, NULL,
				op_data, lockh, extra_lock_flags, 0);
}

static int mdc_finish_intent_lock(struct obd_export *exp,
				  struct ptlrpc_request *request,
				  struct md_op_data *op_data,
				  struct lookup_intent *it,
				  struct lustre_handle *lockh)
{
	struct lustre_handle old_lock;
	struct ldlm_lock *lock;
	int rc = 0;

	ENTRY;
	LASSERT(request != NULL);
	LASSERT(request != LP_POISON);
	LASSERT(request->rq_repmsg != LP_POISON);

	if (it->it_op & IT_READDIR)
		RETURN(0);

	if (it->it_op & (IT_GETXATTR | IT_LAYOUT |
			 IT_SETATTR | IT_WBC_EXLOCK)) {
		if (it->it_status != 0)
			GOTO(out, rc = it->it_status);
	} else {
		if (!it_disposition(it, DISP_IT_EXECD)) {
			/* The server failed before it even started executing
			 * the intent, i.e. because it couldn't unpack the
			 * request.
			 */
			LASSERT(it->it_status != 0);
			GOTO(out, rc = it->it_status);
		}
		rc = it_open_error(DISP_IT_EXECD, it);
		if (rc)
			GOTO(out, rc);

		rc = it_open_error(DISP_LOOKUP_EXECD, it);
		if (rc)
			GOTO(out, rc);

		/* keep requests around for the multiple phases of the call
		 * this shows the DISP_XX must guarantee we make it into the
		 * call
		 */
		if (!it_disposition(it, DISP_ENQ_CREATE_REF) &&
		    it_disposition(it, DISP_OPEN_CREATE) &&
		    !it_open_error(DISP_OPEN_CREATE, it)) {
			it_set_disposition(it, DISP_ENQ_CREATE_REF);
			/* balanced in ll_create_node */
			ptlrpc_request_addref(request);
		}
		if (!it_disposition(it, DISP_ENQ_OPEN_REF) &&
		    it_disposition(it, DISP_OPEN_OPEN) &&
		    !it_open_error(DISP_OPEN_OPEN, it)) {
			it_set_disposition(it, DISP_ENQ_OPEN_REF);
			/* balanced in ll_file_open */
			ptlrpc_request_addref(request);
			/* eviction in middle of open RPC processing b=11546 */
			OBD_FAIL_TIMEOUT(OBD_FAIL_MDC_ENQUEUE_PAUSE,
					 obd_timeout);
		}

		if (it->it_op & IT_CREAT) {
			/* XXX this belongs in ll_create_it */
		} else if (it->it_op == IT_OPEN) {
			LASSERT(!it_disposition(it, DISP_OPEN_CREATE));
		} else {
			LASSERT(it->it_op & (IT_GETATTR | IT_LOOKUP));
		}
	}

	/* If we already have a matching lock, then cancel the new
	 * one.  We have to set the data here instead of in
	 * mdc_enqueue, because we need to use the child's inode as
	 * the l_ast_data to match, and that's not available until
	 * intent_finish has performed the iget().
	 */
	lock = ldlm_handle2lock(lockh);
	if (lock) {
		union ldlm_policy_data policy = lock->l_policy_data;

		LDLM_DEBUG(lock, "matching against this");

		if (it_has_reply_body(it)) {
			struct mdt_body *body;

			body = req_capsule_server_get(&request->rq_pill,
						      &RMF_MDT_BODY);
			/* mdc_enqueue checked */
			LASSERT(body != NULL);
			LASSERTF(fid_res_name_eq(&body->mbo_fid1,
						 &lock->l_resource->lr_name),
				 "Lock res_id: "DLDLMRES", fid: "DFID"\n",
				 PLDLMRES(lock->l_resource),
				 PFID(&body->mbo_fid1));
		}
		LDLM_LOCK_PUT(lock);

		memcpy(&old_lock, lockh, sizeof(*lockh));
		if (ldlm_lock_match(NULL, LDLM_FL_BLOCK_GRANTED, NULL,
				   LDLM_IBITS, &policy, LCK_NL, &old_lock)) {
			ldlm_lock_decref_and_cancel(lockh, it->it_lock_mode);
			memcpy(lockh, &old_lock, sizeof(old_lock));
			it->it_lock_handle = lockh->cookie;
		}
	}

	EXIT;
out:
	CDEBUG(D_DENTRY,
	       "D_IT dentry=%.*s intent=%s status=%d disp=%x: rc = %d\n",
		(int)op_data->op_namelen, op_data->op_name,
		ldlm_it2str(it->it_op), it->it_status, it->it_disposition, rc);

	return rc;
}

int mdc_revalidate_lock(struct obd_export *exp, struct lookup_intent *it,
			struct lu_fid *fid, __u64 *bits)
{
	/* We could just return 1 immediately, but as we should only be called
	 * in revalidate_it if we already have a lock, let's verify that.
	 */
	struct ldlm_res_id res_id;
	struct lustre_handle lockh;
	union ldlm_policy_data policy;
	enum ldlm_mode mode;

	ENTRY;
	if (it->it_lock_handle) {
		lockh.cookie = it->it_lock_handle;
		mode = ldlm_revalidate_lock_handle(&lockh, bits);
	} else {
		fid_build_reg_res_name(fid, &res_id);
		switch (it->it_op) {
		case IT_GETATTR:
			/* File attributes are held under multiple bits:
			 * nlink is under lookup lock, size and times are
			 * under UPDATE lock and recently we've also got
			 * a separate permissions lock for owner/group/acl that
			 * were protected by lookup lock before.
			 * Getattr must provide all of that information,
			 * so we need to ensure we have all of those locks.
			 * Unfortunately, if the bits are split across multiple
			 * locks, there's no easy way to match all of them here,
			 * so an extra RPC would be performed to fetch all
			 * of those bits at once for now.
			 */
			/* For new MDTs(> 2.4), UPDATE|PERM should be enough,
			 * but for old MDTs (< 2.4), permission is covered
			 * by LOOKUP lock, so it needs to match all bits here.
			 */
			policy.l_inodebits.bits = MDS_INODELOCK_UPDATE |
						  MDS_INODELOCK_PERM;
			break;
		case IT_READDIR:
			policy.l_inodebits.bits = MDS_INODELOCK_UPDATE;
			break;
		case IT_LAYOUT:
			policy.l_inodebits.bits = MDS_INODELOCK_LAYOUT;
			break;
		default:
			policy.l_inodebits.bits = MDS_INODELOCK_LOOKUP;
			break;
		}

		mode = mdc_lock_match(exp, LDLM_FL_BLOCK_GRANTED, fid,
				      LDLM_IBITS, &policy,
				      LCK_CR | LCK_CW | LCK_PR |
				      LCK_PW | LCK_EX, &lockh);
	}

	if (mode) {
		it->it_lock_handle = lockh.cookie;
		it->it_lock_mode = mode;
	} else {
		it->it_lock_handle = 0;
		it->it_lock_mode = 0;
	}

	RETURN(!!mode);
}

/*
 * This long block is all about fixing up the lock and request state
 * so that it is correct as of the moment _before_ the operation was
 * applied; that way, the VFS will think that everything is normal and
 * call Lustre's regular VFS methods.
 *
 * If we're performing a creation, that means that unless the creation
 * failed with EEXIST, we should fake up a negative dentry.
 *
 * For everything else, we want to lookup to succeed.
 *
 * One additional note: if CREATE or OPEN succeeded, we add an extra
 * reference to the request because we need to keep it around until
 * ll_create/ll_open gets called.
 *
 * The server will return to us, in it_disposition, an indication of
 * exactly what it_status refers to.
 *
 * If DISP_OPEN_OPEN is set, then it_status refers to the open() call,
 * otherwise if DISP_OPEN_CREATE is set, then it status is the
 * creation failure mode.  In either case, one of DISP_LOOKUP_NEG or
 * DISP_LOOKUP_POS will be set, indicating whether the child lookup
 * was successful.
 *
 * Else, if DISP_LOOKUP_EXECD then it_status is the rc of the
 * child lookup.
 */
int mdc_intent_lock(struct obd_export *exp, struct md_op_data *op_data,
		    struct lookup_intent *it, struct ptlrpc_request **reqp,
		    ldlm_blocking_callback cb_blocking, __u64 extra_lock_flags)
{
	struct ldlm_enqueue_info einfo = {
		.ei_type	= LDLM_IBITS,
		.ei_mode	= it_to_lock_mode(it),
		.ei_cb_bl	= cb_blocking,
		.ei_cb_cp	= ldlm_completion_ast,
		.ei_cb_gl	= mdc_ldlm_glimpse_ast,
	};
	struct lustre_handle lockh;
	int rc = 0;

	ENTRY;
	LASSERT(it);
	CDEBUG(D_DLMTRACE, "(name: %.*s,"DFID") in obj "DFID
		", intent: %s flags %#llo\n", (int)op_data->op_namelen,
		op_data->op_name, PFID(&op_data->op_fid2),
		PFID(&op_data->op_fid1), ldlm_it2str(it->it_op),
		it->it_flags);

	lockh.cookie = 0;
	if (fid_is_sane(&op_data->op_fid2) &&
	    (it->it_op & (IT_LOOKUP | IT_GETATTR | IT_READDIR))) {
		/* We could just return 1 immediately, but since we should only
		 * be called in revalidate_it if we already have a lock, let's
		 * verify that.
		 */
		it->it_lock_handle = 0;
		rc = mdc_revalidate_lock(exp, it, &op_data->op_fid2, NULL);
		/* Only return failure if it was not GETATTR by cfid
		 * (from inode_revalidate()).
		 */
		if (rc || op_data->op_namelen != 0)
			RETURN(rc);
	}

	/* For case if upper layer did not alloc fid, do it now. */
	if (!fid_is_sane(&op_data->op_fid2) && it->it_op & IT_CREAT) {
		rc = mdc_fid_alloc(NULL, exp, &op_data->op_fid2, op_data);
		if (rc < 0) {
			CERROR("%s: cannot allocate new FID: rc=%d\n",
			       exp->exp_obd->obd_name, rc);
			RETURN(rc);
		}
	}

	rc = mdc_enqueue_base(exp, &einfo, NULL, it, op_data, &lockh,
			      extra_lock_flags, 0);
	if (rc < 0)
		RETURN(rc);

	*reqp = it->it_request;
	rc = mdc_finish_intent_lock(exp, *reqp, op_data, it, &lockh);
	RETURN(rc);
}

static int mdc_intent_getattr_async_interpret(const struct lu_env *env,
					      struct ptlrpc_request *req,
					      void *args, int rc)
{
	struct mdc_getattr_args *ga = args;
	struct obd_export *exp = ga->ga_exp;
	struct md_op_item *item = ga->ga_item;
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;
	struct lookup_intent *it = &item->mop_it;
	struct lustre_handle *lockh = &item->mop_lockh;
	struct req_capsule *pill = &req->rq_pill;
	struct ldlm_reply *lockrep;
	__u64 flags = LDLM_FL_HAS_INTENT;

	ENTRY;
	if (OBD_FAIL_CHECK(OBD_FAIL_MDC_GETATTR_ENQUEUE))
		rc = -ETIMEDOUT;

	rc = ldlm_cli_enqueue_fini(exp, pill, einfo, 1, &flags, NULL, 0,
				   lockh, rc);
	if (rc < 0) {
		CERROR("%s: ldlm_cli_enqueue_fini() failed: rc = %d\n",
		       exp->exp_obd->obd_name, rc);
		mdc_clear_replay_flag(req, rc);
		GOTO(out, rc);
	}

	lockrep = req_capsule_server_get(pill, &RMF_DLM_REP);
	LASSERT(lockrep != NULL);

	lockrep->lock_policy_res2 =
		ptlrpc_status_ntoh(lockrep->lock_policy_res2);

	rc = mdc_finish_enqueue(exp, pill, einfo, it, lockh, rc);
	if (rc)
		GOTO(out, rc);

	rc = mdc_finish_intent_lock(exp, req, &item->mop_data, it, lockh);
	EXIT;

out:
	item->mop_cb(pill, item, rc);
	return 0;
}

int mdc_intent_getattr_async(struct obd_export *exp,
			     struct md_op_item *item)
{
	struct md_op_data *op_data = &item->mop_data;
	struct lookup_intent *it = &item->mop_it;
	struct ptlrpc_request *req;
	struct mdc_getattr_args *ga;
	struct ldlm_res_id res_id;
	union ldlm_policy_data policy = {
		.l_inodebits = { MDS_INODELOCK_LOOKUP | MDS_INODELOCK_UPDATE }
	};
	__u64 flags = LDLM_FL_HAS_INTENT;
	int rc = 0;

	ENTRY;
	CDEBUG(D_DLMTRACE,
	       "name: %.*s in inode "DFID", intent: %s flags %#llo\n",
	       (int)op_data->op_namelen, op_data->op_name,
	       PFID(&op_data->op_fid1), ldlm_it2str(it->it_op), it->it_flags);

	fid_build_reg_res_name(&op_data->op_fid1, &res_id);
	/* If the MDT return -ERANGE because of large ACL, then the sponsor
	 * of the async getattr RPC will handle that by itself.
	 */
	req = mdc_intent_getattr_pack(exp, it, op_data,
				      LUSTRE_POSIX_ACL_MAX_SIZE_OLD);
	if (IS_ERR(req))
		RETURN(PTR_ERR(req));

	/* With Data-on-MDT the glimpse callback is needed too.
	 * It is set here in advance but not in mdc_finish_enqueue()
	 * to avoid possible races. It is safe to have glimpse handler
	 * for non-DOM locks and costs nothing.
	 */
	if (item->mop_einfo.ei_cb_gl == NULL)
		item->mop_einfo.ei_cb_gl = mdc_ldlm_glimpse_ast;

	rc = ldlm_cli_enqueue(exp, &req, &item->mop_einfo, &res_id, &policy,
			      &flags, NULL, 0, LVB_T_NONE, &item->mop_lockh, 1);
	if (rc < 0) {
		ptlrpc_req_finished(req);
		RETURN(rc);
	}

	ga = ptlrpc_req_async_args(ga, req);
	ga->ga_exp = exp;
	ga->ga_item = item;

	req->rq_interpret_reply = mdc_intent_getattr_async_interpret;
	ptlrpcd_add_req(req);

	RETURN(0);
}

struct mdc_intent_lock_args {
	struct obd_export	*ita_exp;
	struct md_op_item	*ita_item;
};

static int mdc_intent_lock_async_interpret(const struct lu_env *env,
					   struct ptlrpc_request *req,
					   void *args, int rc)
{
	struct mdc_intent_lock_args *aa = args;
	struct obd_export *exp = aa->ita_exp;
	struct md_op_item *item = aa->ita_item;
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;
	struct lustre_handle *lockh = &item->mop_lockh;
	struct lookup_intent *it = &item->mop_it;
	struct ldlm_reply *lockrep;
	__u64 flags = item->mop_lock_flags;

	ENTRY;

	rc = ldlm_cli_enqueue_fini(exp, &req->rq_pill, einfo, 1,
				   &flags, NULL, 0, lockh, rc);
	if (rc < 0) {
		CERROR("ldlm_cli_enqueue_fini: %d\n", rc);
		mdc_clear_replay_flag(req, rc);
		GOTO(out, rc);
	}

	lockrep = req_capsule_server_get(&req->rq_pill, &RMF_DLM_REP);
	LASSERT(lockrep != NULL);

	lockrep->lock_policy_res2 =
			ptlrpc_status_ntoh(lockrep->lock_policy_res2);

	rc = mdc_finish_enqueue(exp, &req->rq_pill, einfo, it, lockh, rc);
	if (rc)
		GOTO(out, rc);

	rc = mdc_finish_intent_lock(exp, req, &item->mop_data, it, lockh);

out:
	item->mop_cb(&req->rq_pill, item, rc);
	RETURN(0);
}

int mdc_intent_lock_async(struct obd_export *exp,
			  struct md_op_item *item,
			  struct ptlrpc_request_set *rqset)
{
	struct md_op_data *op_data = &item->mop_data;
	struct lookup_intent *it = &item->mop_it;
	struct mdc_intent_lock_args *aa;
	int rc;

	ENTRY;

	CDEBUG(D_DLMTRACE, "(name: %.*s,"DFID") in obj "DFID
		", intent: %s flags %#llo\n", (int)op_data->op_namelen,
		op_data->op_name, PFID(&op_data->op_fid2),
		PFID(&op_data->op_fid1), ldlm_it2str(it->it_op),
		it->it_flags);

	if (fid_is_sane(&op_data->op_fid2) &&
	    (it->it_op & (IT_LOOKUP | IT_GETATTR | IT_READDIR))) {
		/*
		 * We could just return 1 immediately, but since we should only
		 * be called in revalidate_it if we already have a lock, let's
		 * verify that.
		 */
		it->it_lock_handle = 0;
		rc = mdc_revalidate_lock(exp, it, &op_data->op_fid2, NULL);
		/*
		 * Only return failure if it was not GETATTR by cfid
		 * (from inode_revalidate)
		 */
		if (rc || op_data->op_namelen != 0)
			RETURN(rc);
	}

	/* For case if upper layer did not alloc fid, do it now. */
	if (!fid_is_sane(&op_data->op_fid2) && it->it_op & IT_CREAT) {
		rc = mdc_fid_alloc(NULL, exp, &op_data->op_fid2, op_data);
		if (rc < 0) {
			CERROR("Can't alloc new fid, rc %d\n", rc);
			RETURN(rc);
		}
	}

	rc = mdc_enqueue_base(exp, &item->mop_einfo, NULL, it, op_data,
			      &item->mop_lockh, item->mop_lock_flags, 1);
	if (rc < 0)
		RETURN(rc);

	LASSERT(it->it_request != NULL);
	it->it_request->rq_interpret_reply = mdc_intent_lock_async_interpret;
	aa = ptlrpc_req_async_args(aa, it->it_request);
	aa->ita_exp = exp;
	aa->ita_item = item;

	if (rqset) {
		ptlrpc_set_add_req(rqset, it->it_request);
		ptlrpc_check_set(NULL, rqset);
	} else {
		ptlrpcd_add_req(it->it_request);
	}

	RETURN(0);
}
