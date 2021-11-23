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
 * Copyright (c) 2020, DDN Storage Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */
/*
 * lustre/mdc/mdc_batch.c
 *
 * Batch Metadata Updating on the client (MDC)
 *
 * Author: Qian Yingjin <qian@ddn.com>
 */

#define DEBUG_SUBSYSTEM S_MDC

#include <linux/module.h>
#include <lustre_update.h>
#include <lustre_acl.h>

#include "mdc_internal.h"

struct batch_update_buffer {
	struct batch_update_request	*bub_req;
	size_t				 bub_size;
	size_t				 bub_end;
	struct list_head		 bub_item;
};

struct batch_update_head {
	struct obd_export	*buh_exp;
	struct lu_batch		*buh_batch;
	int			 buh_flags;
	__u32			 buh_count;
	__u32			 buh_update_count;
	__u32			 buh_buf_count;
	__u32			 buh_reqsize;
	__u32			 buh_repsize;
	__u32			 buh_batchid;
	struct list_head	 buh_buf_list;
	struct list_head	 buh_cb_list;
};

struct batch_update_args {
	struct batch_update_head	*ba_head;
};

struct object_update_callback;
typedef int (*object_update_interpret_t)(struct ptlrpc_request *req,
					 struct lustre_msg *repmsg,
					 struct object_update_callback *ouc,
					 int rc);

struct object_update_callback {
	struct list_head		 ouc_item;
	object_update_interpret_t	 ouc_interpret;
	struct batch_update_head	*ouc_head;
	void				*ouc_data;
};


struct mdc_batch {
	struct lu_batch			  mbh_super;
	struct batch_update_head	 *mbh_head;
};

/**
 * Prepare inline update request
 *
 * Prepare OUT update ptlrpc inline request, and the request usuanlly includes
 * one update buffer, which does not need bulk transfer.
 */
static int batch_prep_inline_update_req(struct batch_update_head *head,
					struct ptlrpc_request *req,
					int repsize)
{
	struct batch_update_buffer *buf;
	struct but_update_header *buh;
	int rc;

	buf = list_entry(head->buh_buf_list.next,
			  struct batch_update_buffer, bub_item);
	req_capsule_set_size(&req->rq_pill, &RMF_BUT_HEADER, RCL_CLIENT,
			     buf->bub_end + sizeof(*buh));

	rc = ptlrpc_request_pack(req, LUSTRE_MDS_VERSION, MDS_BATCH);
	if (rc != 0)
		RETURN(rc);

	buh = req_capsule_client_get(&req->rq_pill, &RMF_BUT_HEADER);
	buh->buh_magic = BUT_HEADER_MAGIC;
	buh->buh_count = 1;
	buh->buh_inline_length = buf->bub_end;
	buh->buh_reply_size = repsize;
	buh->buh_update_count = head->buh_update_count;

	memcpy(buh->buh_inline_data, buf->bub_req, buf->bub_end);

	req_capsule_set_size(&req->rq_pill, &RMF_BUT_REPLY,
			     RCL_SERVER, repsize);

	ptlrpc_request_set_replen(req);
	req->rq_request_portal = OUT_PORTAL;
	req->rq_reply_portal = OSC_REPLY_PORTAL;

	RETURN(rc);
}

static int batch_prep_update_req(struct batch_update_head *head,
				 struct ptlrpc_request **reqp)
{
	struct ptlrpc_request *req;
	struct ptlrpc_bulk_desc *desc;
	struct batch_update_buffer *buf;
	struct but_update_header *buh;
	struct but_update_buffer *bub;
	int page_count = 0;
	int total = 0;
	int repsize;
	int rc;

	ENTRY;

	repsize = head->buh_repsize +
		  cfs_size_round(offsetof(struct batch_update_reply,
					  burp_repmsg[0]));
	if (repsize < OUT_UPDATE_REPLY_SIZE)
		repsize = OUT_UPDATE_REPLY_SIZE;

	LASSERT(head->buh_buf_count > 0);

	req = ptlrpc_request_alloc(class_exp2cliimp(head->buh_exp),
				   &RQF_MDS_BATCH);
	if (req == NULL)
		RETURN(-ENOMEM);

	if (head->buh_buf_count == 1) {
		buf = list_entry(head->buh_buf_list.next,
				 struct batch_update_buffer, bub_item);

		/* Check whether it can be packed inline */
		if (buf->bub_end + sizeof(struct but_update_header) <
		    OUT_UPDATE_MAX_INLINE_SIZE) {
			rc = batch_prep_inline_update_req(head, req, repsize);
			if (rc == 0)
				*reqp = req;
			GOTO(out_req, rc);
		}
	}

	req_capsule_set_size(&req->rq_pill, &RMF_BUT_HEADER, RCL_CLIENT,
			     sizeof(struct but_update_header));
	req_capsule_set_size(&req->rq_pill, &RMF_BUT_BUF, RCL_CLIENT,
			     head->buh_buf_count * sizeof(*bub));

