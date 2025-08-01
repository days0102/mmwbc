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
 * lustre/ptlrpc/layout.c
 *
 * Lustre Metadata Target (mdt) request handler
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */
/*
 * This file contains the "capsule/pill" abstraction layered above PTLRPC.
 *
 * Every struct ptlrpc_request contains a "pill", which points to a description
 * of the format that the request conforms to.
 */

#define DEBUG_SUBSYSTEM S_RPC

#include <linux/module.h>

#include <llog_swab.h>
#include <lustre_swab.h>
#include <obd.h>
#include <obd_support.h>

/* struct ptlrpc_request, lustre_msg* */
#include <lustre_req_layout.h>
#include <lustre_acl.h>
#include <lustre_nodemap.h>

/*
 * RQFs (see below) refer to two struct req_msg_field arrays describing the
 * client request and server reply, respectively.
 */
/* empty set of fields... for suitable definition of emptiness. */
static const struct req_msg_field *empty[] = {
        &RMF_PTLRPC_BODY
};

static const struct req_msg_field *mgs_target_info_only[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MGS_TARGET_INFO
};

#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(2, 18, 53, 0)
static const struct req_msg_field *mgs_set_info[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MGS_SEND_PARAM
};
#endif

static const struct req_msg_field *mgs_config_read_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MGS_CONFIG_BODY
};

static const struct req_msg_field *mgs_config_read_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MGS_CONFIG_RES
};

static const struct req_msg_field *mdt_body_only[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY
};

static const struct req_msg_field *mdt_body_capa[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY,
        &RMF_CAPA1
};

static const struct req_msg_field *quotactl_only[] = {
	&RMF_PTLRPC_BODY,
	&RMF_OBD_QUOTACTL
};

static const struct req_msg_field *quota_body_only[] = {
	&RMF_PTLRPC_BODY,
	&RMF_QUOTA_BODY
};

static const struct req_msg_field *ldlm_intent_quota_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_QUOTA_BODY
};

static const struct req_msg_field *ldlm_intent_quota_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REP,
	&RMF_DLM_LVB,
	&RMF_QUOTA_BODY
};

static const struct req_msg_field *mdt_close_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_EPOCH,
        &RMF_REC_REINT,
        &RMF_CAPA1
};

static const struct req_msg_field *mdt_close_intent_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_EPOCH,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_CLOSE_DATA,
	&RMF_U32
};

static const struct req_msg_field *obd_statfs_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_OBD_STATFS
};

static const struct req_msg_field *seq_query_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_SEQ_OPC,
        &RMF_SEQ_RANGE
};

static const struct req_msg_field *seq_query_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_SEQ_RANGE
};

static const struct req_msg_field *fld_query_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_FLD_OPC,
        &RMF_FLD_MDFLD
};

static const struct req_msg_field *fld_query_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_FLD_MDFLD
};

static const struct req_msg_field *fld_read_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_FLD_MDFLD
};

static const struct req_msg_field *fld_read_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_GENERIC_DATA
};

static const struct req_msg_field *mds_getattr_name_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY,
        &RMF_CAPA1,
        &RMF_NAME
};

static const struct req_msg_field *mds_reint_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_REC_REINT
};

static const struct req_msg_field *mds_reint_create_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_REC_REINT,
        &RMF_CAPA1,
        &RMF_NAME
};

static const struct req_msg_field *mds_reint_create_slave_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_REC_REINT,
        &RMF_CAPA1,
        &RMF_NAME,
        &RMF_EADATA,
        &RMF_DLM_REQ
};

static const struct req_msg_field *mds_reint_create_acl_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_NAME,
	&RMF_EADATA,
	&RMF_DLM_REQ,
	&RMF_FILE_SECCTX_NAME,
	&RMF_FILE_SECCTX,
	&RMF_SELINUX_POL,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *mds_reint_create_sym_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_NAME,
	&RMF_SYMTGT,
	&RMF_DLM_REQ,
	&RMF_FILE_SECCTX_NAME,
	&RMF_FILE_SECCTX,
	&RMF_SELINUX_POL,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *mds_reint_open_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_CAPA2,
	&RMF_NAME,
	&RMF_EADATA,
	&RMF_FILE_SECCTX_NAME,
	&RMF_FILE_SECCTX,
	&RMF_SELINUX_POL,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *mds_reint_open_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY,
        &RMF_MDT_MD,
        &RMF_ACL,
        &RMF_CAPA1,
        &RMF_CAPA2
};

static const struct req_msg_field *mds_reint_unlink_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_NAME,
	&RMF_DLM_REQ,
	&RMF_SELINUX_POL
};

static const struct req_msg_field *mds_reint_link_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_CAPA2,
	&RMF_NAME,
	&RMF_DLM_REQ,
	&RMF_SELINUX_POL
};

static const struct req_msg_field *mds_reint_rename_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_CAPA2,
	&RMF_NAME,
	&RMF_SYMTGT,
	&RMF_DLM_REQ,
	&RMF_SELINUX_POL
};

static const struct req_msg_field *mds_reint_migrate_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_CAPA2,
	&RMF_NAME,
	&RMF_SYMTGT,
	&RMF_DLM_REQ,
	&RMF_SELINUX_POL,
	&RMF_MDT_EPOCH,
	&RMF_CLOSE_DATA,
	&RMF_EADATA
};

static const struct req_msg_field *mds_last_unlink_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY,
        &RMF_MDT_MD,
        &RMF_LOGCOOKIES,
        &RMF_CAPA1,
        &RMF_CAPA2
};

static const struct req_msg_field *mds_reint_setattr_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_REC_REINT,
        &RMF_CAPA1,
        &RMF_MDT_EPOCH,
        &RMF_EADATA,
        &RMF_LOGCOOKIES,
        &RMF_DLM_REQ
};

static const struct req_msg_field *mds_reint_setxattr_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_REC_REINT,
        &RMF_CAPA1,
        &RMF_NAME,
        &RMF_EADATA,
	&RMF_DLM_REQ,
	&RMF_SELINUX_POL
};

static const struct req_msg_field *mds_reint_resync[] = {
	&RMF_PTLRPC_BODY,
	&RMF_REC_REINT,
	&RMF_DLM_REQ
};

static const struct req_msg_field *mdt_swap_layouts[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_SWAP_LAYOUTS,
	&RMF_CAPA1,
	&RMF_CAPA2,
	&RMF_DLM_REQ
};

static const struct req_msg_field *mds_rmfid_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_FID_ARRAY,
	&RMF_CAPA1,
	&RMF_CAPA2,
};

static const struct req_msg_field *mds_rmfid_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_FID_ARRAY,
	&RMF_RCS,
};

static const struct req_msg_field *obd_connect_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_TGTUUID,
	&RMF_CLUUID,
	&RMF_CONN,
	&RMF_CONNECT_DATA,
	&RMF_SELINUX_POL
};

static const struct req_msg_field *obd_connect_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_CONNECT_DATA
};

static const struct req_msg_field *obd_set_info_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_SETINFO_KEY,
        &RMF_SETINFO_VAL
};

static const struct req_msg_field *mdt_set_info_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_SETINFO_KEY,
	&RMF_SETINFO_VAL,
	&RMF_MDT_BODY
};

static const struct req_msg_field *ost_grant_shrink_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_SETINFO_KEY,
        &RMF_OST_BODY
};

static const struct req_msg_field *mds_getinfo_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_GETINFO_KEY,
        &RMF_GETINFO_VALLEN
};

static const struct req_msg_field *mds_getinfo_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_GETINFO_VAL,
};

static const struct req_msg_field *ldlm_enqueue_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_DLM_REQ
};

static const struct req_msg_field *ldlm_enqueue_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_DLM_REP
};

static const struct req_msg_field *ldlm_enqueue_lvb_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_DLM_REP,
        &RMF_DLM_LVB
};

static const struct req_msg_field *ldlm_cp_callback_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_DLM_REQ,
        &RMF_DLM_LVB
};

static const struct req_msg_field *ldlm_gl_callback_desc_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_DLM_GL_DESC
};

static const struct req_msg_field *ldlm_gl_callback_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_DLM_LVB
};

static const struct req_msg_field *ldlm_intent_basic_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
};

static const struct req_msg_field *ldlm_intent_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_DLM_REQ,
        &RMF_LDLM_INTENT,
        &RMF_REC_REINT
};

static const struct req_msg_field *ldlm_intent_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_DLM_REP,
        &RMF_MDT_BODY,
        &RMF_MDT_MD,
        &RMF_ACL
};

static const struct req_msg_field *ldlm_intent_layout_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_LAYOUT_INTENT,
	&RMF_EADATA /* for new layout to be set up */
};

static const struct req_msg_field *ldlm_intent_open_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REP,
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
	&RMF_ACL,
	&RMF_CAPA1,
	&RMF_CAPA2,
	&RMF_NIOBUF_INLINE,
	&RMF_FILE_SECCTX,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *ldlm_intent_getattr_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_MDT_BODY,     /* coincides with mds_getattr_name_client[] */
	&RMF_CAPA1,
	&RMF_NAME,
	&RMF_FILE_SECCTX_NAME
};

static const struct req_msg_field *ldlm_intent_getattr_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REP,
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
	&RMF_ACL,
	&RMF_CAPA1,
	&RMF_FILE_SECCTX,
	&RMF_DEFAULT_MDT_MD,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *ldlm_intent_create_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_REC_REINT,    /* coincides with mds_reint_create_client[] */
	&RMF_CAPA1,
	&RMF_NAME,
	&RMF_EADATA,
	&RMF_FILE_SECCTX_NAME,
	&RMF_FILE_SECCTX,
	&RMF_SELINUX_POL,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *ldlm_intent_open_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_REC_REINT,    /* coincides with mds_reint_open_client[] */
	&RMF_CAPA1,
	&RMF_CAPA2,
	&RMF_NAME,
	&RMF_EADATA,
	&RMF_FILE_SECCTX_NAME,
	&RMF_FILE_SECCTX,
	&RMF_SELINUX_POL,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *ldlm_intent_getxattr_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_MDT_BODY,
	&RMF_CAPA1,
	&RMF_SELINUX_POL
};

static const struct req_msg_field *ldlm_intent_getxattr_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REP,
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
	&RMF_ACL, /* for req_capsule_extend/mdt_intent_policy */
	&RMF_EADATA,
	&RMF_EAVALS,
	&RMF_EAVALS_LENS
};

static const struct req_msg_field *ldlm_intent_setattr_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_REC_REINT,
	&RMF_CAPA1,
	&RMF_MDT_EPOCH,
	&RMF_EADATA,
	&RMF_LOGCOOKIES,
};

static const struct req_msg_field *ldlm_intent_setattr_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_DLM_REP,
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
	&RMF_ACL,
	&RMF_CAPA1,
	&RMF_CAPA2
};

static const struct req_msg_field *mds_get_root_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_NAME
};

static const struct req_msg_field *mds_getxattr_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_CAPA1,
	&RMF_NAME,
	&RMF_EADATA,
	&RMF_SELINUX_POL
};

static const struct req_msg_field *mds_getxattr_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY,
        &RMF_EADATA
};

static const struct req_msg_field *mds_getattr_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY,
        &RMF_MDT_MD,
        &RMF_ACL,
        &RMF_CAPA1,
        &RMF_CAPA2
};

static const struct req_msg_field *mds_setattr_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_MDT_BODY,
        &RMF_MDT_MD,
        &RMF_ACL,
        &RMF_CAPA1,
        &RMF_CAPA2
};

static const struct req_msg_field *mds_create_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
	&RMF_CAPA1,
};

static const struct req_msg_field *mds_batch_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_BUT_HEADER,
	&RMF_BUT_BUF,
};

static const struct req_msg_field *mds_batch_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_BUT_REPLY,
};

static const struct req_msg_field *llog_origin_handle_create_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_LLOGD_BODY,
	&RMF_NAME,
	&RMF_MDT_BODY
};

static const struct req_msg_field *llogd_body_only[] = {
        &RMF_PTLRPC_BODY,
        &RMF_LLOGD_BODY
};

static const struct req_msg_field *llog_log_hdr_only[] = {
        &RMF_PTLRPC_BODY,
        &RMF_LLOG_LOG_HDR
};

static const struct req_msg_field *llog_origin_handle_next_block_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_LLOGD_BODY,
        &RMF_EADATA
};

static const struct req_msg_field *obd_idx_read_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_IDX_INFO
};

static const struct req_msg_field *obd_idx_read_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_IDX_INFO
};

static const struct req_msg_field *ost_body_only[] = {
        &RMF_PTLRPC_BODY,
        &RMF_OST_BODY
};

static const struct req_msg_field *ost_body_capa[] = {
        &RMF_PTLRPC_BODY,
        &RMF_OST_BODY,
        &RMF_CAPA1
};

static const struct req_msg_field *ost_destroy_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_OST_BODY,
        &RMF_DLM_REQ,
        &RMF_CAPA1
};


static const struct req_msg_field *ost_brw_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_OST_BODY,
	&RMF_OBD_IOOBJ,
	&RMF_NIOBUF_REMOTE,
	&RMF_CAPA1,
	&RMF_SHORT_IO
};

static const struct req_msg_field *ost_brw_read_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_OST_BODY,
	&RMF_SHORT_IO
};

static const struct req_msg_field *ost_brw_write_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_OST_BODY,
        &RMF_RCS
};

static const struct req_msg_field *ost_get_info_generic_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_GENERIC_DATA,
};

static const struct req_msg_field *ost_get_info_generic_client[] = {
        &RMF_PTLRPC_BODY,
	&RMF_GETINFO_KEY
};

static const struct req_msg_field *ost_get_last_id_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_OBD_ID
};

static const struct req_msg_field *ost_get_last_fid_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_GETINFO_KEY,
	&RMF_FID,
};

static const struct req_msg_field *ost_get_last_fid_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_FID,
};

static const struct req_msg_field *ost_get_fiemap_client[] = {
        &RMF_PTLRPC_BODY,
        &RMF_FIEMAP_KEY,
        &RMF_FIEMAP_VAL
};

static const struct req_msg_field *ost_ladvise[] = {
	&RMF_PTLRPC_BODY,
	&RMF_OST_BODY,
	&RMF_OST_LADVISE_HDR,
	&RMF_OST_LADVISE,
};

