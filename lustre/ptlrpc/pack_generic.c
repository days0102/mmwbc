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
 *
 * lustre/ptlrpc/pack_generic.c
 *
 * (Un)packing of OST requests
 *
 * Author: Peter J. Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Eric Barton <eeb@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_RPC

#include <linux/crc32.h>

#include <libcfs/libcfs.h>

#include <llog_swab.h>
#include <lustre_net.h>
#include <lustre_swab.h>
#include <obd_cksum.h>
#include <obd_class.h>
#include <obd_support.h>
#include "ptlrpc_internal.h"

static inline __u32 lustre_msg_hdr_size_v2(__u32 count)
{
	return cfs_size_round(offsetof(struct lustre_msg_v2,
				       lm_buflens[count]));
}

__u32 lustre_msg_hdr_size(__u32 magic, __u32 count)
{
	LASSERT(count > 0);

	switch (magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_msg_hdr_size_v2(count);
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", magic);
		return 0;
	}
}

static inline int lustre_msg_check_version_v2(struct lustre_msg_v2 *msg,
					      enum lustre_msg_version version)
{
	enum lustre_msg_version ver = lustre_msg_get_version(msg);

	return (ver & LUSTRE_VERSION_MASK) != version;
}

int lustre_msg_check_version(struct lustre_msg *msg,
			     enum lustre_msg_version version)
{
#define LUSTRE_MSG_MAGIC_V1 0x0BD00BD0
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V1:
		CERROR("msg v1 not supported - please upgrade you system\n");
		return -EINVAL;
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_msg_check_version_v2(msg, version);
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return -EPROTO;
	}
#undef LUSTRE_MSG_MAGIC_V1
}

/* early reply size */
__u32 lustre_msg_early_size()
{
	__u32 pblen = sizeof(struct ptlrpc_body);

	return lustre_msg_size(LUSTRE_MSG_MAGIC_V2, 1, &pblen);
}
EXPORT_SYMBOL(lustre_msg_early_size);

__u32 lustre_msg_size_v2(int count, __u32 *lengths)
{
	__u32 size;
	int i;

	LASSERT(count > 0);
	size = lustre_msg_hdr_size_v2(count);
	for (i = 0; i < count; i++)
		size += cfs_size_round(lengths[i]);

	return size;
}
EXPORT_SYMBOL(lustre_msg_size_v2);

/*
 * This returns the size of the buffer that is required to hold a lustre_msg
 * with the given sub-buffer lengths.
 * NOTE: this should only be used for NEW requests, and should always be
 *       in the form of a v2 request.  If this is a connection to a v1
 *       target then the first buffer will be stripped because the ptlrpc
 *       data is part of the lustre_msg_v1 header. b=14043
 */
__u32 lustre_msg_size(__u32 magic, int count, __u32 *lens)
{
	__u32 size[] = { sizeof(struct ptlrpc_body) };

	if (!lens) {
		LASSERT(count == 1);
		lens = size;
	}

	LASSERT(count > 0);
	LASSERT(lens[MSG_PTLRPC_BODY_OFF] >= sizeof(struct ptlrpc_body_v2));

	switch (magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_msg_size_v2(count, lens);
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", magic);
		return 0;
	}
}

/*
 * This is used to determine the size of a buffer that was already packed
 * and will correctly handle the different message formats.
 */
__u32 lustre_packed_msg_size(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_msg_size_v2(msg->lm_bufcount, msg->lm_buflens);
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_packed_msg_size);

void lustre_init_msg_v2(struct lustre_msg_v2 *msg, int count, __u32 *lens,
			char **bufs)
{
	char *ptr;
	int i;

	LASSERT(count > 0);

	msg->lm_bufcount = count;
	/* XXX: lm_secflvr uninitialized here */
	msg->lm_magic = LUSTRE_MSG_MAGIC_V2;

	for (i = 0; i < count; i++)
		msg->lm_buflens[i] = lens[i];

	if (bufs == NULL)
		return;

	ptr = (char *)msg + lustre_msg_hdr_size_v2(count);
	for (i = 0; i < count; i++) {
		char *tmp = bufs[i];

		if (tmp)
			memcpy(ptr, tmp, lens[i]);
		ptr += cfs_size_round(lens[i]);
	}
}
EXPORT_SYMBOL(lustre_init_msg_v2);

static int lustre_pack_request_v2(struct ptlrpc_request *req,
				  int count, __u32 *lens, char **bufs)
{
	int reqlen, rc;

	reqlen = lustre_msg_size_v2(count, lens);

	rc = sptlrpc_cli_alloc_reqbuf(req, reqlen);
	if (rc)
		return rc;

	req->rq_reqlen = reqlen;

	lustre_init_msg_v2(req->rq_reqmsg, count, lens, bufs);
	lustre_msg_add_version(req->rq_reqmsg, PTLRPC_MSG_VERSION);
	return 0;
}

int lustre_pack_request(struct ptlrpc_request *req, __u32 magic, int count,
			__u32 *lens, char **bufs)
{
	__u32 size[] = { sizeof(struct ptlrpc_body) };

	if (!lens) {
		LASSERT(count == 1);
		lens = size;
	}

	LASSERT(count > 0);
	LASSERT(lens[MSG_PTLRPC_BODY_OFF] == sizeof(struct ptlrpc_body));

	/* only use new format, we don't need to be compatible with 1.4 */
	magic = LUSTRE_MSG_MAGIC_V2;

	switch (magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_pack_request_v2(req, count, lens, bufs);
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", magic);
		return -EINVAL;
	}
}

#if RS_DEBUG
struct list_head ptlrpc_rs_debug_lru =
	LIST_HEAD_INIT(ptlrpc_rs_debug_lru);
spinlock_t ptlrpc_rs_debug_lock;

#define PTLRPC_RS_DEBUG_LRU_ADD(rs)					\
do {									\
	spin_lock(&ptlrpc_rs_debug_lock);				\
	list_add_tail(&(rs)->rs_debug_list, &ptlrpc_rs_debug_lru);	\
	spin_unlock(&ptlrpc_rs_debug_lock);				\
} while (0)

#define PTLRPC_RS_DEBUG_LRU_DEL(rs)					\
do {									\
	spin_lock(&ptlrpc_rs_debug_lock);				\
	list_del(&(rs)->rs_debug_list);				\
	spin_unlock(&ptlrpc_rs_debug_lock);				\
} while (0)
#else
# define PTLRPC_RS_DEBUG_LRU_ADD(rs) do {} while(0)
# define PTLRPC_RS_DEBUG_LRU_DEL(rs) do {} while(0)
#endif

struct ptlrpc_reply_state *
lustre_get_emerg_rs(struct ptlrpc_service_part *svcpt)
{
	struct ptlrpc_reply_state *rs = NULL;

	spin_lock(&svcpt->scp_rep_lock);

	/* See if we have anything in a pool, and wait if nothing */
	while (list_empty(&svcpt->scp_rep_idle)) {
		int			rc;

		spin_unlock(&svcpt->scp_rep_lock);
		/* If we cannot get anything for some long time, we better
		 * bail out instead of waiting infinitely */
		rc = wait_event_idle_timeout(svcpt->scp_rep_waitq,
					     !list_empty(&svcpt->scp_rep_idle),
					     cfs_time_seconds(10));
		if (rc <= 0)
			goto out;
		spin_lock(&svcpt->scp_rep_lock);
	}

	rs = list_entry(svcpt->scp_rep_idle.next,
			    struct ptlrpc_reply_state, rs_list);
	list_del(&rs->rs_list);

	spin_unlock(&svcpt->scp_rep_lock);

	memset(rs, 0, svcpt->scp_service->srv_max_reply_size);
	rs->rs_size = svcpt->scp_service->srv_max_reply_size;
	rs->rs_svcpt = svcpt;
	rs->rs_prealloc = 1;
out:
	return rs;
}

void lustre_put_emerg_rs(struct ptlrpc_reply_state *rs)
{
	struct ptlrpc_service_part *svcpt = rs->rs_svcpt;

	spin_lock(&svcpt->scp_rep_lock);
	list_add(&rs->rs_list, &svcpt->scp_rep_idle);
	spin_unlock(&svcpt->scp_rep_lock);
	wake_up(&svcpt->scp_rep_waitq);
}

int lustre_pack_reply_v2(struct ptlrpc_request *req, int count,
			 __u32 *lens, char **bufs, int flags)
{
	struct ptlrpc_reply_state *rs;
	int msg_len, rc;
	ENTRY;

	LASSERT(req->rq_reply_state == NULL);
	LASSERT(count > 0);

	if ((flags & LPRFL_EARLY_REPLY) == 0) {
		spin_lock(&req->rq_lock);
		req->rq_packed_final = 1;
		spin_unlock(&req->rq_lock);
	}

	msg_len = lustre_msg_size_v2(count, lens);
	rc = sptlrpc_svc_alloc_rs(req, msg_len);
	if (rc)
		RETURN(rc);

	rs = req->rq_reply_state;
	atomic_set(&rs->rs_refcount, 1); /* 1 ref for rq_reply_state */
	rs->rs_cb_id.cbid_fn = reply_out_callback;
	rs->rs_cb_id.cbid_arg = rs;
	rs->rs_svcpt = req->rq_rqbd->rqbd_svcpt;
	INIT_LIST_HEAD(&rs->rs_exp_list);
	INIT_LIST_HEAD(&rs->rs_obd_list);
	INIT_LIST_HEAD(&rs->rs_list);
	spin_lock_init(&rs->rs_lock);

	req->rq_replen = msg_len;
	req->rq_reply_state = rs;
	req->rq_repmsg = rs->rs_msg;

	lustre_init_msg_v2(rs->rs_msg, count, lens, bufs);
	lustre_msg_add_version(rs->rs_msg, PTLRPC_MSG_VERSION);

	PTLRPC_RS_DEBUG_LRU_ADD(rs);

	RETURN(0);
}
EXPORT_SYMBOL(lustre_pack_reply_v2);

int lustre_pack_reply_flags(struct ptlrpc_request *req, int count, __u32 *lens,
			    char **bufs, int flags)
{
	int rc = 0;
	__u32 size[] = { sizeof(struct ptlrpc_body) };

	if (!lens) {
		LASSERT(count == 1);
		lens = size;
	}

	LASSERT(count > 0);
	LASSERT(lens[MSG_PTLRPC_BODY_OFF] == sizeof(struct ptlrpc_body));

	switch (req->rq_reqmsg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		rc = lustre_pack_reply_v2(req, count, lens, bufs, flags);
		break;
	default:
		LASSERTF(0, "incorrect message magic: %08x\n",
			 req->rq_reqmsg->lm_magic);
		rc = -EINVAL;
	}
	if (rc != 0)
		CERROR("lustre_pack_reply failed: rc=%d size=%d\n", rc,
		       lustre_msg_size(req->rq_reqmsg->lm_magic, count, lens));
	return rc;
}

int lustre_pack_reply(struct ptlrpc_request *req, int count, __u32 *lens,
		      char **bufs)
{
	return lustre_pack_reply_flags(req, count, lens, bufs, 0);
}
EXPORT_SYMBOL(lustre_pack_reply);

void *lustre_msg_buf_v2(struct lustre_msg_v2 *m, __u32 n, __u32 min_size)
{
	__u32 i, offset, buflen, bufcount;

	LASSERT(m != NULL);
	LASSERT(m->lm_bufcount > 0);

	bufcount = m->lm_bufcount;
	if (unlikely(n >= bufcount)) {
		CDEBUG(D_INFO, "msg %p buffer[%d] not present (count %d)\n",
		       m, n, bufcount);
		return NULL;
	}

	buflen = m->lm_buflens[n];
	if (unlikely(buflen < min_size)) {
		CERROR("msg %p buffer[%d] size %d too small "
		       "(required %d, opc=%d)\n", m, n, buflen, min_size,
		       n == MSG_PTLRPC_BODY_OFF ? -1 : lustre_msg_get_opc(m));
		return NULL;
	}

	offset = lustre_msg_hdr_size_v2(bufcount);
	for (i = 0; i < n; i++)
		offset += cfs_size_round(m->lm_buflens[i]);

	return (char *)m + offset;
}