	rc = ptlrpc_request_pack(req, LUSTRE_MDS_VERSION, MDS_BATCH);
	if (rc != 0)
		GOTO(out_req, rc);

	buh = req_capsule_client_get(&req->rq_pill, &RMF_BUT_HEADER);
	buh->buh_magic = BUT_HEADER_MAGIC;
	buh->buh_count = head->buh_buf_count;
	buh->buh_inline_length = 0;
	buh->buh_reply_size = repsize;
	buh->buh_update_count = head->buh_update_count;
	bub = req_capsule_client_get(&req->rq_pill, &RMF_BUT_BUF);
	list_for_each_entry(buf, &head->buh_buf_list, bub_item) {
		bub->bub_size = buf->bub_size;
		bub++;
		/* First *and* last might be partial pages, hence +1 */
		page_count += DIV_ROUND_UP(buf->bub_size, PAGE_SIZE) + 1;
	}

	req->rq_bulk_write = 1;
	desc = ptlrpc_prep_bulk_imp(req, page_count,
				    MD_MAX_BRW_SIZE >> LNET_MTU_BITS,
				    PTLRPC_BULK_GET_SOURCE,
				    MDS_BULK_PORTAL,
				    &ptlrpc_bulk_kiov_nopin_ops);
	if (desc == NULL)
		GOTO(out_req, rc = -ENOMEM);

	list_for_each_entry(buf, &head->buh_buf_list, bub_item) {
		desc->bd_frag_ops->add_iov_frag(desc, buf->bub_req,
						buf->bub_size);
		total += buf->bub_size;
	}
	CDEBUG(D_OTHER, "Total %d in %u\n", total, head->buh_update_count);

	req_capsule_set_size(&req->rq_pill, &RMF_BUT_REPLY,
			     RCL_SERVER, repsize);

	ptlrpc_request_set_replen(req);
	req->rq_request_portal = OUT_PORTAL;
	req->rq_reply_portal = OSC_REPLY_PORTAL;
	*reqp = req;

out_req:
	if (rc < 0)
		ptlrpc_req_finished(req);

	RETURN(rc);
}

static struct batch_update_buffer *
current_batch_update_buffer(struct batch_update_head *head)
{
	if (list_empty(&head->buh_buf_list))
		return NULL;

	return list_entry(head->buh_buf_list.prev, struct batch_update_buffer,
			  bub_item);
}

static int batch_update_buffer_create(struct batch_update_head *head,
				      size_t size)
{
	struct batch_update_buffer *buf;
	struct batch_update_request *bur;

	OBD_ALLOC_PTR(buf);
	if (buf == NULL)
		return -ENOMEM;

	LASSERT(size > 0);
	size = round_up(size, PAGE_SIZE);
	OBD_ALLOC_LARGE(bur, size);
	if (bur == NULL) {
		OBD_FREE_PTR(buf);
		return -ENOMEM;
	}

	bur->burq_magic = BUT_REQUEST_MAGIC;
	bur->burq_count = 0;
	buf->bub_req = bur;
	buf->bub_size = size;
	buf->bub_end = sizeof(*bur);
	INIT_LIST_HEAD(&buf->bub_item);
	list_add_tail(&buf->bub_item, &head->buh_buf_list);
	head->buh_buf_count++;

	return 0;
}

/**
 * Destroy an @object_update_callback.
 */
static void object_update_callback_fini(struct object_update_callback *ouc)
{
	LASSERT(list_empty(&ouc->ouc_item));

	OBD_FREE_PTR(ouc);
}

/**
 * Insert an @object_update_callback into the the @batch_update_head.
 *
 * Usually each update in @batch_update_head will have one correspondent
 * callback, and these callbacks will be called in ->rq_interpret_reply.
 */
static int
batch_insert_update_callback(struct batch_update_head *head, void *data,
			     object_update_interpret_t interpret)
{
	struct object_update_callback *ouc;

	OBD_ALLOC_PTR(ouc);
	if (ouc == NULL)
		return -ENOMEM;

	INIT_LIST_HEAD(&ouc->ouc_item);
	ouc->ouc_interpret = interpret;
	ouc->ouc_head = head;
	ouc->ouc_data = data;
	list_add_tail(&ouc->ouc_item, &head->buh_cb_list);

	return 0;
}

/**
 * Allocate and initialize batch update request.
 *
 * @batch_update_head is being used to track updates being executed on
 * this OBD device. The update buffer will be 4K initially, and increased
 * if needed.
 */