static const struct req_msg_field *ost_get_fiemap_server[] = {
        &RMF_PTLRPC_BODY,
        &RMF_FIEMAP_VAL
};

static const struct req_msg_field *mdt_hsm_progress[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_MDS_HSM_PROGRESS,
};

static const struct req_msg_field *mdt_hsm_ct_register[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_MDS_HSM_ARCHIVE,
};

static const struct req_msg_field *mdt_hsm_ct_unregister[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
};

static const struct req_msg_field *mdt_hsm_action_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_MDS_HSM_CURRENT_ACTION,
};

static const struct req_msg_field *mdt_hsm_state_get_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_HSM_USER_STATE,
};

static const struct req_msg_field *mdt_hsm_state_set[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_CAPA1,
	&RMF_HSM_STATE_SET,
};

static const struct req_msg_field *mdt_hsm_request[] = {
	&RMF_PTLRPC_BODY,
	&RMF_MDT_BODY,
	&RMF_MDS_HSM_REQUEST,
	&RMF_MDS_HSM_USER_ITEM,
	&RMF_GENERIC_DATA,
};

static const struct req_msg_field *obd_lfsck_request[] = {
	&RMF_PTLRPC_BODY,
	&RMF_LFSCK_REQUEST,
};

static const struct req_msg_field *obd_lfsck_reply[] = {
	&RMF_PTLRPC_BODY,
	&RMF_LFSCK_REPLY,
};

static const struct req_msg_field *mds_batch_getattr_client[] = {
	&RMF_DLM_REQ,
	&RMF_LDLM_INTENT,
	&RMF_MDT_BODY,     /* coincides with mds_getattr_name_client[] */
	&RMF_CAPA1,
	&RMF_NAME,
	&RMF_FILE_SECCTX_NAME
};

static const struct req_msg_field *mds_batch_getattr_server[] = {
	&RMF_DLM_REP,
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
	&RMF_ACL,
	&RMF_CAPA1,
	&RMF_FILE_SECCTX,
	&RMF_DEFAULT_MDT_MD,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *create_exlock_client[] = {
	&RMF_DLM_REQ,
	&RMF_REC_REINT,
	&RMF_NAME,
	&RMF_EADATA,
	&RMF_FILE_SECCTX_NAME,
	&RMF_FILE_SECCTX,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *create_exlock_server[] = {
	&RMF_DLM_REP,
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
};

static const struct req_msg_field *create_lockless_client[] = {
	&RMF_REC_REINT,
	&RMF_NAME,
	&RMF_EADATA,
	&RMF_FILE_SECCTX_NAME,
	&RMF_FILE_SECCTX,
	&RMF_FILE_ENCCTX,
};

static const struct req_msg_field *create_lockless_server[] = {
	&RMF_MDT_BODY,
	&RMF_MDT_MD,
};

static const struct req_msg_field *setattr_exlock_client[] = {
	&RMF_DLM_REQ,
	&RMF_REC_REINT,
};

static const struct req_msg_field *setattr_exlock_server[] = {
	&RMF_DLM_REP,
};

static const struct req_msg_field *setattr_lockless_client[] = {
	&RMF_REC_REINT,
};

static const struct req_msg_field *exlock_only_client[] = {
	&RMF_DLM_REQ,
};

static const struct req_msg_field *exlock_only_server[] = {
	&RMF_DLM_REP,
};

static struct req_format *req_formats[] = {
	&RQF_OBD_PING,
	&RQF_OBD_SET_INFO,
	&RQF_MDT_SET_INFO,
	&RQF_OBD_IDX_READ,
	&RQF_SEC_CTX,
	&RQF_MGS_TARGET_REG,
#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(2, 18, 53, 0)
	&RQF_MGS_SET_INFO,
#endif
	&RQF_MGS_CONFIG_READ,
	&RQF_SEQ_QUERY,
	&RQF_FLD_QUERY,
	&RQF_FLD_READ,
	&RQF_MDS_CONNECT,
	&RQF_MDS_DISCONNECT,
	&RQF_MDS_GET_INFO,
	&RQF_MDS_GET_ROOT,
	&RQF_MDS_STATFS,
	&RQF_MDS_STATFS_NEW,
	&RQF_MDS_GETATTR,
	&RQF_MDS_GETATTR_NAME,
	&RQF_MDS_GETXATTR,
	&RQF_MDS_SYNC,
	&RQF_MDS_CLOSE,
	&RQF_MDS_CLOSE_INTENT,
	&RQF_MDS_READPAGE,
	&RQF_MDS_REINT,
	&RQF_MDS_REINT_CREATE,
	&RQF_MDS_REINT_CREATE_ACL,
	&RQF_MDS_REINT_CREATE_SLAVE,
	&RQF_MDS_REINT_CREATE_SYM,
	&RQF_MDS_REINT_CREATE_REG,
	&RQF_MDS_REINT_OPEN,
	&RQF_MDS_REINT_UNLINK,
	&RQF_MDS_REINT_LINK,
	&RQF_MDS_REINT_RENAME,
	&RQF_MDS_REINT_MIGRATE,
	&RQF_MDS_REINT_SETATTR,
	&RQF_MDS_REINT_SETXATTR,
	&RQF_MDS_REINT_RESYNC,
	&RQF_MDS_QUOTACTL,
	&RQF_MDS_HSM_PROGRESS,
	&RQF_MDS_HSM_CT_REGISTER,
	&RQF_MDS_HSM_CT_UNREGISTER,
	&RQF_MDS_HSM_STATE_GET,
	&RQF_MDS_HSM_STATE_SET,
	&RQF_MDS_HSM_ACTION,
	&RQF_MDS_HSM_REQUEST,
	&RQF_MDS_SWAP_LAYOUTS,
	&RQF_MDS_RMFID,
#ifdef HAVE_SERVER_SUPPORT
	&RQF_OUT_UPDATE,
#endif
	&RQF_OST_CONNECT,
	&RQF_OST_DISCONNECT,
	&RQF_OST_QUOTACTL,
	&RQF_OST_GETATTR,
	&RQF_OST_SETATTR,
	&RQF_OST_CREATE,
	&RQF_OST_PUNCH,
	&RQF_OST_FALLOCATE,
	&RQF_OST_SYNC,
	&RQF_OST_DESTROY,
	&RQF_OST_BRW_READ,
	&RQF_OST_BRW_WRITE,
	&RQF_OST_STATFS,
	&RQF_OST_SET_GRANT_INFO,
	&RQF_OST_GET_INFO,
	&RQF_OST_GET_INFO_LAST_ID,
	&RQF_OST_GET_INFO_LAST_FID,
	&RQF_OST_SET_INFO_LAST_FID,
	&RQF_OST_GET_INFO_FIEMAP,
	&RQF_OST_LADVISE,
	&RQF_OST_SEEK,
	&RQF_LDLM_ENQUEUE,
	&RQF_LDLM_ENQUEUE_LVB,
	&RQF_LDLM_CONVERT,
	&RQF_LDLM_CANCEL,
	&RQF_LDLM_CALLBACK,
	&RQF_LDLM_CP_CALLBACK,
	&RQF_LDLM_BL_CALLBACK,
	&RQF_LDLM_GL_CALLBACK,
	&RQF_LDLM_GL_CALLBACK_DESC,
	&RQF_LDLM_INTENT,
	&RQF_LDLM_INTENT_BASIC,
	&RQF_LDLM_INTENT_LAYOUT,
	&RQF_LDLM_INTENT_GETATTR,
	&RQF_LDLM_INTENT_OPEN,
	&RQF_LDLM_INTENT_CREATE,
	&RQF_LDLM_INTENT_GETXATTR,
	&RQF_LDLM_INTENT_QUOTA,
	&RQF_LDLM_INTENT_SETATTR,
	&RQF_QUOTA_DQACQ,
	&RQF_LLOG_ORIGIN_HANDLE_CREATE,
	&RQF_LLOG_ORIGIN_HANDLE_NEXT_BLOCK,
	&RQF_LLOG_ORIGIN_HANDLE_PREV_BLOCK,
	&RQF_LLOG_ORIGIN_HANDLE_READ_HEADER,
	&RQF_CONNECT,
	&RQF_LFSCK_NOTIFY,
	&RQF_LFSCK_QUERY,
	&RQF_BUT_GETATTR,
	&RQF_BUT_CREATE_EXLOCK,
	&RQF_BUT_CREATE_LOCKLESS,
	&RQF_BUT_SETATTR_EXLOCK,
	&RQF_BUT_SETATTR_LOCKLESS,
	&RQF_BUT_EXLOCK_ONLY,
	&RQF_MDS_BATCH,
};

struct req_msg_field {
	const __u32 rmf_flags;
	const char  *rmf_name;
	/**
	 * Field length. (-1) means "variable length".  If the
	 * \a RMF_F_STRUCT_ARRAY flag is set the field is also variable-length,
	 * but the actual size must be a whole multiple of \a rmf_size.
	 */
	const int   rmf_size;
	void	    (*rmf_swabber)(void *);
	/**
	 * Pass buffer size to swabbing function
	 * \retval	> 0		the number of bytes swabbed
	 *		-EOVERFLOW	on error
	 */
	int	    (*rmf_swab_len)(void *, __u32);
	void	    (*rmf_dumper)(void *);
	int	    rmf_offset[ARRAY_SIZE(req_formats)][RCL_NR];
};

enum rmf_flags {
	/**
	 * The field is a string, must be NUL-terminated.
	 */
	RMF_F_STRING		= BIT(0),
	/**
	 * The field's buffer size need not match the declared \a rmf_size.
	 */
	RMF_F_NO_SIZE_CHECK	= BIT(1),
	/**
	 * The field's buffer size must be a whole multiple of the declared \a
	 * rmf_size and the \a rmf_swabber function must work on the declared \a
	 * rmf_size worth of bytes.
	 */
	RMF_F_STRUCT_ARRAY	= BIT(2),
};

struct req_capsule;

/*
 * Request fields.
 */
#define DEFINE_MSGF(name, flags, size, swabber, dumper) {       \
        .rmf_name    = (name),                                  \
        .rmf_flags   = (flags),                                 \
        .rmf_size    = (size),                                  \
        .rmf_swabber = (void (*)(void*))(swabber),              \
        .rmf_dumper  = (void (*)(void*))(dumper)                \
}

#define DEFINE_MSGFL(name, flags, size, swab_len, dumper) {	\
	.rmf_name     = (name),					\
	.rmf_flags    = (flags),				\
	.rmf_size     = (size),					\
	.rmf_swab_len = (int (*)(void *, __u32))(swab_len),	\
	.rmf_dumper   = (void (*)(void *))(dumper)		\
}

struct req_msg_field RMF_GENERIC_DATA =
        DEFINE_MSGF("generic_data", 0,
                    -1, NULL, NULL);
EXPORT_SYMBOL(RMF_GENERIC_DATA);

struct req_msg_field RMF_MGS_TARGET_INFO =
        DEFINE_MSGF("mgs_target_info", 0,
                    sizeof(struct mgs_target_info),
                    lustre_swab_mgs_target_info, NULL);
EXPORT_SYMBOL(RMF_MGS_TARGET_INFO);

#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(2, 18, 53, 0)
struct req_msg_field RMF_MGS_SEND_PARAM =
	DEFINE_MSGF("mgs_send_param", 0,
		    sizeof(struct mgs_send_param),
		    NULL, NULL);
EXPORT_SYMBOL(RMF_MGS_SEND_PARAM);
#endif

struct req_msg_field RMF_MGS_CONFIG_BODY =
        DEFINE_MSGF("mgs_config_read request", 0,
                    sizeof(struct mgs_config_body),
                    lustre_swab_mgs_config_body, NULL);
EXPORT_SYMBOL(RMF_MGS_CONFIG_BODY);

struct req_msg_field RMF_MGS_CONFIG_RES =
        DEFINE_MSGF("mgs_config_read reply ", 0,
                    sizeof(struct mgs_config_res),
                    lustre_swab_mgs_config_res, NULL);
EXPORT_SYMBOL(RMF_MGS_CONFIG_RES);

struct req_msg_field RMF_U32 =
	DEFINE_MSGF("generic u32", RMF_F_STRUCT_ARRAY,
		    sizeof(__u32), lustre_swab_generic_32s, NULL);
EXPORT_SYMBOL(RMF_U32);

struct req_msg_field RMF_SETINFO_VAL =
        DEFINE_MSGF("setinfo_val", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_SETINFO_VAL);

struct req_msg_field RMF_GETINFO_KEY =
        DEFINE_MSGF("getinfo_key", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_GETINFO_KEY);

struct req_msg_field RMF_GETINFO_VALLEN =
        DEFINE_MSGF("getinfo_vallen", 0,
                    sizeof(__u32), lustre_swab_generic_32s, NULL);
EXPORT_SYMBOL(RMF_GETINFO_VALLEN);

struct req_msg_field RMF_GETINFO_VAL =
        DEFINE_MSGF("getinfo_val", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_GETINFO_VAL);

struct req_msg_field RMF_SEQ_OPC =
        DEFINE_MSGF("seq_query_opc", 0,
                    sizeof(__u32), lustre_swab_generic_32s, NULL);
EXPORT_SYMBOL(RMF_SEQ_OPC);

struct req_msg_field RMF_SEQ_RANGE =
        DEFINE_MSGF("seq_query_range", 0,
                    sizeof(struct lu_seq_range),
                    lustre_swab_lu_seq_range, NULL);
EXPORT_SYMBOL(RMF_SEQ_RANGE);

struct req_msg_field RMF_FLD_OPC =
        DEFINE_MSGF("fld_query_opc", 0,
                    sizeof(__u32), lustre_swab_generic_32s, NULL);
EXPORT_SYMBOL(RMF_FLD_OPC);

struct req_msg_field RMF_FLD_MDFLD =
        DEFINE_MSGF("fld_query_mdfld", 0,
                    sizeof(struct lu_seq_range),
                    lustre_swab_lu_seq_range, NULL);
EXPORT_SYMBOL(RMF_FLD_MDFLD);