void *lustre_msg_buf(struct lustre_msg *m, __u32 n, __u32 min_size)
{
	switch (m->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_msg_buf_v2(m, n, min_size);
	default:
		LASSERTF(0, "incorrect message magic: %08x (msg:%p)\n",
			 m->lm_magic, m);
		return NULL;
	}
}
EXPORT_SYMBOL(lustre_msg_buf);

static int lustre_shrink_msg_v2(struct lustre_msg_v2 *msg, __u32 segment,
				unsigned int newlen, int move_data)
{
	char *tail = NULL, *newpos;
	int tail_len = 0, n;

	LASSERT(msg);
	LASSERT(msg->lm_bufcount > segment);
	LASSERT(msg->lm_buflens[segment] >= newlen);

	if (msg->lm_buflens[segment] == newlen)
		goto out;

	if (move_data && msg->lm_bufcount > segment + 1) {
		tail = lustre_msg_buf_v2(msg, segment + 1, 0);
		for (n = segment + 1; n < msg->lm_bufcount; n++)
			tail_len += cfs_size_round(msg->lm_buflens[n]);
	}

	msg->lm_buflens[segment] = newlen;

	if (tail && tail_len) {
		newpos = lustre_msg_buf_v2(msg, segment + 1, 0);
		LASSERT(newpos <= tail);
		if (newpos != tail)
			memmove(newpos, tail, tail_len);
	}
out:
	return lustre_msg_size_v2(msg->lm_bufcount, msg->lm_buflens);
}

/*
 * for @msg, shrink @segment to size @newlen. if @move_data is non-zero,
 * we also move data forward from @segment + 1.
 *
 * if @newlen == 0, we remove the segment completely, but we still keep the
 * totally bufcount the same to save possible data moving. this will leave a
 * unused segment with size 0 at the tail, but that's ok.
 *
 * return new msg size after shrinking.
 *
 * CAUTION:
 * + if any buffers higher than @segment has been filled in, must call shrink
 *   with non-zero @move_data.
 * + caller should NOT keep pointers to msg buffers which higher than @segment
 *   after call shrink.
 */
int lustre_shrink_msg(struct lustre_msg *msg, int segment,
		      unsigned int newlen, int move_data)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_shrink_msg_v2(msg, segment, newlen, move_data);
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_shrink_msg);

static int lustre_grow_msg_v2(struct lustre_msg_v2 *msg, __u32 segment,
			      unsigned int newlen)
{
	char *tail = NULL, *newpos;
	int tail_len = 0, n;

	LASSERT(msg);
	LASSERT(msg->lm_bufcount > segment);
	LASSERT(msg->lm_buflens[segment] <= newlen);

	if (msg->lm_buflens[segment] == newlen)
		goto out;

	if (msg->lm_bufcount > segment + 1) {
		tail = lustre_msg_buf_v2(msg, segment + 1, 0);
		for (n = segment + 1; n < msg->lm_bufcount; n++)
			tail_len += cfs_size_round(msg->lm_buflens[n]);
	}

	msg->lm_buflens[segment] = newlen;

	if (tail && tail_len) {
		newpos = lustre_msg_buf_v2(msg, segment + 1, 0);
		memmove(newpos, tail, tail_len);
	}
out:
	return lustre_msg_size_v2(msg->lm_bufcount, msg->lm_buflens);
}

/*
 * for @msg, grow @segment to size @newlen.
 * Always move higher buffer forward.
 *
 * return new msg size after growing.
 *
 * CAUTION:
 * - caller must make sure there is enough space in allocated message buffer
 * - caller should NOT keep pointers to msg buffers which higher than @segment
 *   after call shrink.
 */
int lustre_grow_msg(struct lustre_msg *msg, int segment, unsigned int newlen)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_grow_msg_v2(msg, segment, newlen);
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_grow_msg);

void lustre_free_reply_state(struct ptlrpc_reply_state *rs)
{
	PTLRPC_RS_DEBUG_LRU_DEL(rs);

	LASSERT(atomic_read(&rs->rs_refcount) == 0);
	LASSERT(!rs->rs_difficult || rs->rs_handled);
	LASSERT(!rs->rs_on_net);
	LASSERT(!rs->rs_scheduled);
	LASSERT(rs->rs_export == NULL);
	LASSERT(rs->rs_nlocks == 0);
	LASSERT(list_empty(&rs->rs_exp_list));
	LASSERT(list_empty(&rs->rs_obd_list));

	sptlrpc_svc_free_rs(rs);
}

static int lustre_unpack_msg_v2(struct lustre_msg_v2 *m, int len)
{
	int swabbed, required_len, i, buflen;

	/* Now we know the sender speaks my language. */
	required_len = lustre_msg_hdr_size_v2(0);
	if (len < required_len) {
		/* can't even look inside the message */
		CERROR("message length %d too small for lustre_msg\n", len);
		return -EINVAL;
	}

	swabbed = (m->lm_magic == LUSTRE_MSG_MAGIC_V2_SWABBED);

	if (swabbed) {
		__swab32s(&m->lm_magic);
		__swab32s(&m->lm_bufcount);
		__swab32s(&m->lm_secflvr);
		__swab32s(&m->lm_repsize);
		__swab32s(&m->lm_cksum);
		__swab32s(&m->lm_flags);
		__swab32s(&m->lm_opc);
		BUILD_BUG_ON(offsetof(typeof(*m), lm_padding_3) == 0);
	}

	if (m->lm_bufcount == 0 || m->lm_bufcount > PTLRPC_MAX_BUFCOUNT) {
		CERROR("message bufcount %d is not valid\n", m->lm_bufcount);
		return -EINVAL;
	}
	required_len = lustre_msg_hdr_size_v2(m->lm_bufcount);
	if (len < required_len) {
		/* didn't receive all the buffer lengths */
		CERROR("message length %d too small for %d buflens\n",
		       len, m->lm_bufcount);
		return -EINVAL;
	}

	for (i = 0; i < m->lm_bufcount; i++) {
		if (swabbed)
			__swab32s(&m->lm_buflens[i]);
		buflen = cfs_size_round(m->lm_buflens[i]);
		if (buflen < 0 || buflen > PTLRPC_MAX_BUFLEN) {
			CERROR("buffer %d length %d is not valid\n", i, buflen);
			return -EINVAL;
		}
		required_len += buflen;
	}
	if (len < required_len || required_len > PTLRPC_MAX_BUFLEN) {
		CERROR("len: %d, required_len %d, bufcount: %d\n",
		       len, required_len, m->lm_bufcount);
		for (i = 0; i < m->lm_bufcount; i++)
			CERROR("buffer %d length %d\n", i, m->lm_buflens[i]);
		return -EINVAL;
	}

	return swabbed;
}

int __lustre_unpack_msg(struct lustre_msg *m, int len)
{
	int required_len, rc;

	ENTRY;
	/*
	 * We can provide a slightly better error log, if we check the
	 * message magic and version first.  In the future, struct
	 * lustre_msg may grow, and we'd like to log a version mismatch,
	 * rather than a short message.
	 */
	required_len = offsetof(struct lustre_msg, lm_magic) +
				sizeof(m->lm_magic);
	if (len < required_len) {
		/* can't even look inside the message */
		CERROR("message length %d too small for magic/version check\n",
		       len);
		RETURN(-EINVAL);
	}

	rc = lustre_unpack_msg_v2(m, len);

	RETURN(rc);
}
EXPORT_SYMBOL(__lustre_unpack_msg);

int ptlrpc_unpack_req_msg(struct ptlrpc_request *req, int len)
{
	int rc;

	rc = __lustre_unpack_msg(req->rq_reqmsg, len);
	if (rc == 1) {
		req_capsule_set_req_swabbed(&req->rq_pill,
					    MSG_PTLRPC_HEADER_OFF);
		rc = 0;
	}
	return rc;
}

int ptlrpc_unpack_rep_msg(struct ptlrpc_request *req, int len)
{
	int rc;

	rc = __lustre_unpack_msg(req->rq_repmsg, len);
	if (rc == 1) {
		req_capsule_set_rep_swabbed(&req->rq_pill,
					    MSG_PTLRPC_HEADER_OFF);
		rc = 0;
	}
	return rc;
}

static inline int
lustre_unpack_ptlrpc_body_v2(struct ptlrpc_request *req,
			     enum req_location loc, int offset)
{
	struct ptlrpc_body *pb;
	struct lustre_msg_v2 *m;

	m = loc == RCL_CLIENT ? req->rq_reqmsg : req->rq_repmsg;

	pb = lustre_msg_buf_v2(m, offset, sizeof(struct ptlrpc_body_v2));
	if (!pb) {
		CERROR("error unpacking ptlrpc body\n");
		return -EFAULT;
	}
	if (req_capsule_need_swab(&req->rq_pill, loc, offset)) {
		lustre_swab_ptlrpc_body(pb);
		req_capsule_set_swabbed(&req->rq_pill, loc, offset);
	}

	if ((pb->pb_version & ~LUSTRE_VERSION_MASK) != PTLRPC_MSG_VERSION) {
		CERROR("wrong lustre_msg version %08x\n", pb->pb_version);
		return -EINVAL;
	}

	if (loc == RCL_SERVER)
		pb->pb_status = ptlrpc_status_ntoh(pb->pb_status);

	return 0;
}

int lustre_unpack_req_ptlrpc_body(struct ptlrpc_request *req, int offset)
{
	switch (req->rq_reqmsg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_unpack_ptlrpc_body_v2(req, RCL_CLIENT, offset);
	default:
		CERROR("bad lustre msg magic: %08x\n",
		       req->rq_reqmsg->lm_magic);
		return -EINVAL;
	}
}

int lustre_unpack_rep_ptlrpc_body(struct ptlrpc_request *req, int offset)
{
	switch (req->rq_repmsg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_unpack_ptlrpc_body_v2(req, RCL_SERVER, offset);
	default:
		CERROR("bad lustre msg magic: %08x\n",
		       req->rq_repmsg->lm_magic);
		return -EINVAL;
	}
}

static inline __u32 lustre_msg_buflen_v2(struct lustre_msg_v2 *m, __u32 n)
{
	if (n >= m->lm_bufcount)
		return 0;

	return m->lm_buflens[n];
}

/**
 * lustre_msg_buflen - return the length of buffer \a n in message \a m
 * \param m lustre_msg (request or reply) to look at
 * \param n message index (base 0)
 *
 * returns zero for non-existent message indices
 */