static struct batch_update_head *
batch_update_request_create(struct obd_export *exp, struct lu_batch *bh)
{
	struct batch_update_head *head;
	int rc;

	OBD_ALLOC_PTR(head);
	if (head == NULL)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&head->buh_cb_list);
	INIT_LIST_HEAD(&head->buh_buf_list);
	head->buh_exp = exp;
	head->buh_batch = bh;

	rc = batch_update_buffer_create(head, PAGE_SIZE);
	if (rc != 0) {
		OBD_FREE_PTR(head);
		RETURN(ERR_PTR(rc));
	}

	return head;
}

static void batch_update_request_destroy(struct batch_update_head *head)
{
	struct batch_update_buffer *bub, *tmp;

	if (head == NULL)
		return;

	list_for_each_entry_safe(bub, tmp, &head->buh_buf_list, bub_item) {
		list_del(&bub->bub_item);
		if (bub->bub_req)
			OBD_FREE_LARGE(bub->bub_req, bub->bub_size);
		OBD_FREE_PTR(bub);
	}

	OBD_FREE_PTR(head);
}

static int batch_update_request_fini(struct batch_update_head *head,
				     struct ptlrpc_request *req,
				     struct batch_update_reply *reply, int rc)
{
	struct object_update_callback *ouc, *next;
	struct lustre_msg *repmsg = NULL;
	int count = 0;
	int index = 0;

	ENTRY;

	if (reply)
		count = reply->burp_count;

	list_for_each_entry_safe(ouc, next, &head->buh_cb_list, ouc_item) {
		int rc1 = 0;

		list_del_init(&ouc->ouc_item);

		/*
		 * The peer may only have handled some requests (indicated by
		 * @count) in the packaged OUT PRC, we can only get results
		 * for the handled part.
		 */
		if (index < count) {
			repmsg = batch_update_repmsg_next(reply, repmsg);
			if (repmsg == NULL)
				rc1 = -EPROTO;
			else
				rc1 = repmsg->lm_result;
		} else {
			/*
			 * The peer did not handle these request, let us return
			 * -ECANCELED to the update interpreter for now.
			 */
			repmsg = NULL;
			rc1 = -ECANCELED;
			/*
			 * TODO: resend the unfinished sub request when the
			 * return code is -EOVERFLOW.
			 */
		}

		if (ouc->ouc_interpret != NULL)
			ouc->ouc_interpret(req, repmsg, ouc, rc1);

		index++;
		object_update_callback_fini(ouc);
		if (rc == 0 && rc1 < 0)
			rc = rc1;
	}

	batch_update_request_destroy(head);

	RETURN(rc);
}

static int batch_update_interpret(const struct lu_env *env,
				  struct ptlrpc_request *req,
				  void *args, int rc)
{
	struct batch_update_args *aa = (struct batch_update_args *)args;
	struct batch_update_reply *reply = NULL;

	ENTRY;

	if (aa->ba_head == NULL)
		RETURN(0);

	/* Unpack the results from the reply message. */
	if (req->rq_repmsg != NULL && req->rq_replied) {
		reply = req_capsule_server_sized_get(&req->rq_pill,
						     &RMF_BUT_REPLY,
						     sizeof(*reply));
		if ((reply == NULL ||
		     reply->burp_magic != BUT_REPLY_MAGIC) && rc == 0)
			rc = -EPROTO;
	}

	rc = batch_update_request_fini(aa->ba_head, req, reply, rc);

	RETURN(rc);
}

static int batch_send_update_req(const struct lu_env *env,
				 struct batch_update_head *head)
{
	struct obd_device *obd;
	struct ptlrpc_request *req = NULL;
	struct batch_update_args *aa;
	struct lu_batch *bh;
	int rc;

	ENTRY;

	if (head == NULL)
		RETURN(0);

	bh = head->buh_batch;
	rc = batch_prep_update_req(head, &req);
	if (rc) {
		rc = batch_update_request_fini(head, NULL, NULL, rc);
		RETURN(rc);
	}

	aa = ptlrpc_req_async_args(aa, req);
	aa->ba_head = head;
	req->rq_interpret_reply = batch_update_interpret;

	if (bh->bh_flags & BATCH_FL_SYNC) {
		rc = ptlrpc_queue_wait(req);
	} else {
		if ((bh->bh_flags & (BATCH_FL_RDONLY | BATCH_FL_RQSET)) ==
		    BATCH_FL_RDONLY) {
			ptlrpcd_add_req(req);
		} else if (bh->bh_flags & BATCH_FL_RQSET) {
			ptlrpc_set_add_req(bh->bh_rqset, req);
			ptlrpc_check_set(env, bh->bh_rqset);
		} else {
			ptlrpcd_add_req(req);
		}
		req = NULL;
	}

	if (req != NULL)
		ptlrpc_req_finished(req);

	obd = class_exp2obd(head->buh_exp);
	lprocfs_oh_tally_log2(&obd->u.cli.cl_batch_rpc_hist,
			      head->buh_update_count);

	RETURN(rc);
}

typedef int (*md_update_pack_t)(struct batch_update_head *head,
				struct lustre_msg *reqmsg,
				size_t *max_pack_size,
				struct md_op_item *item);