struct req_msg_field RMF_MDT_BODY =
        DEFINE_MSGF("mdt_body", 0,
                    sizeof(struct mdt_body), lustre_swab_mdt_body, NULL);
EXPORT_SYMBOL(RMF_MDT_BODY);

struct req_msg_field RMF_OBD_QUOTACTL =
	DEFINE_MSGFL("obd_quotactl",
		     0,
		     sizeof(struct obd_quotactl),
		     lustre_swab_obd_quotactl, NULL);
EXPORT_SYMBOL(RMF_OBD_QUOTACTL);

struct req_msg_field RMF_QUOTA_BODY =
	DEFINE_MSGF("quota_body", 0,
		    sizeof(struct quota_body), lustre_swab_quota_body, NULL);
EXPORT_SYMBOL(RMF_QUOTA_BODY);

struct req_msg_field RMF_MDT_EPOCH =
        DEFINE_MSGF("mdt_ioepoch", 0,
                    sizeof(struct mdt_ioepoch), lustre_swab_mdt_ioepoch, NULL);
EXPORT_SYMBOL(RMF_MDT_EPOCH);

struct req_msg_field RMF_PTLRPC_BODY =
        DEFINE_MSGF("ptlrpc_body", 0,
                    sizeof(struct ptlrpc_body), lustre_swab_ptlrpc_body, NULL);
EXPORT_SYMBOL(RMF_PTLRPC_BODY);

struct req_msg_field RMF_CLOSE_DATA =
	DEFINE_MSGF("data_version", 0,
		    sizeof(struct close_data), lustre_swab_close_data, NULL);
EXPORT_SYMBOL(RMF_CLOSE_DATA);

struct req_msg_field RMF_OBD_STATFS =
        DEFINE_MSGF("obd_statfs", 0,
                    sizeof(struct obd_statfs), lustre_swab_obd_statfs, NULL);
EXPORT_SYMBOL(RMF_OBD_STATFS);

struct req_msg_field RMF_SETINFO_KEY =
        DEFINE_MSGF("setinfo_key", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_SETINFO_KEY);

struct req_msg_field RMF_NAME =
        DEFINE_MSGF("name", RMF_F_STRING, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_NAME);

struct req_msg_field RMF_FID_ARRAY =
	DEFINE_MSGF("fid_array", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_FID_ARRAY);

struct req_msg_field RMF_SYMTGT =
        DEFINE_MSGF("symtgt", RMF_F_STRING, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_SYMTGT);

struct req_msg_field RMF_TGTUUID =
        DEFINE_MSGF("tgtuuid", RMF_F_STRING, sizeof(struct obd_uuid) - 1, NULL,
        NULL);
EXPORT_SYMBOL(RMF_TGTUUID);

struct req_msg_field RMF_CLUUID =
        DEFINE_MSGF("cluuid", RMF_F_STRING, sizeof(struct obd_uuid) - 1, NULL,
        NULL);
EXPORT_SYMBOL(RMF_CLUUID);

struct req_msg_field RMF_STRING =
        DEFINE_MSGF("string", RMF_F_STRING, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_STRING);

struct req_msg_field RMF_FILE_SECCTX_NAME =
	DEFINE_MSGF("file_secctx_name", RMF_F_STRING, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_FILE_SECCTX_NAME);

struct req_msg_field RMF_FILE_SECCTX =
	DEFINE_MSGF("file_secctx", RMF_F_NO_SIZE_CHECK, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_FILE_SECCTX);

struct req_msg_field RMF_FILE_ENCCTX =
	DEFINE_MSGF("file_encctx", RMF_F_NO_SIZE_CHECK, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_FILE_ENCCTX);

struct req_msg_field RMF_LLOGD_BODY =
        DEFINE_MSGF("llogd_body", 0,
                    sizeof(struct llogd_body), lustre_swab_llogd_body, NULL);
EXPORT_SYMBOL(RMF_LLOGD_BODY);

struct req_msg_field RMF_LLOG_LOG_HDR =
        DEFINE_MSGF("llog_log_hdr", 0,
                    sizeof(struct llog_log_hdr), lustre_swab_llog_hdr, NULL);
EXPORT_SYMBOL(RMF_LLOG_LOG_HDR);

struct req_msg_field RMF_LLOGD_CONN_BODY =
        DEFINE_MSGF("llogd_conn_body", 0,
                    sizeof(struct llogd_conn_body),
                    lustre_swab_llogd_conn_body, NULL);
EXPORT_SYMBOL(RMF_LLOGD_CONN_BODY);

/*
 * connection handle received in MDS_CONNECT request.
 *
 * No swabbing needed because struct lustre_handle contains only a 64-bit cookie
 * that the client does not interpret at all.
 */
struct req_msg_field RMF_CONN =
        DEFINE_MSGF("conn", 0, sizeof(struct lustre_handle), NULL, NULL);
EXPORT_SYMBOL(RMF_CONN);

struct req_msg_field RMF_CONNECT_DATA =
	DEFINE_MSGF("cdata",
		    RMF_F_NO_SIZE_CHECK /* we allow extra space for interop */,
		    sizeof(struct obd_connect_data),
		    lustre_swab_connect, NULL);
EXPORT_SYMBOL(RMF_CONNECT_DATA);

struct req_msg_field RMF_DLM_REQ =
        DEFINE_MSGF("dlm_req", RMF_F_NO_SIZE_CHECK /* ldlm_request_bufsize */,
                    sizeof(struct ldlm_request),
                    lustre_swab_ldlm_request, NULL);
EXPORT_SYMBOL(RMF_DLM_REQ);

struct req_msg_field RMF_DLM_REP =
        DEFINE_MSGF("dlm_rep", 0,
                    sizeof(struct ldlm_reply), lustre_swab_ldlm_reply, NULL);
EXPORT_SYMBOL(RMF_DLM_REP);

struct req_msg_field RMF_LDLM_INTENT =
        DEFINE_MSGF("ldlm_intent", 0,
                    sizeof(struct ldlm_intent), lustre_swab_ldlm_intent, NULL);
EXPORT_SYMBOL(RMF_LDLM_INTENT);

struct req_msg_field RMF_DLM_LVB =
	DEFINE_MSGF("dlm_lvb", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_DLM_LVB);

struct req_msg_field RMF_DLM_GL_DESC =
	DEFINE_MSGF("dlm_gl_desc", 0, sizeof(union ldlm_gl_desc), NULL, NULL);
EXPORT_SYMBOL(RMF_DLM_GL_DESC);

struct req_msg_field RMF_MDT_MD =
        DEFINE_MSGF("mdt_md", RMF_F_NO_SIZE_CHECK, MIN_MD_SIZE, NULL, NULL);
EXPORT_SYMBOL(RMF_MDT_MD);

struct req_msg_field RMF_DEFAULT_MDT_MD =
	DEFINE_MSGF("default_mdt_md", RMF_F_NO_SIZE_CHECK, MIN_MD_SIZE, NULL,
		    NULL);
EXPORT_SYMBOL(RMF_DEFAULT_MDT_MD);

struct req_msg_field RMF_REC_REINT =
        DEFINE_MSGF("rec_reint", 0, sizeof(struct mdt_rec_reint),
                    lustre_swab_mdt_rec_reint, NULL);
EXPORT_SYMBOL(RMF_REC_REINT);

/* FIXME: this length should be defined as a macro */
struct req_msg_field RMF_EADATA = DEFINE_MSGF("eadata", 0, -1,
                                                    NULL, NULL);
EXPORT_SYMBOL(RMF_EADATA);

struct req_msg_field RMF_EAVALS = DEFINE_MSGF("eavals", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_EAVALS);

struct req_msg_field RMF_ACL = DEFINE_MSGF("acl", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_ACL);

/* FIXME: this should be made to use RMF_F_STRUCT_ARRAY */
struct req_msg_field RMF_LOGCOOKIES =
        DEFINE_MSGF("logcookies", RMF_F_NO_SIZE_CHECK /* multiple cookies */,
                    sizeof(struct llog_cookie), NULL, NULL);
EXPORT_SYMBOL(RMF_LOGCOOKIES);

struct req_msg_field RMF_CAPA1 =
	DEFINE_MSGF("capa", 0, 0, NULL, NULL);
EXPORT_SYMBOL(RMF_CAPA1);

struct req_msg_field RMF_CAPA2 =
	DEFINE_MSGF("capa", 0, 0, NULL, NULL);
EXPORT_SYMBOL(RMF_CAPA2);

struct req_msg_field RMF_LAYOUT_INTENT =
	DEFINE_MSGF("layout_intent", 0,
		    sizeof(struct layout_intent), lustre_swab_layout_intent,
		    NULL);
EXPORT_SYMBOL(RMF_LAYOUT_INTENT);

struct req_msg_field RMF_SELINUX_POL =
	DEFINE_MSGF("selinux_pol", RMF_F_STRING, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_SELINUX_POL);

/*
 * OST request field.
 */
struct req_msg_field RMF_OST_BODY =
	DEFINE_MSGF("ost_body", 0,
		    sizeof(struct ost_body), lustre_swab_ost_body,
		    dump_ost_body);
EXPORT_SYMBOL(RMF_OST_BODY);

struct req_msg_field RMF_OBD_IOOBJ =
        DEFINE_MSGF("obd_ioobj", RMF_F_STRUCT_ARRAY,
                    sizeof(struct obd_ioobj), lustre_swab_obd_ioobj, dump_ioo);
EXPORT_SYMBOL(RMF_OBD_IOOBJ);

struct req_msg_field RMF_NIOBUF_REMOTE =
        DEFINE_MSGF("niobuf_remote", RMF_F_STRUCT_ARRAY,
                    sizeof(struct niobuf_remote), lustre_swab_niobuf_remote,
                    dump_rniobuf);
EXPORT_SYMBOL(RMF_NIOBUF_REMOTE);

struct req_msg_field RMF_NIOBUF_INLINE =
	DEFINE_MSGF("niobuf_inline", RMF_F_NO_SIZE_CHECK,
		    sizeof(struct niobuf_remote), lustre_swab_niobuf_remote,
		    dump_rniobuf);
EXPORT_SYMBOL(RMF_NIOBUF_INLINE);

struct req_msg_field RMF_RCS =
	DEFINE_MSGF("niobuf_rcs", RMF_F_STRUCT_ARRAY, sizeof(__u32),
		    lustre_swab_generic_32s, dump_rcs);
EXPORT_SYMBOL(RMF_RCS);

struct req_msg_field RMF_EAVALS_LENS =
	DEFINE_MSGF("eavals_lens", RMF_F_STRUCT_ARRAY, sizeof(__u32),
		lustre_swab_generic_32s, NULL);
EXPORT_SYMBOL(RMF_EAVALS_LENS);

struct req_msg_field RMF_OBD_ID =
	DEFINE_MSGF("obd_id", 0,
		    sizeof(__u64), lustre_swab_ost_last_id, NULL);
EXPORT_SYMBOL(RMF_OBD_ID);

struct req_msg_field RMF_FID =
	DEFINE_MSGF("fid", 0,
		    sizeof(struct lu_fid), lustre_swab_lu_fid, NULL);
EXPORT_SYMBOL(RMF_FID);

struct req_msg_field RMF_OST_ID =
	DEFINE_MSGF("ost_id", 0,
		    sizeof(struct ost_id), lustre_swab_ost_id, NULL);
EXPORT_SYMBOL(RMF_OST_ID);

struct req_msg_field RMF_FIEMAP_KEY =
	DEFINE_MSGF("fiemap_key", 0, sizeof(struct ll_fiemap_info_key),
		    lustre_swab_fiemap_info_key, NULL);
EXPORT_SYMBOL(RMF_FIEMAP_KEY);

struct req_msg_field RMF_FIEMAP_VAL =
	DEFINE_MSGFL("fiemap", 0, -1, lustre_swab_fiemap, NULL);
EXPORT_SYMBOL(RMF_FIEMAP_VAL);

struct req_msg_field RMF_IDX_INFO =
	DEFINE_MSGF("idx_info", 0, sizeof(struct idx_info),
		    lustre_swab_idx_info, NULL);
EXPORT_SYMBOL(RMF_IDX_INFO);
struct req_msg_field RMF_SHORT_IO =
	DEFINE_MSGF("short_io", 0, -1, NULL, NULL);
EXPORT_SYMBOL(RMF_SHORT_IO);
struct req_msg_field RMF_HSM_USER_STATE =
	DEFINE_MSGF("hsm_user_state", 0, sizeof(struct hsm_user_state),
		    lustre_swab_hsm_user_state, NULL);
EXPORT_SYMBOL(RMF_HSM_USER_STATE);

struct req_msg_field RMF_HSM_STATE_SET =
	DEFINE_MSGF("hsm_state_set", 0, sizeof(struct hsm_state_set),
		    lustre_swab_hsm_state_set, NULL);
EXPORT_SYMBOL(RMF_HSM_STATE_SET);

struct req_msg_field RMF_MDS_HSM_PROGRESS =
	DEFINE_MSGF("hsm_progress", 0, sizeof(struct hsm_progress_kernel),
		    lustre_swab_hsm_progress_kernel, NULL);
EXPORT_SYMBOL(RMF_MDS_HSM_PROGRESS);

struct req_msg_field RMF_MDS_HSM_CURRENT_ACTION =
	DEFINE_MSGF("hsm_current_action", 0, sizeof(struct hsm_current_action),
		    lustre_swab_hsm_current_action, NULL);
EXPORT_SYMBOL(RMF_MDS_HSM_CURRENT_ACTION);

struct req_msg_field RMF_MDS_HSM_USER_ITEM =
	DEFINE_MSGF("hsm_user_item", RMF_F_STRUCT_ARRAY,
		    sizeof(struct hsm_user_item), lustre_swab_hsm_user_item,
		    NULL);
EXPORT_SYMBOL(RMF_MDS_HSM_USER_ITEM);

struct req_msg_field RMF_MDS_HSM_ARCHIVE =
	DEFINE_MSGF("hsm_archive", RMF_F_STRUCT_ARRAY,
		    sizeof(__u32), lustre_swab_generic_32s, NULL);
EXPORT_SYMBOL(RMF_MDS_HSM_ARCHIVE);

