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
#define DEBUG_SUBSYSTEM S_CLASS


#include <obd_support.h>
#include <obd.h>
#include <lprocfs_status.h>
#include <lustre_net.h>
#include <obd_class.h>
#include "ptlrpc_internal.h"


static struct ll_rpc_opcode {
	__u32       opcode;
	const char *opname;
} ll_rpc_opcode_table[LUSTRE_MAX_OPCODES] = {
	{ OST_REPLY,        "ost_reply" },
	{ OST_GETATTR,      "ost_getattr" },
	{ OST_SETATTR,      "ost_setattr" },
	{ OST_READ,         "ost_read" },
	{ OST_WRITE,        "ost_write" },
	{ OST_CREATE ,      "ost_create" },
	{ OST_DESTROY,      "ost_destroy" },
	{ OST_GET_INFO,     "ost_get_info" },
	{ OST_CONNECT,      "ost_connect" },
	{ OST_DISCONNECT,   "ost_disconnect" },
	{ OST_PUNCH,        "ost_punch" },
	{ OST_OPEN,         "ost_open" },
	{ OST_CLOSE,        "ost_close" },
	{ OST_STATFS,       "ost_statfs" },
	{ 14,                NULL },    /* formerly OST_SAN_READ */
	{ 15,                NULL },    /* formerly OST_SAN_WRITE */
	{ OST_SYNC,         "ost_sync" },
	{ OST_SET_INFO,     "ost_set_info" },
	{ OST_QUOTACHECK,   "ost_quotacheck" },
	{ OST_QUOTACTL,     "ost_quotactl" },
	{ OST_QUOTA_ADJUST_QUNIT, "ost_quota_adjust_qunit" },
	{ OST_LADVISE,      "ost_ladvise" },
	{ OST_FALLOCATE,    "ost_fallocate" },
	{ OST_SEEK,	    "ost_seek" },
	{ MDS_GETATTR,      "mds_getattr" },
	{ MDS_GETATTR_NAME, "mds_getattr_lock" },
	{ MDS_CLOSE,        "mds_close" },
	{ MDS_REINT,        "mds_reint" },
	{ MDS_READPAGE,     "mds_readpage" },
	{ MDS_CONNECT,      "mds_connect" },
	{ MDS_DISCONNECT,   "mds_disconnect" },
	{ MDS_GET_ROOT,     "mds_get_root" },
	{ MDS_STATFS,       "mds_statfs" },
	{ MDS_PIN,          "mds_pin" },
	{ MDS_UNPIN,        "mds_unpin" },
	{ MDS_SYNC,         "mds_sync" },
	{ MDS_DONE_WRITING, "mds_done_writing" },
	{ MDS_SET_INFO,     "mds_set_info" },
	{ MDS_QUOTACHECK,   "mds_quotacheck" },
	{ MDS_QUOTACTL,     "mds_quotactl" },
	{ MDS_GETXATTR,     "mds_getxattr" },
	{ MDS_SETXATTR,     "mds_setxattr" },
	{ MDS_WRITEPAGE,    "mds_writepage" },
	{ MDS_IS_SUBDIR,    "mds_is_subdir" },
	{ MDS_GET_INFO,     "mds_get_info" },
	{ MDS_HSM_STATE_GET, "mds_hsm_state_get" },
	{ MDS_HSM_STATE_SET, "mds_hsm_state_set" },
	{ MDS_HSM_ACTION,   "mds_hsm_action" },
	{ MDS_HSM_PROGRESS, "mds_hsm_progress" },
	{ MDS_HSM_REQUEST,  "mds_hsm_request" },
	{ MDS_HSM_CT_REGISTER, "mds_hsm_ct_register" },
	{ MDS_HSM_CT_UNREGISTER, "mds_hsm_ct_unregister" },
	{ MDS_SWAP_LAYOUTS,	"mds_swap_layouts" },
	{ MDS_RMFID,        "mds_rmfid" },
	{ MDS_BATCH,        "mds_batch" },
	{ LDLM_ENQUEUE,     "ldlm_enqueue" },
	{ LDLM_CONVERT,     "ldlm_convert" },
	{ LDLM_CANCEL,      "ldlm_cancel" },
	{ LDLM_BL_CALLBACK, "ldlm_bl_callback" },
	{ LDLM_CP_CALLBACK, "ldlm_cp_callback" },
	{ LDLM_GL_CALLBACK, "ldlm_gl_callback" },
	{ LDLM_SET_INFO,    "ldlm_set_info" },
	{ MGS_CONNECT,      "mgs_connect" },
	{ MGS_DISCONNECT,   "mgs_disconnect" },
	{ MGS_EXCEPTION,    "mgs_exception" },
	{ MGS_TARGET_REG,   "mgs_target_reg" },
	{ MGS_TARGET_DEL,   "mgs_target_del" },
	{ MGS_SET_INFO,     "mgs_set_info" },
	{ MGS_CONFIG_READ,  "mgs_config_read" },
	{ OBD_PING,			 "obd_ping" },
	{ 401, /* was OBD_LOG_CANCEL */ "llog_cancel" },
	{ 402, /* was OBD_QC_CALLBACK */ "obd_quota_callback" },
	{ OBD_IDX_READ, "dt_index_read" },
	{ LLOG_ORIGIN_HANDLE_CREATE, "llog_origin_handle_open" },
	{ LLOG_ORIGIN_HANDLE_NEXT_BLOCK, "llog_origin_handle_next_block" },
	{ LLOG_ORIGIN_HANDLE_READ_HEADER, "llog_origin_handle_read_header" },
	{ 504, /*LLOG_ORIGIN_HANDLE_WRITE_REC*/"llog_origin_handle_write_rec" },
	{ 505, /* was LLOG_ORIGIN_HANDLE_CLOSE */ "llog_origin_handle_close" },
	{ 506, /* was LLOG_ORIGIN_CONNECT */ "llog_origin_connect" },
	{ 507, /* was LLOG_CATINFO */ "llog_catinfo" },
	{ LLOG_ORIGIN_HANDLE_PREV_BLOCK, "llog_origin_handle_prev_block" },
	{ LLOG_ORIGIN_HANDLE_DESTROY,    "llog_origin_handle_destroy" },
	{ QUOTA_DQACQ,      "quota_acquire" },
	{ QUOTA_DQREL,      "quota_release" },
	{ SEQ_QUERY,        "seq_query" },
	{ SEC_CTX_INIT,     "sec_ctx_init" },
	{ SEC_CTX_INIT_CONT, "sec_ctx_init_cont" },
	{ SEC_CTX_FINI,     "sec_ctx_fini" },
	{ FLD_QUERY,        "fld_query" },
	{ FLD_READ,	    "fld_read" },
#ifdef HAVE_SERVER_SUPPORT
	{ OUT_UPDATE,	    "out_update" },
	{ LFSCK_NOTIFY,	    "lfsck_notify" },
	{ LFSCK_QUERY,	    "lfsck_query" },
#endif
};