static int batch_update_request_add(struct batch_update_head **headp,
				    struct md_op_item *item,
				    md_update_pack_t packer,
				    object_update_interpret_t interpreter)
{
	struct batch_update_head *head = *headp;
	struct lu_batch *bh = head->buh_batch;
	struct batch_update_buffer *buf;
	struct lustre_msg *reqmsg;
	size_t max_len;
	int rc;

	ENTRY;

	for (; ;) {
		buf = current_batch_update_buffer(head);
		LASSERT(buf != NULL);
		max_len = buf->bub_size - buf->bub_end;
		reqmsg = (struct lustre_msg *)((char *)buf->bub_req +
						buf->bub_end);
		rc = packer(head, reqmsg, &max_len, item);
		if (rc == -E2BIG) {
			int rc2;

			/* Create new batch object update buffer */
			rc2 = batch_update_buffer_create(head,
				max_len + offsetof(struct batch_update_request,
						   burq_reqmsg[0]) + 1);
			if (rc2 != 0) {
				rc = rc2;
				break;
			}
		} else {
			if (rc == 0) {
				buf->bub_end += max_len;
				buf->bub_req->burq_count++;
				head->buh_update_count++;
				head->buh_repsize += reqmsg->lm_repsize;
			}
			break;
		}
	}

	if (rc)
		GOTO(out, rc);

	rc = batch_insert_update_callback(head, item, interpreter);
	if (rc)
		GOTO(out, rc);

	/* Unplug the batch queue if accumulated enough update requests. */
	if (bh->bh_max_count && head->buh_update_count >= bh->bh_max_count) {
		rc = batch_send_update_req(NULL, head);
		*headp = NULL;
	}
out:
	if (rc) {
		batch_update_request_destroy(head);
		*headp = NULL;
	}

	RETURN(rc);
}

static int mdc_ldlm_lock_pack(struct obd_export *exp,
			      struct req_capsule *pill,
			      union ldlm_policy_data *policy,
			      struct lu_fid *fid, struct md_op_item *item)
{
	struct ldlm_request *dlmreq;
	struct ldlm_res_id res_id;
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;
	int rc;

	ENTRY;

	dlmreq = req_capsule_client_get(pill, &RMF_DLM_REQ);
	if (IS_ERR(dlmreq))
		RETURN(PTR_ERR(dlmreq));

	/* With Data-on-MDT the glimpse callback is needed too.
	 * It is set here in advance but not in mdc_finish_enqueue()
	 * to avoid possible races. It is safe to have glimpse handler
	 * for non-DOM locks and costs nothing.
	 */
	if (einfo->ei_cb_gl == NULL)
		einfo->ei_cb_gl = mdc_ldlm_glimpse_ast;

	fid_build_reg_res_name(fid, &res_id);
	rc = ldlm_cli_lock_create_pack(exp, dlmreq, einfo, &res_id,
				       policy, &item->mop_lock_flags,
				       NULL, 0, LVB_T_NONE, &item->mop_lockh);

	RETURN(rc);
}

static int mdc_batch_getattr_pack(struct batch_update_head *head,
				  struct lustre_msg *reqmsg,
				  size_t *max_pack_size,
				  struct md_op_item *item)
{
	struct obd_export *exp = head->buh_exp;
	struct lookup_intent *it = &item->mop_it;
	struct md_op_data *op_data = &item->mop_data;
	u64 valid = OBD_MD_FLGETATTR | OBD_MD_FLEASIZE | OBD_MD_FLMODEASIZE |
		    OBD_MD_FLDIREA | OBD_MD_MEA | OBD_MD_FLACL |
		    OBD_MD_DEFAULT_MEA;
	union ldlm_policy_data policy = {
		.l_inodebits = { MDS_INODELOCK_LOOKUP | MDS_INODELOCK_UPDATE }
	};
	struct ldlm_intent *lit;
	bool have_secctx = false;
	struct req_capsule pill;
	__u32 easize;
	__u32 size;
	int rc;

	ENTRY;

	req_capsule_subreq_init(&pill, &RQF_BUT_GETATTR, NULL,
				reqmsg, NULL, RCL_CLIENT);

	/* send name of security xattr to get upon intent */
	if (it->it_op & (IT_LOOKUP | IT_GETATTR) &&
	    req_capsule_has_field(&pill, &RMF_FILE_SECCTX_NAME,
				  RCL_CLIENT) &&
	    op_data->op_file_secctx_name_size > 0 &&
	    op_data->op_file_secctx_name != NULL) {
		have_secctx = true;
		req_capsule_set_size(&pill, &RMF_FILE_SECCTX_NAME, RCL_CLIENT,
				     op_data->op_file_secctx_name_size);
	}

	req_capsule_set_size(&pill, &RMF_NAME, RCL_CLIENT,
			     op_data->op_namelen + 1);