__u32 lustre_msg_buflen(struct lustre_msg *m, __u32 n)
{
	switch (m->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return lustre_msg_buflen_v2(m, n);
	default:
		CERROR("incorrect message magic: %08x\n", m->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_buflen);

static inline void
lustre_msg_set_buflen_v2(struct lustre_msg_v2 *m, __u32 n, __u32 len)
{
	if (n >= m->lm_bufcount)
		LBUG();

	m->lm_buflens[n] = len;
}

void lustre_msg_set_buflen(struct lustre_msg *m, __u32 n, __u32 len)
{
	switch (m->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		lustre_msg_set_buflen_v2(m, n, len);
		return;
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", m->lm_magic);
	}
}

/*
 * NB return the bufcount for lustre_msg_v2 format, so if message is packed
 * in V1 format, the result is one bigger. (add struct ptlrpc_body).
 */
__u32 lustre_msg_bufcount(struct lustre_msg *m)
{
	switch (m->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return m->lm_bufcount;
	default:
		CERROR("incorrect message magic: %08x\n", m->lm_magic);
		return 0;
	}
}

char *lustre_msg_string(struct lustre_msg *m, __u32 index, __u32 max_len)
{
	/* max_len == 0 means the string should fill the buffer */
	char *str;
	__u32 slen, blen;

	switch (m->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		str = lustre_msg_buf_v2(m, index, 0);
		blen = lustre_msg_buflen_v2(m, index);
		break;
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", m->lm_magic);
	}

	if (str == NULL) {
		CERROR("can't unpack string in msg %p buffer[%d]\n", m, index);
		return NULL;
	}

	slen = strnlen(str, blen);

	if (slen == blen) { /* not NULL terminated */
		CERROR("can't unpack non-NULL terminated string in msg %p buffer[%d] len %d\n",
		       m, index, blen);
		return NULL;
	}
	if (blen > PTLRPC_MAX_BUFLEN) {
		CERROR("buffer length of msg %p buffer[%d] is invalid(%d)\n",
		       m, index, blen);
		return NULL;
	}

	if (max_len == 0) {
		if (slen != blen - 1) {
			CERROR("can't unpack short string in msg %p buffer[%d] len %d: strlen %d\n",
			       m, index, blen, slen);
			return NULL;
		}
	} else if (slen > max_len) {
		CERROR("can't unpack oversized string in msg %p buffer[%d] len %d strlen %d: max %d expected\n",
		       m, index, blen, slen, max_len);
		return NULL;
	}

	return str;
}

/* Wrap up the normal fixed length cases */
static inline void *__lustre_swab_buf(struct lustre_msg *msg, __u32 index,
				      __u32 min_size, void *swabber)
{
	void *ptr = NULL;

	LASSERT(msg != NULL);
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		ptr = lustre_msg_buf_v2(msg, index, min_size);
		break;
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
	}

	if (ptr != NULL && swabber != NULL)
		((void (*)(void *))swabber)(ptr);

	return ptr;
}

static inline struct ptlrpc_body *lustre_msg_ptlrpc_body(struct lustre_msg *msg)
{
	return lustre_msg_buf_v2(msg, MSG_PTLRPC_BODY_OFF,
				 sizeof(struct ptlrpc_body_v2));
}

enum lustre_msghdr lustre_msghdr_get_flags(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		/* already in host endian */
		return msg->lm_flags;
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msghdr_get_flags);

void lustre_msghdr_set_flags(struct lustre_msg *msg, __u32 flags)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		msg->lm_flags = flags;
		return;
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

__u32 lustre_msg_get_flags(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb != NULL)
			return pb->pb_flags;

		CERROR("invalid msg %p: no ptlrpc body!\n", msg);
	}
	/* fallthrough */
	default:
		/*
		 * flags might be printed in debug code while message
		 * uninitialized
		 */
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_get_flags);

void lustre_msg_add_flags(struct lustre_msg *msg, __u32 flags)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_flags |= flags;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_add_flags);

void lustre_msg_set_flags(struct lustre_msg *msg, __u32 flags)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_flags = flags;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_clear_flags(struct lustre_msg *msg, __u32 flags)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_flags &= ~flags;

		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_clear_flags);

__u32 lustre_msg_get_op_flags(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb != NULL)
			return pb->pb_op_flags;

		CERROR("invalid msg %p: no ptlrpc body!\n", msg);
	}
	/* fallthrough */
	default:
		return 0;
	}
}

void lustre_msg_add_op_flags(struct lustre_msg *msg, __u32 flags)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_op_flags |= flags;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_add_op_flags);

struct lustre_handle *lustre_msg_get_handle(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return NULL;
		}
		return &pb->pb_handle;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return NULL;
	}
}

__u32 lustre_msg_get_type(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return PTL_RPC_MSG_ERR;
		}
		return pb->pb_type;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return PTL_RPC_MSG_ERR;
	}
}
EXPORT_SYMBOL(lustre_msg_get_type);

enum lustre_msg_version lustre_msg_get_version(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_version;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}

void lustre_msg_add_version(struct lustre_msg *msg, __u32 version)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_version |= version;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

__u32 lustre_msg_get_opc(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_opc;
	}
	default:
		CERROR("incorrect message magic: %08x (msg:%p)\n",
		       msg->lm_magic, msg);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_get_opc);

__u64 lustre_msg_get_last_xid(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_last_xid;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_get_last_xid);

__u16 lustre_msg_get_tag(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (!pb) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_tag;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_get_tag);

__u64 lustre_msg_get_last_committed(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_last_committed;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_get_last_committed);

__u64 *lustre_msg_get_versions(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return NULL;
		}
		return pb->pb_pre_versions;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return NULL;
	}
}
EXPORT_SYMBOL(lustre_msg_get_versions);

__u64 lustre_msg_get_transno(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_transno;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_get_transno);

int lustre_msg_get_status(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb != NULL)
			return pb->pb_status;
		CERROR("invalid msg %p: no ptlrpc body!\n", msg);
	}
	/* fallthrough */
	default:
		/*
		 * status might be printed in debug code while message
		 * uninitialized
		 */
		return -EINVAL;
	}
}
EXPORT_SYMBOL(lustre_msg_get_status);

__u64 lustre_msg_get_slv(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return -EINVAL;
		}
		return pb->pb_slv;
	}
	default:
		CERROR("invalid msg magic %08x\n", msg->lm_magic);
		return -EINVAL;
	}
}


void lustre_msg_set_slv(struct lustre_msg *msg, __u64 slv)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return;
		}
		pb->pb_slv = slv;
		return;
	}
	default:
		CERROR("invalid msg magic %x\n", msg->lm_magic);
		return;
	}
}

__u32 lustre_msg_get_limit(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return -EINVAL;
		}
		return pb->pb_limit;
	}
	default:
		CERROR("invalid msg magic %x\n", msg->lm_magic);
		return -EINVAL;
	}
}


void lustre_msg_set_limit(struct lustre_msg *msg, __u64 limit)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return;
		}
		pb->pb_limit = limit;
		return;
	}
	default:
		CERROR("invalid msg magic %08x\n", msg->lm_magic);
		return;
	}
}

__u32 lustre_msg_get_conn_cnt(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_conn_cnt;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}
EXPORT_SYMBOL(lustre_msg_get_conn_cnt);

__u32 lustre_msg_get_magic(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return msg->lm_magic;
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}

timeout_t lustre_msg_get_timeout(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);

		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_timeout;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}

timeout_t lustre_msg_get_service_timeout(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);

		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_service_time;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}

char *lustre_msg_get_jobid(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb;

		/* the old pltrpc_body_v2 is smaller; doesn't include jobid */
		if (msg->lm_buflens[MSG_PTLRPC_BODY_OFF] <
		    sizeof(struct ptlrpc_body))
			return NULL;

		pb = lustre_msg_buf_v2(msg, MSG_PTLRPC_BODY_OFF,
					  sizeof(struct ptlrpc_body));
		if (!pb)
			return NULL;

		return pb->pb_jobid;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return NULL;
	}
}
EXPORT_SYMBOL(lustre_msg_get_jobid);

__u32 lustre_msg_get_cksum(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return msg->lm_cksum;
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}

__u64 lustre_msg_get_mbits(struct lustre_msg *msg)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		if (pb == NULL) {
			CERROR("invalid msg %p: no ptlrpc body!\n", msg);
			return 0;
		}
		return pb->pb_mbits;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}

__u32 lustre_msg_calc_cksum(struct lustre_msg *msg, __u32 buf)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_buf_v2(msg, buf, 0);
		__u32 len = lustre_msg_buflen(msg, buf);
		__u32 crc;

#if IS_ENABLED(CONFIG_CRC32)
		/* about 10x faster than crypto_hash for small buffers */
		crc = crc32_le(~(__u32)0, (unsigned char *)pb, len);
#elif IS_ENABLED(CONFIG_CRYPTO_CRC32)
		unsigned int hsize = 4;

		cfs_crypto_hash_digest(CFS_HASH_ALG_CRC32, (unsigned char *)pb,
				       len, NULL, 0, (unsigned char *)&crc,
				       &hsize);
#else
#error "need either CONFIG_CRC32 or CONFIG_CRYPTO_CRC32 enabled in the kernel"
#endif
		return crc;
	}
	default:
		CERROR("incorrect message magic: %08x\n", msg->lm_magic);
		return 0;
	}
}

void lustre_msg_set_handle(struct lustre_msg *msg, struct lustre_handle *handle)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_handle = *handle;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_type(struct lustre_msg *msg, __u32 type)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_type = type;
		return;
		}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_opc(struct lustre_msg *msg, __u32 opc)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_opc = opc;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_last_xid(struct lustre_msg *msg, __u64 last_xid)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_last_xid = last_xid;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_set_last_xid);

void lustre_msg_set_tag(struct lustre_msg *msg, __u16 tag)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_tag = tag;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_set_tag);

void lustre_msg_set_last_committed(struct lustre_msg *msg, __u64 last_committed)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_last_committed = last_committed;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_versions(struct lustre_msg *msg, __u64 *versions)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_pre_versions[0] = versions[0];
		pb->pb_pre_versions[1] = versions[1];
		pb->pb_pre_versions[2] = versions[2];
		pb->pb_pre_versions[3] = versions[3];
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_set_versions);

void lustre_msg_set_transno(struct lustre_msg *msg, __u64 transno)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_transno = transno;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_set_transno);

void lustre_msg_set_status(struct lustre_msg *msg, __u32 status)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_status = status;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_set_status);

void lustre_msg_set_conn_cnt(struct lustre_msg *msg, __u32 conn_cnt)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_conn_cnt = conn_cnt;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_timeout(struct lustre_msg *msg, timeout_t timeout)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);

		LASSERT(timeout >= 0);
		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_timeout = timeout;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_service_timeout(struct lustre_msg *msg,
				    timeout_t service_timeout)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);

		LASSERT(service_timeout >= 0);
		LASSERTF(pb, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_service_time = service_timeout;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_jobid(struct lustre_msg *msg, char *jobid)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		__u32 opc = lustre_msg_get_opc(msg);
		struct ptlrpc_body *pb;

		/* Don't set jobid for ldlm ast RPCs, they've been shrinked.
		 * See the comment in ptlrpc_request_pack(). */
		if (!opc || opc == LDLM_BL_CALLBACK ||
		    opc == LDLM_CP_CALLBACK || opc == LDLM_GL_CALLBACK)
			return;

		pb = lustre_msg_buf_v2(msg, MSG_PTLRPC_BODY_OFF,
				       sizeof(struct ptlrpc_body));
		LASSERTF(pb, "invalid msg %p: no ptlrpc body!\n", msg);

		if (jobid != NULL)
			memcpy(pb->pb_jobid, jobid, sizeof(pb->pb_jobid));
		else if (pb->pb_jobid[0] == '\0')
			lustre_get_jobid(pb->pb_jobid, sizeof(pb->pb_jobid));
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}
EXPORT_SYMBOL(lustre_msg_set_jobid);

void lustre_msg_set_cksum(struct lustre_msg *msg, __u32 cksum)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		msg->lm_cksum = cksum;
		return;
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void lustre_msg_set_mbits(struct lustre_msg *msg, __u64 mbits)
{
	switch (msg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2: {
		struct ptlrpc_body *pb = lustre_msg_ptlrpc_body(msg);

		LASSERTF(pb != NULL, "invalid msg %p: no ptlrpc body!\n", msg);
		pb->pb_mbits = mbits;
		return;
	}
	default:
		LASSERTF(0, "incorrect message magic: %08x\n", msg->lm_magic);
	}
}