struct req_msg_field RMF_MDS_HSM_REQUEST =
	DEFINE_MSGF("hsm_request", 0, sizeof(struct hsm_request),
		    lustre_swab_hsm_request, NULL);
EXPORT_SYMBOL(RMF_MDS_HSM_REQUEST);

struct req_msg_field RMF_SWAP_LAYOUTS =
	DEFINE_MSGF("swap_layouts", 0, sizeof(struct  mdc_swap_layouts),
		    lustre_swab_swap_layouts, NULL);
EXPORT_SYMBOL(RMF_SWAP_LAYOUTS);

struct req_msg_field RMF_LFSCK_REQUEST =
	DEFINE_MSGF("lfsck_request", 0, sizeof(struct lfsck_request),
		    lustre_swab_lfsck_request, NULL);
EXPORT_SYMBOL(RMF_LFSCK_REQUEST);

struct req_msg_field RMF_LFSCK_REPLY =
	DEFINE_MSGF("lfsck_reply", 0, sizeof(struct lfsck_reply),
		    lustre_swab_lfsck_reply, NULL);
EXPORT_SYMBOL(RMF_LFSCK_REPLY);

struct req_msg_field RMF_OST_LADVISE_HDR =
	DEFINE_MSGF("ladvise_request", 0,
		    sizeof(struct ladvise_hdr),
		    lustre_swab_ladvise_hdr, NULL);
EXPORT_SYMBOL(RMF_OST_LADVISE_HDR);

struct req_msg_field RMF_OST_LADVISE =
	DEFINE_MSGF("ladvise_request", RMF_F_STRUCT_ARRAY,
		    sizeof(struct lu_ladvise),
		    lustre_swab_ladvise, NULL);
EXPORT_SYMBOL(RMF_OST_LADVISE);

struct req_msg_field RMF_BUT_REPLY =
			DEFINE_MSGF("batch_update_reply", 0, -1,
				    lustre_swab_batch_update_reply, NULL);
EXPORT_SYMBOL(RMF_BUT_REPLY);

struct req_msg_field RMF_BUT_HEADER = DEFINE_MSGF("but_update_header", 0,
				-1, lustre_swab_but_update_header, NULL);
EXPORT_SYMBOL(RMF_BUT_HEADER);

struct req_msg_field RMF_BUT_BUF = DEFINE_MSGF("but_update_buf",
			RMF_F_STRUCT_ARRAY, sizeof(struct but_update_buffer),
			lustre_swab_but_update_buffer, NULL);
EXPORT_SYMBOL(RMF_BUT_BUF);

/*
 * Request formats.
 */

struct req_format {
	const char *rf_name;
	size_t	    rf_idx;
	struct {
		size_t			     nr;
		const struct req_msg_field **d;
	} rf_fields[RCL_NR];
};

#define DEFINE_REQ_FMT(name, client, client_nr, server, server_nr) {    \
        .rf_name   = name,                                              \
        .rf_fields = {                                                  \
                [RCL_CLIENT] = {                                        \
                        .nr = client_nr,                                \
                        .d  = client                                    \
                },                                                      \
                [RCL_SERVER] = {                                        \
                        .nr = server_nr,                                \
                        .d  = server                                    \
                }                                                       \
        }                                                               \
}

#define DEFINE_REQ_FMT0(name, client, server)                                  \
DEFINE_REQ_FMT(name, client, ARRAY_SIZE(client), server, ARRAY_SIZE(server))

struct req_format RQF_OBD_PING =
        DEFINE_REQ_FMT0("OBD_PING", empty, empty);
EXPORT_SYMBOL(RQF_OBD_PING);

struct req_format RQF_OBD_SET_INFO =
        DEFINE_REQ_FMT0("OBD_SET_INFO", obd_set_info_client, empty);
EXPORT_SYMBOL(RQF_OBD_SET_INFO);

struct req_format RQF_MDT_SET_INFO =
	DEFINE_REQ_FMT0("MDT_SET_INFO", mdt_set_info_client, empty);
EXPORT_SYMBOL(RQF_MDT_SET_INFO);

/* Read index file through the network */
struct req_format RQF_OBD_IDX_READ =
	DEFINE_REQ_FMT0("OBD_IDX_READ",
			obd_idx_read_client, obd_idx_read_server);
EXPORT_SYMBOL(RQF_OBD_IDX_READ);

struct req_format RQF_SEC_CTX =
        DEFINE_REQ_FMT0("SEC_CTX", empty, empty);
EXPORT_SYMBOL(RQF_SEC_CTX);

struct req_format RQF_MGS_TARGET_REG =
        DEFINE_REQ_FMT0("MGS_TARGET_REG", mgs_target_info_only,
                         mgs_target_info_only);
EXPORT_SYMBOL(RQF_MGS_TARGET_REG);

#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(2, 18, 53, 0)
struct req_format RQF_MGS_SET_INFO =
	DEFINE_REQ_FMT0("MGS_SET_INFO", mgs_set_info,
			mgs_set_info);
EXPORT_SYMBOL(RQF_MGS_SET_INFO);
#endif

struct req_format RQF_MGS_CONFIG_READ =
        DEFINE_REQ_FMT0("MGS_CONFIG_READ", mgs_config_read_client,
                         mgs_config_read_server);
EXPORT_SYMBOL(RQF_MGS_CONFIG_READ);

struct req_format RQF_SEQ_QUERY =
        DEFINE_REQ_FMT0("SEQ_QUERY", seq_query_client, seq_query_server);
EXPORT_SYMBOL(RQF_SEQ_QUERY);

struct req_format RQF_FLD_QUERY =
        DEFINE_REQ_FMT0("FLD_QUERY", fld_query_client, fld_query_server);
EXPORT_SYMBOL(RQF_FLD_QUERY);

/* The 'fld_read_server' uses 'RMF_GENERIC_DATA' to hold the 'FLD_QUERY'
 * RPC reply that is composed of 'struct lu_seq_range_array'. But there
 * is not registered swabber function for 'RMF_GENERIC_DATA'. So the RPC
 * peers need to handle the RPC reply with fixed little-endian format.
 *
 * In theory, we can define new structure with some swabber registered to
 * handle the 'FLD_QUERY' RPC reply result automatically. But from the
 * implementation view, it is not easy to be done within current "struct
 * req_msg_field" framework. Because the sequence range array in the RPC
 * reply is not fixed length, instead, its length depends on 'lu_seq_range'
 * count, that is unknown when prepare the RPC buffer. Generally, for such
 * flexible length RPC usage, there will be a field in the RPC layout to
 * indicate the data length. But for the 'FLD_READ' RPC, we have no way to
 * do that unless we add new length filed that will broken the on-wire RPC
 * protocol and cause interoperability trouble with old peer. */
struct req_format RQF_FLD_READ =
	DEFINE_REQ_FMT0("FLD_READ", fld_read_client, fld_read_server);
EXPORT_SYMBOL(RQF_FLD_READ);

struct req_format RQF_MDS_QUOTACTL =
        DEFINE_REQ_FMT0("MDS_QUOTACTL", quotactl_only, quotactl_only);
EXPORT_SYMBOL(RQF_MDS_QUOTACTL);

struct req_format RQF_OST_QUOTACTL =
        DEFINE_REQ_FMT0("OST_QUOTACTL", quotactl_only, quotactl_only);
EXPORT_SYMBOL(RQF_OST_QUOTACTL);

struct req_format RQF_QUOTA_DQACQ =
	DEFINE_REQ_FMT0("QUOTA_DQACQ", quota_body_only, quota_body_only);
EXPORT_SYMBOL(RQF_QUOTA_DQACQ);

struct req_format RQF_LDLM_INTENT_QUOTA =
	DEFINE_REQ_FMT0("LDLM_INTENT_QUOTA",
			ldlm_intent_quota_client,
			ldlm_intent_quota_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_QUOTA);

struct req_format RQF_MDS_GET_ROOT =
	DEFINE_REQ_FMT0("MDS_GET_ROOT", mds_get_root_client, mdt_body_capa);
EXPORT_SYMBOL(RQF_MDS_GET_ROOT);

struct req_format RQF_MDS_STATFS =
	DEFINE_REQ_FMT0("MDS_STATFS", empty, obd_statfs_server);
EXPORT_SYMBOL(RQF_MDS_STATFS);

struct req_format RQF_MDS_STATFS_NEW =
	DEFINE_REQ_FMT0("MDS_STATFS_NEW", mdt_body_only, obd_statfs_server);
EXPORT_SYMBOL(RQF_MDS_STATFS_NEW);

struct req_format RQF_MDS_SYNC =
        DEFINE_REQ_FMT0("MDS_SYNC", mdt_body_capa, mdt_body_only);
EXPORT_SYMBOL(RQF_MDS_SYNC);

struct req_format RQF_MDS_GETATTR =
        DEFINE_REQ_FMT0("MDS_GETATTR", mdt_body_capa, mds_getattr_server);
EXPORT_SYMBOL(RQF_MDS_GETATTR);

struct req_format RQF_MDS_GETXATTR =
        DEFINE_REQ_FMT0("MDS_GETXATTR",
                        mds_getxattr_client, mds_getxattr_server);
EXPORT_SYMBOL(RQF_MDS_GETXATTR);

struct req_format RQF_MDS_GETATTR_NAME =
        DEFINE_REQ_FMT0("MDS_GETATTR_NAME",
                        mds_getattr_name_client, mds_getattr_server);
EXPORT_SYMBOL(RQF_MDS_GETATTR_NAME);

struct req_format RQF_MDS_REINT =
        DEFINE_REQ_FMT0("MDS_REINT", mds_reint_client, mdt_body_only);
EXPORT_SYMBOL(RQF_MDS_REINT);

struct req_format RQF_MDS_REINT_CREATE =
        DEFINE_REQ_FMT0("MDS_REINT_CREATE",
                        mds_reint_create_client, mdt_body_capa);
EXPORT_SYMBOL(RQF_MDS_REINT_CREATE);

struct req_format RQF_MDS_REINT_CREATE_ACL =
	DEFINE_REQ_FMT0("MDS_REINT_CREATE_ACL",
			mds_reint_create_acl_client, mdt_body_capa);
EXPORT_SYMBOL(RQF_MDS_REINT_CREATE_ACL);

struct req_format RQF_MDS_REINT_CREATE_SLAVE =
        DEFINE_REQ_FMT0("MDS_REINT_CREATE_EA",
                        mds_reint_create_slave_client, mdt_body_capa);
EXPORT_SYMBOL(RQF_MDS_REINT_CREATE_SLAVE);

struct req_format RQF_MDS_REINT_CREATE_SYM =
        DEFINE_REQ_FMT0("MDS_REINT_CREATE_SYM",
                        mds_reint_create_sym_client, mdt_body_capa);
EXPORT_SYMBOL(RQF_MDS_REINT_CREATE_SYM);

struct req_format RQF_MDS_REINT_CREATE_REG =
	DEFINE_REQ_FMT0("MDS_REINT_CREATE_REG",
			mds_reint_create_acl_client, mds_create_server);
EXPORT_SYMBOL(RQF_MDS_REINT_CREATE_REG);

struct req_format RQF_MDS_REINT_OPEN =
        DEFINE_REQ_FMT0("MDS_REINT_OPEN",
                        mds_reint_open_client, mds_reint_open_server);
EXPORT_SYMBOL(RQF_MDS_REINT_OPEN);

struct req_format RQF_MDS_REINT_UNLINK =
        DEFINE_REQ_FMT0("MDS_REINT_UNLINK", mds_reint_unlink_client,
                        mds_last_unlink_server);
EXPORT_SYMBOL(RQF_MDS_REINT_UNLINK);

struct req_format RQF_MDS_REINT_LINK =
        DEFINE_REQ_FMT0("MDS_REINT_LINK",
                        mds_reint_link_client, mdt_body_only);
EXPORT_SYMBOL(RQF_MDS_REINT_LINK);

struct req_format RQF_MDS_REINT_RENAME =
        DEFINE_REQ_FMT0("MDS_REINT_RENAME", mds_reint_rename_client,
                        mds_last_unlink_server);
EXPORT_SYMBOL(RQF_MDS_REINT_RENAME);

struct req_format RQF_MDS_REINT_MIGRATE =
	DEFINE_REQ_FMT0("MDS_REINT_MIGRATE", mds_reint_migrate_client,
			mds_last_unlink_server);
EXPORT_SYMBOL(RQF_MDS_REINT_MIGRATE);

struct req_format RQF_MDS_REINT_SETATTR =
        DEFINE_REQ_FMT0("MDS_REINT_SETATTR",
                        mds_reint_setattr_client, mds_setattr_server);
EXPORT_SYMBOL(RQF_MDS_REINT_SETATTR);

struct req_format RQF_MDS_REINT_SETXATTR =
        DEFINE_REQ_FMT0("MDS_REINT_SETXATTR",
			mds_reint_setxattr_client, mdt_body_only);
EXPORT_SYMBOL(RQF_MDS_REINT_SETXATTR);

struct req_format RQF_MDS_REINT_RESYNC =
	DEFINE_REQ_FMT0("MDS_REINT_RESYNC", mds_reint_resync, mdt_body_only);
EXPORT_SYMBOL(RQF_MDS_REINT_RESYNC);

struct req_format RQF_MDS_CONNECT =
        DEFINE_REQ_FMT0("MDS_CONNECT",
                        obd_connect_client, obd_connect_server);
EXPORT_SYMBOL(RQF_MDS_CONNECT);

struct req_format RQF_MDS_DISCONNECT =
        DEFINE_REQ_FMT0("MDS_DISCONNECT", empty, empty);
EXPORT_SYMBOL(RQF_MDS_DISCONNECT);

struct req_format RQF_MDS_GET_INFO =
        DEFINE_REQ_FMT0("MDS_GET_INFO", mds_getinfo_client,
                        mds_getinfo_server);
EXPORT_SYMBOL(RQF_MDS_GET_INFO);

struct req_format RQF_MDS_BATCH =
	DEFINE_REQ_FMT0("MDS_BATCH", mds_batch_client,
			mds_batch_server);
EXPORT_SYMBOL(RQF_MDS_BATCH);

struct req_format RQF_LDLM_ENQUEUE =
        DEFINE_REQ_FMT0("LDLM_ENQUEUE",
                        ldlm_enqueue_client, ldlm_enqueue_lvb_server);