	size = req_capsule_msg_size(&pill, RCL_CLIENT);
	if (unlikely(size >= *max_pack_size)) {
		*max_pack_size = size;
		return -E2BIG;
	}

	req_capsule_client_pack(&pill);
	/* pack the intent */
	lit = req_capsule_client_get(&pill, &RMF_LDLM_INTENT);
	lit->opc = (__u64)it->it_op;

	easize = MAX_MD_SIZE_OLD; //obd->u.cli.cl_default_mds_easize;

	/* pack the intended request */
	mdc_getattr_pack(&pill, valid, it->it_flags, op_data, easize);

	item->mop_lock_flags |= LDLM_FL_HAS_INTENT;
	rc = mdc_ldlm_lock_pack(head->buh_exp, &pill, &policy,
				&item->mop_data.op_fid1, item);
	if (rc)
		RETURN(rc);

	req_capsule_set_size(&pill, &RMF_MDT_MD, RCL_SERVER, easize);
	req_capsule_set_size(&pill, &RMF_ACL, RCL_SERVER,
			     LUSTRE_POSIX_ACL_MAX_SIZE_OLD);
	req_capsule_set_size(&pill, &RMF_DEFAULT_MDT_MD, RCL_SERVER,
			     /*sizeof(struct lmv_user_md)*/MIN_MD_SIZE);

	if (have_secctx) {
		char *secctx_name;

		secctx_name = req_capsule_client_get(&pill,
						     &RMF_FILE_SECCTX_NAME);
		memcpy(secctx_name, op_data->op_file_secctx_name,
		       op_data->op_file_secctx_name_size);

		req_capsule_set_size(&pill, &RMF_FILE_SECCTX,
				     RCL_SERVER, easize);

		CDEBUG(D_SEC, "packed '%.*s' as security xattr name\n",
		       op_data->op_file_secctx_name_size,
		       op_data->op_file_secctx_name);
	} else {
		req_capsule_set_size(&pill, &RMF_FILE_SECCTX, RCL_SERVER, 0);
	}

	if (exp_connect_encrypt(exp) && it->it_op & (IT_LOOKUP | IT_GETATTR))
		req_capsule_set_size(&pill, &RMF_FILE_ENCCTX,
				     RCL_SERVER, easize);
	else
		req_capsule_set_size(&pill, &RMF_FILE_ENCCTX,
				     RCL_SERVER, 0);

	req_capsule_set_replen(&pill);
	reqmsg->lm_opc = BUT_GETATTR;
	*max_pack_size = size;
	RETURN(rc);
}

static int mdc_batch_getattr_interpret(struct ptlrpc_request *req,
				       struct lustre_msg *repmsg,
				       struct object_update_callback *ouc,
				       int rc)
{
	struct md_op_item *item = (struct md_op_item *)ouc->ouc_data;
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;
	struct batch_update_head *head = ouc->ouc_head;
	struct obd_export *exp = head->buh_exp;
	struct req_capsule pill;

	req_capsule_subreq_init(&pill, &RQF_BUT_GETATTR, req,
				NULL, repmsg, RCL_CLIENT);

	rc = ldlm_cli_enqueue_fini(exp, &pill, einfo, 1, &item->mop_lock_flags,
				   NULL, 0, &item->mop_lockh, rc);
	if (rc)
		GOTO(out, rc);

	rc = mdc_finish_enqueue(exp, &pill, einfo, &item->mop_it,
				&item->mop_lockh, rc);
out:
	return item->mop_cb(&pill, item, rc);
}

static void mdc_create_capsule_pack(struct req_capsule *pill,
				    struct md_op_data *op_data,
				    struct lookup_intent *it)
{
	int lmm_size = 0;

	req_capsule_set_size(pill, &RMF_NAME, RCL_CLIENT,
			     op_data->op_namelen + 1);
	req_capsule_set_size(pill, &RMF_FILE_SECCTX_NAME,
			     RCL_CLIENT, op_data->op_file_secctx_name != NULL ?
			     strlen(op_data->op_file_secctx_name) + 1 : 0);
	req_capsule_set_size(pill, &RMF_FILE_SECCTX, RCL_CLIENT,
			     op_data->op_file_secctx_size);
	req_capsule_set_size(pill, &RMF_FILE_ENCCTX, RCL_CLIENT,
			     op_data->op_file_encctx_size);

	if ((it->it_flags & MDS_OPEN_PCC || S_ISLNK(it->it_create_mode)) &&
	    op_data->op_data && op_data->op_data_size)
		lmm_size = op_data->op_data_size;

	req_capsule_set_size(pill, &RMF_EADATA, RCL_CLIENT, lmm_size);
}