static struct ll_eopcode {
	__u32       opcode;
	const char *opname;
} ll_eopcode_table[EXTRA_LAST_OPC] = {
	{ LDLM_GLIMPSE_ENQUEUE, "ldlm_glimpse_enqueue" },
	{ LDLM_PLAIN_ENQUEUE,   "ldlm_plain_enqueue" },
	{ LDLM_EXTENT_ENQUEUE,  "ldlm_extent_enqueue" },
	{ LDLM_FLOCK_ENQUEUE,   "ldlm_flock_enqueue" },
	{ LDLM_IBITS_ENQUEUE,   "ldlm_ibits_enqueue" },
	{ MDS_REINT_SETATTR,    "mds_reint_setattr" },
	{ MDS_REINT_CREATE,     "mds_reint_create" },
	{ MDS_REINT_LINK,       "mds_reint_link" },
	{ MDS_REINT_UNLINK,     "mds_reint_unlink" },
	{ MDS_REINT_RENAME,     "mds_reint_rename" },
	{ MDS_REINT_OPEN,       "mds_reint_open" },
	{ MDS_REINT_SETXATTR,   "mds_reint_setxattr" },
	{ MDS_REINT_RESYNC,	"mds_reint_resync" },
	{ BRW_READ_BYTES,       "read_bytes" },
	{ BRW_WRITE_BYTES,      "write_bytes" },
};

const char *ll_opcode2str(__u32 opcode)
{
        /* When one of the assertions below fail, chances are that:
         *     1) A new opcode was added in include/lustre/lustre_idl.h,
         *        but is missing from the table above.
         * or  2) The opcode space was renumbered or rearranged,
         *        and the opcode_offset() function in
         *        ptlrpc_internal.h needs to be modified.
         */
        __u32 offset = opcode_offset(opcode);
        LASSERTF(offset < LUSTRE_MAX_OPCODES,
                 "offset %u >= LUSTRE_MAX_OPCODES %u\n",
                 offset, LUSTRE_MAX_OPCODES);
        LASSERTF(ll_rpc_opcode_table[offset].opcode == opcode,
                 "ll_rpc_opcode_table[%u].opcode %u != opcode %u\n",
                 offset, ll_rpc_opcode_table[offset].opcode, opcode);
        return ll_rpc_opcode_table[offset].opname;
}

const int ll_str2opcode(const char *ops)
{
	int i;

	for (i = 0; i < LUSTRE_MAX_OPCODES; i++) {
		if (ll_rpc_opcode_table[i].opname != NULL &&
		    strcmp(ll_rpc_opcode_table[i].opname, ops) == 0)
			return ll_rpc_opcode_table[i].opcode;
	}

	return -EINVAL;
}

static const char *ll_eopcode2str(__u32 opcode)
{
        LASSERT(ll_eopcode_table[opcode].opcode == opcode);
        return ll_eopcode_table[opcode].opname;
}

static void
ptlrpc_ldebugfs_register(struct dentry *root, char *dir, char *name,
			 struct dentry **debugfs_root_ret,
			 struct lprocfs_stats **stats_ret)
{
	struct dentry *svc_debugfs_entry;
	struct lprocfs_stats *svc_stats;
	int i;
	unsigned int svc_counter_config = LPROCFS_CNTR_AVGMINMAX |
					  LPROCFS_CNTR_STDDEV;

	LASSERT(!*debugfs_root_ret);
	LASSERT(!*stats_ret);

	svc_stats = lprocfs_alloc_stats(EXTRA_MAX_OPCODES + LUSTRE_MAX_OPCODES,
					0);
	if (!svc_stats)
                return;

	if (dir)
		svc_debugfs_entry = debugfs_create_dir(dir, root);
	else
		svc_debugfs_entry = root;

        lprocfs_counter_init(svc_stats, PTLRPC_REQWAIT_CNTR,
                             svc_counter_config, "req_waittime", "usec");
        lprocfs_counter_init(svc_stats, PTLRPC_REQQDEPTH_CNTR,
                             svc_counter_config, "req_qdepth", "reqs");
        lprocfs_counter_init(svc_stats, PTLRPC_REQACTIVE_CNTR,
                             svc_counter_config, "req_active", "reqs");
        lprocfs_counter_init(svc_stats, PTLRPC_TIMEOUT,
                             svc_counter_config, "req_timeout", "sec");
        lprocfs_counter_init(svc_stats, PTLRPC_REQBUF_AVAIL_CNTR,
                             svc_counter_config, "reqbuf_avail", "bufs");
        for (i = 0; i < EXTRA_LAST_OPC; i++) {
                char *units;

		switch (i) {
                case BRW_WRITE_BYTES:
                case BRW_READ_BYTES:
                        units = "bytes";
                        break;
                default:
                        units = "reqs";
                        break;
                }
                lprocfs_counter_init(svc_stats, PTLRPC_LAST_CNTR + i,
                                     svc_counter_config,
                                     ll_eopcode2str(i), units);
        }
        for (i = 0; i < LUSTRE_MAX_OPCODES; i++) {
                __u32 opcode = ll_rpc_opcode_table[i].opcode;
                lprocfs_counter_init(svc_stats,
                                     EXTRA_MAX_OPCODES + i, svc_counter_config,
                                     ll_opcode2str(opcode), "usec");
        }

	debugfs_create_file(name, 0644, svc_debugfs_entry, svc_stats,
			    &ldebugfs_stats_seq_fops);

	if (dir)
		*debugfs_root_ret = svc_debugfs_entry;
	*stats_ret = svc_stats;
}

static int
ptlrpc_lprocfs_req_history_len_seq_show(struct seq_file *m, void *v)
{
	struct ptlrpc_service *svc = m->private;
	struct ptlrpc_service_part *svcpt;
	int	total = 0;
	int	i;

	ptlrpc_service_for_each_part(svcpt, i, svc)
		total += svcpt->scp_hist_nrqbds;

	seq_printf(m, "%d\n", total);
	return 0;
}


LDEBUGFS_SEQ_FOPS_RO(ptlrpc_lprocfs_req_history_len);