EXPORT_SYMBOL(RQF_LDLM_ENQUEUE);

struct req_format RQF_LDLM_ENQUEUE_LVB =
        DEFINE_REQ_FMT0("LDLM_ENQUEUE_LVB",
                        ldlm_enqueue_client, ldlm_enqueue_lvb_server);
EXPORT_SYMBOL(RQF_LDLM_ENQUEUE_LVB);

struct req_format RQF_LDLM_CONVERT =
        DEFINE_REQ_FMT0("LDLM_CONVERT",
                        ldlm_enqueue_client, ldlm_enqueue_server);
EXPORT_SYMBOL(RQF_LDLM_CONVERT);

struct req_format RQF_LDLM_CANCEL =
        DEFINE_REQ_FMT0("LDLM_CANCEL", ldlm_enqueue_client, empty);
EXPORT_SYMBOL(RQF_LDLM_CANCEL);

struct req_format RQF_LDLM_CALLBACK =
        DEFINE_REQ_FMT0("LDLM_CALLBACK", ldlm_enqueue_client, empty);
EXPORT_SYMBOL(RQF_LDLM_CALLBACK);

struct req_format RQF_LDLM_CP_CALLBACK =
        DEFINE_REQ_FMT0("LDLM_CP_CALLBACK", ldlm_cp_callback_client, empty);
EXPORT_SYMBOL(RQF_LDLM_CP_CALLBACK);

struct req_format RQF_LDLM_BL_CALLBACK =
        DEFINE_REQ_FMT0("LDLM_BL_CALLBACK", ldlm_enqueue_client, empty);
EXPORT_SYMBOL(RQF_LDLM_BL_CALLBACK);

struct req_format RQF_LDLM_GL_CALLBACK =
        DEFINE_REQ_FMT0("LDLM_GL_CALLBACK", ldlm_enqueue_client,
                        ldlm_gl_callback_server);
EXPORT_SYMBOL(RQF_LDLM_GL_CALLBACK);

struct req_format RQF_LDLM_GL_CALLBACK_DESC =
	DEFINE_REQ_FMT0("LDLM_GL_CALLBACK", ldlm_gl_callback_desc_client,
			ldlm_gl_callback_server);
EXPORT_SYMBOL(RQF_LDLM_GL_CALLBACK_DESC);

struct req_format RQF_LDLM_INTENT_BASIC =
	DEFINE_REQ_FMT0("LDLM_INTENT_BASIC",
			ldlm_intent_basic_client, ldlm_enqueue_lvb_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_BASIC);

struct req_format RQF_LDLM_INTENT =
        DEFINE_REQ_FMT0("LDLM_INTENT",
                        ldlm_intent_client, ldlm_intent_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT);

struct req_format RQF_LDLM_INTENT_LAYOUT =
	DEFINE_REQ_FMT0("LDLM_INTENT_LAYOUT",
			ldlm_intent_layout_client, ldlm_enqueue_lvb_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_LAYOUT);

struct req_format RQF_LDLM_INTENT_GETATTR =
        DEFINE_REQ_FMT0("LDLM_INTENT_GETATTR",
                        ldlm_intent_getattr_client, ldlm_intent_getattr_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_GETATTR);

struct req_format RQF_LDLM_INTENT_OPEN =
        DEFINE_REQ_FMT0("LDLM_INTENT_OPEN",
                        ldlm_intent_open_client, ldlm_intent_open_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_OPEN);

struct req_format RQF_LDLM_INTENT_CREATE =
        DEFINE_REQ_FMT0("LDLM_INTENT_CREATE",
                        ldlm_intent_create_client, ldlm_intent_getattr_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_CREATE);

struct req_format RQF_LDLM_INTENT_GETXATTR =
	DEFINE_REQ_FMT0("LDLM_INTENT_GETXATTR",
			ldlm_intent_getxattr_client,
			ldlm_intent_getxattr_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_GETXATTR);

struct req_format RQF_LDLM_INTENT_SETATTR =
	DEFINE_REQ_FMT0("LDLM_INTENT_SETATTR",
			ldlm_intent_setattr_client, ldlm_intent_setattr_server);
EXPORT_SYMBOL(RQF_LDLM_INTENT_SETATTR);

struct req_format RQF_MDS_CLOSE =
        DEFINE_REQ_FMT0("MDS_CLOSE",
                        mdt_close_client, mds_last_unlink_server);
EXPORT_SYMBOL(RQF_MDS_CLOSE);

struct req_format RQF_MDS_CLOSE_INTENT =
	DEFINE_REQ_FMT0("MDS_CLOSE_INTENT",
			mdt_close_intent_client, mds_last_unlink_server);
EXPORT_SYMBOL(RQF_MDS_CLOSE_INTENT);

struct req_format RQF_MDS_READPAGE =
        DEFINE_REQ_FMT0("MDS_READPAGE",
                        mdt_body_capa, mdt_body_only);
EXPORT_SYMBOL(RQF_MDS_READPAGE);

struct req_format RQF_MDS_HSM_ACTION =
	DEFINE_REQ_FMT0("MDS_HSM_ACTION", mdt_body_capa, mdt_hsm_action_server);
EXPORT_SYMBOL(RQF_MDS_HSM_ACTION);

struct req_format RQF_MDS_HSM_PROGRESS =
	DEFINE_REQ_FMT0("MDS_HSM_PROGRESS", mdt_hsm_progress, empty);
EXPORT_SYMBOL(RQF_MDS_HSM_PROGRESS);

struct req_format RQF_MDS_HSM_CT_REGISTER =
	DEFINE_REQ_FMT0("MDS_HSM_CT_REGISTER", mdt_hsm_ct_register, empty);
EXPORT_SYMBOL(RQF_MDS_HSM_CT_REGISTER);

struct req_format RQF_MDS_HSM_CT_UNREGISTER =
	DEFINE_REQ_FMT0("MDS_HSM_CT_UNREGISTER", mdt_hsm_ct_unregister, empty);
EXPORT_SYMBOL(RQF_MDS_HSM_CT_UNREGISTER);

struct req_format RQF_MDS_HSM_STATE_GET =
	DEFINE_REQ_FMT0("MDS_HSM_STATE_GET",
			mdt_body_capa, mdt_hsm_state_get_server);
EXPORT_SYMBOL(RQF_MDS_HSM_STATE_GET);

struct req_format RQF_MDS_HSM_STATE_SET =
	DEFINE_REQ_FMT0("MDS_HSM_STATE_SET", mdt_hsm_state_set, empty);
EXPORT_SYMBOL(RQF_MDS_HSM_STATE_SET);

struct req_format RQF_MDS_HSM_REQUEST =
	DEFINE_REQ_FMT0("MDS_HSM_REQUEST", mdt_hsm_request, empty);
EXPORT_SYMBOL(RQF_MDS_HSM_REQUEST);

struct req_format RQF_MDS_SWAP_LAYOUTS =
	DEFINE_REQ_FMT0("MDS_SWAP_LAYOUTS",
			mdt_swap_layouts, empty);
EXPORT_SYMBOL(RQF_MDS_SWAP_LAYOUTS);

struct req_format RQF_MDS_RMFID =
	DEFINE_REQ_FMT0("MDS_RMFID", mds_rmfid_client,
			mds_rmfid_server);
EXPORT_SYMBOL(RQF_MDS_RMFID);

struct req_format RQF_LLOG_ORIGIN_HANDLE_CREATE =
        DEFINE_REQ_FMT0("LLOG_ORIGIN_HANDLE_CREATE",
                        llog_origin_handle_create_client, llogd_body_only);
EXPORT_SYMBOL(RQF_LLOG_ORIGIN_HANDLE_CREATE);

struct req_format RQF_LLOG_ORIGIN_HANDLE_NEXT_BLOCK =
        DEFINE_REQ_FMT0("LLOG_ORIGIN_HANDLE_NEXT_BLOCK",
                        llogd_body_only, llog_origin_handle_next_block_server);
EXPORT_SYMBOL(RQF_LLOG_ORIGIN_HANDLE_NEXT_BLOCK);

struct req_format RQF_LLOG_ORIGIN_HANDLE_PREV_BLOCK =
        DEFINE_REQ_FMT0("LLOG_ORIGIN_HANDLE_PREV_BLOCK",
                        llogd_body_only, llog_origin_handle_next_block_server);
EXPORT_SYMBOL(RQF_LLOG_ORIGIN_HANDLE_PREV_BLOCK);

struct req_format RQF_LLOG_ORIGIN_HANDLE_READ_HEADER =
        DEFINE_REQ_FMT0("LLOG_ORIGIN_HANDLE_READ_HEADER",
                        llogd_body_only, llog_log_hdr_only);
EXPORT_SYMBOL(RQF_LLOG_ORIGIN_HANDLE_READ_HEADER);

struct req_format RQF_CONNECT =
	DEFINE_REQ_FMT0("CONNECT", obd_connect_client, obd_connect_server);
EXPORT_SYMBOL(RQF_CONNECT);

struct req_format RQF_OST_CONNECT =
        DEFINE_REQ_FMT0("OST_CONNECT",
                        obd_connect_client, obd_connect_server);
EXPORT_SYMBOL(RQF_OST_CONNECT);

struct req_format RQF_OST_DISCONNECT =
        DEFINE_REQ_FMT0("OST_DISCONNECT", empty, empty);
EXPORT_SYMBOL(RQF_OST_DISCONNECT);

struct req_format RQF_OST_GETATTR =
        DEFINE_REQ_FMT0("OST_GETATTR", ost_body_capa, ost_body_only);
EXPORT_SYMBOL(RQF_OST_GETATTR);

struct req_format RQF_OST_SETATTR =
        DEFINE_REQ_FMT0("OST_SETATTR", ost_body_capa, ost_body_only);
EXPORT_SYMBOL(RQF_OST_SETATTR);

struct req_format RQF_OST_CREATE =
        DEFINE_REQ_FMT0("OST_CREATE", ost_body_only, ost_body_only);
EXPORT_SYMBOL(RQF_OST_CREATE);

struct req_format RQF_OST_PUNCH =
        DEFINE_REQ_FMT0("OST_PUNCH", ost_body_capa, ost_body_only);
EXPORT_SYMBOL(RQF_OST_PUNCH);

struct req_format RQF_OST_FALLOCATE =
	DEFINE_REQ_FMT0("OST_FALLOCATE", ost_body_capa, ost_body_only);
EXPORT_SYMBOL(RQF_OST_FALLOCATE);

struct req_format RQF_OST_SEEK =
	DEFINE_REQ_FMT0("OST_SEEK", ost_body_only, ost_body_only);
EXPORT_SYMBOL(RQF_OST_SEEK);

struct req_format RQF_OST_SYNC =
        DEFINE_REQ_FMT0("OST_SYNC", ost_body_capa, ost_body_only);
EXPORT_SYMBOL(RQF_OST_SYNC);

struct req_format RQF_OST_DESTROY =
        DEFINE_REQ_FMT0("OST_DESTROY", ost_destroy_client, ost_body_only);
EXPORT_SYMBOL(RQF_OST_DESTROY);

struct req_format RQF_OST_BRW_READ =
        DEFINE_REQ_FMT0("OST_BRW_READ", ost_brw_client, ost_brw_read_server);
EXPORT_SYMBOL(RQF_OST_BRW_READ);

struct req_format RQF_OST_BRW_WRITE =
        DEFINE_REQ_FMT0("OST_BRW_WRITE", ost_brw_client, ost_brw_write_server);
EXPORT_SYMBOL(RQF_OST_BRW_WRITE);

struct req_format RQF_OST_STATFS =
        DEFINE_REQ_FMT0("OST_STATFS", empty, obd_statfs_server);
EXPORT_SYMBOL(RQF_OST_STATFS);

struct req_format RQF_OST_SET_GRANT_INFO =
        DEFINE_REQ_FMT0("OST_SET_GRANT_INFO", ost_grant_shrink_client,
                         ost_body_only);
EXPORT_SYMBOL(RQF_OST_SET_GRANT_INFO);

struct req_format RQF_OST_GET_INFO =
        DEFINE_REQ_FMT0("OST_GET_INFO", ost_get_info_generic_client,
                                        ost_get_info_generic_server);
EXPORT_SYMBOL(RQF_OST_GET_INFO);

struct req_format RQF_OST_GET_INFO_LAST_ID =
        DEFINE_REQ_FMT0("OST_GET_INFO_LAST_ID", ost_get_info_generic_client,
                                                ost_get_last_id_server);
EXPORT_SYMBOL(RQF_OST_GET_INFO_LAST_ID);

struct req_format RQF_OST_GET_INFO_LAST_FID =
	DEFINE_REQ_FMT0("OST_GET_INFO_LAST_FID", ost_get_last_fid_client,
						 ost_get_last_fid_server);
EXPORT_SYMBOL(RQF_OST_GET_INFO_LAST_FID);

struct req_format RQF_OST_SET_INFO_LAST_FID =
	DEFINE_REQ_FMT0("OST_SET_INFO_LAST_FID", obd_set_info_client,
						 empty);
EXPORT_SYMBOL(RQF_OST_SET_INFO_LAST_FID);

struct req_format RQF_OST_GET_INFO_FIEMAP =
        DEFINE_REQ_FMT0("OST_GET_INFO_FIEMAP", ost_get_fiemap_client,
                                               ost_get_fiemap_server);
EXPORT_SYMBOL(RQF_OST_GET_INFO_FIEMAP);

struct req_format RQF_LFSCK_NOTIFY =
	DEFINE_REQ_FMT0("LFSCK_NOTIFY", obd_lfsck_request, empty);
EXPORT_SYMBOL(RQF_LFSCK_NOTIFY);

struct req_format RQF_LFSCK_QUERY =
	DEFINE_REQ_FMT0("LFSCK_QUERY", obd_lfsck_request, obd_lfsck_reply);
EXPORT_SYMBOL(RQF_LFSCK_QUERY);

struct req_format RQF_OST_LADVISE =
	DEFINE_REQ_FMT0("OST_LADVISE", ost_ladvise, ost_body_only);
EXPORT_SYMBOL(RQF_OST_LADVISE);