static int mdc_create_exlock_pack(struct batch_update_head *head,
				  struct lustre_msg *reqmsg,
				  size_t *max_pack_size,
				  struct md_op_item *item)
{
	static union ldlm_policy_data exlock_policy = {
				.l_inodebits = { MDS_INODELOCK_UPDATE } };
	struct md_op_data *op_data = &item->mop_data;
	struct lookup_intent *it = &item->mop_it;
	struct req_capsule pill;
	size_t size;
	int rc;

	ENTRY;

	req_capsule_subreq_init(&pill, &RQF_BUT_CREATE_EXLOCK, NULL,
				reqmsg, NULL, RCL_CLIENT);
	mdc_create_capsule_pack(&pill, op_data, it);

	size = req_capsule_msg_size(&pill, RCL_CLIENT);
	if (unlikely(size >= *max_pack_size)) {
		*max_pack_size = size;
		return -E2BIG;
	}

	req_capsule_client_pack(&pill);
	mdc_create_pack(&pill, op_data, op_data->op_data,
			op_data->op_data_size, it->it_create_mode,
			op_data->op_fsuid, op_data->op_fsgid, op_data->op_cap,
			op_data->op_rdev, it->it_flags);

	rc = mdc_ldlm_lock_pack(head->buh_exp, &pill, &exlock_policy,
				&op_data->op_fid2, item);
	if (rc)
		RETURN(rc);

	/* FIXME: Set buffer size for LMV/LOV EA properly.*/
	if (S_ISREG(item->mop_it.it_create_mode))
		req_capsule_set_size(&pill, &RMF_MDT_MD, RCL_SERVER,
				     MAX_MD_SIZE);
	else
		req_capsule_set_size(&pill, &RMF_MDT_MD, RCL_SERVER, 0);

	req_capsule_set_replen(&pill);
	reqmsg->lm_opc = BUT_CREATE_EXLOCK;
	*max_pack_size = size;
	RETURN(rc);
}

static int mdc_create_exlock_interpret(struct ptlrpc_request *req,
				       struct lustre_msg *repmsg,
				       struct object_update_callback *ouc,
				       int rc)
{
	struct md_op_item *item = (struct md_op_item *)ouc->ouc_data;
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;
	struct batch_update_head *head = ouc->ouc_head;
	struct obd_export *exp = head->buh_exp;
	struct req_capsule pill;

	req_capsule_subreq_init(&pill, &RQF_BUT_CREATE_EXLOCK, req,
				NULL, repmsg, RCL_CLIENT);

	rc = ldlm_cli_enqueue_fini(exp, &pill, einfo, 1, &item->mop_lock_flags,
				   NULL, 0, &item->mop_lockh, rc);
	rc = mdc_finish_enqueue(exp, &pill, einfo, &item->mop_it,
				&item->mop_lockh, rc);

	return item->mop_cb(&pill, item, rc);
}

static int mdc_create_lockless_pack(struct batch_update_head *head,
				    struct lustre_msg *reqmsg,
				    size_t *max_pack_size,
				    struct md_op_item *item)
{
	struct md_op_data *op_data = &item->mop_data;
	struct lookup_intent *it = &item->mop_it;
	struct req_capsule pill;
	size_t size;

	ENTRY;

	req_capsule_subreq_init(&pill, &RQF_BUT_CREATE_LOCKLESS, NULL,
				reqmsg, NULL, RCL_CLIENT);
	mdc_create_capsule_pack(&pill, op_data, it);
	size = req_capsule_msg_size(&pill, RCL_CLIENT);
	if (unlikely(size >= *max_pack_size)) {
		*max_pack_size = size;
		return -E2BIG;
	}

	req_capsule_client_pack(&pill);
	mdc_create_pack(&pill, op_data, op_data->op_data,
			op_data->op_data_size, it->it_create_mode,
			op_data->op_fsuid, op_data->op_fsgid, op_data->op_cap,
			op_data->op_rdev, it->it_flags);

	/* FIXME: Set buffer size for LMV/LOV EA properly.*/
	if (S_ISREG(item->mop_it.it_create_mode))
		req_capsule_set_size(&pill, &RMF_MDT_MD, RCL_SERVER,
				     MAX_MD_SIZE);
	else
		req_capsule_set_size(&pill, &RMF_MDT_MD, RCL_SERVER, 0);

	req_capsule_set_replen(&pill);
	reqmsg->lm_opc = BUT_CREATE_LOCKLESS;
	*max_pack_size = size;
	RETURN(0);
}

static int mdc_create_lockless_interpret(struct ptlrpc_request *req,
					 struct lustre_msg *repmsg,
					 struct object_update_callback *ouc,
					 int rc)
{
	struct md_op_item *item = (struct md_op_item *)ouc->ouc_data;
	struct req_capsule pill;

	req_capsule_subreq_init(&pill, &RQF_BUT_CREATE_LOCKLESS, req,
				NULL, repmsg, RCL_CLIENT);

	return item->mop_cb(&pill, item, rc);
}