static int
ptlrpc_lprocfs_req_history_max_seq_show(struct seq_file *m, void *n)
{
	struct ptlrpc_service *svc = m->private;
	struct ptlrpc_service_part *svcpt;
	int	total = 0;
	int	i;

	ptlrpc_service_for_each_part(svcpt, i, svc)
		total += svc->srv_hist_nrqbds_cpt_max;

	seq_printf(m, "%d\n", total);
	return 0;
}

static ssize_t
ptlrpc_lprocfs_req_history_max_seq_write(struct file *file,
					 const char __user *buffer,
					 size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct ptlrpc_service *svc = m->private;
	unsigned long long val;
	unsigned long long limit;
	int bufpages;
	int rc;

	rc = kstrtoull_from_user(buffer, count, 0, &val);
	if (rc < 0)
		return rc;

	if (val < 0 || val > INT_MAX)
		return -ERANGE;

	/* This sanity check is more of an insanity check; we can still
	 * hose a kernel by allowing the request history to grow too
	 * far. The roundup to the next power of two is an empirical way
	 * to take care that request buffer is allocated in Slab and thus
	 * will be upgraded */
	bufpages = (roundup_pow_of_two(svc->srv_buf_size) + PAGE_SIZE - 1) >>
							PAGE_SHIFT;
	limit = cfs_totalram_pages() / (2 * bufpages);
	/* do not allow history to consume more than half max number of rqbds */
	if ((svc->srv_nrqbds_max == 0 && val > limit) ||
	    (svc->srv_nrqbds_max != 0 && val > svc->srv_nrqbds_max / 2))
		return -ERANGE;

	spin_lock(&svc->srv_lock);

	if (val == 0)
		svc->srv_hist_nrqbds_cpt_max = 0;
	else
		svc->srv_hist_nrqbds_cpt_max =
			max(1, ((int)val / svc->srv_ncpts));

	spin_unlock(&svc->srv_lock);

	return count;
}

LDEBUGFS_SEQ_FOPS(ptlrpc_lprocfs_req_history_max);

static int
ptlrpc_lprocfs_req_buffers_max_seq_show(struct seq_file *m, void *n)
{
	struct ptlrpc_service *svc = m->private;

	seq_printf(m, "%d\n", svc->srv_nrqbds_max);
	return 0;
}

static ssize_t
ptlrpc_lprocfs_req_buffers_max_seq_write(struct file *file,
					 const char __user *buffer,
					 size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct ptlrpc_service *svc = m->private;
	int val;
	int rc;

	rc = kstrtoint_from_user(buffer, count, 0, &val);
	if (rc < 0)
		return rc;

	if (val < svc->srv_nbuf_per_group && val != 0)
		return -ERANGE;

	spin_lock(&svc->srv_lock);

	svc->srv_nrqbds_max = (uint)val;

	spin_unlock(&svc->srv_lock);

	return count;
}

LDEBUGFS_SEQ_FOPS(ptlrpc_lprocfs_req_buffers_max);

static ssize_t threads_min_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);

	return sprintf(buf, "%d\n", svc->srv_nthrs_cpt_init * svc->srv_ncpts);
}

static ssize_t threads_min_store(struct kobject *kobj, struct attribute *attr,
				 const char *buffer, size_t count)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);
	unsigned long val;
	int rc;

	rc = kstrtoul(buffer, 10, &val);
	if (rc < 0)
		return rc;

	if (val / svc->srv_ncpts < PTLRPC_NTHRS_INIT)
		return -ERANGE;

	spin_lock(&svc->srv_lock);
	if (val > svc->srv_nthrs_cpt_limit * svc->srv_ncpts) {
		spin_unlock(&svc->srv_lock);
		return -ERANGE;
	}

	svc->srv_nthrs_cpt_init = (int)val / svc->srv_ncpts;

	spin_unlock(&svc->srv_lock);

	return count;
}
LUSTRE_RW_ATTR(threads_min);

static ssize_t threads_started_show(struct kobject *kobj,
				    struct attribute *attr,
				    char *buf)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);
	struct ptlrpc_service_part *svcpt;
	int total = 0;
	int i;

	ptlrpc_service_for_each_part(svcpt, i, svc)
		total += svcpt->scp_nthrs_running;

	return sprintf(buf, "%d\n", total);
}
LUSTRE_RO_ATTR(threads_started);

static ssize_t threads_max_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);

	return sprintf(buf, "%d\n", svc->srv_nthrs_cpt_limit * svc->srv_ncpts);
}

static ssize_t threads_max_store(struct kobject *kobj, struct attribute *attr,
				 const char *buffer, size_t count)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);
	unsigned long val;
	int rc;

	rc = kstrtoul(buffer, 10, &val);
	if (rc < 0)
		return rc;

	if (val / svc->srv_ncpts < PTLRPC_NTHRS_INIT)
		return -ERANGE;

	spin_lock(&svc->srv_lock);
	if (val < svc->srv_nthrs_cpt_init * svc->srv_ncpts) {
		spin_unlock(&svc->srv_lock);
		return -ERANGE;
	}

	svc->srv_nthrs_cpt_limit = (int)val / svc->srv_ncpts;

	spin_unlock(&svc->srv_lock);

	return count;
}
LUSTRE_RW_ATTR(threads_max);

/**
 * Translates \e ptlrpc_nrs_pol_state values to human-readable strings.
 *
 * \param[in] state The policy state
 */
static const char *nrs_state2str(enum ptlrpc_nrs_pol_state state)
{
	switch (state) {
	default:
		LBUG();
	case NRS_POL_STATE_INVALID:
		return "invalid";
	case NRS_POL_STATE_STOPPED:
		return "stopped";
	case NRS_POL_STATE_STOPPING:
		return "stopping";
	case NRS_POL_STATE_STARTING:
		return "starting";
	case NRS_POL_STATE_STARTED:
		return "started";
	}
}

/**
 * Obtains status information for \a policy.
 *
 * Information is copied in \a info.
 *
 * \param[in] policy The policy
 * \param[out] info  Holds returned status information
 */
static void nrs_policy_get_info_locked(struct ptlrpc_nrs_policy *policy,
				       struct ptlrpc_nrs_pol_info *info)
{
	LASSERT(policy != NULL);
	LASSERT(info != NULL);
	assert_spin_locked(&policy->pol_nrs->nrs_lock);

	BUILD_BUG_ON(sizeof(info->pi_arg) != sizeof(policy->pol_arg));
	memcpy(info->pi_name, policy->pol_desc->pd_name, NRS_POL_NAME_MAX);
	memcpy(info->pi_arg, policy->pol_arg, sizeof(policy->pol_arg));