struct req_format RQF_BUT_GETATTR =
	DEFINE_REQ_FMT0("MDS_BATCH_GETATTR", mds_batch_getattr_client,
			mds_batch_getattr_server);
EXPORT_SYMBOL(RQF_BUT_GETATTR);

struct req_format RQF_BUT_CREATE_EXLOCK =
	DEFINE_REQ_FMT0("CREATE_EXLOCK", create_exlock_client,
					 create_exlock_server);
EXPORT_SYMBOL(RQF_BUT_CREATE_EXLOCK);

struct req_format RQF_BUT_CREATE_LOCKLESS =
	DEFINE_REQ_FMT0("CREATE_LOCKLESS", create_lockless_client,
					   create_lockless_server);
EXPORT_SYMBOL(RQF_BUT_CREATE_LOCKLESS);

struct req_format RQF_BUT_SETATTR_EXLOCK =
	DEFINE_REQ_FMT0("SETATTR_EXLOCK", setattr_exlock_client,
					  setattr_exlock_server);
EXPORT_SYMBOL(RQF_BUT_SETATTR_EXLOCK);

struct req_format RQF_BUT_SETATTR_LOCKLESS =
	DEFINE_REQ_FMT0("SETATTR_LOCKLESS", setattr_lockless_client, empty);
EXPORT_SYMBOL(RQF_BUT_SETATTR_LOCKLESS);

struct req_format RQF_BUT_EXLOCK_ONLY =
	DEFINE_REQ_FMT0("EXLOCK_ONLY", exlock_only_client, exlock_only_server);
EXPORT_SYMBOL(RQF_BUT_EXLOCK_ONLY);

/* Convenience macro */
#define FMT_FIELD(fmt, i, j) (fmt)->rf_fields[(i)].d[(j)]

/**
 * Initializes the capsule abstraction by computing and setting the \a rf_idx
 * field of RQFs and the \a rmf_offset field of RMFs.
 */
int req_layout_init(void)
{
	size_t i;
	size_t j;
	size_t k;
        struct req_format *rf = NULL;

        for (i = 0; i < ARRAY_SIZE(req_formats); ++i) {
                rf = req_formats[i];
                rf->rf_idx = i;
                for (j = 0; j < RCL_NR; ++j) {
                        LASSERT(rf->rf_fields[j].nr <= REQ_MAX_FIELD_NR);
                        for (k = 0; k < rf->rf_fields[j].nr; ++k) {
                                struct req_msg_field *field;

                                field = (typeof(field))rf->rf_fields[j].d[k];
                                LASSERT(!(field->rmf_flags & RMF_F_STRUCT_ARRAY)
                                        || field->rmf_size > 0);
                                LASSERT(field->rmf_offset[i][j] == 0);
                                /*
                                 * k + 1 to detect unused format/field
                                 * combinations.
                                 */
                                field->rmf_offset[i][j] = k + 1;
                        }
                }
        }
        return 0;
}
EXPORT_SYMBOL(req_layout_init);

void req_layout_fini(void)
{
}
EXPORT_SYMBOL(req_layout_fini);

/**
 * Initializes the expected sizes of each RMF in a \a pill (\a rc_area) to -1.
 *
 * Actual/expected field sizes are set elsewhere in functions in this file:
 * req_capsule_init(), req_capsule_server_pack(), req_capsule_set_size() and
 * req_capsule_msg_size().  The \a rc_area information is used by.
 * ptlrpc_request_set_replen().
 */
void req_capsule_init_area(struct req_capsule *pill)
{
	size_t i;

        for (i = 0; i < ARRAY_SIZE(pill->rc_area[RCL_CLIENT]); i++) {
                pill->rc_area[RCL_CLIENT][i] = -1;
                pill->rc_area[RCL_SERVER][i] = -1;
        }
}
EXPORT_SYMBOL(req_capsule_init_area);

/**
 * Initialize a pill.
 *
 * The \a location indicates whether the caller is executing on the client side
 * (RCL_CLIENT) or server side (RCL_SERVER)..
 */
void req_capsule_init(struct req_capsule *pill,
                      struct ptlrpc_request *req,
                      enum req_location location)
{
        LASSERT(location == RCL_SERVER || location == RCL_CLIENT);

        /*
         * Today all capsules are embedded in ptlrpc_request structs,
         * but just in case that ever isn't the case, we don't reach
         * into req unless req != NULL and pill is the one embedded in
         * the req.
         *
         * The req->rq_pill_init flag makes it safe to initialize a pill
         * twice, which might happen in the OST paths as a result of the
         * high-priority RPC queue getting peeked at before ost_handle()
         * handles an OST RPC.
         */
        if (req != NULL && pill == &req->rq_pill && req->rq_pill_init)
                return;

	pill->rc_fmt = NULL;
        pill->rc_req = req;
        pill->rc_loc = location;
	req_capsule_init_area(pill);

	if (req != NULL && pill == &req->rq_pill)
		req->rq_pill_init = 1;
}
EXPORT_SYMBOL(req_capsule_init);

void req_capsule_fini(struct req_capsule *pill)
{
}
EXPORT_SYMBOL(req_capsule_fini);

static int __req_format_is_sane(const struct req_format *fmt)
{
	return fmt->rf_idx < ARRAY_SIZE(req_formats) &&
		req_formats[fmt->rf_idx] == fmt;
}

static struct lustre_msg *__req_msg(const struct req_capsule *pill,
                                    enum req_location loc)
{
	return loc == RCL_CLIENT ? pill->rc_reqmsg : pill->rc_repmsg;
}

/**
 * Set the format (\a fmt) of a \a pill; format changes are not allowed here
 * (see req_capsule_extend()).
 */
void req_capsule_set(struct req_capsule *pill, const struct req_format *fmt)
{
        LASSERT(pill->rc_fmt == NULL || pill->rc_fmt == fmt);
        LASSERT(__req_format_is_sane(fmt));

        pill->rc_fmt = fmt;
}
EXPORT_SYMBOL(req_capsule_set);

/**
 * Fills in any parts of the \a rc_area of a \a pill that haven't been filled in
 * yet.

 * \a rc_area is an array of REQ_MAX_FIELD_NR elements, used to store sizes of
 * variable-sized fields.  The field sizes come from the declared \a rmf_size
 * field of a \a pill's \a rc_fmt's RMF's.
 */
size_t req_capsule_filled_sizes(struct req_capsule *pill,
				enum req_location loc)
{
	const struct req_format *fmt = pill->rc_fmt;
	size_t			 i;

        LASSERT(fmt != NULL);

        for (i = 0; i < fmt->rf_fields[loc].nr; ++i) {
                if (pill->rc_area[loc][i] == -1) {
                        pill->rc_area[loc][i] =
                                            fmt->rf_fields[loc].d[i]->rmf_size;
                        if (pill->rc_area[loc][i] == -1) {
                                /*
                                 * Skip the following fields.
                                 *
                                 * If this LASSERT() trips then you're missing a
                                 * call to req_capsule_set_size().
                                 */
                                LASSERT(loc != RCL_SERVER);
                                break;
                        }
                }
        }
        return i;
}
EXPORT_SYMBOL(req_capsule_filled_sizes);

/**
 * Capsule equivalent of lustre_pack_request() and lustre_pack_reply().
 *
 * This function uses the \a pill's \a rc_area as filled in by
 * req_capsule_set_size() or req_capsule_filled_sizes() (the latter is called by
 * this function).
 */
int req_capsule_server_pack(struct req_capsule *pill)
{
	const struct req_format *fmt;
	int count;
	int rc;

	LASSERT(pill->rc_loc == RCL_SERVER);
	fmt = pill->rc_fmt;
	LASSERT(fmt != NULL);

	count = req_capsule_filled_sizes(pill, RCL_SERVER);
	if (req_capsule_ptlreq(pill)) {
		rc = lustre_pack_reply(pill->rc_req, count,
				       pill->rc_area[RCL_SERVER], NULL);
		if (rc != 0) {
			DEBUG_REQ(D_ERROR, pill->rc_req,
				  "Cannot pack %d fields in format '%s'",
				   count, fmt->rf_name);
		}
	} else { /* SUB request */
		struct ptlrpc_request *req = pill->rc_req;
		__u32 used_len;
		__u32 msg_len;

		msg_len = lustre_msg_size_v2(count, pill->rc_area[RCL_SERVER]);
		used_len = (char *)pill->rc_repmsg - (char *)req->rq_repmsg;
		/* Overflow the reply buffer */
		if (used_len + msg_len > req->rq_replen) {
			__u32 len;
			__u32 max;

			if (!req_capsule_has_field(&req->rq_pill,
						   &RMF_BUT_REPLY, RCL_SERVER))
				return -EINVAL;

			if (!req_capsule_field_present(&req->rq_pill,
						       &RMF_BUT_REPLY,
						       RCL_SERVER))
				return -EINVAL;

			if (used_len + msg_len > BUT_MAXREPSIZE)
				return -EOVERFLOW;

			len = req_capsule_get_size(&req->rq_pill,
						   &RMF_BUT_REPLY, RCL_SERVER);
			/*
			 * Currently just increase the batch reply buffer
			 * by 2.
			 */
			LASSERT(req->rq_replen <= BUT_MAXREPSIZE);
			max = BUT_MAXREPSIZE - req->rq_replen;
			if (len > max)
				len += max;
			else
				len += len;
			rc = req_capsule_server_grow(&req->rq_pill,
						     &RMF_BUT_REPLY, len);
			if (rc)
				return rc;

			pill->rc_repmsg =
				(struct lustre_msg *)((char *)req->rq_repmsg +
						      used_len);
		}
		if (msg_len > pill->rc_reqmsg->lm_repsize)
			/* TODO: Check whether there is enough buffer size */
			CDEBUG(D_INFO,
			       "Overflow pack %d fields in format '%s' for "
			       "the SUB request with message len %u:%u\n",
			       count, fmt->rf_name, msg_len,
			       pill->rc_reqmsg->lm_repsize);

		rc = 0;
		lustre_init_msg_v2(pill->rc_repmsg, count,
				   pill->rc_area[RCL_SERVER], NULL);
	}

	return rc;
}
EXPORT_SYMBOL(req_capsule_server_pack);

int req_capsule_client_pack(struct req_capsule *pill)
{
	const struct req_format *fmt;
	int count;
	int rc = 0;

	LASSERT(pill->rc_loc == RCL_CLIENT);
	fmt = pill->rc_fmt;
	LASSERT(fmt != NULL);

	count = req_capsule_filled_sizes(pill, RCL_CLIENT);
	if (req_capsule_ptlreq(pill)) {
		struct ptlrpc_request *req = pill->rc_req;

		rc = lustre_pack_request(req, req->rq_import->imp_msg_magic,
					 count, pill->rc_area[RCL_CLIENT],
					 NULL);
	} else {
		/* Sub request in a batch PTLRPC request */
		lustre_init_msg_v2(pill->rc_reqmsg, count,
				   pill->rc_area[RCL_CLIENT], NULL);
	}
	return rc;
}
EXPORT_SYMBOL(req_capsule_client_pack);

/**
 * Returns the PTLRPC request or reply (\a loc) buffer offset of a \a pill
 * corresponding to the given RMF (\a field).
 */
__u32 __req_capsule_offset(const struct req_capsule *pill,
			   const struct req_msg_field *field,
			   enum req_location loc)
{
	unsigned int offset;

	offset = field->rmf_offset[pill->rc_fmt->rf_idx][loc];
	LASSERTF(offset > 0, "%s:%s, off=%d, loc=%d\n",
			     pill->rc_fmt->rf_name,
			     field->rmf_name, offset, loc);
	offset--;

	LASSERT(offset < REQ_MAX_FIELD_NR);
        return offset;
}

void req_capsule_set_swabbed(struct req_capsule *pill, enum req_location loc,
			    __u32 index)
{
	if (loc == RCL_CLIENT)
		req_capsule_set_req_swabbed(pill, index);
	else
		req_capsule_set_rep_swabbed(pill, index);
}

bool req_capsule_need_swab(struct req_capsule *pill, enum req_location loc,
			   __u32 index)
{
	if (loc == RCL_CLIENT)
		return (req_capsule_req_need_swab(pill) &&
			!req_capsule_req_swabbed(pill, index));

	return (req_capsule_rep_need_swab(pill) &&
	       !req_capsule_rep_swabbed(pill, index));
}

/**
 * Helper for __req_capsule_get(); swabs value / array of values and/or dumps
 * them if desired.
 */
static int
swabber_dumper_helper(struct req_capsule *pill,
		      const struct req_msg_field *field,
		      enum req_location loc,
		      int offset,
		      void *value, int len, bool dump, void (*swabber)(void *))
{
	void *p;
	int i;
	int n;
	int size;
	int rc = 0;
	bool do_swab;
	bool array = field->rmf_flags & RMF_F_STRUCT_ARRAY;

	swabber = swabber ?: field->rmf_swabber;

	if (req_capsule_need_swab(pill, loc, offset) &&
	    (swabber != NULL || field->rmf_swab_len != NULL) && value != NULL)
		do_swab = true;
	else
		do_swab = false;

	if (!field->rmf_dumper)
		dump = false;

	/*
	 * We're swabbing an array; swabber() swabs a single array element, so
	 * swab every element.
	 */
	if (array && (len % field->rmf_size)) {
		static const struct req_msg_field *last_field;

		if (field != last_field) {
			CERROR("%s: array buffer size %u is not a multiple of element size %u\n",
			       field->rmf_name, len, field->rmf_size);
			last_field = field;
		}
	}
	/* For the non-array cases, the process of swab/dump/swab only
	 * needs to be done once. (n = 1)
	 */
	if (!array)
		len = field->rmf_size;
	for (p = value, i = 0, n = len / field->rmf_size;
	     i < n;
	     i++, p += field->rmf_size) {
		if (dump) {
			CDEBUG(D_RPCTRACE, "Dump of %s%sfield %s element %d follows\n",
			       do_swab ? "unswabbed " : "",
			       array ? "array " : "",
			       field->rmf_name, i);
			field->rmf_dumper(p);
		}
		if (!do_swab) {
			if (array)
				continue;
			else
				break;
		}
		if (!field->rmf_swab_len) {
			swabber(p);
		} else {
			size = field->rmf_swab_len(p, len);
			if (size > 0) {
				len -= size;
			} else {
				rc = size;
				break;
			}
		}
		if (dump) {
			CDEBUG(D_RPCTRACE, "Dump of swabbed %sfield %s, element %d follows\n",
			       array ? "array " : "", field->rmf_name, i);
			field->rmf_dumper(value);
		}
        }
        if (do_swab)
		req_capsule_set_swabbed(pill, loc, offset);

	return rc;
}