static int mdc_setattr_exlock_pack(struct batch_update_head *head,
				   struct lustre_msg *reqmsg,
				   size_t *max_pack_size,
				   struct md_op_item *item)
{
	static union ldlm_policy_data exlock_policy = {
				.l_inodebits = { MDS_INODELOCK_UPDATE } };
	struct req_capsule pill;
	__u32 size;
	int rc;

	ENTRY;

	req_capsule_subreq_init(&pill, &RQF_BUT_SETATTR_EXLOCK, NULL,
				reqmsg, NULL, RCL_CLIENT);
	size = req_capsule_msg_size(&pill, RCL_CLIENT);
	if (unlikely(size >= *max_pack_size)) {
		*max_pack_size = size;
		return -E2BIG;
	}

	req_capsule_client_pack(&pill);
	mdc_setattr_pack(&pill, &item->mop_data, NULL, 0);
	rc = mdc_ldlm_lock_pack(head->buh_exp, &pill, &exlock_policy,
				&item->mop_data.op_fid1, item);
	if (rc)
		RETURN(rc);

	req_capsule_set_replen(&pill);
	reqmsg->lm_opc = BUT_SETATTR_EXLOCK;
	*max_pack_size = size;
	RETURN(rc);
}

static int mdc_setattr_exlock_interpret(struct ptlrpc_request *req,
					struct lustre_msg *repmsg,
					struct object_update_callback *ouc,
					int rc)
{
	struct md_op_item *item = (struct md_op_item *)ouc->ouc_data;
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;
	struct batch_update_head *head = ouc->ouc_head;
	struct obd_export *exp = head->buh_exp;
	struct req_capsule pill;

	req_capsule_subreq_init(&pill, &RQF_BUT_SETATTR_EXLOCK, req,
				NULL, repmsg, RCL_CLIENT);

	rc = ldlm_cli_enqueue_fini(exp, &pill, einfo, 1, &item->mop_lock_flags,
				   NULL, 0, &item->mop_lockh, rc);
	rc = mdc_finish_enqueue(exp, &pill, einfo, &item->mop_it,
				&item->mop_lockh, rc);

	return item->mop_cb(&pill, item, rc);
}

static int mdc_setattr_lockless_pack(struct batch_update_head *head,
				     struct lustre_msg *reqmsg,
				     size_t *max_pack_size,
				     struct md_op_item *item)
{
	struct req_capsule pill;
	__u32 size;

	ENTRY;

	req_capsule_subreq_init(&pill, &RQF_BUT_SETATTR_LOCKLESS, NULL,
				reqmsg, NULL, RCL_CLIENT);
	size = req_capsule_msg_size(&pill, RCL_CLIENT);
	if (unlikely(size >= *max_pack_size)) {
		*max_pack_size = size;
		return -E2BIG;
	}

	req_capsule_client_pack(&pill);
	mdc_setattr_pack(&pill, &item->mop_data, NULL, 0);
	req_capsule_set_replen(&pill);
	reqmsg->lm_opc = BUT_SETATTR_LOCKLESS;
	*max_pack_size = size;
	RETURN(0);
}

static int mdc_setattr_lockless_interpret(struct ptlrpc_request *req,
					  struct lustre_msg *repmsg,
					  struct object_update_callback *ouc,
					  int rc)
{
	struct md_op_item *item = (struct md_op_item *)ouc->ouc_data;
	struct req_capsule pill;

	req_capsule_subreq_init(&pill, &RQF_BUT_SETATTR_LOCKLESS, req,
				NULL, repmsg, RCL_CLIENT);

	return item->mop_cb(&pill, item, rc);
}

static int mdc_exlock_only_pack(struct batch_update_head *head,
				struct lustre_msg *reqmsg,
			    size_t *max_pack_size,
			    struct md_op_item *item)
{
	static union ldlm_policy_data exlock_policy = {
				.l_inodebits = { MDS_INODELOCK_UPDATE } };
	struct req_capsule pill;
	__u32 size;
	int rc;

	ENTRY;

	req_capsule_subreq_init(&pill, &RQF_BUT_EXLOCK_ONLY, NULL,
				reqmsg, NULL, RCL_CLIENT);
	size = req_capsule_msg_size(&pill, RCL_CLIENT);
	if (unlikely(size >= *max_pack_size)) {
		*max_pack_size = size;
		RETURN(-E2BIG);
	}

	req_capsule_client_pack(&pill);
	rc = mdc_ldlm_lock_pack(head->buh_exp, &pill, &exlock_policy,
				&item->mop_data.op_fid1, item);
	if (rc)
		RETURN(rc);

	req_capsule_set_replen(&pill);
	reqmsg->lm_opc = BUT_EXLOCK_ONLY;
	*max_pack_size = size;
	RETURN(rc);
}