void ptlrpc_request_set_replen(struct ptlrpc_request *req)
{
	int count = req_capsule_filled_sizes(&req->rq_pill, RCL_SERVER);

	req->rq_replen = lustre_msg_size(req->rq_reqmsg->lm_magic, count,
					 req->rq_pill.rc_area[RCL_SERVER]);
	if (req->rq_reqmsg->lm_magic == LUSTRE_MSG_MAGIC_V2)
		req->rq_reqmsg->lm_repsize = req->rq_replen;
}
EXPORT_SYMBOL(ptlrpc_request_set_replen);

void ptlrpc_req_set_repsize(struct ptlrpc_request *req, int count, __u32 *lens)
{
	req->rq_replen = lustre_msg_size(req->rq_reqmsg->lm_magic, count, lens);
	if (req->rq_reqmsg->lm_magic == LUSTRE_MSG_MAGIC_V2)
		req->rq_reqmsg->lm_repsize = req->rq_replen;
}

/**
 * Send a remote set_info_async.
 *
 * This may go from client to server or server to client.
 */
int do_set_info_async(struct obd_import *imp,
		      int opcode, int version,
		      size_t keylen, void *key,
		      size_t vallen, void *val,
		      struct ptlrpc_request_set *set)
{
	struct ptlrpc_request *req;
	char *tmp;
	int rc;

	ENTRY;

	req = ptlrpc_request_alloc(imp, KEY_IS(KEY_CHANGELOG_CLEAR) ?
						&RQF_MDT_SET_INFO :
						&RQF_OBD_SET_INFO);
	if (req == NULL)
		RETURN(-ENOMEM);

	req_capsule_set_size(&req->rq_pill, &RMF_SETINFO_KEY,
			     RCL_CLIENT, keylen);
	req_capsule_set_size(&req->rq_pill, &RMF_SETINFO_VAL,
			     RCL_CLIENT, vallen);
	rc = ptlrpc_request_pack(req, version, opcode);
	if (rc) {
		ptlrpc_request_free(req);
		RETURN(rc);
	}

	if (KEY_IS(KEY_CHANGELOG_CLEAR))
		do_pack_body(req);

	tmp = req_capsule_client_get(&req->rq_pill, &RMF_SETINFO_KEY);
	memcpy(tmp, key, keylen);
	tmp = req_capsule_client_get(&req->rq_pill, &RMF_SETINFO_VAL);
	memcpy(tmp, val, vallen);

	ptlrpc_request_set_replen(req);

	if (set) {
		ptlrpc_set_add_req(set, req);
		ptlrpc_check_set(NULL, set);
	} else {
		rc = ptlrpc_queue_wait(req);
		ptlrpc_req_finished(req);
	}

	RETURN(rc);
}
EXPORT_SYMBOL(do_set_info_async);

/* byte flipping routines for all wire types declared in
 * lustre_idl.h implemented here.
 */
void lustre_swab_ptlrpc_body(struct ptlrpc_body *body)
{
	__swab32s(&body->pb_type);
	__swab32s(&body->pb_version);
	__swab32s(&body->pb_opc);
	__swab32s(&body->pb_status);
	__swab64s(&body->pb_last_xid);
	__swab16s(&body->pb_tag);
	BUILD_BUG_ON(offsetof(typeof(*body), pb_padding0) == 0);
	BUILD_BUG_ON(offsetof(typeof(*body), pb_padding1) == 0);
	__swab64s(&body->pb_last_committed);
	__swab64s(&body->pb_transno);
	__swab32s(&body->pb_flags);
	__swab32s(&body->pb_op_flags);
	__swab32s(&body->pb_conn_cnt);
	__swab32s(&body->pb_timeout);
	__swab32s(&body->pb_service_time);
	__swab32s(&body->pb_limit);
	__swab64s(&body->pb_slv);
	__swab64s(&body->pb_pre_versions[0]);
	__swab64s(&body->pb_pre_versions[1]);
	__swab64s(&body->pb_pre_versions[2]);
	__swab64s(&body->pb_pre_versions[3]);
	__swab64s(&body->pb_mbits);
	BUILD_BUG_ON(offsetof(typeof(*body), pb_padding64_0) == 0);
	BUILD_BUG_ON(offsetof(typeof(*body), pb_padding64_1) == 0);
	BUILD_BUG_ON(offsetof(typeof(*body), pb_padding64_2) == 0);
	/*
	 * While we need to maintain compatibility between
	 * clients and servers without ptlrpc_body_v2 (< 2.3)
	 * do not swab any fields beyond pb_jobid, as we are
	 * using this swab function for both ptlrpc_body
	 * and ptlrpc_body_v2.
	 */
	/* pb_jobid is an ASCII string and should not be swabbed */
	BUILD_BUG_ON(offsetof(typeof(*body), pb_jobid) == 0);
}

void lustre_swab_connect(struct obd_connect_data *ocd)
{
	__swab64s(&ocd->ocd_connect_flags);
	__swab32s(&ocd->ocd_version);
	__swab32s(&ocd->ocd_grant);
	__swab64s(&ocd->ocd_ibits_known);
	__swab32s(&ocd->ocd_index);
	__swab32s(&ocd->ocd_brw_size);
	/*
	 * ocd_blocksize and ocd_inodespace don't need to be swabbed because
	 * they are 8-byte values
	 */
	__swab16s(&ocd->ocd_grant_tax_kb);
	__swab32s(&ocd->ocd_grant_max_blks);
	__swab64s(&ocd->ocd_transno);
	__swab32s(&ocd->ocd_group);
	__swab32s(&ocd->ocd_cksum_types);
	__swab32s(&ocd->ocd_instance);
	/*
	 * Fields after ocd_cksum_types are only accessible by the receiver
	 * if the corresponding flag in ocd_connect_flags is set. Accessing
	 * any field after ocd_maxbytes on the receiver without a valid flag
	 * may result in out-of-bound memory access and kernel oops.
	 */
	if (ocd->ocd_connect_flags & OBD_CONNECT_MAX_EASIZE)
		__swab32s(&ocd->ocd_max_easize);
	if (ocd->ocd_connect_flags & OBD_CONNECT_MAXBYTES)
		__swab64s(&ocd->ocd_maxbytes);
	if (ocd->ocd_connect_flags & OBD_CONNECT_MULTIMODRPCS)
		__swab16s(&ocd->ocd_maxmodrpcs);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding0) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding1) == 0);
	if (ocd->ocd_connect_flags & OBD_CONNECT_FLAGS2)
		__swab64s(&ocd->ocd_connect_flags2);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding3) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding4) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding5) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding6) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding7) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding8) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), padding9) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), paddingA) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), paddingB) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), paddingC) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), paddingD) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), paddingE) == 0);
	BUILD_BUG_ON(offsetof(typeof(*ocd), paddingF) == 0);
}

static void lustre_swab_ost_layout(struct ost_layout *ol)
{
	__swab32s(&ol->ol_stripe_size);
	__swab32s(&ol->ol_stripe_count);
	__swab64s(&ol->ol_comp_start);
	__swab64s(&ol->ol_comp_end);
	__swab32s(&ol->ol_comp_id);
}

void lustre_swab_obdo(struct obdo *o)
{
	__swab64s(&o->o_valid);
	lustre_swab_ost_id(&o->o_oi);
	__swab64s(&o->o_parent_seq);
	__swab64s(&o->o_size);
	__swab64s(&o->o_mtime);
	__swab64s(&o->o_atime);
	__swab64s(&o->o_ctime);
	__swab64s(&o->o_blocks);
	__swab64s(&o->o_grant);
	__swab32s(&o->o_blksize);
	__swab32s(&o->o_mode);
	__swab32s(&o->o_uid);
	__swab32s(&o->o_gid);
	__swab32s(&o->o_flags);
	__swab32s(&o->o_nlink);
	__swab32s(&o->o_parent_oid);
	__swab32s(&o->o_misc);
	__swab64s(&o->o_ioepoch);
	__swab32s(&o->o_stripe_idx);
	__swab32s(&o->o_parent_ver);
	lustre_swab_ost_layout(&o->o_layout);
	__swab32s(&o->o_layout_version);
	__swab32s(&o->o_uid_h);
	__swab32s(&o->o_gid_h);
	__swab64s(&o->o_data_version);
	__swab32s(&o->o_projid);
	BUILD_BUG_ON(offsetof(typeof(*o), o_padding_4) == 0);
	BUILD_BUG_ON(offsetof(typeof(*o), o_padding_5) == 0);
	BUILD_BUG_ON(offsetof(typeof(*o), o_padding_6) == 0);

}
EXPORT_SYMBOL(lustre_swab_obdo);

void lustre_swab_obd_statfs(struct obd_statfs *os)
{
	__swab64s(&os->os_type);
	__swab64s(&os->os_blocks);
	__swab64s(&os->os_bfree);
	__swab64s(&os->os_bavail);
	__swab64s(&os->os_files);
	__swab64s(&os->os_ffree);
	/* no need to swab os_fsid */
	__swab32s(&os->os_bsize);
	__swab32s(&os->os_namelen);
	__swab64s(&os->os_maxbytes);
	__swab32s(&os->os_state);
	__swab32s(&os->os_fprecreated);
	__swab32s(&os->os_granted);
	BUILD_BUG_ON(offsetof(typeof(*os), os_spare3) == 0);
	BUILD_BUG_ON(offsetof(typeof(*os), os_spare4) == 0);
	BUILD_BUG_ON(offsetof(typeof(*os), os_spare5) == 0);
	BUILD_BUG_ON(offsetof(typeof(*os), os_spare6) == 0);
	BUILD_BUG_ON(offsetof(typeof(*os), os_spare7) == 0);
	BUILD_BUG_ON(offsetof(typeof(*os), os_spare8) == 0);
	BUILD_BUG_ON(offsetof(typeof(*os), os_spare9) == 0);
}

void lustre_swab_obd_ioobj(struct obd_ioobj *ioo)
{
	lustre_swab_ost_id(&ioo->ioo_oid);
	__swab32s(&ioo->ioo_max_brw);
	__swab32s(&ioo->ioo_bufcnt);
}

void lustre_swab_niobuf_remote(struct niobuf_remote *nbr)
{
	__swab64s(&nbr->rnb_offset);
	__swab32s(&nbr->rnb_len);
	__swab32s(&nbr->rnb_flags);
}

void lustre_swab_ost_body(struct ost_body *b)
{
	lustre_swab_obdo(&b->oa);
}

void lustre_swab_ost_last_id(u64 *id)
{
	__swab64s(id);
}

void lustre_swab_generic_32s(__u32 *val)
{
	__swab32s(val);
}

void lustre_swab_gl_lquota_desc(struct ldlm_gl_lquota_desc *desc)
{
	lustre_swab_lu_fid(&desc->gl_id.qid_fid);
	__swab64s(&desc->gl_flags);
	__swab64s(&desc->gl_ver);
	__swab64s(&desc->gl_hardlimit);
	__swab64s(&desc->gl_softlimit);
	__swab64s(&desc->gl_time);
	BUILD_BUG_ON(offsetof(typeof(*desc), gl_pad2) == 0);
}
EXPORT_SYMBOL(lustre_swab_gl_lquota_desc);

void lustre_swab_gl_barrier_desc(struct ldlm_gl_barrier_desc *desc)
{
	__swab32s(&desc->lgbd_status);
	__swab32s(&desc->lgbd_timeout);
	BUILD_BUG_ON(offsetof(typeof(*desc), lgbd_padding) == 0);
}
EXPORT_SYMBOL(lustre_swab_gl_barrier_desc);

void lustre_swab_ost_lvb_v1(struct ost_lvb_v1 *lvb)
{
	__swab64s(&lvb->lvb_size);
	__swab64s(&lvb->lvb_mtime);
	__swab64s(&lvb->lvb_atime);
	__swab64s(&lvb->lvb_ctime);
	__swab64s(&lvb->lvb_blocks);
}
EXPORT_SYMBOL(lustre_swab_ost_lvb_v1);