/**
 * Returns the pointer to a PTLRPC request or reply (\a loc) buffer of a \a pill
 * corresponding to the given RMF (\a field).
 *
 * The buffer will be swabbed using the given \a swabber.  If \a swabber == NULL
 * then the \a rmf_swabber from the RMF will be used.  Soon there will be no
 * calls to __req_capsule_get() with a non-NULL \a swabber; \a swabber will then
 * be removed.  Fields with the \a RMF_F_STRUCT_ARRAY flag set will have each
 * element of the array swabbed.
 */
static void *__req_capsule_get(struct req_capsule *pill,
			       const struct req_msg_field *field,
			       enum req_location loc,
			       void (*swabber)(void *),
			       bool dump)
{
	const struct req_format *fmt;
	struct lustre_msg       *msg;
	void                    *value;
	__u32                    len;
	__u32                    offset;

	void *(*getter)(struct lustre_msg *m, __u32 n, __u32 minlen);

        static const char *rcl_names[RCL_NR] = {
                [RCL_CLIENT] = "client",
                [RCL_SERVER] = "server"
        };

        LASSERT(pill != NULL);
        LASSERT(pill != LP_POISON);
        fmt = pill->rc_fmt;
        LASSERT(fmt != NULL);
        LASSERT(fmt != LP_POISON);
        LASSERT(__req_format_is_sane(fmt));

        offset = __req_capsule_offset(pill, field, loc);

        msg = __req_msg(pill, loc);
        LASSERT(msg != NULL);

        getter = (field->rmf_flags & RMF_F_STRING) ?
                (typeof(getter))lustre_msg_string : lustre_msg_buf;

	if (field->rmf_flags & (RMF_F_STRUCT_ARRAY|RMF_F_NO_SIZE_CHECK)) {
		/*
		 * We've already asserted that field->rmf_size > 0 in
		 * req_layout_init().
		 */
		len = lustre_msg_buflen(msg, offset);
		if (!(field->rmf_flags & RMF_F_NO_SIZE_CHECK) &&
		    (len % field->rmf_size) != 0) {
			CERROR("%s: array field size mismatch "
				"%d modulo %u != 0 (%d)\n",
				field->rmf_name, len, field->rmf_size, loc);
			return NULL;
		}
        } else if (pill->rc_area[loc][offset] != -1) {
                len = pill->rc_area[loc][offset];
        } else {
		len = max_t(typeof(field->rmf_size), field->rmf_size, 0);
        }
        value = getter(msg, offset, len);

        if (value == NULL) {
		LASSERT(pill->rc_req != NULL);
                DEBUG_REQ(D_ERROR, pill->rc_req,
			   "Wrong buffer for field '%s' (%u of %u) in format '%s', %u vs. %u (%s)",
			   field->rmf_name, offset, lustre_msg_bufcount(msg),
			   fmt->rf_name, lustre_msg_buflen(msg, offset), len,
			   rcl_names[loc]);
        } else {
                swabber_dumper_helper(pill, field, loc, offset, value, len,
                                      dump, swabber);
        }

        return value;
}

/**
 * Dump a request and/or reply
 */
void __req_capsule_dump(struct req_capsule *pill, enum req_location loc)
{
	const struct req_format *fmt;
	const struct req_msg_field *field;
	__u32 len;
	size_t i;

	fmt = pill->rc_fmt;

	DEBUG_REQ(D_RPCTRACE, pill->rc_req, "BEGIN REQ CAPSULE DUMP");
	for (i = 0; i < fmt->rf_fields[loc].nr; ++i) {
		field = FMT_FIELD(fmt, loc, i);
		if (field->rmf_dumper == NULL) {
			/*
			 * FIXME Add a default hex dumper for fields that don't
			 * have a specific dumper
			 */
			len = req_capsule_get_size(pill, field, loc);
			CDEBUG(D_RPCTRACE,
			       "Field %s has no dumper function; field size is %u\n",
			       field->rmf_name, len);
		} else {
			/* It's dumping side-effect that we're interested in */
			(void) __req_capsule_get(pill, field, loc, NULL, true);
		}
	}
	CDEBUG(D_RPCTRACE, "END REQ CAPSULE DUMP\n");
}

/**
 * Dump a request.
 */
void req_capsule_client_dump(struct req_capsule *pill)
{
        __req_capsule_dump(pill, RCL_CLIENT);
}
EXPORT_SYMBOL(req_capsule_client_dump);

/**
 * Dump a reply
 */
void req_capsule_server_dump(struct req_capsule *pill)
{
        __req_capsule_dump(pill, RCL_SERVER);
}
EXPORT_SYMBOL(req_capsule_server_dump);

/**
 * Trivial wrapper around __req_capsule_get(), that returns the PTLRPC request
 * buffer corresponding to the given RMF (\a field) of a \a pill.
 */
void *req_capsule_client_get(struct req_capsule *pill,
			     const struct req_msg_field *field)
{
	return __req_capsule_get(pill, field, RCL_CLIENT, NULL, false);
}
EXPORT_SYMBOL(req_capsule_client_get);

/**
 * Same as req_capsule_client_get(), but with a \a swabber argument.
 *
 * Currently unused; will be removed when req_capsule_server_swab_get() is
 * unused too.
 */
void *req_capsule_client_swab_get(struct req_capsule *pill,
				  const struct req_msg_field *field,
				  void *swabber)
{
	return __req_capsule_get(pill, field, RCL_CLIENT, swabber, false);
}
EXPORT_SYMBOL(req_capsule_client_swab_get);

/**
 * Utility that combines req_capsule_set_size() and req_capsule_client_get().
 *
 * First the \a pill's request \a field's size is set (\a rc_area) using
 * req_capsule_set_size() with the given \a len.  Then the actual buffer is
 * returned.
 */
void *req_capsule_client_sized_get(struct req_capsule *pill,
				   const struct req_msg_field *field,
				   __u32 len)
{
	req_capsule_set_size(pill, field, RCL_CLIENT, len);
	return __req_capsule_get(pill, field, RCL_CLIENT, NULL, false);
}
EXPORT_SYMBOL(req_capsule_client_sized_get);

/**
 * Trivial wrapper around __req_capsule_get(), that returns the PTLRPC reply
 * buffer corresponding to the given RMF (\a field) of a \a pill.
 */
void *req_capsule_server_get(struct req_capsule *pill,
                             const struct req_msg_field *field)
{
	return __req_capsule_get(pill, field, RCL_SERVER, NULL, false);
}
EXPORT_SYMBOL(req_capsule_server_get);

/**
 * Same as req_capsule_server_get(), but with a \a swabber argument.
 *
 * Ideally all swabbing should be done pursuant to RMF definitions, with no
 * swabbing done outside this capsule abstraction.
 */
void *req_capsule_server_swab_get(struct req_capsule *pill,
				  const struct req_msg_field *field,
				  void *swabber)
{
	return __req_capsule_get(pill, field, RCL_SERVER, swabber, false);
}
EXPORT_SYMBOL(req_capsule_server_swab_get);

/**
 * Utility that combines req_capsule_set_size() and req_capsule_server_get().
 *
 * First the \a pill's request \a field's size is set (\a rc_area) using
 * req_capsule_set_size() with the given \a len.  Then the actual buffer is
 * returned.
 */
void *req_capsule_server_sized_get(struct req_capsule *pill,
				   const struct req_msg_field *field,
				   __u32 len)
{
	req_capsule_set_size(pill, field, RCL_SERVER, len);
	return __req_capsule_get(pill, field, RCL_SERVER, NULL, false);
}
EXPORT_SYMBOL(req_capsule_server_sized_get);

void *req_capsule_server_sized_swab_get(struct req_capsule *pill,
					const struct req_msg_field *field,
					__u32 len, void *swabber)
{
	req_capsule_set_size(pill, field, RCL_SERVER, len);
	return __req_capsule_get(pill, field, RCL_SERVER, swabber, false);
}
EXPORT_SYMBOL(req_capsule_server_sized_swab_get);

/**
 * Returns the buffer of a \a pill corresponding to the given \a field from the
 * request (if the caller is executing on the server-side) or reply (if the
 * caller is executing on the client-side).
 *
 * This function convienient for use is code that could be executed on the
 * client and server alike.
 */
const void *req_capsule_other_get(struct req_capsule *pill,
				  const struct req_msg_field *field)
{
	return __req_capsule_get(pill, field, pill->rc_loc ^ 1, NULL, false);
}
EXPORT_SYMBOL(req_capsule_other_get);

/**
 * Set the size of the PTLRPC request/reply (\a loc) buffer for the given \a
 * field of the given \a pill.
 *
 * This function must be used when constructing variable sized fields of a
 * request or reply.
 */
void req_capsule_set_size(struct req_capsule *pill,
			  const struct req_msg_field *field,
			  enum req_location loc, __u32 size)
{
	LASSERT(loc == RCL_SERVER || loc == RCL_CLIENT);

	if ((size != (__u32)field->rmf_size) &&
	    (field->rmf_size != -1) &&
	    !(field->rmf_flags & RMF_F_NO_SIZE_CHECK) &&
	    (size > 0)) {
		__u32 rmf_size = (__u32)field->rmf_size;
		if ((field->rmf_flags & RMF_F_STRUCT_ARRAY) &&
		    (size % rmf_size != 0)) {
			CERROR("%s: array field size mismatch "
				"%u %% %u != 0 (%d)\n",
				field->rmf_name, size, rmf_size, loc);
			LBUG();
		} else if (!(field->rmf_flags & RMF_F_STRUCT_ARRAY) &&
			   size < rmf_size) {
			CERROR("%s: field size mismatch %u != %u (%d)\n",
				field->rmf_name, size, rmf_size, loc);
			LBUG();
		}
	}

	pill->rc_area[loc][__req_capsule_offset(pill, field, loc)] = size;
}
EXPORT_SYMBOL(req_capsule_set_size);

/**
 * Return the actual PTLRPC buffer length of a request or reply (\a loc)
 * for the given \a pill's given \a field.
 *
 * NB: this function doesn't correspond with req_capsule_set_size(), which
 * actually sets the size in pill.rc_area[loc][offset], but this function
 * returns the message buflen[offset], maybe we should use another name.
 */
__u32 req_capsule_get_size(const struct req_capsule *pill,
                         const struct req_msg_field *field,
                         enum req_location loc)
{
        LASSERT(loc == RCL_SERVER || loc == RCL_CLIENT);

        return lustre_msg_buflen(__req_msg(pill, loc),
                                 __req_capsule_offset(pill, field, loc));
}
EXPORT_SYMBOL(req_capsule_get_size);

/**
 * Wrapper around lustre_msg_size() that returns the PTLRPC size needed for the
 * given \a pill's request or reply (\a loc) given the field size recorded in
 * the \a pill's rc_area.
 *
 * See also req_capsule_set_size().
 */
__u32 req_capsule_msg_size(struct req_capsule *pill, enum req_location loc)
{
	if (req_capsule_ptlreq(pill)) {
		return lustre_msg_size(pill->rc_req->rq_import->imp_msg_magic,
				       pill->rc_fmt->rf_fields[loc].nr,
				       pill->rc_area[loc]);
	} else { /* SUB request in a batch request */
		int count;

		count = req_capsule_filled_sizes(pill, loc);
		return lustre_msg_size_v2(count, pill->rc_area[loc]);
	}
}
EXPORT_SYMBOL(req_capsule_msg_size);

/**
 * While req_capsule_msg_size() computes the size of a PTLRPC request or reply
 * (\a loc) given a \a pill's \a rc_area, this function computes the size of a
 * PTLRPC request or reply given only an RQF (\a fmt).
 *
 * This function should not be used for formats which contain variable size
 * fields.
 */
__u32 req_capsule_fmt_size(__u32 magic, const struct req_format *fmt,
                         enum req_location loc)
{
	__u32 size;
	size_t i = 0;

        /*
         * This function should probably LASSERT() that fmt has no fields with
         * RMF_F_STRUCT_ARRAY in rmf_flags, since we can't know here how many
         * elements in the array there will ultimately be, but then, we could
         * assume that there will be at least one element, and that's just what
         * we do.
         */
        size = lustre_msg_hdr_size(magic, fmt->rf_fields[loc].nr);
	if (size == 0)
		return size;

	for (; i < fmt->rf_fields[loc].nr; ++i)
		if (fmt->rf_fields[loc].d[i]->rmf_size != -1)
			size += cfs_size_round(fmt->rf_fields[loc].d[i]->
					       rmf_size);
	return size;
}
EXPORT_SYMBOL(req_capsule_fmt_size);

/**
 * Changes the format of an RPC.
 *
 * The pill must already have been initialized, which means that it already has
 * a request format.  The new format \a fmt must be an extension of the pill's
 * old format.  Specifically: the new format must have as many request and reply
 * fields as the old one, and all fields shared by the old and new format must
 * be at least as large in the new format.
 *
 * The new format's fields may be of different "type" than the old format, but
 * only for fields that are "opaque" blobs: fields which have a) have no
 * \a rmf_swabber, b) \a rmf_flags == 0 or RMF_F_NO_SIZE_CHECK, and c) \a
 * rmf_size == -1 or \a rmf_flags == RMF_F_NO_SIZE_CHECK.  For example,
 * OBD_SET_INFO has a key field and an opaque value field that gets interpreted
 * according to the key field.  When the value, according to the key, contains a
 * structure (or array thereof) to be swabbed, the format should be changed to
 * one where the value field has \a rmf_size/rmf_flags/rmf_swabber set
 * accordingly.
 */