	info->pi_fallback    = !!(policy->pol_flags & PTLRPC_NRS_FL_FALLBACK);
	info->pi_state	     = policy->pol_state;
	/**
	 * XXX: These are accessed without holding
	 * ptlrpc_service_part::scp_req_lock.
	 */
	info->pi_req_queued  = policy->pol_req_queued;
	info->pi_req_started = policy->pol_req_started;
}

/**
 * Reads and prints policy status information for all policies of a PTLRPC
 * service.
 */
static int ptlrpc_lprocfs_nrs_seq_show(struct seq_file *m, void *n)
{
	struct ptlrpc_service	       *svc = m->private;
	struct ptlrpc_service_part     *svcpt;
	struct ptlrpc_nrs	       *nrs;
	struct ptlrpc_nrs_policy       *policy;
	struct ptlrpc_nrs_pol_info     *infos;
	struct ptlrpc_nrs_pol_info	tmp;
	unsigned			num_pols;
	unsigned			pol_idx = 0;
	bool				hp = false;
	int				i;
	int				rc = 0;
	ENTRY;

	/**
	 * Serialize NRS core lprocfs operations with policy registration/
	 * unregistration.
	 */
	mutex_lock(&nrs_core.nrs_mutex);

	/**
	 * Use the first service partition's regular NRS head in order to obtain
	 * the number of policies registered with NRS heads of this service. All
	 * service partitions will have the same number of policies.
	 */
	nrs = nrs_svcpt2nrs(svc->srv_parts[0], false);

	spin_lock(&nrs->nrs_lock);
	num_pols = svc->srv_parts[0]->scp_nrs_reg.nrs_num_pols;
	spin_unlock(&nrs->nrs_lock);

	OBD_ALLOC_PTR_ARRAY(infos, num_pols);
	if (infos == NULL)
		GOTO(out, rc = -ENOMEM);
again:

	ptlrpc_service_for_each_part(svcpt, i, svc) {
		nrs = nrs_svcpt2nrs(svcpt, hp);
		spin_lock(&nrs->nrs_lock);

		pol_idx = 0;

		list_for_each_entry(policy, &nrs->nrs_policy_list,
				    pol_list) {
			LASSERT(pol_idx < num_pols);

			nrs_policy_get_info_locked(policy, &tmp);
			/**
			 * Copy values when handling the first service
			 * partition.
			 */
			if (i == 0) {
				memcpy(infos[pol_idx].pi_name, tmp.pi_name,
				       NRS_POL_NAME_MAX);
				memcpy(infos[pol_idx].pi_arg, tmp.pi_arg,
				       sizeof(tmp.pi_arg));
				memcpy(&infos[pol_idx].pi_state, &tmp.pi_state,
				       sizeof(tmp.pi_state));
				infos[pol_idx].pi_fallback = tmp.pi_fallback;
				/**
				 * For the rest of the service partitions
				 * sanity-check the values we get.
				 */
			} else {
				LASSERT(strncmp(infos[pol_idx].pi_name,
						tmp.pi_name,
						NRS_POL_NAME_MAX) == 0);
				LASSERT(strncmp(infos[pol_idx].pi_arg,
						tmp.pi_arg,
						sizeof(tmp.pi_arg)) == 0);
				/**
				 * Not asserting ptlrpc_nrs_pol_info::pi_state,
				 * because it may be different between
				 * instances of the same policy in different
				 * service partitions.
				 */
				LASSERT(infos[pol_idx].pi_fallback ==
					tmp.pi_fallback);
			}

			infos[pol_idx].pi_req_queued += tmp.pi_req_queued;
			infos[pol_idx].pi_req_started += tmp.pi_req_started;

			pol_idx++;
		}
		spin_unlock(&nrs->nrs_lock);
	}

	/**
	 * Policy status information output is in YAML format.
	 * For example:
	 *
	 *	regular_requests:
	 *	  - name: fifo
	 *	    state: started
	 *	    fallback: yes
	 *	    queued: 0
	 *	    active: 0
	 *
	 *	  - name: crrn
	 *	    state: started
	 *	    fallback: no
	 *	    queued: 2015
	 *	    active: 384
	 *
	 *	high_priority_requests:
	 *	  - name: fifo
	 *	    state: started
	 *	    fallback: yes
	 *	    queued: 0
	 *	    active: 2
	 *
	 *	  - name: crrn
	 *	    state: stopped
	 *	    fallback: no
	 *	    queued: 0
	 *	    active: 0
	 */
	seq_printf(m, "%s\n", !hp ? "\nregular_requests:" :
		   "high_priority_requests:");

	for (pol_idx = 0; pol_idx < num_pols; pol_idx++) {
		if (strlen(infos[pol_idx].pi_arg) > 0)
			seq_printf(m, "  - name: %s %s\n",
				   infos[pol_idx].pi_name,
				   infos[pol_idx].pi_arg);
		else
			seq_printf(m, "  - name: %s\n",
				   infos[pol_idx].pi_name);


		seq_printf(m, "    state: %s\n"
			   "    fallback: %s\n"
			   "    queued: %-20d\n"
			   "    active: %-20d\n\n",
			   nrs_state2str(infos[pol_idx].pi_state),
			   infos[pol_idx].pi_fallback ? "yes" : "no",
			   (int)infos[pol_idx].pi_req_queued,
			   (int)infos[pol_idx].pi_req_started);
	}

	if (!hp && nrs_svc_has_hp(svc)) {
		memset(infos, 0, num_pols * sizeof(*infos));

		/**
		 * Redo the processing for the service's HP NRS heads' policies.
		 */
		hp = true;
		goto again;
	}

out:
	if (infos)
		OBD_FREE_PTR_ARRAY(infos, num_pols);

	mutex_unlock(&nrs_core.nrs_mutex);

	RETURN(rc);
}


#define LPROCFS_NRS_WR_MAX_ARG (1024)
/**
 * The longest valid command string is the maxium policy name size, plus the
 * length of the " reg" substring, plus the lenght of argument
 */
#define LPROCFS_NRS_WR_MAX_CMD	(NRS_POL_NAME_MAX + sizeof(" reg") - 1 \
				 + LPROCFS_NRS_WR_MAX_ARG)

/**
 * Starts and stops a given policy on a PTLRPC service.
 *
 * Commands consist of the policy name, followed by an optional [reg|hp] token;
 * if the optional token is omitted, the operation is performed on both the
 * regular and high-priority (if the service has one) NRS head.
 */