void lustre_swab_ost_lvb(struct ost_lvb *lvb)
{
	__swab64s(&lvb->lvb_size);
	__swab64s(&lvb->lvb_mtime);
	__swab64s(&lvb->lvb_atime);
	__swab64s(&lvb->lvb_ctime);
	__swab64s(&lvb->lvb_blocks);
	__swab32s(&lvb->lvb_mtime_ns);
	__swab32s(&lvb->lvb_atime_ns);
	__swab32s(&lvb->lvb_ctime_ns);
	__swab32s(&lvb->lvb_padding);
}
EXPORT_SYMBOL(lustre_swab_ost_lvb);

void lustre_swab_lquota_lvb(struct lquota_lvb *lvb)
{
	__swab64s(&lvb->lvb_flags);
	__swab64s(&lvb->lvb_id_may_rel);
	__swab64s(&lvb->lvb_id_rel);
	__swab64s(&lvb->lvb_id_qunit);
	__swab64s(&lvb->lvb_pad1);
}
EXPORT_SYMBOL(lustre_swab_lquota_lvb);

void lustre_swab_barrier_lvb(struct barrier_lvb *lvb)
{
	__swab32s(&lvb->lvb_status);
	__swab32s(&lvb->lvb_index);
	BUILD_BUG_ON(offsetof(typeof(*lvb), lvb_padding) == 0);
}
EXPORT_SYMBOL(lustre_swab_barrier_lvb);

void lustre_swab_mdt_body(struct mdt_body *b)
{
	lustre_swab_lu_fid(&b->mbo_fid1);
	lustre_swab_lu_fid(&b->mbo_fid2);
	/* handle is opaque */
	__swab64s(&b->mbo_valid);
	__swab64s(&b->mbo_size);
	__swab64s(&b->mbo_mtime);
	__swab64s(&b->mbo_atime);
	__swab64s(&b->mbo_ctime);
	__swab64s(&b->mbo_blocks);
	__swab64s(&b->mbo_version);
	__swab64s(&b->mbo_t_state);
	__swab32s(&b->mbo_fsuid);
	__swab32s(&b->mbo_fsgid);
	__swab32s(&b->mbo_capability);
	__swab32s(&b->mbo_mode);
	__swab32s(&b->mbo_uid);
	__swab32s(&b->mbo_gid);
	__swab32s(&b->mbo_flags);
	__swab32s(&b->mbo_rdev);
	__swab32s(&b->mbo_nlink);
	__swab32s(&b->mbo_layout_gen);
	__swab32s(&b->mbo_suppgid);
	__swab32s(&b->mbo_eadatasize);
	__swab32s(&b->mbo_aclsize);
	__swab32s(&b->mbo_max_mdsize);
	BUILD_BUG_ON(offsetof(typeof(*b), mbo_unused3) == 0);
	__swab32s(&b->mbo_uid_h);
	__swab32s(&b->mbo_gid_h);
	__swab32s(&b->mbo_projid);
	__swab64s(&b->mbo_dom_size);
	__swab64s(&b->mbo_dom_blocks);
	__swab64s(&b->mbo_btime);
	BUILD_BUG_ON(offsetof(typeof(*b), mbo_padding_9) == 0);
	BUILD_BUG_ON(offsetof(typeof(*b), mbo_padding_10) == 0);
}

void lustre_swab_mdt_ioepoch(struct mdt_ioepoch *b)
{
	/* mio_open_handle is opaque */
	BUILD_BUG_ON(offsetof(typeof(*b), mio_unused1) == 0);
	BUILD_BUG_ON(offsetof(typeof(*b), mio_unused2) == 0);
	BUILD_BUG_ON(offsetof(typeof(*b), mio_padding) == 0);
}

void lustre_swab_mgs_target_info(struct mgs_target_info *mti)
{
	int i;

	__swab32s(&mti->mti_lustre_ver);
	__swab32s(&mti->mti_stripe_index);
	__swab32s(&mti->mti_config_ver);
	__swab32s(&mti->mti_flags);
	__swab32s(&mti->mti_instance);
	__swab32s(&mti->mti_nid_count);
	BUILD_BUG_ON(sizeof(lnet_nid_t) != sizeof(__u64));
	for (i = 0; i < MTI_NIDS_MAX; i++)
		__swab64s(&mti->mti_nids[i]);
}

void lustre_swab_mgs_nidtbl_entry(struct mgs_nidtbl_entry *entry)
{
	__u8 i;

	__swab64s(&entry->mne_version);
	__swab32s(&entry->mne_instance);
	__swab32s(&entry->mne_index);
	__swab32s(&entry->mne_length);

	/* mne_nid_(count|type) must be one byte size because we're gonna
	 * access it w/o swapping. */
	BUILD_BUG_ON(sizeof(entry->mne_nid_count) != sizeof(__u8));
	BUILD_BUG_ON(sizeof(entry->mne_nid_type) != sizeof(__u8));

	/* remove this assertion if ipv6 is supported. */
	LASSERT(entry->mne_nid_type == 0);
	for (i = 0; i < entry->mne_nid_count; i++) {
		BUILD_BUG_ON(sizeof(lnet_nid_t) != sizeof(__u64));
		__swab64s(&entry->u.nids[i]);
	}
}
EXPORT_SYMBOL(lustre_swab_mgs_nidtbl_entry);

void lustre_swab_mgs_config_body(struct mgs_config_body *body)
{
	__swab64s(&body->mcb_offset);
	__swab32s(&body->mcb_units);
	__swab16s(&body->mcb_type);
}

void lustre_swab_mgs_config_res(struct mgs_config_res *body)
{
	__swab64s(&body->mcr_offset);
	__swab64s(&body->mcr_size);
}

static void lustre_swab_obd_dqinfo(struct obd_dqinfo *i)
{
	__swab64s(&i->dqi_bgrace);
	__swab64s(&i->dqi_igrace);
	__swab32s(&i->dqi_flags);
	__swab32s(&i->dqi_valid);
}

static void lustre_swab_obd_dqblk(struct obd_dqblk *b)
{
	__swab64s(&b->dqb_ihardlimit);
	__swab64s(&b->dqb_isoftlimit);
	__swab64s(&b->dqb_curinodes);
	__swab64s(&b->dqb_bhardlimit);
	__swab64s(&b->dqb_bsoftlimit);
	__swab64s(&b->dqb_curspace);
	__swab64s(&b->dqb_btime);
	__swab64s(&b->dqb_itime);
	__swab32s(&b->dqb_valid);
	BUILD_BUG_ON(offsetof(typeof(*b), dqb_padding) == 0);
}

int lustre_swab_obd_quotactl(struct obd_quotactl *q, __u32 len)
{
	if (unlikely(len <= sizeof(struct obd_quotactl)))
		return -EOVERFLOW;

	__swab32s(&q->qc_cmd);
	__swab32s(&q->qc_type);
	__swab32s(&q->qc_id);
	__swab32s(&q->qc_stat);
	lustre_swab_obd_dqinfo(&q->qc_dqinfo);
	lustre_swab_obd_dqblk(&q->qc_dqblk);

	return len;
}

void lustre_swab_fid2path(struct getinfo_fid2path *gf)
{
	lustre_swab_lu_fid(&gf->gf_fid);
	__swab64s(&gf->gf_recno);
	__swab32s(&gf->gf_linkno);
	__swab32s(&gf->gf_pathlen);
}
EXPORT_SYMBOL(lustre_swab_fid2path);

static void lustre_swab_fiemap_extent(struct fiemap_extent *fm_extent)
{
	__swab64s(&fm_extent->fe_logical);
	__swab64s(&fm_extent->fe_physical);
	__swab64s(&fm_extent->fe_length);
	__swab32s(&fm_extent->fe_flags);
	__swab32s(&fm_extent->fe_device);
}

static void lustre_swab_fiemap_hdr(struct fiemap *fiemap)
{
	__swab64s(&fiemap->fm_start);
	__swab64s(&fiemap->fm_length);
	__swab32s(&fiemap->fm_flags);
	__swab32s(&fiemap->fm_mapped_extents);
	__swab32s(&fiemap->fm_extent_count);
	__swab32s(&fiemap->fm_reserved);
}

int lustre_swab_fiemap(struct fiemap *fiemap, __u32 len)
{
	__u32 i, size, count;

	lustre_swab_fiemap_hdr(fiemap);

	size = fiemap_count_to_size(fiemap->fm_mapped_extents);
	count = fiemap->fm_mapped_extents;
	if (unlikely(size > len)) {
		count = (len - sizeof(struct fiemap)) /
			sizeof(struct fiemap_extent);
		fiemap->fm_mapped_extents = count;
		size = -EOVERFLOW;
	}
	/* still swab extents as we cannot yet pass rc to callers */
	for (i = 0; i < count; i++)
		lustre_swab_fiemap_extent(&fiemap->fm_extents[i]);

	return size;
}

void lustre_swab_fiemap_info_key(struct ll_fiemap_info_key *fiemap_info)
{
	lustre_swab_obdo(&fiemap_info->lfik_oa);
	lustre_swab_fiemap_hdr(&fiemap_info->lfik_fiemap);
}

void lustre_swab_idx_info(struct idx_info *ii)
{
	__swab32s(&ii->ii_magic);
	__swab32s(&ii->ii_flags);
	__swab16s(&ii->ii_count);
	__swab32s(&ii->ii_attrs);
	lustre_swab_lu_fid(&ii->ii_fid);
	__swab64s(&ii->ii_version);
	__swab64s(&ii->ii_hash_start);
	__swab64s(&ii->ii_hash_end);
	__swab16s(&ii->ii_keysize);
	__swab16s(&ii->ii_recsize);
}

void lustre_swab_lip_header(struct lu_idxpage *lip)
{
	/* swab header */
	__swab32s(&lip->lip_magic);
	__swab16s(&lip->lip_flags);
	__swab16s(&lip->lip_nr);
}
EXPORT_SYMBOL(lustre_swab_lip_header);

void lustre_swab_mdt_rec_reint (struct mdt_rec_reint *rr)
{
	__swab32s(&rr->rr_opcode);
	__swab32s(&rr->rr_cap);
	__swab32s(&rr->rr_fsuid);
	/* rr_fsuid_h is unused */
	__swab32s(&rr->rr_fsgid);
	/* rr_fsgid_h is unused */
	__swab32s(&rr->rr_suppgid1);
	/* rr_suppgid1_h is unused */
	__swab32s(&rr->rr_suppgid2);
	/* rr_suppgid2_h is unused */
	lustre_swab_lu_fid(&rr->rr_fid1);
	lustre_swab_lu_fid(&rr->rr_fid2);
	__swab64s(&rr->rr_mtime);
	__swab64s(&rr->rr_atime);
	__swab64s(&rr->rr_ctime);
	__swab64s(&rr->rr_size);
	__swab64s(&rr->rr_blocks);
	__swab32s(&rr->rr_bias);
	__swab32s(&rr->rr_mode);
	__swab32s(&rr->rr_flags);
	__swab32s(&rr->rr_flags_h);
	__swab32s(&rr->rr_umask);
	__swab16s(&rr->rr_mirror_id);

	BUILD_BUG_ON(offsetof(typeof(*rr), rr_padding_4) == 0);
};

void lustre_swab_lov_desc(struct lov_desc *ld)
{
	__swab32s(&ld->ld_tgt_count);
	__swab32s(&ld->ld_active_tgt_count);
	__swab32s(&ld->ld_default_stripe_count);
	__swab32s(&ld->ld_pattern);
	__swab64s(&ld->ld_default_stripe_size);
	__swab64s(&ld->ld_default_stripe_offset);
	__swab32s(&ld->ld_qos_maxage);
	/* uuid endian insensitive */
}
EXPORT_SYMBOL(lustre_swab_lov_desc);