void req_capsule_extend(struct req_capsule *pill, const struct req_format *fmt)
{
	int i;
	size_t j;

        const struct req_format *old;

        LASSERT(pill->rc_fmt != NULL);
        LASSERT(__req_format_is_sane(fmt));

        old = pill->rc_fmt;
        /*
         * Sanity checking...
         */
        for (i = 0; i < RCL_NR; ++i) {
                LASSERT(fmt->rf_fields[i].nr >= old->rf_fields[i].nr);
                for (j = 0; j < old->rf_fields[i].nr - 1; ++j) {
                        const struct req_msg_field *ofield = FMT_FIELD(old, i, j);

                        /* "opaque" fields can be transmogrified */
                        if (ofield->rmf_swabber == NULL &&
                            (ofield->rmf_flags & ~RMF_F_NO_SIZE_CHECK) == 0 &&
                            (ofield->rmf_size == -1 ||
                            ofield->rmf_flags == RMF_F_NO_SIZE_CHECK))
                                continue;
                        LASSERT(FMT_FIELD(fmt, i, j) == FMT_FIELD(old, i, j));
                }
                /*
                 * Last field in old format can be shorter than in new.
                 */
                LASSERT(FMT_FIELD(fmt, i, j)->rmf_size >=
                        FMT_FIELD(old, i, j)->rmf_size);
        }

        pill->rc_fmt = fmt;
}
EXPORT_SYMBOL(req_capsule_extend);

/**
 * This function returns a non-zero value if the given \a field is present in
 * the format (\a rc_fmt) of \a pill's PTLRPC request or reply (\a loc), else it
 * returns 0.
 */
int req_capsule_has_field(const struct req_capsule *pill,
                          const struct req_msg_field *field,
                          enum req_location loc)
{
        LASSERT(loc == RCL_SERVER || loc == RCL_CLIENT);

        return field->rmf_offset[pill->rc_fmt->rf_idx][loc];
}
EXPORT_SYMBOL(req_capsule_has_field);

/**
 * Returns a non-zero value if the given \a field is present in the given \a
 * pill's PTLRPC request or reply (\a loc), else it returns 0.
 */
int req_capsule_field_present(const struct req_capsule *pill,
                              const struct req_msg_field *field,
                              enum req_location loc)
{
	__u32 offset;

        LASSERT(loc == RCL_SERVER || loc == RCL_CLIENT);
        LASSERT(req_capsule_has_field(pill, field, loc));

        offset = __req_capsule_offset(pill, field, loc);
        return lustre_msg_bufcount(__req_msg(pill, loc)) > offset;
}
EXPORT_SYMBOL(req_capsule_field_present);

/**
 * This function shrinks the size of the _buffer_ of the \a pill's PTLRPC
 * request or reply (\a loc).
 *
 * This is not the opposite of req_capsule_extend().
 */
void req_capsule_shrink(struct req_capsule *pill,
			const struct req_msg_field *field,
			__u32 newlen,
			enum req_location loc)
{
        const struct req_format *fmt;
        struct lustre_msg       *msg;
	__u32			 len;
        int                      offset;

        fmt = pill->rc_fmt;
        LASSERT(fmt != NULL);
        LASSERT(__req_format_is_sane(fmt));
        LASSERT(req_capsule_has_field(pill, field, loc));
        LASSERT(req_capsule_field_present(pill, field, loc));

        offset = __req_capsule_offset(pill, field, loc);

	msg = __req_msg(pill, loc);
	len = lustre_msg_buflen(msg, offset);
	LASSERTF(newlen <= len, "%s:%s, oldlen=%u, newlen=%u\n",
		 fmt->rf_name, field->rmf_name, len, newlen);

	len = lustre_shrink_msg(msg, offset, newlen, 1);
	if (loc == RCL_CLIENT) {
		if (req_capsule_ptlreq(pill))
			pill->rc_req->rq_reqlen = len;
	} else {
		/* update also field size in reply lenghts arrays for possible
		 * reply re-pack due to req_capsule_server_grow() call.
		 */
		req_capsule_set_size(pill, field, loc, newlen);
		if (req_capsule_ptlreq(pill))
			pill->rc_req->rq_replen = len;
	}
}
EXPORT_SYMBOL(req_capsule_shrink);

int req_capsule_server_grow(struct req_capsule *pill,
			    const struct req_msg_field *field,
			    __u32 newlen)
{
	struct ptlrpc_request *req = pill->rc_req;
	struct ptlrpc_reply_state *rs = req->rq_reply_state, *nrs;
	char *from, *to, *sptr = NULL;
	__u32 slen = 0, snewlen = 0;
	__u32 offset, len, max, diff;
	int rc;

	LASSERT(pill->rc_fmt != NULL);
	LASSERT(__req_format_is_sane(pill->rc_fmt));
	LASSERT(req_capsule_has_field(pill, field, RCL_SERVER));
	LASSERT(req_capsule_field_present(pill, field, RCL_SERVER));

	if (req_capsule_subreq(pill)) {
		if (!req_capsule_has_field(&req->rq_pill, &RMF_BUT_REPLY,
					   RCL_SERVER))
			return -EINVAL;

		if (!req_capsule_field_present(&req->rq_pill, &RMF_BUT_REPLY,
					       RCL_SERVER))
			return -EINVAL;

		len = req_capsule_get_size(&req->rq_pill, &RMF_BUT_REPLY,
					   RCL_SERVER);
		sptr = req_capsule_server_get(&req->rq_pill, &RMF_BUT_REPLY);
		slen = req_capsule_get_size(pill, field, RCL_SERVER);

		LASSERT(len >= (char *)pill->rc_repmsg - sptr +
			       lustre_packed_msg_size(pill->rc_repmsg));
		if (len >= (char *)pill->rc_repmsg - sptr +
			   lustre_packed_msg_size(pill->rc_repmsg) - slen +
			   newlen) {
			req_capsule_set_size(pill, field, RCL_SERVER, newlen);
			offset = __req_capsule_offset(pill, field, RCL_SERVER);
			lustre_grow_msg(pill->rc_repmsg, offset, newlen);
			return 0;
		}

		/*
		 * Currently first try to increase the reply buffer by
		 * 2 * newlen with reply buffer limit of BUT_MAXREPSIZE.
		 * TODO: Enlarge the reply buffer properly according to the
		 * left SUB requests in the batch PTLRPC request.
		 */
		snewlen = newlen;
		diff = snewlen - slen;
		max = BUT_MAXREPSIZE - req->rq_replen;
		if (diff > max)
			return -EOVERFLOW;

		if (diff * 2 + len < max)
			newlen = (len + diff) * 2;
		else
			newlen = len + max;

		req_capsule_set_size(pill, field, RCL_SERVER, snewlen);
		req_capsule_set_size(&req->rq_pill, &RMF_BUT_REPLY, RCL_SERVER,
				     newlen);
		offset = __req_capsule_offset(&req->rq_pill, &RMF_BUT_REPLY,
					      RCL_SERVER);
	} else {
		len = req_capsule_get_size(pill, field, RCL_SERVER);
		offset = __req_capsule_offset(pill, field, RCL_SERVER);
		req_capsule_set_size(pill, field, RCL_SERVER, newlen);
	}

	CDEBUG(D_INFO, "Reply packed: %d, allocated: %d, field len %d -> %d\n",
	       lustre_packed_msg_size(rs->rs_msg), rs->rs_repbuf_len,
				      len, newlen);

	/**
	 * There can be enough space in current reply buffer, make sure
	 * that rs_repbuf is not a wrapper but real reply msg, otherwise
	 * re-packing is still needed.
	 */
	if (rs->rs_msg == rs->rs_repbuf &&
	    rs->rs_repbuf_len >=
	    lustre_packed_msg_size(rs->rs_msg) - len + newlen) {
		req->rq_replen = lustre_grow_msg(rs->rs_msg, offset, newlen);
		return 0;
	}

	/* Re-allocate replay state */
	req->rq_reply_state = NULL;
	rc = req_capsule_server_pack(&req->rq_pill);
	if (rc) {
		/* put old values back, the caller should decide what to do */
		if (req_capsule_subreq(pill)) {
			req_capsule_set_size(&req->rq_pill, &RMF_BUT_REPLY,
					     RCL_SERVER, len);
			req_capsule_set_size(pill, field, RCL_SERVER, slen);
		} else {
			req_capsule_set_size(pill, field, RCL_SERVER, len);
		}
		pill->rc_req->rq_reply_state = rs;
		return rc;
	}
	nrs = req->rq_reply_state;
	LASSERT(lustre_packed_msg_size(nrs->rs_msg) >
		lustre_packed_msg_size(rs->rs_msg));

	/* Now we need only buffers, copy them and grow the needed one */
	to = lustre_msg_buf(nrs->rs_msg, 0, 0);
	from = lustre_msg_buf(rs->rs_msg, 0, 0);
	memcpy(to, from,
	       (char *)rs->rs_msg + lustre_packed_msg_size(rs->rs_msg) - from);
	lustre_msg_set_buflen(nrs->rs_msg, offset, len);
	req->rq_replen = lustre_grow_msg(nrs->rs_msg, offset, newlen);

	if (req_capsule_subreq(pill)) {
		char *ptr;

		ptr = req_capsule_server_get(&req->rq_pill, &RMF_BUT_REPLY);
		pill->rc_repmsg = (struct lustre_msg *)(ptr +
				  ((char *)pill->rc_repmsg - sptr));
		offset = __req_capsule_offset(pill, field, RCL_SERVER);
		lustre_grow_msg(pill->rc_repmsg, offset, snewlen);
	}

        if (rs->rs_difficult) {
                /* copy rs data */
                int i;

                nrs->rs_difficult = 1;
                nrs->rs_no_ack = rs->rs_no_ack;
		nrs->rs_convert_lock = rs->rs_convert_lock;
                for (i = 0; i < rs->rs_nlocks; i++) {
                        nrs->rs_locks[i] = rs->rs_locks[i];
                        nrs->rs_modes[i] = rs->rs_modes[i];
                        nrs->rs_nlocks++;
                }
                rs->rs_nlocks = 0;
                rs->rs_difficult = 0;
                rs->rs_no_ack = 0;
        }
        ptlrpc_rs_decref(rs);
        return 0;
}
EXPORT_SYMBOL(req_capsule_server_grow);

#ifdef HAVE_SERVER_SUPPORT
static const struct req_msg_field *mds_update_client[] = {
	&RMF_PTLRPC_BODY,
	&RMF_OUT_UPDATE_HEADER,
	&RMF_OUT_UPDATE_BUF,
};

static const struct req_msg_field *mds_update_server[] = {
	&RMF_PTLRPC_BODY,
	&RMF_OUT_UPDATE_REPLY,
};

struct req_msg_field RMF_OUT_UPDATE = DEFINE_MSGFL("object_update", 0, -1,
				lustre_swab_object_update_request, NULL);
EXPORT_SYMBOL(RMF_OUT_UPDATE);

struct req_msg_field RMF_OUT_UPDATE_REPLY =
			DEFINE_MSGFL("object_update_reply", 0, -1,
				    lustre_swab_object_update_reply, NULL);
EXPORT_SYMBOL(RMF_OUT_UPDATE_REPLY);

struct req_msg_field RMF_OUT_UPDATE_HEADER = DEFINE_MSGF("out_update_header", 0,
				-1, lustre_swab_out_update_header, NULL);
EXPORT_SYMBOL(RMF_OUT_UPDATE_HEADER);

struct req_msg_field RMF_OUT_UPDATE_BUF = DEFINE_MSGF("update_buf",
			RMF_F_STRUCT_ARRAY, sizeof(struct out_update_buffer),
			lustre_swab_out_update_buffer, NULL);
EXPORT_SYMBOL(RMF_OUT_UPDATE_BUF);

struct req_format RQF_OUT_UPDATE =
	DEFINE_REQ_FMT0("OUT_UPDATE", mds_update_client,
			mds_update_server);
EXPORT_SYMBOL(RQF_OUT_UPDATE);

int req_check_sepol(struct req_capsule *pill)
{
	int rc = 0;
	struct obd_export *export;
	struct lu_nodemap *nm = NULL;
	const char *sepol = NULL;
	const char *nm_sepol = NULL;

	if (req_capsule_subreq(pill))
		return 0;

	if (!pill->rc_req)
		return -EPROTO;

	export = pill->rc_req->rq_export;
	if (!export || !exp_connect_sepol(export) ||
	    !req_capsule_has_field(pill, &RMF_SELINUX_POL, RCL_CLIENT))
		goto nm;

	if (req_capsule_get_size(pill, &RMF_SELINUX_POL, RCL_CLIENT) == 0)
		goto nm;

	sepol = req_capsule_client_get(pill, &RMF_SELINUX_POL);
	CDEBUG(D_SEC, "retrieved sepol %s\n", sepol);

nm:
	if (export) {
		nm = nodemap_get_from_exp(export);
		if (!IS_ERR_OR_NULL(nm)) {
			nm_sepol = nodemap_get_sepol(nm);
			if (nm_sepol && nm_sepol[0])
				if (sepol == NULL ||
				    strcmp(sepol, nm_sepol) != 0)
					rc = -EACCES;
		}
	}

	if (!IS_ERR_OR_NULL(nm))
		nodemap_putref(nm);

	return rc;
}
EXPORT_SYMBOL(req_check_sepol);
#endif

void req_capsule_subreq_init(struct req_capsule *pill,
			     const struct req_format *fmt,
			     struct ptlrpc_request *req,
			     struct lustre_msg *reqmsg,
			     struct lustre_msg *repmsg,
			     enum req_location loc)
{
	req_capsule_init(pill, req, loc);
	req_capsule_set(pill, fmt);
	pill->rc_reqmsg = reqmsg;
	pill->rc_repmsg = repmsg;
}
EXPORT_SYMBOL(req_capsule_subreq_init);

void req_capsule_set_replen(struct req_capsule *pill)
{
	if (req_capsule_ptlreq(pill)) {
		ptlrpc_request_set_replen(pill->rc_req);
	} else { /* SUB request in a batch request */
		int count;

		count = req_capsule_filled_sizes(pill, RCL_SERVER);
		pill->rc_reqmsg->lm_repsize =
				lustre_msg_size_v2(count,
						   pill->rc_area[RCL_SERVER]);
	}
}
EXPORT_SYMBOL(req_capsule_set_replen);