static ssize_t
ptlrpc_lprocfs_nrs_seq_write(struct file *file, const char __user *buffer,
			     size_t count, loff_t *off)
{
	struct seq_file		       *m = file->private_data;
	struct ptlrpc_service	       *svc = m->private;
	enum ptlrpc_nrs_queue_type	queue = PTLRPC_NRS_QUEUE_BOTH;
	char			       *cmd;
	char			       *cmd_copy = NULL;
	char			       *policy_name;
	char			       *queue_name;
	int				rc = 0;
	ENTRY;

	if (count >= LPROCFS_NRS_WR_MAX_CMD)
		GOTO(out, rc = -EINVAL);

	OBD_ALLOC(cmd, LPROCFS_NRS_WR_MAX_CMD);
	if (cmd == NULL)
		GOTO(out, rc = -ENOMEM);
	/**
	 * strsep() modifies its argument, so keep a copy
	 */
	cmd_copy = cmd;

	if (copy_from_user(cmd, buffer, count))
		GOTO(out, rc = -EFAULT);

	cmd[count] = '\0';

	policy_name = strsep(&cmd, " ");

	if (strlen(policy_name) > NRS_POL_NAME_MAX - 1)
		GOTO(out, rc = -EINVAL);

	/**
	 * No [reg|hp] token has been specified
	 */
	if (cmd == NULL)
		goto default_queue;

	queue_name = strsep(&cmd, " ");
	/**
	 * The second token is either an optional [reg|hp] string,
	 * or arguments
	 */
	if (strcmp(queue_name, "reg") == 0)
		queue = PTLRPC_NRS_QUEUE_REG;
	else if (strcmp(queue_name, "hp") == 0)
		queue = PTLRPC_NRS_QUEUE_HP;
	else {
		if (cmd != NULL)
			*(cmd - 1) = ' ';
		cmd = queue_name;
	}

default_queue:

	if (queue == PTLRPC_NRS_QUEUE_HP && !nrs_svc_has_hp(svc))
		GOTO(out, rc = -ENODEV);
	else if (queue == PTLRPC_NRS_QUEUE_BOTH && !nrs_svc_has_hp(svc))
		queue = PTLRPC_NRS_QUEUE_REG;

	/**
	 * Serialize NRS core lprocfs operations with policy registration/
	 * unregistration.
	 */
	mutex_lock(&nrs_core.nrs_mutex);

	rc = ptlrpc_nrs_policy_control(svc, queue, policy_name,
				       PTLRPC_NRS_CTL_START,
				       false, cmd);

	mutex_unlock(&nrs_core.nrs_mutex);
out:
	if (cmd_copy)
		OBD_FREE(cmd_copy, LPROCFS_NRS_WR_MAX_CMD);

	RETURN(rc < 0 ? rc : count);
}

LDEBUGFS_SEQ_FOPS(ptlrpc_lprocfs_nrs);

/** @} nrs */

struct ptlrpc_srh_iterator {
	int			srhi_idx;
	__u64			srhi_seq;
	struct ptlrpc_request	*srhi_req;
};

static int
ptlrpc_lprocfs_svc_req_history_seek(struct ptlrpc_service_part *svcpt,
				    struct ptlrpc_srh_iterator *srhi,
				    __u64 seq)
{
	struct list_head	*e;
	struct ptlrpc_request	*req;

	if (srhi->srhi_req != NULL &&
	    srhi->srhi_seq > svcpt->scp_hist_seq_culled &&
            srhi->srhi_seq <= seq) {
                /* If srhi_req was set previously, hasn't been culled and
                 * we're searching for a seq on or after it (i.e. more
                 * recent), search from it onwards.
                 * Since the service history is LRU (i.e. culled reqs will
                 * be near the head), we shouldn't have to do long
                 * re-scans */
		LASSERTF(srhi->srhi_seq == srhi->srhi_req->rq_history_seq,
			 "%s:%d: seek seq %llu, request seq %llu\n",
			 svcpt->scp_service->srv_name, svcpt->scp_cpt,
			 srhi->srhi_seq, srhi->srhi_req->rq_history_seq);
		LASSERTF(!list_empty(&svcpt->scp_hist_reqs),
			 "%s:%d: seek offset %llu, request seq %llu, "
			 "last culled %llu\n",
			 svcpt->scp_service->srv_name, svcpt->scp_cpt,
			 seq, srhi->srhi_seq, svcpt->scp_hist_seq_culled);
		e = &srhi->srhi_req->rq_history_list;
	} else {
		/* search from start */
		e = svcpt->scp_hist_reqs.next;
	}

	while (e != &svcpt->scp_hist_reqs) {
		req = list_entry(e, struct ptlrpc_request, rq_history_list);

                if (req->rq_history_seq >= seq) {
                        srhi->srhi_seq = req->rq_history_seq;
                        srhi->srhi_req = req;
                        return 0;
                }
                e = e->next;
        }

        return -ENOENT;
}

/*
 * ptlrpc history sequence is used as "position" of seq_file, in some case,
 * seq_read() will increase "position" to indicate reading the next
 * element, however, low bits of history sequence are reserved for CPT id
 * (check the details from comments before ptlrpc_req_add_history), which
 * means seq_read() might change CPT id of history sequence and never
 * finish reading of requests on a CPT. To make it work, we have to shift
 * CPT id to high bits and timestamp to low bits, so seq_read() will only
 * increase timestamp which can correctly indicate the next position.
 */

/* convert seq_file pos to cpt */
#define PTLRPC_REQ_POS2CPT(svc, pos)			\
	((svc)->srv_cpt_bits == 0 ? 0 :			\
	 (__u64)(pos) >> (64 - (svc)->srv_cpt_bits))

/* make up seq_file pos from cpt */
#define PTLRPC_REQ_CPT2POS(svc, cpt)			\
	((svc)->srv_cpt_bits == 0 ? 0 :			\
	 (cpt) << (64 - (svc)->srv_cpt_bits))

/* convert sequence to position */
#define PTLRPC_REQ_SEQ2POS(svc, seq)			\
	((svc)->srv_cpt_bits == 0 ? (seq) :		\
	 ((seq) >> (svc)->srv_cpt_bits) |		\
	 ((seq) << (64 - (svc)->srv_cpt_bits)))

/* convert position to sequence */
#define PTLRPC_REQ_POS2SEQ(svc, pos)			\
	((svc)->srv_cpt_bits == 0 ? (pos) :		\
	 ((__u64)(pos) << (svc)->srv_cpt_bits) |	\
	 ((__u64)(pos) >> (64 - (svc)->srv_cpt_bits)))