static int mdc_exlock_only_interpret(struct ptlrpc_request *req,
				     struct lustre_msg *repmsg,
				     struct object_update_callback *ouc,
				     int rc)
{
	struct md_op_item *item = (struct md_op_item *)ouc->ouc_data;
	struct ldlm_enqueue_info *einfo = &item->mop_einfo;
	struct batch_update_head *head = ouc->ouc_head;
	struct obd_export *exp = head->buh_exp;
	struct req_capsule pill;

	req_capsule_subreq_init(&pill, &RQF_BUT_EXLOCK_ONLY, req,
				NULL, repmsg, RCL_CLIENT);

	rc = ldlm_cli_enqueue_fini(exp, &pill, einfo, 1, &item->mop_lock_flags,
				   NULL, 0, &item->mop_lockh, rc);
	rc = mdc_finish_enqueue(exp, &pill, einfo, &item->mop_it,
				&item->mop_lockh, rc);

	return item->mop_cb(&pill, item, rc);
}

static md_update_pack_t mdc_update_packers[MD_OP_MAX] = {
	[MD_OP_GETATTR]			= mdc_batch_getattr_pack,
	[MD_OP_CREATE_LOCKLESS]		= mdc_create_lockless_pack,
	[MD_OP_CREATE_EXLOCK]		= mdc_create_exlock_pack,
	[MD_OP_SETATTR_LOCKLESS]	= mdc_setattr_lockless_pack,
	[MD_OP_SETATTR_EXLOCK]		= mdc_setattr_exlock_pack,
	[MD_OP_EXLOCK_ONLY]		= mdc_exlock_only_pack,
};

object_update_interpret_t mdc_update_interpreters[MD_OP_MAX] = {
	[MD_OP_GETATTR]			= mdc_batch_getattr_interpret,
	[MD_OP_CREATE_LOCKLESS]		= mdc_create_lockless_interpret,
	[MD_OP_CREATE_EXLOCK]		= mdc_create_exlock_interpret,
	[MD_OP_SETATTR_LOCKLESS]	= mdc_setattr_lockless_interpret,
	[MD_OP_SETATTR_EXLOCK]		= mdc_setattr_exlock_interpret,
	[MD_OP_EXLOCK_ONLY]		= mdc_exlock_only_interpret,
};

static int mdc_update_request_add(struct batch_update_head **headp,
				  struct md_op_item *item)
{
	__u32 opc = item->mop_opc;

	ENTRY;

	if (opc >= MD_OP_MAX || mdc_update_packers[opc] == NULL ||
	    mdc_update_interpreters[opc] == NULL) {
		CERROR("Unexpected opcode %d\n", opc);
		RETURN(-EFAULT);
	}

	RETURN(batch_update_request_add(headp, item, mdc_update_packers[opc],
					mdc_update_interpreters[opc]));
}

struct lu_batch *mdc_batch_create(struct obd_export *exp,
				  enum lu_batch_flags flags, __u32 max_count)
{
	struct mdc_batch *mbh;
	struct lu_batch *bh;

	ENTRY;

	OBD_ALLOC_PTR(mbh);
	if (!mbh)
		RETURN(ERR_PTR(-ENOMEM));

	bh = &mbh->mbh_super;
	bh->bh_result = 0;
	bh->bh_flags = flags;
	bh->bh_max_count = max_count;

	mbh->mbh_head = batch_update_request_create(exp, bh);
	if (IS_ERR(mbh->mbh_head)) {
		bh = (struct lu_batch *)mbh->mbh_head;
		OBD_FREE_PTR(mbh);
	}

	RETURN(bh);
}

int mdc_batch_stop(struct obd_export *exp, struct lu_batch *bh)
{
	struct mdc_batch *mbh;
	int rc;

	ENTRY;

	mbh = container_of(bh, struct mdc_batch, mbh_super);
	rc = batch_send_update_req(NULL, mbh->mbh_head);

	OBD_FREE_PTR(mbh);
	RETURN(rc);
}

int mdc_batch_flush(struct obd_export *exp, struct lu_batch *bh, bool wait)
{
	struct mdc_batch *mbh;
	int rc;

	ENTRY;

	mbh = container_of(bh, struct mdc_batch, mbh_super);
	if (mbh->mbh_head == NULL)
		RETURN(0);

	rc = batch_send_update_req(NULL, mbh->mbh_head);
	mbh->mbh_head = NULL;

	RETURN(rc);
}

int mdc_batch_add(struct obd_export *exp, struct lu_batch *bh,
		  struct md_op_item *item)
{
	struct mdc_batch *mbh;
	int rc;

	ENTRY;

	mbh = container_of(bh, struct mdc_batch, mbh_super);
	if (mbh->mbh_head == NULL) {
		mbh->mbh_head = batch_update_request_create(exp, bh);
		if (IS_ERR(mbh->mbh_head))
			RETURN(PTR_ERR(mbh->mbh_head));
	}

	rc = mdc_update_request_add(&mbh->mbh_head, item);

	RETURN(rc);
}