void lustre_swab_lmv_desc(struct lmv_desc *ld)
{
	__swab32s(&ld->ld_tgt_count);
	__swab32s(&ld->ld_active_tgt_count);
	__swab32s(&ld->ld_default_stripe_count);
	__swab32s(&ld->ld_pattern);
	__swab64s(&ld->ld_default_hash_size);
	__swab32s(&ld->ld_qos_maxage);
	/* uuid endian insensitive */
}

/* This structure is always in little-endian */
static void lustre_swab_lmv_mds_md_v1(struct lmv_mds_md_v1 *lmm1)
{
	int i;

	__swab32s(&lmm1->lmv_magic);
	__swab32s(&lmm1->lmv_stripe_count);
	__swab32s(&lmm1->lmv_master_mdt_index);
	__swab32s(&lmm1->lmv_hash_type);
	__swab32s(&lmm1->lmv_layout_version);
	for (i = 0; i < lmm1->lmv_stripe_count; i++)
		lustre_swab_lu_fid(&lmm1->lmv_stripe_fids[i]);
}

void lustre_swab_lmv_mds_md(union lmv_mds_md *lmm)
{
	switch (lmm->lmv_magic) {
	case LMV_MAGIC_V1:
		lustre_swab_lmv_mds_md_v1(&lmm->lmv_md_v1);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(lustre_swab_lmv_mds_md);

void lustre_swab_lmv_user_md_objects(struct lmv_user_mds_data *lmd,
				     int stripe_count)
{
	int i;

	for (i = 0; i < stripe_count; i++)
		__swab32s(&(lmd[i].lum_mds));
}
EXPORT_SYMBOL(lustre_swab_lmv_user_md_objects);


void lustre_swab_lmv_user_md(struct lmv_user_md *lum)
{
	__u32 count;

	if (lum->lum_magic == LMV_MAGIC_FOREIGN) {
		__swab32s(&lum->lum_magic);
		__swab32s(&((struct lmv_foreign_md *)lum)->lfm_length);
		__swab32s(&((struct lmv_foreign_md *)lum)->lfm_type);
		__swab32s(&((struct lmv_foreign_md *)lum)->lfm_flags);
		return;
	}

	count = lum->lum_stripe_count;
	__swab32s(&lum->lum_magic);
	__swab32s(&lum->lum_stripe_count);
	__swab32s(&lum->lum_stripe_offset);
	__swab32s(&lum->lum_hash_type);
	__swab32s(&lum->lum_type);
	/* lum_max_inherit and lum_max_inherit_rr do not need to be swabbed */
	BUILD_BUG_ON(offsetof(typeof(*lum), lum_padding1) == 0);
	BUILD_BUG_ON(offsetof(typeof(*lum), lum_padding2) == 0);
	BUILD_BUG_ON(offsetof(typeof(*lum), lum_padding3) == 0);
	switch (lum->lum_magic) {
	case LMV_USER_MAGIC_SPECIFIC:
		count = lum->lum_stripe_count;
		/* fallthrough */
	case __swab32(LMV_USER_MAGIC_SPECIFIC):
		lustre_swab_lmv_user_md_objects(lum->lum_objects, count);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(lustre_swab_lmv_user_md);

static void lustre_print_v1v3(unsigned int lvl, struct lov_user_md *lum,
			      const char *msg)
{
	CDEBUG(lvl, "%s lov_user_md %p:\n", msg, lum);
	CDEBUG(lvl, "\tlmm_magic: %#x\n", lum->lmm_magic);
	CDEBUG(lvl, "\tlmm_pattern: %#x\n", lum->lmm_pattern);
	CDEBUG(lvl, "\tlmm_object_id: %llu\n", lmm_oi_id(&lum->lmm_oi));
	CDEBUG(lvl, "\tlmm_object_gr: %llu\n", lmm_oi_seq(&lum->lmm_oi));
	CDEBUG(lvl, "\tlmm_stripe_size: %#x\n", lum->lmm_stripe_size);
	CDEBUG(lvl, "\tlmm_stripe_count: %#x\n", lum->lmm_stripe_count);
	CDEBUG(lvl, "\tlmm_stripe_offset/lmm_layout_gen: %#x\n",
	       lum->lmm_stripe_offset);
	if (lum->lmm_magic == LOV_USER_MAGIC_V3) {
		struct lov_user_md_v3 *v3 = (void *)lum;
		CDEBUG(lvl, "\tlmm_pool_name: %s\n", v3->lmm_pool_name);
	}
	if (lum->lmm_magic == LOV_USER_MAGIC_SPECIFIC) {
		struct lov_user_md_v3 *v3 = (void *)lum;
		int i;

		if (v3->lmm_pool_name[0] != '\0')
			CDEBUG(lvl, "\tlmm_pool_name: %s\n", v3->lmm_pool_name);

		CDEBUG(lvl, "\ttarget list:\n");
		for (i = 0; i < v3->lmm_stripe_count; i++)
			CDEBUG(lvl, "\t\t%u\n", v3->lmm_objects[i].l_ost_idx);
	}
}

void lustre_print_user_md(unsigned int lvl, struct lov_user_md *lum,
			  const char *msg)
{
	struct lov_comp_md_v1	*comp_v1;
	int			 i;

	if (likely(!cfs_cdebug_show(lvl, DEBUG_SUBSYSTEM)))
		return;

	if (lum->lmm_magic == LOV_USER_MAGIC_V1 ||
	    lum->lmm_magic == LOV_USER_MAGIC_V3) {
		lustre_print_v1v3(lvl, lum, msg);
		return;
	}

	if (lum->lmm_magic != LOV_USER_MAGIC_COMP_V1) {
		CDEBUG(lvl, "%s: bad magic: %x\n", msg, lum->lmm_magic);
		return;
	}

	comp_v1 = (struct lov_comp_md_v1 *)lum;
	CDEBUG(lvl, "%s: lov_comp_md_v1 %p:\n", msg, lum);
	CDEBUG(lvl, "\tlcm_magic: %#x\n", comp_v1->lcm_magic);
	CDEBUG(lvl, "\tlcm_size: %#x\n", comp_v1->lcm_size);
	CDEBUG(lvl, "\tlcm_layout_gen: %#x\n", comp_v1->lcm_layout_gen);
	CDEBUG(lvl, "\tlcm_flags: %#x\n", comp_v1->lcm_flags);
	CDEBUG(lvl, "\tlcm_entry_count: %#x\n\n", comp_v1->lcm_entry_count);
	CDEBUG(lvl, "\tlcm_mirror_count: %#x\n\n", comp_v1->lcm_mirror_count);

	for (i = 0; i < comp_v1->lcm_entry_count; i++) {
		struct lov_comp_md_entry_v1 *ent = &comp_v1->lcm_entries[i];
		struct lov_user_md *v1;

		CDEBUG(lvl, "\tentry %d:\n", i);
		CDEBUG(lvl, "\tlcme_id: %#x\n", ent->lcme_id);
		CDEBUG(lvl, "\tlcme_flags: %#x\n", ent->lcme_flags);
		if (ent->lcme_flags & LCME_FL_NOSYNC)
			CDEBUG(lvl, "\tlcme_timestamp: %llu\n",
					ent->lcme_timestamp);
		CDEBUG(lvl, "\tlcme_extent.e_start: %llu\n",
		       ent->lcme_extent.e_start);
		CDEBUG(lvl, "\tlcme_extent.e_end: %llu\n",
		       ent->lcme_extent.e_end);
		CDEBUG(lvl, "\tlcme_offset: %#x\n", ent->lcme_offset);
		CDEBUG(lvl, "\tlcme_size: %#x\n\n", ent->lcme_size);

		v1 = (struct lov_user_md *)((char *)comp_v1 +
				comp_v1->lcm_entries[i].lcme_offset);
		lustre_print_v1v3(lvl, v1, msg);
	}
}
EXPORT_SYMBOL(lustre_print_user_md);

static void lustre_swab_lmm_oi(struct ost_id *oi)
{
	__swab64s(&oi->oi.oi_id);
	__swab64s(&oi->oi.oi_seq);
}

static void lustre_swab_lov_user_md_common(struct lov_user_md_v1 *lum)
{
	ENTRY;
	__swab32s(&lum->lmm_magic);
	__swab32s(&lum->lmm_pattern);
	lustre_swab_lmm_oi(&lum->lmm_oi);
	__swab32s(&lum->lmm_stripe_size);
	__swab16s(&lum->lmm_stripe_count);
	__swab16s(&lum->lmm_stripe_offset);
	EXIT;
}

void lustre_swab_lov_user_md_v1(struct lov_user_md_v1 *lum)
{
	ENTRY;
	CDEBUG(D_IOCTL, "swabbing lov_user_md v1\n");
	lustre_swab_lov_user_md_common(lum);
	EXIT;
}
EXPORT_SYMBOL(lustre_swab_lov_user_md_v1);

void lustre_swab_lov_user_md_v3(struct lov_user_md_v3 *lum)
{
	ENTRY;
	CDEBUG(D_IOCTL, "swabbing lov_user_md v3\n");
	lustre_swab_lov_user_md_common((struct lov_user_md_v1 *)lum);
	/* lmm_pool_name nothing to do with char */
	EXIT;
}
EXPORT_SYMBOL(lustre_swab_lov_user_md_v3);

void lustre_swab_lov_comp_md_v1(struct lov_comp_md_v1 *lum)
{
	struct lov_comp_md_entry_v1	*ent;
	struct lov_user_md_v1	*v1;
	struct lov_user_md_v3	*v3;
	int	i;
	bool	cpu_endian;
	__u32	off, size;
	__u16	ent_count, stripe_count;
	ENTRY;

	cpu_endian = lum->lcm_magic == LOV_USER_MAGIC_COMP_V1;
	ent_count = lum->lcm_entry_count;
	if (!cpu_endian)
		__swab16s(&ent_count);

	CDEBUG(D_IOCTL, "swabbing lov_user_comp_md v1\n");
	__swab32s(&lum->lcm_magic);
	__swab32s(&lum->lcm_size);
	__swab32s(&lum->lcm_layout_gen);
	__swab16s(&lum->lcm_flags);
	__swab16s(&lum->lcm_entry_count);
	__swab16s(&lum->lcm_mirror_count);
	BUILD_BUG_ON(offsetof(typeof(*lum), lcm_padding1) == 0);
	BUILD_BUG_ON(offsetof(typeof(*lum), lcm_padding2) == 0);

	for (i = 0; i < ent_count; i++) {
		ent = &lum->lcm_entries[i];
		off = ent->lcme_offset;
		size = ent->lcme_size;

		if (!cpu_endian) {
			__swab32s(&off);
			__swab32s(&size);
		}
		__swab32s(&ent->lcme_id);
		__swab32s(&ent->lcme_flags);
		__swab64s(&ent->lcme_timestamp);
		__swab64s(&ent->lcme_extent.e_start);
		__swab64s(&ent->lcme_extent.e_end);
		__swab32s(&ent->lcme_offset);
		__swab32s(&ent->lcme_size);
		__swab32s(&ent->lcme_layout_gen);
		BUILD_BUG_ON(offsetof(typeof(*ent), lcme_padding_1) == 0);

		v1 = (struct lov_user_md_v1 *)((char *)lum + off);
		stripe_count = v1->lmm_stripe_count;
		if (!cpu_endian)
			__swab16s(&stripe_count);

		if (v1->lmm_magic == __swab32(LOV_USER_MAGIC_V1) ||
		    v1->lmm_magic == LOV_USER_MAGIC_V1) {
			lustre_swab_lov_user_md_v1(v1);
			if (size > sizeof(*v1))
				lustre_swab_lov_user_md_objects(v1->lmm_objects,
								stripe_count);
		} else if (v1->lmm_magic == __swab32(LOV_USER_MAGIC_V3) ||
			   v1->lmm_magic == LOV_USER_MAGIC_V3 ||
			   v1->lmm_magic == __swab32(LOV_USER_MAGIC_SPECIFIC) ||
			   v1->lmm_magic == LOV_USER_MAGIC_SPECIFIC) {
			v3 = (struct lov_user_md_v3 *)v1;
			lustre_swab_lov_user_md_v3(v3);
			if (size > sizeof(*v3))
				lustre_swab_lov_user_md_objects(v3->lmm_objects,
								stripe_count);
		} else {
			CERROR("Invalid magic %#x\n", v1->lmm_magic);
		}
	}
}
EXPORT_SYMBOL(lustre_swab_lov_comp_md_v1);

void lustre_swab_lov_user_md_objects(struct lov_user_ost_data *lod,
				     int stripe_count)
{
	int i;

	ENTRY;
	for (i = 0; i < stripe_count; i++) {
		lustre_swab_ost_id(&(lod[i].l_ost_oi));
		__swab32s(&(lod[i].l_ost_gen));
		__swab32s(&(lod[i].l_ost_idx));
	}
	EXIT;
}
EXPORT_SYMBOL(lustre_swab_lov_user_md_objects);

void lustre_swab_lov_user_md(struct lov_user_md *lum, size_t size)
{
	struct lov_user_md_v1 *v1;
	struct lov_user_md_v3 *v3;
	struct lov_foreign_md *lfm;
	__u16 stripe_count;
	ENTRY;

	CDEBUG(D_IOCTL, "swabbing lov_user_md\n");
	switch (lum->lmm_magic) {
	case __swab32(LOV_MAGIC_V1):
	case LOV_USER_MAGIC_V1:
	{
		v1 = (struct lov_user_md_v1 *)lum;
		stripe_count = v1->lmm_stripe_count;

		if (lum->lmm_magic != LOV_USER_MAGIC_V1)
			__swab16s(&stripe_count);

		lustre_swab_lov_user_md_v1(v1);
		if (size > sizeof(*v1))
			lustre_swab_lov_user_md_objects(v1->lmm_objects,
							stripe_count);

		break;
	}
	case __swab32(LOV_MAGIC_V3):
	case LOV_USER_MAGIC_V3:
	{
		v3 = (struct lov_user_md_v3 *)lum;
		stripe_count = v3->lmm_stripe_count;

		if (lum->lmm_magic != LOV_USER_MAGIC_V3)
			__swab16s(&stripe_count);

		lustre_swab_lov_user_md_v3(v3);
		if (size > sizeof(*v3))
			lustre_swab_lov_user_md_objects(v3->lmm_objects,
							stripe_count);
		break;
	}
	case __swab32(LOV_USER_MAGIC_SPECIFIC):
	case LOV_USER_MAGIC_SPECIFIC:
	{
		v3 = (struct lov_user_md_v3 *)lum;
		stripe_count = v3->lmm_stripe_count;

		if (lum->lmm_magic != LOV_USER_MAGIC_SPECIFIC)
			__swab16s(&stripe_count);

		lustre_swab_lov_user_md_v3(v3);
		lustre_swab_lov_user_md_objects(v3->lmm_objects, stripe_count);
		break;
	}
	case __swab32(LOV_MAGIC_COMP_V1):
	case LOV_USER_MAGIC_COMP_V1:
		lustre_swab_lov_comp_md_v1((struct lov_comp_md_v1 *)lum);
		break;
	case __swab32(LOV_MAGIC_FOREIGN):
	case LOV_USER_MAGIC_FOREIGN:
	{
		lfm = (struct lov_foreign_md *)lum;
		__swab32s(&lfm->lfm_magic);
		__swab32s(&lfm->lfm_length);
		__swab32s(&lfm->lfm_type);
		__swab32s(&lfm->lfm_flags);
		break;
	}
	default:
		CDEBUG(D_IOCTL, "Invalid LOV magic %08x\n", lum->lmm_magic);
	}
}
EXPORT_SYMBOL(lustre_swab_lov_user_md);

void lustre_swab_lov_mds_md(struct lov_mds_md *lmm)
{
	ENTRY;
	CDEBUG(D_IOCTL, "swabbing lov_mds_md\n");
	__swab32s(&lmm->lmm_magic);
	__swab32s(&lmm->lmm_pattern);
	lustre_swab_lmm_oi(&lmm->lmm_oi);
	__swab32s(&lmm->lmm_stripe_size);
	__swab16s(&lmm->lmm_stripe_count);
	__swab16s(&lmm->lmm_layout_gen);
	EXIT;
}
EXPORT_SYMBOL(lustre_swab_lov_mds_md);

void lustre_swab_ldlm_res_id(struct ldlm_res_id *id)
{
	int i;

	for (i = 0; i < RES_NAME_SIZE; i++)
		__swab64s(&id->name[i]);
}

void lustre_swab_ldlm_policy_data(union ldlm_wire_policy_data *d)
{
	/* the lock data is a union and the first two fields are always an
	 * extent so it's ok to process an LDLM_EXTENT and LDLM_FLOCK lock
	 * data the same way.
	 */
	__swab64s(&d->l_extent.start);
	__swab64s(&d->l_extent.end);
	__swab64s(&d->l_extent.gid);
	__swab64s(&d->l_flock.lfw_owner);
	__swab32s(&d->l_flock.lfw_pid);
}

void lustre_swab_ldlm_intent(struct ldlm_intent *i)
{
	__swab64s(&i->opc);
}

void lustre_swab_ldlm_resource_desc(struct ldlm_resource_desc *r)
{
	__swab32s(&r->lr_type);
	BUILD_BUG_ON(offsetof(typeof(*r), lr_pad) == 0);
	lustre_swab_ldlm_res_id(&r->lr_name);
}

void lustre_swab_ldlm_lock_desc(struct ldlm_lock_desc *l)
{
	lustre_swab_ldlm_resource_desc(&l->l_resource);
	__swab32s(&l->l_req_mode);
	__swab32s(&l->l_granted_mode);
	lustre_swab_ldlm_policy_data(&l->l_policy_data);
}

void lustre_swab_ldlm_request(struct ldlm_request *rq)
{
	__swab32s(&rq->lock_flags);
	lustre_swab_ldlm_lock_desc(&rq->lock_desc);
	__swab32s(&rq->lock_count);
	/* lock_handle[] opaque */
}

void lustre_swab_ldlm_reply(struct ldlm_reply *r)
{
	__swab32s(&r->lock_flags);
	BUILD_BUG_ON(offsetof(typeof(*r), lock_padding) == 0);
	lustre_swab_ldlm_lock_desc(&r->lock_desc);
	/* lock_handle opaque */
	__swab64s(&r->lock_policy_res1);
	__swab64s(&r->lock_policy_res2);
}

void lustre_swab_quota_body(struct quota_body *b)
{
	lustre_swab_lu_fid(&b->qb_fid);
	lustre_swab_lu_fid((struct lu_fid *)&b->qb_id);
	__swab32s(&b->qb_flags);
	__swab64s(&b->qb_count);
	__swab64s(&b->qb_usage);
	__swab64s(&b->qb_slv_ver);
}

/* Dump functions */
void dump_ioo(struct obd_ioobj *ioo)
{
	CDEBUG(D_RPCTRACE,
	       "obd_ioobj: ioo_oid="DOSTID", ioo_max_brw=%#x, "
	       "ioo_bufct=%d\n", POSTID(&ioo->ioo_oid), ioo->ioo_max_brw,
	       ioo->ioo_bufcnt);
}

void dump_rniobuf(struct niobuf_remote *nb)
{
	CDEBUG(D_RPCTRACE, "niobuf_remote: offset=%llu, len=%d, flags=%x\n",
	       nb->rnb_offset, nb->rnb_len, nb->rnb_flags);
}

void dump_obdo(struct obdo *oa)
{
	u64 valid = oa->o_valid;

	CDEBUG(D_RPCTRACE, "obdo: o_valid = %#llx\n", valid);
	if (valid & OBD_MD_FLID)
		CDEBUG(D_RPCTRACE, "obdo: id = "DOSTID"\n", POSTID(&oa->o_oi));
	if (valid & OBD_MD_FLFID)
		CDEBUG(D_RPCTRACE, "obdo: o_parent_seq = %#llx\n",
		       oa->o_parent_seq);
	if (valid & OBD_MD_FLSIZE)
		CDEBUG(D_RPCTRACE, "obdo: o_size = %lld\n", oa->o_size);
	if (valid & OBD_MD_FLMTIME)
		CDEBUG(D_RPCTRACE, "obdo: o_mtime = %lld\n", oa->o_mtime);
	if (valid & OBD_MD_FLATIME)
		CDEBUG(D_RPCTRACE, "obdo: o_atime = %lld\n", oa->o_atime);
	if (valid & OBD_MD_FLCTIME)
		CDEBUG(D_RPCTRACE, "obdo: o_ctime = %lld\n", oa->o_ctime);
	if (valid & OBD_MD_FLBLOCKS)   /* allocation of space */
		CDEBUG(D_RPCTRACE, "obdo: o_blocks = %lld\n", oa->o_blocks);
	if (valid & OBD_MD_FLGRANT)
		CDEBUG(D_RPCTRACE, "obdo: o_grant = %lld\n", oa->o_grant);
	if (valid & OBD_MD_FLBLKSZ)
		CDEBUG(D_RPCTRACE, "obdo: o_blksize = %d\n", oa->o_blksize);
	if (valid & (OBD_MD_FLTYPE | OBD_MD_FLMODE))
		CDEBUG(D_RPCTRACE, "obdo: o_mode = %o\n",
		       oa->o_mode & ((valid & OBD_MD_FLTYPE ?  S_IFMT : 0) |
				     (valid & OBD_MD_FLMODE ? ~S_IFMT : 0)));
	if (valid & OBD_MD_FLUID)
		CDEBUG(D_RPCTRACE, "obdo: o_uid = %u\n", oa->o_uid);
	if (valid & OBD_MD_FLUID)
		CDEBUG(D_RPCTRACE, "obdo: o_uid_h = %u\n", oa->o_uid_h);
	if (valid & OBD_MD_FLGID)
		CDEBUG(D_RPCTRACE, "obdo: o_gid = %u\n", oa->o_gid);
	if (valid & OBD_MD_FLGID)
		CDEBUG(D_RPCTRACE, "obdo: o_gid_h = %u\n", oa->o_gid_h);
	if (valid & OBD_MD_FLFLAGS)
		CDEBUG(D_RPCTRACE, "obdo: o_flags = %x\n", oa->o_flags);
	if (valid & OBD_MD_FLNLINK)
		CDEBUG(D_RPCTRACE, "obdo: o_nlink = %u\n", oa->o_nlink);
	else if (valid & OBD_MD_FLCKSUM)
		CDEBUG(D_RPCTRACE, "obdo: o_checksum (o_nlink) = %u\n",
		       oa->o_nlink);
	if (valid & OBD_MD_FLPARENT)
		CDEBUG(D_RPCTRACE, "obdo: o_parent_oid = %x\n",
		       oa->o_parent_oid);
	if (valid & OBD_MD_FLFID) {
		CDEBUG(D_RPCTRACE, "obdo: o_stripe_idx = %u\n",
		       oa->o_stripe_idx);
		CDEBUG(D_RPCTRACE, "obdo: o_parent_ver = %x\n",
		       oa->o_parent_ver);
	}
	if (valid & OBD_MD_FLHANDLE)
		CDEBUG(D_RPCTRACE, "obdo: o_handle = %lld\n",
		       oa->o_handle.cookie);
}

void dump_ost_body(struct ost_body *ob)
{
	dump_obdo(&ob->oa);
}

void dump_rcs(__u32 *rc)
{
	CDEBUG(D_RPCTRACE, "rmf_rcs: %d\n", *rc);
}

static inline int req_ptlrpc_body_swabbed(struct ptlrpc_request *req)
{
	LASSERT(req->rq_reqmsg);

	switch (req->rq_reqmsg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return req_capsule_req_swabbed(&req->rq_pill,
					       MSG_PTLRPC_BODY_OFF);
	default:
		CERROR("bad lustre msg magic: %#08X\n",
		       req->rq_reqmsg->lm_magic);
	}
	return 0;
}

static inline int rep_ptlrpc_body_swabbed(struct ptlrpc_request *req)
{
	if (unlikely(!req->rq_repmsg))
		return 0;

	switch (req->rq_repmsg->lm_magic) {
	case LUSTRE_MSG_MAGIC_V2:
		return req_capsule_rep_swabbed(&req->rq_pill,
					       MSG_PTLRPC_BODY_OFF);
	default:
		/* uninitialized yet */
		return 0;
	}
}

void _debug_req(struct ptlrpc_request *req,
		struct libcfs_debug_msg_data *msgdata, const char *fmt, ...)
{
	bool req_ok = req->rq_reqmsg != NULL;
	bool rep_ok = false;
	lnet_nid_t nid = LNET_NID_ANY;
	struct va_format vaf;
	va_list args;
	int rep_flags = -1;
	int rep_status = -1;

	spin_lock(&req->rq_early_free_lock);
	if (req->rq_repmsg)
		rep_ok = true;

	if (req_capsule_req_need_swab(&req->rq_pill)) {
		req_ok = req_ok && req_ptlrpc_body_swabbed(req);
		rep_ok = rep_ok && rep_ptlrpc_body_swabbed(req);
	}

	if (rep_ok) {
		rep_flags = lustre_msg_get_flags(req->rq_repmsg);
		rep_status = lustre_msg_get_status(req->rq_repmsg);
	}
	spin_unlock(&req->rq_early_free_lock);

	if (req->rq_import && req->rq_import->imp_connection)
		nid = req->rq_import->imp_connection->c_peer.nid;
	else if (req->rq_export && req->rq_export->exp_connection)
		nid = req->rq_export->exp_connection->c_peer.nid;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	libcfs_debug_msg(msgdata,
			 "%pV req@%p x%llu/t%lld(%lld) o%d->%s@%s:%d/%d lens %d/%d e %d to %lld dl %lld ref %d fl " REQ_FLAGS_FMT "/%x/%x rc %d/%d job:'%s'\n",
			 &vaf,
			 req, req->rq_xid, req->rq_transno,
			 req_ok ? lustre_msg_get_transno(req->rq_reqmsg) : 0,
			 req_ok ? lustre_msg_get_opc(req->rq_reqmsg) : -1,
			 req->rq_import ?
			 req->rq_import->imp_obd->obd_name :
			 req->rq_export ?
			 req->rq_export->exp_client_uuid.uuid :
			 "<?>",
			 libcfs_nid2str(nid),
			 req->rq_request_portal, req->rq_reply_portal,
			 req->rq_reqlen, req->rq_replen,
			 req->rq_early_count, (s64)req->rq_timedout,
			 (s64)req->rq_deadline,
			 atomic_read(&req->rq_refcount),
			 DEBUG_REQ_FLAGS(req),
			 req_ok ? lustre_msg_get_flags(req->rq_reqmsg) : -1,
			 rep_flags, req->rq_status, rep_status,
			 req_ok ? lustre_msg_get_jobid(req->rq_reqmsg) ?: ""
				: "");
	va_end(args);
}
EXPORT_SYMBOL(_debug_req);

void lustre_swab_hsm_user_state(struct hsm_user_state *state)
{
	__swab32s(&state->hus_states);
	__swab32s(&state->hus_archive_id);
}

void lustre_swab_hsm_state_set(struct hsm_state_set *hss)
{
	__swab32s(&hss->hss_valid);
	__swab64s(&hss->hss_setmask);
	__swab64s(&hss->hss_clearmask);
	__swab32s(&hss->hss_archive_id);
}

static void lustre_swab_hsm_extent(struct hsm_extent *extent)
{
	__swab64s(&extent->offset);
	__swab64s(&extent->length);
}

void lustre_swab_hsm_current_action(struct hsm_current_action *action)
{
	__swab32s(&action->hca_state);
	__swab32s(&action->hca_action);
	lustre_swab_hsm_extent(&action->hca_location);
}

void lustre_swab_hsm_user_item(struct hsm_user_item *hui)
{
	lustre_swab_lu_fid(&hui->hui_fid);
	lustre_swab_hsm_extent(&hui->hui_extent);
}

void lustre_swab_lu_extent(struct lu_extent *le)
{
	__swab64s(&le->e_start);
	__swab64s(&le->e_end);
}

void lustre_swab_layout_intent(struct layout_intent *li)
{
	__swab32s(&li->li_opc);
	__swab32s(&li->li_flags);
	lustre_swab_lu_extent(&li->li_extent);
}

void lustre_swab_hsm_progress_kernel(struct hsm_progress_kernel *hpk)
{
	lustre_swab_lu_fid(&hpk->hpk_fid);
	__swab64s(&hpk->hpk_cookie);
	__swab64s(&hpk->hpk_extent.offset);
	__swab64s(&hpk->hpk_extent.length);
	__swab16s(&hpk->hpk_flags);
	__swab16s(&hpk->hpk_errval);
}

void lustre_swab_hsm_request(struct hsm_request *hr)
{
	__swab32s(&hr->hr_action);
	__swab32s(&hr->hr_archive_id);
	__swab64s(&hr->hr_flags);
	__swab32s(&hr->hr_itemcount);
	__swab32s(&hr->hr_data_len);
}

/* TODO: swab each sub request message */
void lustre_swab_batch_update_request(struct batch_update_request *bur)
{
	__swab32s(&bur->burq_magic);
	__swab16s(&bur->burq_count);
	__swab16s(&bur->burq_padding);
}

/* TODO: swab each sub reply message. */
void lustre_swab_batch_update_reply(struct batch_update_reply *bur)
{
	__swab32s(&bur->burp_magic);
	__swab16s(&bur->burp_count);
	__swab16s(&bur->burp_padding);
}

void lustre_swab_but_update_header(struct but_update_header *buh)
{
	__swab32s(&buh->buh_magic);
	__swab32s(&buh->buh_count);
	__swab32s(&buh->buh_inline_length);
	__swab32s(&buh->buh_reply_size);
	__swab32s(&buh->buh_update_count);
}
EXPORT_SYMBOL(lustre_swab_but_update_header);

void lustre_swab_but_update_buffer(struct but_update_buffer *bub)
{
	__swab32s(&bub->bub_size);
	__swab32s(&bub->bub_padding);
}
EXPORT_SYMBOL(lustre_swab_but_update_buffer);

void lustre_swab_swap_layouts(struct mdc_swap_layouts *msl)
{
	__swab64s(&msl->msl_flags);
}

void lustre_swab_close_data(struct close_data *cd)
{
	lustre_swab_lu_fid(&cd->cd_fid);
	__swab64s(&cd->cd_data_version);
}

void lustre_swab_close_data_resync_done(struct close_data_resync_done *resync)
{
	int i;

	__swab32s(&resync->resync_count);
	/* after swab, resync_count must in CPU endian */
	if (resync->resync_count <= INLINE_RESYNC_ARRAY_SIZE) {
		for (i = 0; i < resync->resync_count; i++)
			__swab32s(&resync->resync_ids_inline[i]);
	}
}
EXPORT_SYMBOL(lustre_swab_close_data_resync_done);

void lustre_swab_lfsck_request(struct lfsck_request *lr)
{
	__swab32s(&lr->lr_event);
	__swab32s(&lr->lr_index);
	__swab32s(&lr->lr_flags);
	__swab32s(&lr->lr_valid);
	__swab32s(&lr->lr_speed);
	__swab16s(&lr->lr_version);
	__swab16s(&lr->lr_active);
	__swab16s(&lr->lr_param);
	__swab16s(&lr->lr_async_windows);
	__swab32s(&lr->lr_flags);
	lustre_swab_lu_fid(&lr->lr_fid);
	lustre_swab_lu_fid(&lr->lr_fid2);
	__swab32s(&lr->lr_comp_id);
	BUILD_BUG_ON(offsetof(typeof(*lr), lr_padding_0) == 0);
	BUILD_BUG_ON(offsetof(typeof(*lr), lr_padding_1) == 0);
	BUILD_BUG_ON(offsetof(typeof(*lr), lr_padding_2) == 0);
	BUILD_BUG_ON(offsetof(typeof(*lr), lr_padding_3) == 0);
}

void lustre_swab_lfsck_reply(struct lfsck_reply *lr)
{
	__swab32s(&lr->lr_status);
	BUILD_BUG_ON(offsetof(typeof(*lr), lr_padding_1) == 0);
	__swab64s(&lr->lr_repaired);
}

static void lustre_swab_orphan_rec(struct lu_orphan_rec *rec)
{
	lustre_swab_lu_fid(&rec->lor_fid);
	__swab32s(&rec->lor_uid);
	__swab32s(&rec->lor_gid);
}

void lustre_swab_orphan_ent(struct lu_orphan_ent *ent)
{
	lustre_swab_lu_fid(&ent->loe_key);
	lustre_swab_orphan_rec(&ent->loe_rec);
}
EXPORT_SYMBOL(lustre_swab_orphan_ent);

void lustre_swab_orphan_ent_v2(struct lu_orphan_ent_v2 *ent)
{
	lustre_swab_lu_fid(&ent->loe_key);
	lustre_swab_orphan_rec(&ent->loe_rec.lor_rec);
	lustre_swab_ost_layout(&ent->loe_rec.lor_layout);
	BUILD_BUG_ON(offsetof(typeof(ent->loe_rec), lor_padding) == 0);
}
EXPORT_SYMBOL(lustre_swab_orphan_ent_v2);

void lustre_swab_orphan_ent_v3(struct lu_orphan_ent_v3 *ent)
{
	lustre_swab_lu_fid(&ent->loe_key);
	lustre_swab_orphan_rec(&ent->loe_rec.lor_rec);
	lustre_swab_ost_layout(&ent->loe_rec.lor_layout);
	__swab32s(&ent->loe_rec.lor_layout_version);
	__swab32s(&ent->loe_rec.lor_range);
	BUILD_BUG_ON(offsetof(typeof(ent->loe_rec), lor_padding_1) == 0);
	BUILD_BUG_ON(offsetof(typeof(ent->loe_rec), lor_padding_2) == 0);
}
EXPORT_SYMBOL(lustre_swab_orphan_ent_v3);

void lustre_swab_ladvise(struct lu_ladvise *ladvise)
{
	__swab16s(&ladvise->lla_advice);
	__swab16s(&ladvise->lla_value1);
	__swab32s(&ladvise->lla_value2);
	__swab64s(&ladvise->lla_start);
	__swab64s(&ladvise->lla_end);
	__swab32s(&ladvise->lla_value3);
	__swab32s(&ladvise->lla_value4);
}
EXPORT_SYMBOL(lustre_swab_ladvise);

void lustre_swab_ladvise_hdr(struct ladvise_hdr *ladvise_hdr)
{
	__swab32s(&ladvise_hdr->lah_magic);
	__swab32s(&ladvise_hdr->lah_count);
	__swab64s(&ladvise_hdr->lah_flags);
	__swab32s(&ladvise_hdr->lah_value1);
	__swab32s(&ladvise_hdr->lah_value2);
	__swab64s(&ladvise_hdr->lah_value3);
}
EXPORT_SYMBOL(lustre_swab_ladvise_hdr);