static void *
ptlrpc_lprocfs_svc_req_history_start(struct seq_file *s, loff_t *pos)
{
	struct ptlrpc_service		*svc = s->private;
	struct ptlrpc_service_part	*svcpt;
	struct ptlrpc_srh_iterator	*srhi;
	unsigned int			cpt;
	int				rc;
	int				i;

	if (sizeof(loff_t) != sizeof(__u64)) { /* can't support */
		CWARN("Failed to read request history because size of loff_t "
		      "%d can't match size of u64\n", (int)sizeof(loff_t));
		return NULL;
	}

	OBD_ALLOC(srhi, sizeof(*srhi));
	if (srhi == NULL)
		return NULL;

	srhi->srhi_seq = 0;
	srhi->srhi_req = NULL;

	cpt = PTLRPC_REQ_POS2CPT(svc, *pos);

	ptlrpc_service_for_each_part(svcpt, i, svc) {
		if (i < cpt) /* skip */
			continue;
		if (i > cpt) /* make up the lowest position for this CPT */
			*pos = PTLRPC_REQ_CPT2POS(svc, i);

		mutex_lock(&svcpt->scp_mutex);
		spin_lock(&svcpt->scp_lock);
		rc = ptlrpc_lprocfs_svc_req_history_seek(svcpt, srhi,
				PTLRPC_REQ_POS2SEQ(svc, *pos));
		spin_unlock(&svcpt->scp_lock);
		mutex_unlock(&svcpt->scp_mutex);
		if (rc == 0) {
			*pos = PTLRPC_REQ_SEQ2POS(svc, srhi->srhi_seq);
			srhi->srhi_idx = i;
			return srhi;
		}
	}

	OBD_FREE(srhi, sizeof(*srhi));
	return NULL;
}

static void
ptlrpc_lprocfs_svc_req_history_stop(struct seq_file *s, void *iter)
{
        struct ptlrpc_srh_iterator *srhi = iter;

        if (srhi != NULL)
                OBD_FREE(srhi, sizeof(*srhi));
}

static void *
ptlrpc_lprocfs_svc_req_history_next(struct seq_file *s,
				    void *iter, loff_t *pos)
{
	struct ptlrpc_service		*svc = s->private;
	struct ptlrpc_srh_iterator	*srhi = iter;
	struct ptlrpc_service_part	*svcpt;
	__u64				seq;
	int				rc;
	int				i;

	for (i = srhi->srhi_idx; i < svc->srv_ncpts; i++) {
		svcpt = svc->srv_parts[i];

		if (i > srhi->srhi_idx) { /* reset iterator for a new CPT */
			srhi->srhi_req = NULL;
			seq = srhi->srhi_seq = 0;
		} else { /* the next sequence */
			seq = srhi->srhi_seq + (1 << svc->srv_cpt_bits);
		}

		mutex_lock(&svcpt->scp_mutex);
		spin_lock(&svcpt->scp_lock);
		rc = ptlrpc_lprocfs_svc_req_history_seek(svcpt, srhi, seq);
		spin_unlock(&svcpt->scp_lock);
		mutex_unlock(&svcpt->scp_mutex);
		if (rc == 0) {
			*pos = PTLRPC_REQ_SEQ2POS(svc, srhi->srhi_seq);
			srhi->srhi_idx = i;
			return srhi;
		}
	}

	OBD_FREE(srhi, sizeof(*srhi));
	++*pos;
	return NULL;
}

/* common ost/mdt so_req_printer */
void target_print_req(void *seq_file, struct ptlrpc_request *req)
{
        /* Called holding srv_lock with irqs disabled.
         * Print specific req contents and a newline.
         * CAVEAT EMPTOR: check request message length before printing!!!
         * You might have received any old crap so you must be just as
         * careful here as the service's request parser!!! */
        struct seq_file *sf = seq_file;

        switch (req->rq_phase) {
        case RQ_PHASE_NEW:
                /* still awaiting a service thread's attention, or rejected
                 * because the generic request message didn't unpack */
                seq_printf(sf, "<not swabbed>\n");
                break;
        case RQ_PHASE_INTERPRET:
                /* being handled, so basic msg swabbed, and opc is valid
                 * but racing with mds_handle() */
        case RQ_PHASE_COMPLETE:
                /* been handled by mds_handle() reply state possibly still
                 * volatile */
                seq_printf(sf, "opc %d\n", lustre_msg_get_opc(req->rq_reqmsg));
                break;
        default:
                DEBUG_REQ(D_ERROR, req, "bad phase %d", req->rq_phase);
        }
}
EXPORT_SYMBOL(target_print_req);

static int ptlrpc_lprocfs_svc_req_history_show(struct seq_file *s, void *iter)
{
	struct ptlrpc_service		*svc = s->private;
	struct ptlrpc_srh_iterator	*srhi = iter;
	struct ptlrpc_service_part	*svcpt;
	struct ptlrpc_request		*req;
	int				rc;

	LASSERT(srhi->srhi_idx < svc->srv_ncpts);

	svcpt = svc->srv_parts[srhi->srhi_idx];

	mutex_lock(&svcpt->scp_mutex);
	spin_lock(&svcpt->scp_lock);

	rc = ptlrpc_lprocfs_svc_req_history_seek(svcpt, srhi, srhi->srhi_seq);

	if (rc == 0) {
		struct timespec64 arrival, sent, arrivaldiff;
		char nidstr[LNET_NIDSTR_SIZE];

		req = srhi->srhi_req;

		arrival.tv_sec = req->rq_arrival_time.tv_sec;
		arrival.tv_nsec = req->rq_arrival_time.tv_nsec;
		sent.tv_sec = req->rq_sent;
		sent.tv_nsec = 0;
		arrivaldiff = timespec64_sub(sent, arrival);

		/* Print common req fields.
		 * CAVEAT EMPTOR: we're racing with the service handler
		 * here.  The request could contain any old crap, so you
		 * must be just as careful as the service's request
		 * parser. Currently I only print stuff here I know is OK
		 * to look at coz it was set up in request_in_callback()!!!
		 */
		seq_printf(s,
			   "%lld:%s:%s:x%llu:%d:%s:%lld.%06lld:%lld.%06llds(%+lld.0s) ",
			   req->rq_history_seq,
			   req->rq_export && req->rq_export->exp_obd ?
				req->rq_export->exp_obd->obd_name :
				libcfs_nid2str_r(req->rq_self, nidstr,
						 sizeof(nidstr)),
			   libcfs_id2str(req->rq_peer), req->rq_xid,
			   req->rq_reqlen, ptlrpc_rqphase2str(req),
			   (s64)req->rq_arrival_time.tv_sec,
			   (s64)(req->rq_arrival_time.tv_nsec / NSEC_PER_USEC),
			   (s64)arrivaldiff.tv_sec,
			   (s64)(arrivaldiff.tv_nsec / NSEC_PER_USEC),
			   (s64)(req->rq_sent - req->rq_deadline));
		if (svc->srv_ops.so_req_printer == NULL)
			seq_printf(s, "\n");
		else
			svc->srv_ops.so_req_printer(s, srhi->srhi_req);
	}

	spin_unlock(&svcpt->scp_lock);
	mutex_unlock(&svcpt->scp_mutex);

	return rc;
}

