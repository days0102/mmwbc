%module lnetconfig
%{
#include "libcfs/util/ioctl.h"
#include "libcfs/util/string.h"
#ifndef LUTF_MISSING_DEFINITIONS_H
#define LUTF_MISSING_DEFINITIONS_H

size_t strlcpy(char *tgt, const char *src, size_t tgt_len);
size_t strlcat(char *tgt, const char *src, size_t tgt_len);

#endif /* LUTF_MISSING_DEFINITIONS_H */


#include <inttypes.h>
#include <stdbool.h>

enum cYAML_object_type {
	CYAML_TYPE_FALSE = 0,
	CYAML_TYPE_TRUE,
	CYAML_TYPE_NULL,
	CYAML_TYPE_NUMBER,
	CYAML_TYPE_STRING,
	CYAML_TYPE_ARRAY,
	CYAML_TYPE_OBJECT
};

struct cYAML {
	/* next/prev allow you to walk array/object chains. */
	struct cYAML *cy_next, *cy_prev;
	/* An array or object item will have a child pointer pointing
	   to a chain of the items in the array/object. */
	struct cYAML *cy_child;
	/* The type of the item, as above. */
	enum cYAML_object_type cy_type;

	/* The item's string, if type==CYAML_TYPE_STRING */
	char *cy_valuestring;
	/* The item's number, if type==CYAML_TYPE_NUMBER */
	int64_t cy_valueint;
	/* The item's number, if type==CYAML_TYPE_NUMBER */
	double cy_valuedouble;
	/* The item's name string, if this item is the child of,
	   or is in the list of subitems of an object. */
	char *cy_string;
	/* user data which might need to be tracked per object */
	void *cy_user_data;
};

typedef void (*cYAML_user_data_free_cb)(void *);

typedef bool (*cYAML_walk_cb)(struct cYAML *, void *, void**);

struct cYAML *cYAML_load(FILE *file, struct cYAML **err_rc, bool debug);

struct cYAML *cYAML_build_tree(char *path, const char *yaml_blk,
				size_t yaml_blk_size,
				struct cYAML **err_str, bool debug);

void cYAML_print_tree(struct cYAML *node);

void cYAML_print_tree2file(FILE *f, struct cYAML *node);

void cYAML_dump(struct cYAML *node, char **buf);

void cYAML_free_tree(struct cYAML *node);

struct cYAML *cYAML_get_object_item(struct cYAML *parent,
				    const char *name);

struct cYAML *cYAML_get_next_seq_item(struct cYAML *seq,
				      struct cYAML **itm);

bool cYAML_is_sequence(struct cYAML *node);

struct cYAML *cYAML_find_object(struct cYAML *root, const char *key);

void cYAML_clean_usr_data(struct cYAML *node,
			  cYAML_user_data_free_cb free_cb);

struct cYAML *cYAML_create_object(struct cYAML *parent, char *key);

struct cYAML *cYAML_create_seq(struct cYAML *parent, char *key);

struct cYAML *cYAML_create_seq_item(struct cYAML *seq);

struct cYAML *cYAML_create_string(struct cYAML *parent, char *key,
				  char *value);

struct cYAML *cYAML_create_number(struct cYAML *parent, char *key,
				  double value);

void cYAML_insert_sibling(struct cYAML *root, struct cYAML *sibling);

void cYAML_insert_child(struct cYAML *parent, struct cYAML *node);

void cYAML_build_error(int rc, int seq_no, char *cmd,
			char *entity, char *err_str,
			struct cYAML **root);




#include <net/if.h>
#include <libcfs/util/string.h>
#include <linux/lnet/lnet-dlc.h>
#include <linux/lnet/nidstr.h>

#define LUSTRE_CFG_RC_NO_ERR			 0
#define LUSTRE_CFG_RC_BAD_PARAM			-1
#define LUSTRE_CFG_RC_MISSING_PARAM		-2
#define LUSTRE_CFG_RC_OUT_OF_RANGE_PARAM	-3
#define LUSTRE_CFG_RC_OUT_OF_MEM		-4
#define LUSTRE_CFG_RC_GENERIC_ERR		-5
#define LUSTRE_CFG_RC_NO_MATCH			-6
#define LUSTRE_CFG_RC_MATCH			-7
#define LUSTRE_CFG_RC_SKIP			-8
#define LUSTRE_CFG_RC_LAST_ELEM			-9
#define LUSTRE_CFG_RC_MARSHAL_FAIL		-10

#define CONFIG_CMD		"configure"
#define UNCONFIG_CMD		"unconfigure"
#define ADD_CMD			"add"
#define DEL_CMD			"del"
#define SHOW_CMD		"show"
#define DBG_CMD			"dbg"
#define MANAGE_CMD		"manage"

#define MAX_NUM_IPS		128

#define modparam_path "/sys/module/lnet/parameters/"
#define o2ib_modparam_path "/sys/module/ko2iblnd/parameters/"
#define gni_nid_path "/proc/cray_xt/"

enum lnetctl_cmd {
	LNETCTL_CONFIG_CMD	= 1,
	LNETCTL_UNCONFIG_CMD	= 2,
	LNETCTL_ADD_CMD		= 3,
	LNETCTL_DEL_CMD		= 4,
	LNETCTL_SHOW_CMD	= 5,
	LNETCTL_DBG_CMD		= 6,
	LNETCTL_MANAGE_CMD	= 7,
	LNETCTL_LAST_CMD
};

#define LNET_MAX_NIDS_PER_PEER 128

struct lnet_dlc_network_descr {
	struct list_head network_on_rule;
	__u32 nw_id;
	struct list_head nw_intflist;
};

struct lnet_dlc_intf_descr {
	struct list_head intf_on_network;
	char intf_name[IFNAMSIZ];
	struct cfs_expr_list *cpt_expr;
};


struct lnet_ud_net_descr {
	__u32 udn_net_type;
	struct list_head udn_net_num_range;
};

struct lnet_ud_nid_descr {
	struct lnet_ud_net_descr ud_net_id;
	struct list_head ud_addr_range;
};

struct lnet_udsp {
	struct list_head udsp_on_list;
	__u32 udsp_idx;
	struct lnet_ud_nid_descr udsp_src;
	struct lnet_ud_nid_descr udsp_dst;
	struct lnet_ud_nid_descr udsp_rte;
	enum lnet_udsp_action_type udsp_action_type;
	union {
		__u32 udsp_priority;
	} udsp_action;
};

union lnet_udsp_action {
	int udsp_priority;
};


int lustre_lnet_config_lib_init();

void lustre_lnet_config_lib_uninit();

int lustre_lnet_config_ni_system(bool up, bool load_ni_from_mod,
				 int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_route(char *nw, char *gw, int hops, int prio,
			     int sen, int seq_no, struct cYAML **err_rc);

int lustre_lnet_del_route(char *nw, char *gw, int seq_no,
			  struct cYAML **err_rc);

int lustre_lnet_show_route(char *nw, char *gw,
			   int hops, int prio, int detail,
			   int seq_no, struct cYAML **show_rc,
			   struct cYAML **err_rc, bool backup);

int lustre_lnet_config_ni(struct lnet_dlc_network_descr *nw_descr,
			  struct cfs_expr_list *global_cpts,
			  char *ip2net,
			  struct lnet_ioctl_config_lnd_tunables *tunables,
			  int seq_no, struct cYAML **err_rc);

int lustre_lnet_del_ni(struct lnet_dlc_network_descr *nw,
		       int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_net(char *nw, int detail, int seq_no,
			 struct cYAML **show_rc, struct cYAML **err_rc,
			 bool backup);

int lustre_lnet_enable_routing(int enable, int seq_no,
			       struct cYAML **err_rc);

int lustre_lnet_config_numa_range(int range, int seq_no,
				  struct cYAML **err_rc);

int lustre_lnet_show_numa_range(int seq_no, struct cYAML **show_rc,
				struct cYAML **err_rc);

int lustre_lnet_config_ni_healthv(int value, bool all, char *ni_nid,
				  int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_peer_ni_healthv(int value, bool all, char *pni_nid,
				       int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_recov_intrv(int intrv, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_recov_intrv(int seq_no, struct cYAML **show_rc,
				 struct cYAML **err_rc);

int lustre_lnet_config_rtr_sensitivity(int sen, int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_hsensitivity(int sen, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_hsensitivity(int seq_no, struct cYAML **show_rc,
				  struct cYAML **err_rc);

int lustre_lnet_show_rtr_sensitivity(int seq_no, struct cYAML **show_rc,
				     struct cYAML **err_rc);

int lustre_lnet_config_transaction_to(int timeout, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_transaction_to(int seq_no, struct cYAML **show_rc,
				    struct cYAML **err_rc);

int lustre_lnet_config_retry_count(int count, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_retry_count(int seq_no, struct cYAML **show_rc,
				 struct cYAML **err_rc);

int lustre_lnet_show_lnd_timeout(int seq_no, struct cYAML **show_rc,
				 struct cYAML **err_rc);

int lustre_lnet_show_local_ni_recovq(int seq_no, struct cYAML **show_rc,
				     struct cYAML **err_rc);

int lustre_lnet_show_peer_ni_recovq(int seq_no, struct cYAML **show_rc,
				    struct cYAML **err_rc);
int lustre_lnet_config_response_tracking(int count, int seq_no,
					 struct cYAML **err_rc);
int lustre_lnet_show_response_tracking(int seq_no, struct cYAML **show_rc,
				       struct cYAML **err_rc);
int lustre_lnet_config_recovery_limit(int val, int seq_no,
				      struct cYAML **err_rc);
int lustre_lnet_show_recovery_limit(int seq_no, struct cYAML **show_rc,
				    struct cYAML **err_rc);

int lustre_lnet_config_max_intf(int max, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_max_intf(int seq_no, struct cYAML **show_rc,
			      struct cYAML **err_rc);

int lustre_lnet_calc_service_id(__u64 *service_id);

int lustre_lnet_config_discovery(int enable, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_discovery(int seq_no, struct cYAML **show_rc,
			       struct cYAML **err_rc);

int lustre_lnet_config_drop_asym_route(int drop, int seq_no,
				       struct cYAML **err_rc);

int lustre_lnet_show_drop_asym_route(int seq_no, struct cYAML **show_rc,
				     struct cYAML **err_rc);

int lustre_lnet_config_buffers(int tiny, int small, int large,
			       int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_routing(int seq_no, struct cYAML **show_rc,
			     struct cYAML **err_rc, bool backup);

int lustre_lnet_show_stats(int seq_no, struct cYAML **show_rc,
			   struct cYAML **err_rc);

int lustre_lnet_modify_peer(char *prim_nid, char *nids, bool is_mr,
			    int cmd, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_peer(char *knid, int detail, int seq_no,
			  struct cYAML **show_rc, struct cYAML **err_rc,
			  bool backup);

int lustre_lnet_list_peer(int seq_no,
			  struct cYAML **show_rc, struct cYAML **err_rc);

int lustre_lnet_ping_nid(char *pnid, int timeout, int seq_no,
			struct cYAML **show_rc, struct cYAML **err_rc);

int lustre_lnet_discover_nid(char *pnid, int force, int seq_no,
			     struct cYAML **show_rc, struct cYAML **err_rc);

int lustre_yaml_config(char *f, struct cYAML **err_rc);

int lustre_yaml_del(char *f, struct cYAML **err_rc);

int lustre_yaml_show(char *f, struct cYAML **show_rc,
		     struct cYAML **err_rc);

int lustre_yaml_exec(char *f, struct cYAML **show_rc,
		     struct cYAML **err_rc);

void lustre_lnet_init_nw_descr(struct lnet_dlc_network_descr *nw_descr);

int lustre_lnet_parse_interfaces(char *intf_str,
				 struct lnet_dlc_network_descr *nw_descr);

int lustre_lnet_parse_nidstr(char *nidstr, lnet_nid_t *lnet_nidlist,
			     int max_nids, char *err_str);

int lustre_lnet_add_udsp(char *src, char *dst, char *rte, char *type,
			 union lnet_udsp_action *action, int idx,
			 int seq_no, struct cYAML **err_rc);

int lustre_lnet_del_udsp(unsigned int idx, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_udsp(int idx, int seq_no, struct cYAML **show_rc,
			  struct cYAML **err_rc);


#ifndef __UAPI_LNET_DLC_H_
#define __UAPI_LNET_DLC_H_

#include <linux/types.h>
#include <linux/lnet/libcfs_ioctl.h>
#include <linux/lnet/lnet-types.h>

#define MAX_NUM_SHOW_ENTRIES	32
#define LNET_MAX_STR_LEN	128
#define LNET_MAX_SHOW_NUM_CPT	128
#define LNET_MAX_SHOW_NUM_NID	128
#define LNET_UNDEFINED_HOPS	((__u32) -1)

#define LNET_RT_ALIVE		(1 << 0)
#define LNET_RT_MULTI_HOP	(1 << 1)

#ifndef __lutf_user
#define __lutf_user
#endif

struct lnet_ioctl_config_lnd_cmn_tunables {
	__u32 lct_version;
	__s32 lct_peer_timeout;
	__s32 lct_peer_tx_credits;
	__s32 lct_peer_rtr_credits;
	__s32 lct_max_tx_credits;
};

struct lnet_ioctl_config_o2iblnd_tunables {
	__u32 lnd_version;
	__u32 lnd_peercredits_hiw;
	__u32 lnd_map_on_demand;
	__u32 lnd_concurrent_sends;
	__u32 lnd_fmr_pool_size;
	__u32 lnd_fmr_flush_trigger;
	__u32 lnd_fmr_cache;
	__u16 lnd_conns_per_peer;
	__u16 lnd_ntx;
};

struct lnet_lnd_tunables {
	union {
		struct lnet_ioctl_config_o2iblnd_tunables lnd_o2ib;
	} lnd_tun_u;
};

struct lnet_ioctl_config_lnd_tunables {
	struct lnet_ioctl_config_lnd_cmn_tunables lt_cmn;
	struct lnet_lnd_tunables lt_tun;
};

struct lnet_ioctl_net_config {
	char ni_interface[LNET_MAX_STR_LEN];
	__u32 ni_status;
	__u32 ni_cpts[LNET_MAX_SHOW_NUM_CPT];
	char cfg_bulk[0];
};

#define LNET_TINY_BUF_IDX	0
#define LNET_SMALL_BUF_IDX	1
#define LNET_LARGE_BUF_IDX	2

#define LNET_NRBPOOLS		(LNET_LARGE_BUF_IDX + 1)

struct lnet_ioctl_pool_cfg {
	struct {
		__u32 pl_npages;
		__u32 pl_nbuffers;
		__u32 pl_credits;
		__u32 pl_mincredits;
	} pl_pools[LNET_NRBPOOLS];
	__u32 pl_routing;
};

struct lnet_ioctl_ping_data {
	struct libcfs_ioctl_hdr ping_hdr;

	__u32 op_param;
	__u32 ping_count;
	__u32 ping_flags;
	__u32 mr_info;
	struct lnet_process_id ping_id;
	struct lnet_process_id  *ping_buf;
};

struct lnet_ioctl_config_data {
	struct libcfs_ioctl_hdr cfg_hdr;

	__u32 cfg_net;
	__u32 cfg_count;
	__u64 cfg_nid;
	__u32 cfg_ncpts;

	union {
		struct {
			__u32 rtr_hop;
			__u32 rtr_priority;
			__u32 rtr_flags;
			__u32 rtr_sensitivity;
		} cfg_route;
		struct {
			char net_intf[LNET_MAX_STR_LEN];
			__s32 net_peer_timeout;
			__s32 net_peer_tx_credits;
			__s32 net_peer_rtr_credits;
			__s32 net_max_tx_credits;
			__u32 net_cksum_algo;
			__u32 net_interface_count;
		} cfg_net;
		struct {
			__u32 buf_enable;
			__s32 buf_tiny;
			__s32 buf_small;
			__s32 buf_large;
		} cfg_buffers;
	} cfg_config_u;

	char cfg_bulk[0];
};

struct lnet_ioctl_comm_count {
	__u32 ico_get_count;
	__u32 ico_put_count;
	__u32 ico_reply_count;
	__u32 ico_ack_count;
	__u32 ico_hello_count;
};

struct lnet_ioctl_element_stats {
	__u32 iel_send_count;
	__u32 iel_recv_count;
	__u32 iel_drop_count;
};

enum lnet_health_type {
	LNET_HEALTH_TYPE_LOCAL_NI = 0,
	LNET_HEALTH_TYPE_PEER_NI,
};

struct lnet_ioctl_local_ni_hstats {
	struct libcfs_ioctl_hdr hlni_hdr;
	lnet_nid_t hlni_nid;
	__u32 hlni_local_interrupt;
	__u32 hlni_local_dropped;
	__u32 hlni_local_aborted;
	__u32 hlni_local_no_route;
	__u32 hlni_local_timeout;
	__u32 hlni_local_error;
	__s32 hlni_health_value;
};

struct lnet_ioctl_peer_ni_hstats {
	__u32 hlpni_remote_dropped;
	__u32 hlpni_remote_timeout;
	__u32 hlpni_remote_error;
	__u32 hlpni_network_timeout;
	__s32 hlpni_health_value;
};

struct lnet_ioctl_element_msg_stats {
	struct libcfs_ioctl_hdr im_hdr;
	__u32 im_idx;
	struct lnet_ioctl_comm_count im_send_stats;
	struct lnet_ioctl_comm_count im_recv_stats;
	struct lnet_ioctl_comm_count im_drop_stats;
};

struct lnet_ioctl_config_ni {
	struct libcfs_ioctl_hdr lic_cfg_hdr;
	lnet_nid_t		lic_nid;
	char			lic_ni_intf[LNET_MAX_STR_LEN];
	char			lic_legacy_ip2nets[LNET_MAX_STR_LEN];
	__u32			lic_cpts[LNET_MAX_SHOW_NUM_CPT];
	__u32			lic_ncpts;
	__u32			lic_status;
	__u32			lic_idx;
	__s32			lic_dev_cpt;
	char			pad[4];
	char			lic_bulk[0];
};

struct lnet_peer_ni_credit_info {
	char cr_aliveness[LNET_MAX_STR_LEN];
	__u32 cr_refcount;
	__s32 cr_ni_peer_tx_credits;
	__s32 cr_peer_tx_credits;
	__s32 cr_peer_min_tx_credits;
	__u32 cr_peer_tx_qnob;
	__s32 cr_peer_rtr_credits;
	__s32 cr_peer_min_rtr_credits;
	__u32 cr_ncpt;
};

struct lnet_ioctl_peer {
	struct libcfs_ioctl_hdr pr_hdr;
	__u32 pr_count;
	__u32 pr_pad;
	lnet_nid_t pr_nid;

	union {
		struct lnet_peer_ni_credit_info  pr_peer_credits;
	} pr_lnd_u;
};

struct lnet_ioctl_peer_cfg {
	struct libcfs_ioctl_hdr prcfg_hdr;
	lnet_nid_t prcfg_prim_nid;
	lnet_nid_t prcfg_cfg_nid;
	__u32 prcfg_count;
	__u32 prcfg_mr;
	__u32 prcfg_state;
	__u32 prcfg_size;
	void  *prcfg_bulk;
};

struct lnet_ioctl_reset_health_cfg {
	struct libcfs_ioctl_hdr rh_hdr;
	enum lnet_health_type rh_type:32;
	__u16 rh_all:1;
	__s16 rh_value;
	lnet_nid_t rh_nid;
};

struct lnet_ioctl_recovery_list {
	struct libcfs_ioctl_hdr rlst_hdr;
	enum lnet_health_type rlst_type:32;
	__u32 rlst_num_nids;
	lnet_nid_t rlst_nid_array[LNET_MAX_SHOW_NUM_NID];
};

struct lnet_ioctl_set_value {
	struct libcfs_ioctl_hdr sv_hdr;
	__u32 sv_value;
};

struct lnet_ioctl_lnet_stats {
	struct libcfs_ioctl_hdr st_hdr;
	struct lnet_counters st_cntrs;
};

struct lnet_range_expr {
	__u32 re_lo;
	__u32 re_hi;
	__u32 re_stride;
};

struct lnet_expressions {
	__u32 le_count;
};

struct lnet_ioctl_udsp_net_descr {
	__u32 ud_net_type;
	struct lnet_expressions ud_net_num_expr;
};

struct lnet_ioctl_udsp_descr_hdr {
	/* The literals SRC, DST and RTE are encoded
	 * here.
	 */
	__u32 ud_descr_type;
	__u32 ud_descr_count;
};

struct lnet_ioctl_udsp_descr {
	struct lnet_ioctl_udsp_descr_hdr iud_src_hdr;
	struct lnet_ioctl_udsp_net_descr iud_net;
};

struct lnet_ioctl_udsp {
	struct libcfs_ioctl_hdr iou_hdr;
	__s32 iou_idx;
	__u32 iou_action_type;
	__u32 iou_bulk_size;
	union {
		__u32 priority;
	} iou_action;
	void  *iou_bulk;
};

struct lnet_ioctl_construct_udsp_info {
	struct libcfs_ioctl_hdr cud_hdr;
	__u32 cud_peer:1;
	lnet_nid_t cud_nid;
	__u32 cud_nid_priority;
	__u32 cud_net_priority;
	lnet_nid_t cud_pref_nid[LNET_MAX_SHOW_NUM_NID];
	lnet_nid_t cud_pref_rtr_nid[LNET_MAX_SHOW_NUM_NID];
};

#endif /* _LNET_DLC_H_ */
PyObject *lutf_parse_nidlist(char *str, int len, int max_nids);
char *lutf_nid2str(unsigned long nid);
%}

/* This is only for python2.7
%typemap(in) FILE * {
        $1 = PyFile_AsFile($input);
}*/

/* typemap for handling cYAML output parameter */
%typemap(in, numinputs=0) struct cYAML** (struct cYAML *temp) {
        temp = NULL;
        $1 = &temp;
}

%typemap(argout) struct cYAML** {
        /* The purpose of this typemap is to be able to handle out params
           Ex: if the function being called is: foo(cYAML**a, cYAML **b)
           then from python you'd call it: o1, o2 = foo()*/
        PyObject *o, *o2, *o3;
        o = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), $*1_descriptor, SWIG_POINTER_OWN);
        if ((!$result) || ($result == Py_None))
                $result = o;
        else
        {
                if(!PyTuple_Check($result))
                {
                        /* insert the original result in the tuple */
                        o2 = $result;
                        $result = PyTuple_New(1);
                        PyTuple_SetItem($result, 0, o2);
                }
                o3 = PyTuple_New(1);
                PyTuple_SetItem(o3, 0, o);
                o2 = $result;
                $result = PySequence_Concat(o2, o3);
                Py_DECREF(o2);
                Py_DECREF(o3);
        }
}

/* typemap for handling cfs_expr_list output parameter */
%typemap(in, numinputs=0) struct cfs_expr_list** (struct cfs_expr_list *temp) {
        temp = NULL;
        $1 = &temp;
}

%typemap(argout) struct cfs_expr_list** {
        /* The purpose of this typemap is to be able to handle out params
           Ex: if the function being called is: rc = foo(cfs_expr_list **a)
           then from python you'd call it: o1, o2 = foo() where o2 becomes
           the out parameter*/
        PyObject *o, *o2, *o3;
        o = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), $*1_descriptor, SWIG_POINTER_OWN);
        if ((!$result) || ($result == Py_None))
                $result = o;
        else
        {
                if(!PyTuple_Check($result))
                {
                        /* insert the original result in the tuple */
                        o2 = $result;
                        $result = PyTuple_New(1);
                        PyTuple_SetItem($result, 0, o2);
                }
                o3 = PyTuple_New(1);
                PyTuple_SetItem(o3, 0, o);
                o2 = $result;
                $result = PySequence_Concat(o2, o3);
                Py_DECREF(o2);
                Py_DECREF(o3);
        }
}

/* typemap for handling array of character array output parameter */
%typemap(in, numinputs=0) char *** (char **temp) {
        temp = NULL;
        $1 = &temp;
}

%typemap(argout) char *** {
        /* The purpose of this typemap is to be able to handle out params
           Ex: if the function being called is: rc = foo(char ***)
           then from python you'd call it: o1, o2 = foo() where o2 becomes
           the out parameter*/
        PyObject *o, *o2, *o3;
        o = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), $*1_descriptor, SWIG_POINTER_OWN);
        if ((!$result) || ($result == Py_None))
                $result = o;
        else
        {
                if(!PyTuple_Check($result))
                {
                        /* insert the original result in the tuple */
                        o2 = $result;
                        $result = PyTuple_New(1);
                        PyTuple_SetItem($result, 0, o2);
                }
                o3 = PyTuple_New(1);
                PyTuple_SetItem(o3, 0, o);
                o2 = $result;
                $result = PySequence_Concat(o2, o3);
                Py_DECREF(o2);
                Py_DECREF(o3);
        }
}


/* This input typemap declares that char** requires no input parameter.
 * Instead, the address of a local char* is used to call the function.
 */
%typemap(in,numinputs=0) char** (char* tmp) %{
    tmp = NULL;
    $1 = &tmp;
%}

/* After the function is called, the char** parameter contains a malloc'ed
 * char* pointer.
 * Construct a Python Unicode object (I'm using Python 3) and append it to
 * any existing return value for the wrapper.
 */
%typemap(argout) char** (PyObject* obj) %{
    if (*$1 == NULL)
       goto fail;
    obj = PyUnicode_FromString(*$1);
    $result = SWIG_Python_AppendOutput($result,obj);
%}

/* The malloc'ed pointer is no longer needed, so make sure it is freed. */
%typemap(freearg) char** %{
    if (*$1)
       free(*$1);
%}

/* typemap for handling lnet_nid_t output parameter */
%typemap(in,numinputs=0) lnet_nid_t ** (lnet_nid_t *temp) {
        temp = NULL;
        $1 = &temp;
}

%typemap(argout) lnet_nid_t ** {
        /* The purpose of this typemap is to be able to handle out params
           Ex: if the function being called is: rc = foo(lnet_nid_t **a)
           then from python you'd call it: o1, o2 = foo() where o2 becomes
           the out parameter*/
        PyObject *o, *o2, *o3;
        o = SWIG_NewPointerObj(SWIG_as_voidptr(*$1), $*1_descriptor, SWIG_POINTER_OWN);
        if ((!$result) || ($result == Py_None)) {
		fprintf(stderr, "AMIR: %d\n", result);
                $result = o;
	} else
        {
                if(!PyTuple_Check($result))
                {
			fprintf(stderr, "AMIR 2\n");
                        /* insert the original result in the tuple */
                        o2 = $result;
                        $result = PyTuple_New(1);
                        PyTuple_SetItem($result, 0, o2);
                }
		fprintf(stderr, "AMIR 3\n");
                o3 = PyTuple_New(1);
                PyTuple_SetItem(o3, 0, o);
                o2 = $result;
                $result = PySequence_Concat(o2, o3);
                Py_DECREF(o2);
                Py_DECREF(o3);
        }
}

/* The malloc'ed pointer is no longer needed, so make sure it is freed. */
%typemap(freearg) lnet_nid_t ** %{
    if (*$1) {
       free(*$1);
    }
%}

/*
 * This is an interesting type map to allow for passing python bytes to
 * C function using char *
%typemap(in) (char *yaml_bytes, int yaml_bytes_len) {
	Py_ssize_t len;
	PyBytes_AsStringAndSize($input, &$1, &len);
	$2 = (int)len;
}
*/
typedef char __s8;
typedef unsigned char __u8;
typedef short __s16;
typedef unsigned short __u16;
typedef int __s32;
typedef unsigned int __u32;
typedef long long __s64;
typedef unsigned long long __u64;
PyObject *lutf_parse_nidlist(char *str, int len, int max_nids);
char *lutf_nid2str(unsigned long nid);
/* Copyright (C) 1991-2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

/*
 *	ISO C99 Standard: 7.10/5.2.4.2.1 Sizes of integer types	<limits.h>
 */

#ifndef _LIBC_LIMITS_H_
#define _LIBC_LIMITS_H_	1

#define __GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION
#include <bits/libc-header-start.h>


/* Maximum length of any multibyte character in any locale.
   We define this value here since the gcc header does not define
   the correct value.  */
#define MB_LEN_MAX	16


/* If we are not using GNU CC we have to define all the symbols ourself.
   Otherwise use gcc's definitions (see below).  */
#if !defined __GNUC__ || __GNUC__ < 2

/* We only protect from multiple inclusion here, because all the other
   #include's protect themselves, and in GCC 2 we may #include_next through
   multiple copies of this file before we get to GCC's.  */
# ifndef _LIMITS_H
#  define _LIMITS_H	1

#include <bits/wordsize.h>

/* We don't have #include_next.
   Define ANSI <limits.h> for standard 32-bit words.  */

/* These assume 8-bit `char's, 16-bit `short int's,
   and 32-bit `int's and `long int's.  */

/* Number of bits in a `char'.	*/
#  define CHAR_BIT	8

/* Minimum and maximum values a `signed char' can hold.  */
#  define SCHAR_MIN	(-128)
#  define SCHAR_MAX	127

/* Maximum value an `unsigned char' can hold.  (Minimum is 0.)  */
#  define UCHAR_MAX	255

/* Minimum and maximum values a `char' can hold.  */
#  ifdef __CHAR_UNSIGNED__
#   define CHAR_MIN	0
#   define CHAR_MAX	UCHAR_MAX
#  else
#   define CHAR_MIN	SCHAR_MIN
#   define CHAR_MAX	SCHAR_MAX
#  endif

/* Minimum and maximum values a `signed short int' can hold.  */
#  define SHRT_MIN	(-32768)
#  define SHRT_MAX	32767

/* Maximum value an `unsigned short int' can hold.  (Minimum is 0.)  */
#  define USHRT_MAX	65535

/* Minimum and maximum values a `signed int' can hold.  */
#  define INT_MIN	(-INT_MAX - 1)
#  define INT_MAX	2147483647

/* Maximum value an `unsigned int' can hold.  (Minimum is 0.)  */
#  define UINT_MAX	4294967295U

/* Minimum and maximum values a `signed long int' can hold.  */
#  if __WORDSIZE == 64
#   define LONG_MAX	9223372036854775807L
#  else
#   define LONG_MAX	2147483647L
#  endif
#  define LONG_MIN	(-LONG_MAX - 1L)

/* Maximum value an `unsigned long int' can hold.  (Minimum is 0.)  */
#  if __WORDSIZE == 64
#   define ULONG_MAX	18446744073709551615UL
#  else
#   define ULONG_MAX	4294967295UL
#  endif

#  ifdef __USE_ISOC99

/* Minimum and maximum values a `signed long long int' can hold.  */
#   define LLONG_MAX	9223372036854775807LL
#   define LLONG_MIN	(-LLONG_MAX - 1LL)

/* Maximum value an `unsigned long long int' can hold.  (Minimum is 0.)  */
#   define ULLONG_MAX	18446744073709551615ULL

#  endif /* ISO C99 */

# endif	/* limits.h  */
#endif	/* GCC 2.  */

#endif	/* !_LIBC_LIMITS_H_ */

 /* Get the compiler's limits.h, which defines almost all the ISO constants.

    We put this #include_next outside the double inclusion check because
    it should be possible to include this file more than once and still get
    the definitions from gcc's header.  */
#if defined __GNUC__ && !defined _GCC_LIMITS_H_
/* `_GCC_LIMITS_H_' is what GCC's file defines.  */
# include_next <limits.h>
#endif

/* The <limits.h> files in some gcc versions don't define LLONG_MIN,
   LLONG_MAX, and ULLONG_MAX.  Instead only the values gcc defined for
   ages are available.  */
#if defined __USE_ISOC99 && defined __GNUC__
# ifndef LLONG_MIN
#  define LLONG_MIN	(-LLONG_MAX-1)
# endif
# ifndef LLONG_MAX
#  define LLONG_MAX	__LONG_LONG_MAX__
# endif
# ifndef ULLONG_MAX
#  define ULLONG_MAX	(LLONG_MAX * 2ULL + 1)
# endif
#endif

/* The integer width macros are not defined by GCC's <limits.h> before
   GCC 7, or if _GNU_SOURCE rather than
   __STDC_WANT_IEC_60559_BFP_EXT__ is used to enable this feature.  */
#if __GLIBC_USE (IEC_60559_BFP_EXT_C2X)
# ifndef CHAR_WIDTH
#  define CHAR_WIDTH 8
# endif
# ifndef SCHAR_WIDTH
#  define SCHAR_WIDTH 8
# endif
# ifndef UCHAR_WIDTH
#  define UCHAR_WIDTH 8
# endif
# ifndef SHRT_WIDTH
#  define SHRT_WIDTH 16
# endif
# ifndef USHRT_WIDTH
#  define USHRT_WIDTH 16
# endif
# ifndef INT_WIDTH
#  define INT_WIDTH 32
# endif
# ifndef UINT_WIDTH
#  define UINT_WIDTH 32
# endif
# ifndef LONG_WIDTH
#  define LONG_WIDTH __WORDSIZE
# endif
# ifndef ULONG_WIDTH
#  define ULONG_WIDTH __WORDSIZE
# endif
# ifndef LLONG_WIDTH
#  define LLONG_WIDTH 64
# endif
# ifndef ULLONG_WIDTH
#  define ULLONG_WIDTH 64
# endif
#endif /* Use IEC_60559_BFP_EXT.  */

/* The macros for _Bool are not defined by GCC's <limits.h> before GCC
   11, or if _GNU_SOURCE is defined rather than enabling C2x support
   with -std.  */
#if __GLIBC_USE (ISOC2X)
# ifndef BOOL_MAX
#  define BOOL_MAX 1
# endif
# ifndef BOOL_WIDTH
#  define BOOL_WIDTH 1
# endif
#endif

#ifdef	__USE_POSIX
/* POSIX adds things to <limits.h>.  */
# include <bits/posix1_lim.h>
#endif

#ifdef	__USE_POSIX2
# include <bits/posix2_lim.h>
#endif

#ifdef	__USE_XOPEN
# include <bits/xopen_lim.h>
#endif


#include <inttypes.h>
#include <stdbool.h>

enum cYAML_object_type {
	CYAML_TYPE_FALSE = 0,
	CYAML_TYPE_TRUE,
	CYAML_TYPE_NULL,
	CYAML_TYPE_NUMBER,
	CYAML_TYPE_STRING,
	CYAML_TYPE_ARRAY,
	CYAML_TYPE_OBJECT
};

struct cYAML {
	/* next/prev allow you to walk array/object chains. */
	struct cYAML *cy_next, *cy_prev;
	/* An array or object item will have a child pointer pointing
	   to a chain of the items in the array/object. */
	struct cYAML *cy_child;
	/* The type of the item, as above. */
	enum cYAML_object_type cy_type;

	/* The item's string, if type==CYAML_TYPE_STRING */
	char *cy_valuestring;
	/* The item's number, if type==CYAML_TYPE_NUMBER */
	int64_t cy_valueint;
	/* The item's number, if type==CYAML_TYPE_NUMBER */
	double cy_valuedouble;
	/* The item's name string, if this item is the child of,
	   or is in the list of subitems of an object. */
	char *cy_string;
	/* user data which might need to be tracked per object */
	void *cy_user_data;
};

typedef void (*cYAML_user_data_free_cb)(void *);

typedef bool (*cYAML_walk_cb)(struct cYAML *, void *, void**);

struct cYAML *cYAML_load(FILE *file, struct cYAML **err_rc, bool debug);

struct cYAML *cYAML_build_tree(char *path, const char *yaml_blk,
				size_t yaml_blk_size,
				struct cYAML **err_str, bool debug);

void cYAML_print_tree(struct cYAML *node);

void cYAML_print_tree2file(FILE *f, struct cYAML *node);

void cYAML_dump(struct cYAML *node, char **buf);

void cYAML_free_tree(struct cYAML *node);

struct cYAML *cYAML_get_object_item(struct cYAML *parent,
				    const char *name);

struct cYAML *cYAML_get_next_seq_item(struct cYAML *seq,
				      struct cYAML **itm);

bool cYAML_is_sequence(struct cYAML *node);

struct cYAML *cYAML_find_object(struct cYAML *root, const char *key);

void cYAML_clean_usr_data(struct cYAML *node,
			  cYAML_user_data_free_cb free_cb);

struct cYAML *cYAML_create_object(struct cYAML *parent, char *key);

struct cYAML *cYAML_create_seq(struct cYAML *parent, char *key);

struct cYAML *cYAML_create_seq_item(struct cYAML *seq);

struct cYAML *cYAML_create_string(struct cYAML *parent, char *key,
				  char *value);

struct cYAML *cYAML_create_number(struct cYAML *parent, char *key,
				  double value);

void cYAML_insert_sibling(struct cYAML *root, struct cYAML *sibling);

void cYAML_insert_child(struct cYAML *parent, struct cYAML *node);

void cYAML_build_error(int rc, int seq_no, char *cmd,
			char *entity, char *err_str,
			struct cYAML **root);




#include <net/if.h>
#include <libcfs/util/string.h>
#include <linux/lnet/lnet-dlc.h>
#include <linux/lnet/nidstr.h>

#define LUSTRE_CFG_RC_NO_ERR			 0
#define LUSTRE_CFG_RC_BAD_PARAM			-1
#define LUSTRE_CFG_RC_MISSING_PARAM		-2
#define LUSTRE_CFG_RC_OUT_OF_RANGE_PARAM	-3
#define LUSTRE_CFG_RC_OUT_OF_MEM		-4
#define LUSTRE_CFG_RC_GENERIC_ERR		-5
#define LUSTRE_CFG_RC_NO_MATCH			-6
#define LUSTRE_CFG_RC_MATCH			-7
#define LUSTRE_CFG_RC_SKIP			-8
#define LUSTRE_CFG_RC_LAST_ELEM			-9
#define LUSTRE_CFG_RC_MARSHAL_FAIL		-10

#define CONFIG_CMD		"configure"
#define UNCONFIG_CMD		"unconfigure"
#define ADD_CMD			"add"
#define DEL_CMD			"del"
#define SHOW_CMD		"show"
#define DBG_CMD			"dbg"
#define MANAGE_CMD		"manage"

#define MAX_NUM_IPS		128

#define modparam_path "/sys/module/lnet/parameters/"
#define o2ib_modparam_path "/sys/module/ko2iblnd/parameters/"
#define gni_nid_path "/proc/cray_xt/"

enum lnetctl_cmd {
	LNETCTL_CONFIG_CMD	= 1,
	LNETCTL_UNCONFIG_CMD	= 2,
	LNETCTL_ADD_CMD		= 3,
	LNETCTL_DEL_CMD		= 4,
	LNETCTL_SHOW_CMD	= 5,
	LNETCTL_DBG_CMD		= 6,
	LNETCTL_MANAGE_CMD	= 7,
	LNETCTL_LAST_CMD
};

#define LNET_MAX_NIDS_PER_PEER 128

struct lnet_dlc_network_descr {
	struct list_head network_on_rule;
	__u32 nw_id;
	struct list_head nw_intflist;
};

struct lnet_dlc_intf_descr {
	struct list_head intf_on_network;
	char intf_name[IFNAMSIZ];
	struct cfs_expr_list *cpt_expr;
};


struct lnet_ud_net_descr {
	__u32 udn_net_type;
	struct list_head udn_net_num_range;
};

struct lnet_ud_nid_descr {
	struct lnet_ud_net_descr ud_net_id;
	struct list_head ud_addr_range;
};

struct lnet_udsp {
	struct list_head udsp_on_list;
	__u32 udsp_idx;
	struct lnet_ud_nid_descr udsp_src;
	struct lnet_ud_nid_descr udsp_dst;
	struct lnet_ud_nid_descr udsp_rte;
	enum lnet_udsp_action_type udsp_action_type;
	union {
		__u32 udsp_priority;
	} udsp_action;
};

union lnet_udsp_action {
	int udsp_priority;
};


int lustre_lnet_config_lib_init();

void lustre_lnet_config_lib_uninit();

int lustre_lnet_config_ni_system(bool up, bool load_ni_from_mod,
				 int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_route(char *nw, char *gw, int hops, int prio,
			     int sen, int seq_no, struct cYAML **err_rc);

int lustre_lnet_del_route(char *nw, char *gw, int seq_no,
			  struct cYAML **err_rc);

int lustre_lnet_show_route(char *nw, char *gw,
			   int hops, int prio, int detail,
			   int seq_no, struct cYAML **show_rc,
			   struct cYAML **err_rc, bool backup);

int lustre_lnet_config_ni(struct lnet_dlc_network_descr *nw_descr,
			  struct cfs_expr_list *global_cpts,
			  char *ip2net,
			  struct lnet_ioctl_config_lnd_tunables *tunables,
			  int seq_no, struct cYAML **err_rc);

int lustre_lnet_del_ni(struct lnet_dlc_network_descr *nw,
		       int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_net(char *nw, int detail, int seq_no,
			 struct cYAML **show_rc, struct cYAML **err_rc,
			 bool backup);

int lustre_lnet_enable_routing(int enable, int seq_no,
			       struct cYAML **err_rc);

int lustre_lnet_config_numa_range(int range, int seq_no,
				  struct cYAML **err_rc);

int lustre_lnet_show_numa_range(int seq_no, struct cYAML **show_rc,
				struct cYAML **err_rc);

int lustre_lnet_config_ni_healthv(int value, bool all, char *ni_nid,
				  int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_peer_ni_healthv(int value, bool all, char *pni_nid,
				       int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_recov_intrv(int intrv, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_recov_intrv(int seq_no, struct cYAML **show_rc,
				 struct cYAML **err_rc);

int lustre_lnet_config_rtr_sensitivity(int sen, int seq_no, struct cYAML **err_rc);

int lustre_lnet_config_hsensitivity(int sen, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_hsensitivity(int seq_no, struct cYAML **show_rc,
				  struct cYAML **err_rc);

int lustre_lnet_show_rtr_sensitivity(int seq_no, struct cYAML **show_rc,
				     struct cYAML **err_rc);

int lustre_lnet_config_transaction_to(int timeout, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_transaction_to(int seq_no, struct cYAML **show_rc,
				    struct cYAML **err_rc);

int lustre_lnet_config_retry_count(int count, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_retry_count(int seq_no, struct cYAML **show_rc,
				 struct cYAML **err_rc);

int lustre_lnet_show_lnd_timeout(int seq_no, struct cYAML **show_rc,
				 struct cYAML **err_rc);

int lustre_lnet_show_local_ni_recovq(int seq_no, struct cYAML **show_rc,
				     struct cYAML **err_rc);

int lustre_lnet_show_peer_ni_recovq(int seq_no, struct cYAML **show_rc,
				    struct cYAML **err_rc);
int lustre_lnet_config_response_tracking(int count, int seq_no,
					 struct cYAML **err_rc);
int lustre_lnet_show_response_tracking(int seq_no, struct cYAML **show_rc,
				       struct cYAML **err_rc);
int lustre_lnet_config_recovery_limit(int val, int seq_no,
				      struct cYAML **err_rc);
int lustre_lnet_show_recovery_limit(int seq_no, struct cYAML **show_rc,
				    struct cYAML **err_rc);

int lustre_lnet_config_max_intf(int max, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_max_intf(int seq_no, struct cYAML **show_rc,
			      struct cYAML **err_rc);

int lustre_lnet_calc_service_id(__u64 *service_id);

int lustre_lnet_config_discovery(int enable, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_discovery(int seq_no, struct cYAML **show_rc,
			       struct cYAML **err_rc);

int lustre_lnet_config_drop_asym_route(int drop, int seq_no,
				       struct cYAML **err_rc);

int lustre_lnet_show_drop_asym_route(int seq_no, struct cYAML **show_rc,
				     struct cYAML **err_rc);

int lustre_lnet_config_buffers(int tiny, int small, int large,
			       int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_routing(int seq_no, struct cYAML **show_rc,
			     struct cYAML **err_rc, bool backup);

int lustre_lnet_show_stats(int seq_no, struct cYAML **show_rc,
			   struct cYAML **err_rc);

int lustre_lnet_modify_peer(char *prim_nid, char *nids, bool is_mr,
			    int cmd, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_peer(char *knid, int detail, int seq_no,
			  struct cYAML **show_rc, struct cYAML **err_rc,
			  bool backup);

int lustre_lnet_list_peer(int seq_no,
			  struct cYAML **show_rc, struct cYAML **err_rc);

int lustre_lnet_ping_nid(char *pnid, int timeout, int seq_no,
			struct cYAML **show_rc, struct cYAML **err_rc);

int lustre_lnet_discover_nid(char *pnid, int force, int seq_no,
			     struct cYAML **show_rc, struct cYAML **err_rc);

int lustre_yaml_config(char *f, struct cYAML **err_rc);

int lustre_yaml_del(char *f, struct cYAML **err_rc);

int lustre_yaml_show(char *f, struct cYAML **show_rc,
		     struct cYAML **err_rc);

int lustre_yaml_exec(char *f, struct cYAML **show_rc,
		     struct cYAML **err_rc);

void lustre_lnet_init_nw_descr(struct lnet_dlc_network_descr *nw_descr);

int lustre_lnet_parse_interfaces(char *intf_str,
				 struct lnet_dlc_network_descr *nw_descr);

int lustre_lnet_parse_nidstr(char *nidstr, lnet_nid_t *lnet_nidlist,
			     int max_nids, char *err_str);

int lustre_lnet_add_udsp(char *src, char *dst, char *rte, char *type,
			 union lnet_udsp_action *action, int idx,
			 int seq_no, struct cYAML **err_rc);

int lustre_lnet_del_udsp(unsigned int idx, int seq_no, struct cYAML **err_rc);

int lustre_lnet_show_udsp(int idx, int seq_no, struct cYAML **show_rc,
			  struct cYAML **err_rc);


#ifndef __UAPI_LNET_DLC_H_
#define __UAPI_LNET_DLC_H_

#include <linux/types.h>
#include <linux/lnet/libcfs_ioctl.h>
#include <linux/lnet/lnet-types.h>

#define MAX_NUM_SHOW_ENTRIES	32
#define LNET_MAX_STR_LEN	128
#define LNET_MAX_SHOW_NUM_CPT	128
#define LNET_MAX_SHOW_NUM_NID	128
#define LNET_UNDEFINED_HOPS	((__u32) -1)

#define LNET_RT_ALIVE		(1 << 0)
#define LNET_RT_MULTI_HOP	(1 << 1)

#ifndef __lutf_user
#define __lutf_user
#endif

struct lnet_ioctl_config_lnd_cmn_tunables {
	__u32 lct_version;
	__s32 lct_peer_timeout;
	__s32 lct_peer_tx_credits;
	__s32 lct_peer_rtr_credits;
	__s32 lct_max_tx_credits;
};

struct lnet_ioctl_config_o2iblnd_tunables {
	__u32 lnd_version;
	__u32 lnd_peercredits_hiw;
	__u32 lnd_map_on_demand;
	__u32 lnd_concurrent_sends;
	__u32 lnd_fmr_pool_size;
	__u32 lnd_fmr_flush_trigger;
	__u32 lnd_fmr_cache;
	__u16 lnd_conns_per_peer;
	__u16 lnd_ntx;
};

struct lnet_lnd_tunables {
	union {
		struct lnet_ioctl_config_o2iblnd_tunables lnd_o2ib;
	} lnd_tun_u;
};

struct lnet_ioctl_config_lnd_tunables {
	struct lnet_ioctl_config_lnd_cmn_tunables lt_cmn;
	struct lnet_lnd_tunables lt_tun;
};

struct lnet_ioctl_net_config {
	char ni_interface[LNET_MAX_STR_LEN];
	__u32 ni_status;
	__u32 ni_cpts[LNET_MAX_SHOW_NUM_CPT];
	char cfg_bulk[0];
};

#define LNET_TINY_BUF_IDX	0
#define LNET_SMALL_BUF_IDX	1
#define LNET_LARGE_BUF_IDX	2

#define LNET_NRBPOOLS		(LNET_LARGE_BUF_IDX + 1)

struct lnet_ioctl_pool_cfg {
	struct {
		__u32 pl_npages;
		__u32 pl_nbuffers;
		__u32 pl_credits;
		__u32 pl_mincredits;
	} pl_pools[LNET_NRBPOOLS];
	__u32 pl_routing;
};

struct lnet_ioctl_ping_data {
	struct libcfs_ioctl_hdr ping_hdr;

	__u32 op_param;
	__u32 ping_count;
	__u32 ping_flags;
	__u32 mr_info;
	struct lnet_process_id ping_id;
	struct lnet_process_id  *ping_buf;
};

struct lnet_ioctl_config_data {
	struct libcfs_ioctl_hdr cfg_hdr;

	__u32 cfg_net;
	__u32 cfg_count;
	__u64 cfg_nid;
	__u32 cfg_ncpts;

	union {
		struct {
			__u32 rtr_hop;
			__u32 rtr_priority;
			__u32 rtr_flags;
			__u32 rtr_sensitivity;
		} cfg_route;
		struct {
			char net_intf[LNET_MAX_STR_LEN];
			__s32 net_peer_timeout;
			__s32 net_peer_tx_credits;
			__s32 net_peer_rtr_credits;
			__s32 net_max_tx_credits;
			__u32 net_cksum_algo;
			__u32 net_interface_count;
		} cfg_net;
		struct {
			__u32 buf_enable;
			__s32 buf_tiny;
			__s32 buf_small;
			__s32 buf_large;
		} cfg_buffers;
	} cfg_config_u;

	char cfg_bulk[0];
};

struct lnet_ioctl_comm_count {
	__u32 ico_get_count;
	__u32 ico_put_count;
	__u32 ico_reply_count;
	__u32 ico_ack_count;
	__u32 ico_hello_count;
};

struct lnet_ioctl_element_stats {
	__u32 iel_send_count;
	__u32 iel_recv_count;
	__u32 iel_drop_count;
};

enum lnet_health_type {
	LNET_HEALTH_TYPE_LOCAL_NI = 0,
	LNET_HEALTH_TYPE_PEER_NI,
};

struct lnet_ioctl_local_ni_hstats {
	struct libcfs_ioctl_hdr hlni_hdr;
	lnet_nid_t hlni_nid;
	__u32 hlni_local_interrupt;
	__u32 hlni_local_dropped;
	__u32 hlni_local_aborted;
	__u32 hlni_local_no_route;
	__u32 hlni_local_timeout;
	__u32 hlni_local_error;
	__s32 hlni_health_value;
};

struct lnet_ioctl_peer_ni_hstats {
	__u32 hlpni_remote_dropped;
	__u32 hlpni_remote_timeout;
	__u32 hlpni_remote_error;
	__u32 hlpni_network_timeout;
	__s32 hlpni_health_value;
};

struct lnet_ioctl_element_msg_stats {
	struct libcfs_ioctl_hdr im_hdr;
	__u32 im_idx;
	struct lnet_ioctl_comm_count im_send_stats;
	struct lnet_ioctl_comm_count im_recv_stats;
	struct lnet_ioctl_comm_count im_drop_stats;
};

struct lnet_ioctl_config_ni {
	struct libcfs_ioctl_hdr lic_cfg_hdr;
	lnet_nid_t		lic_nid;
	char			lic_ni_intf[LNET_MAX_STR_LEN];
	char			lic_legacy_ip2nets[LNET_MAX_STR_LEN];
	__u32			lic_cpts[LNET_MAX_SHOW_NUM_CPT];
	__u32			lic_ncpts;
	__u32			lic_status;
	__u32			lic_idx;
	__s32			lic_dev_cpt;
	char			pad[4];
	char			lic_bulk[0];
};

struct lnet_peer_ni_credit_info {
	char cr_aliveness[LNET_MAX_STR_LEN];
	__u32 cr_refcount;
	__s32 cr_ni_peer_tx_credits;
	__s32 cr_peer_tx_credits;
	__s32 cr_peer_min_tx_credits;
	__u32 cr_peer_tx_qnob;
	__s32 cr_peer_rtr_credits;
	__s32 cr_peer_min_rtr_credits;
	__u32 cr_ncpt;
};

struct lnet_ioctl_peer {
	struct libcfs_ioctl_hdr pr_hdr;
	__u32 pr_count;
	__u32 pr_pad;
	lnet_nid_t pr_nid;

	union {
		struct lnet_peer_ni_credit_info  pr_peer_credits;
	} pr_lnd_u;
};

struct lnet_ioctl_peer_cfg {
	struct libcfs_ioctl_hdr prcfg_hdr;
	lnet_nid_t prcfg_prim_nid;
	lnet_nid_t prcfg_cfg_nid;
	__u32 prcfg_count;
	__u32 prcfg_mr;
	__u32 prcfg_state;
	__u32 prcfg_size;
	void  *prcfg_bulk;
};

struct lnet_ioctl_reset_health_cfg {
	struct libcfs_ioctl_hdr rh_hdr;
	enum lnet_health_type rh_type:32;
	__u16 rh_all:1;
	__s16 rh_value;
	lnet_nid_t rh_nid;
};

struct lnet_ioctl_recovery_list {
	struct libcfs_ioctl_hdr rlst_hdr;
	enum lnet_health_type rlst_type:32;
	__u32 rlst_num_nids;
	lnet_nid_t rlst_nid_array[LNET_MAX_SHOW_NUM_NID];
};

struct lnet_ioctl_set_value {
	struct libcfs_ioctl_hdr sv_hdr;
	__u32 sv_value;
};

struct lnet_ioctl_lnet_stats {
	struct libcfs_ioctl_hdr st_hdr;
	struct lnet_counters st_cntrs;
};

struct lnet_range_expr {
	__u32 re_lo;
	__u32 re_hi;
	__u32 re_stride;
};

struct lnet_expressions {
	__u32 le_count;
};

struct lnet_ioctl_udsp_net_descr {
	__u32 ud_net_type;
	struct lnet_expressions ud_net_num_expr;
};

struct lnet_ioctl_udsp_descr_hdr {
	/* The literals SRC, DST and RTE are encoded
	 * here.
	 */
	__u32 ud_descr_type;
	__u32 ud_descr_count;
};

struct lnet_ioctl_udsp_descr {
	struct lnet_ioctl_udsp_descr_hdr iud_src_hdr;
	struct lnet_ioctl_udsp_net_descr iud_net;
};

struct lnet_ioctl_udsp {
	struct libcfs_ioctl_hdr iou_hdr;
	__s32 iou_idx;
	__u32 iou_action_type;
	__u32 iou_bulk_size;
	union {
		__u32 priority;
	} iou_action;
	void  *iou_bulk;
};

struct lnet_ioctl_construct_udsp_info {
	struct libcfs_ioctl_hdr cud_hdr;
	__u32 cud_peer:1;
	lnet_nid_t cud_nid;
	__u32 cud_nid_priority;
	__u32 cud_net_priority;
	lnet_nid_t cud_pref_nid[LNET_MAX_SHOW_NUM_NID];
	lnet_nid_t cud_pref_rtr_nid[LNET_MAX_SHOW_NUM_NID];
};

#endif /* _LNET_DLC_H_ */

#ifndef __LIBCFS_UTIL_STRING_H__
#define __LIBCFS_UTIL_STRING_H__

#include <stddef.h>
#include <stdarg.h>

#include <linux/types.h>
#include <linux/lnet/lnet-types.h>
#include <libcfs/util/list.h>


#ifndef __printf
#define __printf(a, b)		__attribute__((__format__(printf, a, b)))
#endif

struct netstrfns {
	__u32	nf_type;
	char	*nf_name;
	char	*nf_modname;
	void	(*nf_addr2str)(__u32 addr, char *str, size_t size);
	int	(*nf_str2addr)(const char *str, int nob, __u32 *addr);
	int	(*nf_parse_addrlist)(char *str, int len,
				     struct list_head *list);
	int	(*nf_print_addrlist)(char *buffer, int count,
				     struct list_head *list);
	int	(*nf_match_addr)(__u32 addr, struct list_head *list);
	int	(*nf_min_max)(struct list_head *nidlist, __u32 *min_nid,
			      __u32 *max_nid);
	int	(*nf_expand_addrrange)(struct list_head *addrranges,
				       __u32 *addrs, int max_addrs);
};

struct cfs_lstr {
	char		*ls_str;
	int		ls_len;
};

struct cfs_range_expr {
	/*
	 * Link to cfs_expr_list::el_exprs.
	 */
	struct list_head	re_link;
	__u32			re_lo;
	__u32			re_hi;
	__u32			re_stride;
};

struct cfs_expr_list {
	struct list_head	el_link;
	struct list_head	el_exprs;
};

int cfs_expr_list_values(struct cfs_expr_list *expr_list, int max, __u32 **valpp);
int cfs_gettok(struct cfs_lstr *next, char delim, struct cfs_lstr *res);
int cfs_str2num_check(char *str, int nob, unsigned *num,
		      unsigned min, unsigned max);
int cfs_expr2str(struct list_head *list, char *str, size_t size);
int cfs_expr_list_match(__u32 value, struct cfs_expr_list *expr_list);
int cfs_expr_list_print(char *buffer, int count,
			struct cfs_expr_list *expr_list);
int cfs_expr_list_parse(char *str, int len, unsigned min, unsigned max,
			struct cfs_expr_list **elpp);
void cfs_expr_list_free(struct cfs_expr_list *expr_list);
void cfs_expr_list_free_list(struct list_head *list);
int cfs_ip_addr_parse(char *str, int len, struct list_head *list);
int cfs_ip_addr_range_gen(__u32 *ip_list, int count,
			  struct list_head *ip_addr_expr);
int cfs_ip_addr_match(__u32 addr, struct list_head *list);
int cfs_expand_nidlist(struct list_head *nidlist, lnet_nid_t *lnet_nidlist,
		       int max_nids);
int cfs_parse_nid_parts(char *str, struct list_head *addr,
			struct list_head *net_num, __u32 *net_type);
int cfs_abs_path(const char *request_path, char **resolved_path);

#endif
#ifndef _LNET_NIDSTRINGS_H
#define _LNET_NIDSTRINGS_H

#include <linux/types.h>
#include <linux/lnet/lnet-types.h>

enum {
	/* Only add to these values (i.e. don't ever change or redefine them):
	 * network addresses depend on them... */
	/*QSWLND	= 1, removed v2_7_50                 */
	SOCKLND		= 2,
	/*GMLND		= 3, removed v2_0_0-rc1a-16-gc660aac */
	/*PTLLND	= 4, removed v2_7_50                 */
	O2IBLND		= 5,
	/*CIBLND	= 6, removed v2_0_0-rc1a-175-gd2b8a0e */
	/*OPENIBLND	= 7, removed v2_0_0-rc1a-175-gd2b8a0e */
	/*IIBLND	= 8, removed v2_0_0-rc1a-175-gd2b8a0e */
	LOLND		= 9,
	/*RALND		= 10, removed v2_7_50_0-34-g8be9e41    */
	/*VIBLND	= 11, removed v2_0_0-rc1a-175-gd2b8a0e */
	/*MXLND		= 12, removed v2_7_50_0-34-g8be9e41    */
	GNILND		= 13,
	GNIIPLND	= 14,
	PTL4LND		= 15,

	NUM_LNDS
};

struct list_head;

#define LNET_NIDSTR_COUNT 1024	/* # of nidstrings */
#define LNET_NIDSTR_SIZE  32	/* size of each one (see below for usage) */

char *libcfs_next_nidstring(void);
int libcfs_isknown_lnd(__u32 lnd);
char *libcfs_lnd2modname(__u32 lnd);
char *libcfs_lnd2str_r(__u32 lnd, char *buf, __kernel_size_t buf_size);
static inline char *libcfs_lnd2str(__u32 lnd)
{
	return libcfs_lnd2str_r(lnd, libcfs_next_nidstring(),
				LNET_NIDSTR_SIZE);
}
int libcfs_str2lnd(const char *str);
char *libcfs_net2str_r(__u32 net, char *buf, __kernel_size_t buf_size);
static inline char *libcfs_net2str(__u32 net)
{
	return libcfs_net2str_r(net, libcfs_next_nidstring(),
				LNET_NIDSTR_SIZE);
}
char *libcfs_nid2str_r(lnet_nid_t nid, char *buf, __kernel_size_t buf_size);
static inline char *libcfs_nid2str(lnet_nid_t nid)
{
	return libcfs_nid2str_r(nid, libcfs_next_nidstring(),
				LNET_NIDSTR_SIZE);
}
__u32 libcfs_str2net(const char *str);
lnet_nid_t libcfs_str2nid(const char *str);
int libcfs_str2anynid(lnet_nid_t *nid, const char *str);
int libcfs_num_parse(char *str, int len, struct list_head *list);
char *libcfs_id2str(struct lnet_process_id id);
void cfs_free_nidlist(struct list_head *list);
int cfs_parse_nidlist(char *str, int len, struct list_head *list);
int cfs_print_nidlist(char *buffer, int count, struct list_head *list);
int cfs_match_nid(lnet_nid_t nid, struct list_head *list);
int cfs_match_nid_net(lnet_nid_t nid, __u32 net, struct list_head *net_num_list,
		      struct list_head *addr);
int cfs_match_net(__u32 net_id, __u32 net_type,
		  struct list_head *net_num_list);

int cfs_ip_addr_parse(char *str, int len, struct list_head *list);
int cfs_ip_addr_match(__u32 addr, struct list_head *list);
int cfs_nidrange_find_min_max(struct list_head *nidlist, char *min_nid,
			       char *max_nid, __kernel_size_t nidstr_length);
void cfs_expr_list_free_list(struct list_head *list);

#endif /* _LNET_NIDSTRINGS_H */
%inline %{
PyObject *lutf_parse_nidlist(char *str, int len, int max_nids) {
	int rc, num_nids, i;
	lnet_nid_t *nidl = calloc(sizeof(*nidl) * max_nids, 1);
	struct list_head *l = calloc(sizeof(*l), 1);
	PyObject *pylist;
	if (!l || !nidl) {
		if (l)
			free(l);
		if (nidl)
			free(nidl);
		return NULL;
	}
	INIT_LIST_HEAD(l);
	rc = cfs_parse_nidlist(str, len, l);
	if (!rc) {
		free(l);
		return NULL;
	}
	num_nids = cfs_expand_nidlist(l, nidl, max_nids);
	cfs_free_nidlist(l);
	pylist = PyList_New(num_nids);
	for (i = 0; i < num_nids; i++) {
		PyList_SetItem(pylist, i, PyLong_FromUnsignedLongLong(nidl[i]));
	}
	free(l);
	free(nidl);
	return pylist;
}
char *lutf_nid2str(unsigned long nid) {
	return libcfs_nid2str(nid);
}
%}