static int
ptlrpc_lprocfs_svc_req_history_open(struct inode *inode, struct file *file)
{
	static const struct seq_operations sops = {
		.start = ptlrpc_lprocfs_svc_req_history_start,
		.stop  = ptlrpc_lprocfs_svc_req_history_stop,
		.next  = ptlrpc_lprocfs_svc_req_history_next,
		.show  = ptlrpc_lprocfs_svc_req_history_show,
	};
	struct seq_file	*seqf;
	int		rc;

	rc = seq_open(file, &sops);
	if (rc)
		return rc;

	seqf = file->private_data;
	seqf->private = inode->i_private;
	return 0;
}

/* See also lprocfs_rd_timeouts */
static int ptlrpc_lprocfs_timeouts_seq_show(struct seq_file *m, void *n)
{
	struct ptlrpc_service *svc = m->private;
	struct ptlrpc_service_part *svcpt;
	time64_t worst_timestamp;
	timeout_t cur_timeout;
	timeout_t worst_timeout;
	int i;

	if (AT_OFF) {
		seq_printf(m, "adaptive timeouts off, using obd_timeout %u\n",
			   obd_timeout);
		return 0;
	}

	ptlrpc_service_for_each_part(svcpt, i, svc) {
		cur_timeout = at_get(&svcpt->scp_at_estimate);
		worst_timeout = svcpt->scp_at_estimate.at_worst_timeout_ever;
		worst_timestamp = svcpt->scp_at_estimate.at_worst_timestamp;

		seq_printf(m, "%10s : cur %3u  worst %3u (at %lld, %llds ago) ",
			   "service", cur_timeout, worst_timeout,
			   worst_timestamp,
			   ktime_get_real_seconds() - worst_timestamp);

		lprocfs_at_hist_helper(m, &svcpt->scp_at_estimate);
	}

	return 0;
}

LDEBUGFS_SEQ_FOPS_RO(ptlrpc_lprocfs_timeouts);

static ssize_t high_priority_ratio_show(struct kobject *kobj,
					struct attribute *attr,
					char *buf)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);

	return sprintf(buf, "%d\n", svc->srv_hpreq_ratio);
}

static ssize_t high_priority_ratio_store(struct kobject *kobj,
					 struct attribute *attr,
					 const char *buffer,
					 size_t count)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);
	int rc;
	unsigned long val;

	rc = kstrtoul(buffer, 10, &val);
	if (rc < 0)
		return rc;

	spin_lock(&svc->srv_lock);
	svc->srv_hpreq_ratio = val;
	spin_unlock(&svc->srv_lock);

	return count;
}
LUSTRE_RW_ATTR(high_priority_ratio);

static struct attribute *ptlrpc_svc_attrs[] = {
	&lustre_attr_threads_min.attr,
	&lustre_attr_threads_started.attr,
	&lustre_attr_threads_max.attr,
	&lustre_attr_high_priority_ratio.attr,
	NULL,
};

static void ptlrpc_sysfs_svc_release(struct kobject *kobj)
{
	struct ptlrpc_service *svc = container_of(kobj, struct ptlrpc_service,
						  srv_kobj);

	complete(&svc->srv_kobj_unregister);
}

static struct kobj_type ptlrpc_svc_ktype = {
	.default_attrs	= ptlrpc_svc_attrs,
	.sysfs_ops	= &lustre_sysfs_ops,
	.release	= ptlrpc_sysfs_svc_release,
};

void ptlrpc_sysfs_unregister_service(struct ptlrpc_service *svc)
{
	/* Let's see if we had a chance at initialization first */
	if (svc->srv_kobj.kset) {
		kobject_put(&svc->srv_kobj);
		wait_for_completion(&svc->srv_kobj_unregister);
	}
}

int ptlrpc_sysfs_register_service(struct kset *parent,
				  struct ptlrpc_service *svc)
{
	svc->srv_kobj.kset = parent;
	init_completion(&svc->srv_kobj_unregister);
	return kobject_init_and_add(&svc->srv_kobj, &ptlrpc_svc_ktype,
				    &parent->kobj, "%s", svc->srv_name);
}

void ptlrpc_ldebugfs_register_service(struct dentry *entry,
				      struct ptlrpc_service *svc)
{
	struct ldebugfs_vars ldebugfs_vars[] = {
		{ .name	= "req_buffer_history_len",
		  .fops	= &ptlrpc_lprocfs_req_history_len_fops,
		  .data	= svc },
		{ .name = "req_buffer_history_max",
		  .fops	= &ptlrpc_lprocfs_req_history_max_fops,
		  .data	= svc },
		{ .name = "timeouts",
		  .fops = &ptlrpc_lprocfs_timeouts_fops,
		  .data = svc },
		{ .name = "nrs_policies",
		  .fops = &ptlrpc_lprocfs_nrs_fops,
		  .data = svc },
		{ .name = "req_buffers_max",
		  .fops = &ptlrpc_lprocfs_req_buffers_max_fops,
		  .data = svc },
		{ NULL }
	};
	static const struct file_operations req_history_fops = {
		.owner		= THIS_MODULE,
		.open		= ptlrpc_lprocfs_svc_req_history_open,
		.read		= seq_read,
		.llseek		= seq_lseek,
		.release	= lprocfs_seq_release,
	};

	ptlrpc_ldebugfs_register(entry, svc->srv_name, "stats",
				 &svc->srv_debugfs_entry, &svc->srv_stats);
	if (!svc->srv_debugfs_entry)
		return;

	ldebugfs_add_vars(svc->srv_debugfs_entry, ldebugfs_vars, NULL);

	debugfs_create_file("req_history", 0400, svc->srv_debugfs_entry, svc,
			    &req_history_fops);
}

void ptlrpc_lprocfs_register_obd(struct obd_device *obd)
{
	ptlrpc_ldebugfs_register(obd->obd_debugfs_entry, NULL, "stats",
				 &obd->obd_svc_debugfs_entry,
				 &obd->obd_svc_stats);
}
EXPORT_SYMBOL(ptlrpc_lprocfs_register_obd);

void ptlrpc_lprocfs_rpc_sent(struct ptlrpc_request *req, long amount)
{
        struct lprocfs_stats *svc_stats;
        __u32 op = lustre_msg_get_opc(req->rq_reqmsg);
        int opc = opcode_offset(op);

        svc_stats = req->rq_import->imp_obd->obd_svc_stats;
        if (svc_stats == NULL || opc <= 0)
                return;
        LASSERT(opc < LUSTRE_MAX_OPCODES);
        if (!(op == LDLM_ENQUEUE || op == MDS_REINT))
                lprocfs_counter_add(svc_stats, opc + EXTRA_MAX_OPCODES, amount);
}

void ptlrpc_lprocfs_brw(struct ptlrpc_request *req, int bytes)
{
        struct lprocfs_stats *svc_stats;
        int idx;

        if (!req->rq_import)
                return;
        svc_stats = req->rq_import->imp_obd->obd_svc_stats;
        if (!svc_stats)
                return;
        idx = lustre_msg_get_opc(req->rq_reqmsg);
        switch (idx) {
        case OST_READ:
                idx = BRW_READ_BYTES + PTLRPC_LAST_CNTR;
                break;
        case OST_WRITE:
                idx = BRW_WRITE_BYTES + PTLRPC_LAST_CNTR;
                break;
        default:
                LASSERTF(0, "unsupported opcode %u\n", idx);
                break;
        }

        lprocfs_counter_add(svc_stats, idx, bytes);
}

EXPORT_SYMBOL(ptlrpc_lprocfs_brw);

void ptlrpc_lprocfs_unregister_service(struct ptlrpc_service *svc)
{
	debugfs_remove_recursive(svc->srv_debugfs_entry);

	if (svc->srv_stats)
		lprocfs_free_stats(&svc->srv_stats);
}

void ptlrpc_lprocfs_unregister_obd(struct obd_device *obd)
{
	/* cleanup first to allow concurrent access to device's
	 * stats via debugfs to complete safely
	 */
	lprocfs_obd_cleanup(obd);

	debugfs_remove_recursive(obd->obd_svc_debugfs_entry);

	if (obd->obd_svc_stats)
		lprocfs_free_stats(&obd->obd_svc_stats);
}
EXPORT_SYMBOL(ptlrpc_lprocfs_unregister_obd);

ssize_t ping_show(struct kobject *kobj, struct attribute *attr,
		  char *buffer)
{
	struct obd_device *obd = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct obd_import *imp;
	struct ptlrpc_request *req;
	int rc;

	ENTRY;
	with_imp_locked(obd, imp, rc)
		req = ptlrpc_prep_ping(imp);

	if (rc)
		RETURN(rc);
	if (!req)
		RETURN(-ENOMEM);

	req->rq_send_state = LUSTRE_IMP_FULL;

	rc = ptlrpc_queue_wait(req);
	ptlrpc_req_finished(req);

	RETURN(rc);
}
EXPORT_SYMBOL(ping_show);

/* kept for older verison of tools. */
ssize_t ping_store(struct kobject *kobj, struct attribute *attr,
		   const char *buffer, size_t count)
{
	int rc = ping_show(kobj, attr, (char *)buffer);

	return (rc < 0) ? rc : count;
}
EXPORT_SYMBOL(ping_store);

/* Write the connection UUID to this file to attempt to connect to that node.
 * The connection UUID is a node's primary NID. For example,
 * "echo connection=192.168.0.1@tcp0::instance > .../import".
 */
ssize_t
ldebugfs_import_seq_write(struct file *file, const char __user *buffer,
			  size_t count, loff_t *off)
{
	struct seq_file	  *m	= file->private_data;
	struct obd_device *obd	= m->private;
	struct obd_import *imp;
	char *kbuf = NULL;
	char *uuid;
	char *ptr;
	int do_reconn = 1;
	const char prefix[] = "connection=";
	const int prefix_len = sizeof(prefix) - 1;
	int rc = 0;

	if (count > PAGE_SIZE - 1 || count <= prefix_len)
		return -EINVAL;

	OBD_ALLOC(kbuf, count + 1);
	if (kbuf == NULL)
		return -ENOMEM;

	if (copy_from_user(kbuf, buffer, count))
		GOTO(out, rc = -EFAULT);

	kbuf[count] = 0;

	/* only support connection=uuid::instance now */
	if (strncmp(prefix, kbuf, prefix_len) != 0)
		GOTO(out, rc = -EINVAL);

	with_imp_locked(obd, imp, rc) {
		uuid = kbuf + prefix_len;
		ptr = strstr(uuid, "::");
		if (ptr) {
			u32 inst;
			int rc;

			*ptr = 0;
			do_reconn = 0;
			ptr += 2; /* Skip :: */
			rc = kstrtouint(ptr, 10, &inst);
			if (rc) {
				CERROR("config: wrong instance # %s\n", ptr);
			} else if (inst != imp->imp_connect_data.ocd_instance) {
				CDEBUG(D_INFO,
				       "IR: %s is connecting to an obsoleted target(%u/%u), reconnecting...\n",
				       imp->imp_obd->obd_name,
				       imp->imp_connect_data.ocd_instance,
				       inst);
				do_reconn = 1;
			} else {
				CDEBUG(D_INFO,
				       "IR: %s has already been connecting to "
				       "new target(%u)\n",
				       imp->imp_obd->obd_name, inst);
			}
		}

		if (do_reconn)
			ptlrpc_recover_import(imp, uuid, 1);
	}

out:
	OBD_FREE(kbuf, count + 1);
	return rc ?: count;
}
EXPORT_SYMBOL(ldebugfs_import_seq_write);

int lprocfs_pinger_recov_seq_show(struct seq_file *m, void *n)
{
	struct obd_device *obd = m->private;
	struct obd_import *imp;
	int rc;

	with_imp_locked(obd, imp, rc)
		seq_printf(m, "%d\n", !imp->imp_no_pinger_recover);

	return rc;
}
EXPORT_SYMBOL(lprocfs_pinger_recov_seq_show);

ssize_t
lprocfs_pinger_recov_seq_write(struct file *file, const char __user *buffer,
			       size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct obd_device *obd = m->private;
	struct obd_import *imp;
	bool val;
	int rc;

	rc = kstrtobool_from_user(buffer, count, &val);
	if (rc < 0)
		return rc;

	with_imp_locked(obd, imp, rc) {
		spin_lock(&imp->imp_lock);
		imp->imp_no_pinger_recover = !val;
		spin_unlock(&imp->imp_lock);
	}

	return rc ?: count;
}
EXPORT_SYMBOL(lprocfs_pinger_recov_seq_write);
