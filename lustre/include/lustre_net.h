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
 * Copyright (c) 2010, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */
/** \defgroup PtlRPC Portal RPC and networking module.
 *
 * PortalRPC is the layer used by rest of lustre code to achieve network
 * communications: establish connections with corresponding export and import
 * states, listen for a service, send and receive RPCs.
 * PortalRPC also includes base recovery framework: packet resending and
 * replaying, reconnections, pinger.
 *
 * PortalRPC utilizes LNet as its transport layer.
 *
 * @{
 */


#ifndef _LUSTRE_NET_H
#define _LUSTRE_NET_H

/** \defgroup net net
 *
 * @{
 */
#include <linux/kobject.h>
#include <linux/rhashtable.h>
#include <linux/uio.h>
#include <libcfs/libcfs.h>
#include <lnet/api.h>
#include <lnet/lib-types.h>
#include <uapi/linux/lnet/nidstr.h>
#include <uapi/linux/lustre/lustre_idl.h>
#include <lustre_ha.h>
#include <lustre_sec.h>
#include <lustre_import.h>
#include <lprocfs_status.h>
#include <lu_object.h>
#include <lustre_req_layout.h>
#include <obd_support.h>
#include <uapi/linux/lustre/lustre_ver.h>

/* MD flags we _always_ use */
#define PTLRPC_MD_OPTIONS  0

/**
 * log2 max # of bulk operations in one request: 2=4MB/RPC, 5=32MB/RPC, ...
 * In order for the client and server to properly negotiate the maximum
 * possible transfer size, PTLRPC_BULK_OPS_COUNT must be a power-of-two
 * value.  The client is free to limit the actual RPC size for any bulk
 * transfer via cl_max_pages_per_rpc to some non-power-of-two value.
 * NOTE: This is limited to 16 (=64GB RPCs) by IOOBJ_MAX_BRW_BITS. */
#define PTLRPC_BULK_OPS_BITS	6
#if PTLRPC_BULK_OPS_BITS > 16
#error "More than 65536 BRW RPCs not allowed by IOOBJ_MAX_BRW_BITS."
#endif
#define PTLRPC_BULK_OPS_COUNT	(1U << PTLRPC_BULK_OPS_BITS)
/**
 * PTLRPC_BULK_OPS_MASK is for the convenience of the client only, and
 * should not be used on the server at all.  Otherwise, it imposes a
 * protocol limitation on the maximum RPC size that can be used by any
 * RPC sent to that server in the future.  Instead, the server should
 * use the negotiated per-client ocd_brw_size to determine the bulk
 * RPC count. */
#define PTLRPC_BULK_OPS_MASK	(~((__u64)PTLRPC_BULK_OPS_COUNT - 1))

/**
 * Define maxima for bulk I/O.
 *
 * A single PTLRPC BRW request is sent via up to PTLRPC_BULK_OPS_COUNT
 * of LNET_MTU sized RDMA transfers.  Clients and servers negotiate the
 * currently supported maximum between peers at connect via ocd_brw_size.
 */
#define PTLRPC_MAX_BRW_BITS	(LNET_MTU_BITS + PTLRPC_BULK_OPS_BITS)
#define PTLRPC_MAX_BRW_SIZE	(1U << PTLRPC_MAX_BRW_BITS)
#define PTLRPC_MAX_BRW_PAGES	(PTLRPC_MAX_BRW_SIZE >> PAGE_SHIFT)

#define ONE_MB_BRW_SIZE		(1U << LNET_MTU_BITS)
#define MD_MAX_BRW_SIZE		(1U << LNET_MTU_BITS)
#define MD_MAX_BRW_PAGES	(MD_MAX_BRW_SIZE >> PAGE_SHIFT)
#define DT_MAX_BRW_SIZE		PTLRPC_MAX_BRW_SIZE
#define DT_DEF_BRW_SIZE		(4 * ONE_MB_BRW_SIZE)
#define DT_MAX_BRW_PAGES	(DT_MAX_BRW_SIZE >> PAGE_SHIFT)
#define OFD_MAX_BRW_SIZE	(1U << LNET_MTU_BITS)

/* When PAGE_SIZE is a constant, we can check our arithmetic here with cpp! */
#if ((PTLRPC_MAX_BRW_PAGES & (PTLRPC_MAX_BRW_PAGES - 1)) != 0)
# error "PTLRPC_MAX_BRW_PAGES isn't a power of two"
#endif
#if (PTLRPC_MAX_BRW_SIZE != (PTLRPC_MAX_BRW_PAGES * PAGE_SIZE))
# error "PTLRPC_MAX_BRW_SIZE isn't PTLRPC_MAX_BRW_PAGES * PAGE_SIZE"
#endif
#if (PTLRPC_MAX_BRW_SIZE > LNET_MTU * PTLRPC_BULK_OPS_COUNT)
# error "PTLRPC_MAX_BRW_SIZE too big"
#endif
#if (PTLRPC_MAX_BRW_PAGES > LNET_MAX_IOV * PTLRPC_BULK_OPS_COUNT)
# error "PTLRPC_MAX_BRW_PAGES too big"
#endif

#define PTLRPC_NTHRS_INIT	2

/**
 * Buffer Constants
 *
 * Constants determine how memory is used to buffer incoming service requests.
 *
 * ?_NBUFS              # buffers to allocate when growing the pool
 * ?_BUFSIZE            # bytes in a single request buffer
 * ?_MAXREQSIZE         # maximum request service will receive
 *
 * When fewer than ?_NBUFS/2 buffers are posted for receive, another chunk
 * of ?_NBUFS is added to the pool.
 *
 * Messages larger than ?_MAXREQSIZE are dropped.  Request buffers are
 * considered full when less than ?_MAXREQSIZE is left in them.
 */
/**
 * Thread Constants
 *
 * Constants determine how threads are created for ptlrpc service.
 *
 * ?_NTHRS_INIT	        # threads to create for each service partition on
 *			  initializing. If it's non-affinity service and
 *			  there is only one partition, it's the overall #
 *			  threads for the service while initializing.
 * ?_NTHRS_BASE		# threads should be created at least for each
 *			  ptlrpc partition to keep the service healthy.
 *			  It's the low-water mark of threads upper-limit
 *			  for each partition.
 * ?_THR_FACTOR         # threads can be added on threads upper-limit for
 *			  each CPU core. This factor is only for reference,
 *			  we might decrease value of factor if number of cores
 *			  per CPT is above a limit.
 * ?_NTHRS_MAX		# overall threads can be created for a service,
 *			  it's a soft limit because if service is running
 *			  on machine with hundreds of cores and tens of
 *			  CPU partitions, we need to guarantee each partition
 *			  has ?_NTHRS_BASE threads, which means total threads
 *			  will be ?_NTHRS_BASE * number_of_cpts which can
 *			  exceed ?_NTHRS_MAX.
 *
 * Examples
 *
 * #define MDS_NTHRS_INIT	2
 * #define MDS_NTHRS_BASE	64
 * #define MDS_NTHRS_FACTOR	8
 * #define MDS_NTHRS_MAX	1024
 *
 * Example 1):
 * ---------------------------------------------------------------------
 * Server(A) has 16 cores, user configured it to 4 partitions so each
 * partition has 4 cores, then actual number of service threads on each
 * partition is:
 *     MDS_NTHRS_BASE(64) + cores(4) * MDS_NTHRS_FACTOR(8) = 96
 *
 * Total number of threads for the service is:
 *     96 * partitions(4) = 384
 *
 * Example 2):
 * ---------------------------------------------------------------------
 * Server(B) has 32 cores, user configured it to 4 partitions so each
 * partition has 8 cores, then actual number of service threads on each
 * partition is:
 *     MDS_NTHRS_BASE(64) + cores(8) * MDS_NTHRS_FACTOR(8) = 128
 *
 * Total number of threads for the service is:
 *     128 * partitions(4) = 512
 *
 * Example 3):
 * ---------------------------------------------------------------------
 * Server(B) has 96 cores, user configured it to 8 partitions so each
 * partition has 12 cores, then actual number of service threads on each
 * partition is:
 *     MDS_NTHRS_BASE(64) + cores(12) * MDS_NTHRS_FACTOR(8) = 160
 *
 * Total number of threads for the service is:
 *     160 * partitions(8) = 1280
 *
 * However, it's above the soft limit MDS_NTHRS_MAX, so we choose this number
 * as upper limit of threads number for each partition:
 *     MDS_NTHRS_MAX(1024) / partitions(8) = 128
 *
 * Example 4):
 * ---------------------------------------------------------------------
 * Server(C) have a thousand of cores and user configured it to 32 partitions
 *     MDS_NTHRS_BASE(64) * 32 = 2048
 *
 * which is already above soft limit MDS_NTHRS_MAX(1024), but we still need
 * to guarantee that each partition has at least MDS_NTHRS_BASE(64) threads
 * to keep service healthy, so total number of threads will just be 2048.
 *
 * NB: we don't suggest to choose server with that many cores because backend
 *     filesystem itself, buffer cache, or underlying network stack might
 *     have some SMP scalability issues at that large scale.
 *
 *     If user already has a fat machine with hundreds or thousands of cores,
 *     there are two choices for configuration:
 *     a) create CPU table from subset of all CPUs and run Lustre on
 *        top of this subset
 *     b) bind service threads on a few partitions, see modparameters of
 *        MDS and OSS for details
*
 * NB: these calculations (and examples below) are simplified to help
 *     understanding, the real implementation is a little more complex,
 *     please see ptlrpc_server_nthreads_check() for details.
 *
 */

 /*
  * LDLM threads constants:
  *
  * Given 8 as factor and 24 as base threads number
  *
  * example 1)
  * On 4-core machine we will have 24 + 8 * 4 = 56 threads.
  *
  * example 2)
  * On 8-core machine with 2 partitions we will have 24 + 4 * 8 = 56
  * threads for each partition and total threads number will be 112.
  *
  * example 3)
  * On 64-core machine with 8 partitions we will need LDLM_NTHRS_BASE(24)
  * threads for each partition to keep service healthy, so total threads
  * number should be 24 * 8 = 192.
  *
  * So with these constants, threads number will be at the similar level
  * of old versions, unless target machine has over a hundred cores
  */
#define LDLM_THR_FACTOR		8
#define LDLM_NTHRS_INIT		PTLRPC_NTHRS_INIT
#define LDLM_NTHRS_BASE		24
#define LDLM_NTHRS_MAX		(num_online_cpus() == 1 ? 64 : 128)

#define LDLM_BL_THREADS   LDLM_NTHRS_AUTO_INIT
#define LDLM_CLIENT_NBUFS 1
#define LDLM_SERVER_NBUFS 64
#define LDLM_BUFSIZE      (8 * 1024)
#define LDLM_MAXREQSIZE   (5 * 1024)
#define LDLM_MAXREPSIZE   (1024)

 /*
  * MDS threads constants:
  *
  * Please see examples in "Thread Constants", MDS threads number will be at
  * the comparable level of old versions, unless the server has many cores.
  */
#ifndef MDS_MAX_THREADS
#define MDS_MAX_THREADS		1024
#define MDS_MAX_OTHR_THREADS	256

#else /* MDS_MAX_THREADS */
#if MDS_MAX_THREADS < PTLRPC_NTHRS_INIT
#undef MDS_MAX_THREADS
#define MDS_MAX_THREADS	PTLRPC_NTHRS_INIT
#endif
#define MDS_MAX_OTHR_THREADS	max(PTLRPC_NTHRS_INIT, MDS_MAX_THREADS / 2)
#endif

/* default service */
#define MDS_THR_FACTOR		8
#define MDS_NTHRS_INIT		PTLRPC_NTHRS_INIT
#define MDS_NTHRS_MAX		MDS_MAX_THREADS
#define MDS_NTHRS_BASE		min(64, MDS_NTHRS_MAX)

/* read-page service */
#define MDS_RDPG_THR_FACTOR	4
#define MDS_RDPG_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define MDS_RDPG_NTHRS_MAX	MDS_MAX_OTHR_THREADS
#define MDS_RDPG_NTHRS_BASE	min(48, MDS_RDPG_NTHRS_MAX)

/* these should be removed when we remove setattr service in the future */
#define MDS_SETA_THR_FACTOR	4
#define MDS_SETA_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define MDS_SETA_NTHRS_MAX	MDS_MAX_OTHR_THREADS
#define MDS_SETA_NTHRS_BASE	min(48, MDS_SETA_NTHRS_MAX)

/* non-affinity threads */
#define MDS_OTHR_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define MDS_OTHR_NTHRS_MAX	MDS_MAX_OTHR_THREADS

#define MDS_NBUFS		64

/**
 * Assume file name length = FNAME_MAX = 256 (true for ext3).
 *	  path name length = PATH_MAX = 4096
 *	  LOV MD size max  = EA_MAX = 24 * 2000
 *	  	(NB: 24 is size of lov_ost_data)
 *	  LOV LOGCOOKIE size max = 32 * 2000
 *	  	(NB: 32 is size of llog_cookie)
 * symlink:  FNAME_MAX + PATH_MAX  <- largest
 * link:     FNAME_MAX + PATH_MAX  (mds_rec_link < mds_rec_create)
 * rename:   FNAME_MAX + FNAME_MAX
 * open:     FNAME_MAX + EA_MAX
 *
 * MDS_MAXREQSIZE ~= 4736 bytes =
 * lustre_msg + ldlm_request + mdt_body + mds_rec_create + FNAME_MAX + PATH_MAX
 * MDS_MAXREPSIZE ~= 8300 bytes = lustre_msg + llog_header
 *
 * Realistic size is about 512 bytes (20 character name + 128 char symlink),
 * except in the open case where there are a large number of OSTs in a LOV.
 */
#define MDS_MAXREQSIZE		(5 * 1024)	/* >= 4736 */
#define MDS_MAXREPSIZE		(9 * 1024)	/* >= 8300 */

/**
 * MDS incoming request with LOV EA
 * 24 = sizeof(struct lov_ost_data), i.e: replay of opencreate
 */
#define MDS_LOV_MAXREQSIZE	max(MDS_MAXREQSIZE, \
				    362 + LOV_MAX_STRIPE_COUNT * 24)
/**
 * MDS outgoing reply with LOV EA
 *
 * NB: max reply size Lustre 2.4+ client can get from old MDS is:
 * LOV_MAX_STRIPE_COUNT * (llog_cookie + lov_ost_data) + extra bytes
 *
 * but 2.4 or later MDS will never send reply with llog_cookie to any
 * version client. This macro is defined for server side reply buffer size.
 */
#define MDS_LOV_MAXREPSIZE	MDS_LOV_MAXREQSIZE

/**
 * This is the size of a maximum REINT_SETXATTR request:
 *
 *   lustre_msg		 56 (32 + 4 x 5 + 4)
 *   ptlrpc_body	184
 *   mdt_rec_setxattr	136
 *   lustre_capa	120
 *   name		256 (XATTR_NAME_MAX)
 *   value	      65536 (XATTR_SIZE_MAX)
 */
#define MDS_EA_MAXREQSIZE	66288

/**
 * These are the maximum request and reply sizes (rounded up to 1 KB
 * boundaries) for the "regular" MDS_REQUEST_PORTAL and MDS_REPLY_PORTAL.
 */
#define MDS_REG_MAXREQSIZE	(((max(MDS_EA_MAXREQSIZE, \
				       MDS_LOV_MAXREQSIZE) + 1023) >> 10) << 10)
#define MDS_REG_MAXREPSIZE	MDS_REG_MAXREQSIZE

/**
 * The update request includes all of updates from the create, which might
 * include linkea (4K maxim), together with other updates, we set it to 1000K:
 * lustre_msg + ptlrpc_body + OUT_UPDATE_BUFFER_SIZE_MAX
 */
#define OUT_MAXREQSIZE	(1000 * 1024)
#define OUT_MAXREPSIZE	MDS_MAXREPSIZE

#define BUT_MAXREQSIZE	OUT_MAXREQSIZE
#define BUT_MAXREPSIZE	BUT_MAXREQSIZE

/** MDS_BUFSIZE = max_reqsize (w/o LOV EA) + max sptlrpc payload size */
#define MDS_BUFSIZE		max(MDS_MAXREQSIZE + SPTLRPC_MAX_PAYLOAD, \
				    8 * 1024)

/**
 * MDS_REG_BUFSIZE should at least be MDS_REG_MAXREQSIZE + SPTLRPC_MAX_PAYLOAD.
 * However, we need to allocate a much larger buffer for it because LNet
 * requires each MD(rqbd) has at least MDS_REQ_MAXREQSIZE bytes left to avoid
 * dropping of maximum-sized incoming request.  So if MDS_REG_BUFSIZE is only a
 * little larger than MDS_REG_MAXREQSIZE, then it can only fit in one request
 * even there are about MDS_REG_MAX_REQSIZE bytes left in a rqbd, and memory
 * utilization is very low.
 *
 * In the meanwhile, size of rqbd can't be too large, because rqbd can't be
 * reused until all requests fit in it have been processed and released,
 * which means one long blocked request can prevent the rqbd be reused.
 * Now we set request buffer size to 160 KB, so even each rqbd is unlinked
 * from LNet with unused 65 KB, buffer utilization will be about 59%.
 * Please check LU-2432 for details.
 */
#define MDS_REG_BUFSIZE		max(MDS_REG_MAXREQSIZE + SPTLRPC_MAX_PAYLOAD, \
				    160 * 1024)

/**
 * OUT_BUFSIZE = max_out_reqsize + max sptlrpc payload (~1K) which is
 * about 10K, for the same reason as MDS_REG_BUFSIZE, we also give some
 * extra bytes to each request buffer to improve buffer utilization rate.
  */
#define OUT_BUFSIZE		max(OUT_MAXREQSIZE + SPTLRPC_MAX_PAYLOAD, \
				    24 * 1024)

/** FLD_MAXREQSIZE == lustre_msg + __u32 padding + ptlrpc_body + opc */
#define FLD_MAXREQSIZE  (160)

/** FLD_MAXREPSIZE == lustre_msg + ptlrpc_body */
#define FLD_MAXREPSIZE  (152)
#define FLD_BUFSIZE	(1 << 12)

/**
 * SEQ_MAXREQSIZE == lustre_msg + __u32 padding + ptlrpc_body + opc + lu_range +
 * __u32 padding */
#define SEQ_MAXREQSIZE  (160)

/** SEQ_MAXREPSIZE == lustre_msg + ptlrpc_body + lu_range */
#define SEQ_MAXREPSIZE  (152)
#define SEQ_BUFSIZE	(1 << 12)

/** MGS threads must be >= 3, see bug 22458 comment #28 */
#define MGS_NTHRS_INIT	(PTLRPC_NTHRS_INIT + 1)
#define MGS_NTHRS_MAX	32

#define MGS_NBUFS       64
#define MGS_BUFSIZE     (8 * 1024)
#define MGS_MAXREQSIZE  (7 * 1024)
#define MGS_MAXREPSIZE  (9 * 1024)

 /*
  * OSS threads constants:
  *
  * Given 8 as factor and 64 as base threads number
  *
  * example 1):
  * On 8-core server configured to 2 partitions, we will have
  * 64 + 8 * 4 = 96 threads for each partition, 192 total threads.
  *
  * example 2):
  * On 32-core machine configured to 4 partitions, we will have
  * 64 + 8 * 8 = 112 threads for each partition, so total threads number
  * will be 112 * 4 = 448.
  *
  * example 3):
  * On 64-core machine configured to 4 partitions, we will have
  * 64 + 16 * 8 = 192 threads for each partition, so total threads number
  * will be 192 * 4 = 768 which is above limit OSS_NTHRS_MAX(512), so we
  * cut off the value to OSS_NTHRS_MAX(512) / 4 which is 128 threads
  * for each partition.
  *
  * So we can see that with these constants, threads number wil be at the
  * similar level of old versions, unless the server has many cores.
  */
 /* depress threads factor for VM with small memory size */
#define OSS_THR_FACTOR		min_t(int, 8, \
				NUM_CACHEPAGES >> (28 - PAGE_SHIFT))
#define OSS_NTHRS_INIT		(PTLRPC_NTHRS_INIT + 1)
#define OSS_NTHRS_BASE		64

/* threads for handling "create" request */
#define OSS_CR_THR_FACTOR	1
#define OSS_CR_NTHRS_INIT	PTLRPC_NTHRS_INIT
#define OSS_CR_NTHRS_BASE	8
#define OSS_CR_NTHRS_MAX	64

/**
 * OST_IO_MAXREQSIZE ~=
 *	lustre_msg + ptlrpc_body + obdo + obd_ioobj +
 *	DT_MAX_BRW_PAGES * niobuf_remote
 *
 * - single object with 16 pages is 512 bytes
 * - OST_IO_MAXREQSIZE must be at least 1 niobuf per page of data
 * - Must be a multiple of 1024
 * - should allow a reasonably large SHORT_IO_BYTES size (64KB)
 */
#define _OST_MAXREQSIZE_BASE ((unsigned long)(sizeof(struct lustre_msg)   + \
			     /* lm_buflens */ sizeof(__u32) * 4		  + \
					      sizeof(struct ptlrpc_body)  + \
					      sizeof(struct obdo)	  + \
					      sizeof(struct obd_ioobj)	  + \
					      sizeof(struct niobuf_remote)))
#define _OST_MAXREQSIZE_SUM ((unsigned long)(_OST_MAXREQSIZE_BASE	  + \
					     sizeof(struct niobuf_remote) * \
					     DT_MAX_BRW_PAGES))
/**
 * FIEMAP request can be 4K+ for now
 */
#define OST_MAXREQSIZE		(16UL * 1024UL)
#define OST_IO_MAXREQSIZE	max(OST_MAXREQSIZE,			\
				   ((_OST_MAXREQSIZE_SUM - 1) |		\
				    (1024UL - 1)) + 1)
/* Safe estimate of free space in standard RPC, provides upper limit for # of
 * bytes of i/o to pack in RPC (skipping bulk transfer). */
#define OST_MAX_SHORT_IO_BYTES	((OST_IO_MAXREQSIZE - _OST_MAXREQSIZE_BASE) & \
				 PAGE_MASK)

/* Actual size used for short i/o buffer.  Calculation means this:
 * At least one page (for large PAGE_SIZE), or 16 KiB, but not more
 * than the available space aligned to a page boundary. */
#define OBD_DEF_SHORT_IO_BYTES	min(max(PAGE_SIZE, 16UL * 1024UL), \
				    OST_MAX_SHORT_IO_BYTES)

#define OST_MAXREPSIZE		(9 * 1024)
#define OST_IO_MAXREPSIZE	OST_MAXREPSIZE

#define OST_NBUFS		64
/** OST_BUFSIZE = max_reqsize + max sptlrpc payload size */
#define OST_BUFSIZE		max_t(int, OST_MAXREQSIZE + 1024, 32 * 1024)
/**
 * OST_IO_MAXREQSIZE is 18K, giving extra 46K can increase buffer utilization
 * rate of request buffer, please check comment of MDS_LOV_BUFSIZE for details.
 */
#define OST_IO_BUFSIZE		max_t(int, OST_IO_MAXREQSIZE + 1024, 64 * 1024)

/* Macro to hide a typecast and BUILD_BUG. */
#define ptlrpc_req_async_args(_var, req) ({				\
		BUILD_BUG_ON(sizeof(*_var) > sizeof(req->rq_async_args)); \
		(typeof(_var))&req->rq_async_args;			\
	})

struct ptlrpc_replay_async_args {
	int		praa_old_state;
	int		praa_old_status;
};

/**
 * Structure to single define portal connection.
 */
struct ptlrpc_connection {
	/** linkage for connections hash table */
	struct rhash_head	c_hash;
	/** Our own lnet nid for this connection */
	lnet_nid_t              c_self;
	/** Remote side nid for this connection */
	struct lnet_process_id       c_peer;
	/** UUID of the other side */
	struct obd_uuid         c_remote_uuid;
	/** reference counter for this connection */
	atomic_t            c_refcount;
};

/** Client definition for PortalRPC */
struct ptlrpc_client {
        /** What lnet portal does this client send messages to by default */
        __u32                   cli_request_portal;
        /** What portal do we expect replies on */
        __u32                   cli_reply_portal;
        /** Name of the client */
	const char		*cli_name;
};

/** state flags of requests */
/* XXX only ones left are those used by the bulk descs as well! */
#define PTL_RPC_FL_INTR		BIT(0)	/* reply wait was interrupted by user */
#define PTL_RPC_FL_TIMEOUT	BIT(7)	/* request timed out waiting for reply */

#define REQ_MAX_ACK_LOCKS 8

union ptlrpc_async_args {
	/**
	 * Scratchpad for passing args to completion interpreter. Users
	 * cast to the struct of their choosing, and BUILD_BUG_ON that this is
	 * big enough.  For _tons_ of context, OBD_ALLOC a struct and store
	 * a pointer to it here.  The pointer_arg ensures this struct is at
	 * least big enough for that.
	 */
	void    *pointer_arg[11];
	__u64   space[7];
};

struct ptlrpc_request_set;
typedef int (*set_producer_func)(struct ptlrpc_request_set *, void *);

/**
 * Definition of request set structure.
 * Request set is a list of requests (not necessary to the same target) that
 * once populated with RPCs could be sent in parallel.
 * There are two kinds of request sets. General purpose and with dedicated
 * serving thread. Example of the latter is ptlrpcd set.
 * For general purpose sets once request set started sending it is impossible
 * to add new requests to such set.
 * Provides a way to call "completion callbacks" when all requests in the set
 * returned.
 */
struct ptlrpc_request_set {
	atomic_t		set_refcount;
	/** number of in queue requests */
	atomic_t		set_new_count;
	/** number of uncompleted requests */
	atomic_t		set_remaining;
	/** wait queue to wait on for request events */
	wait_queue_head_t	set_waitq;
	/** List of requests in the set */
	struct list_head	set_requests;
	/**
	 * Lock for \a set_new_requests manipulations
	 * locked so that any old caller can communicate requests to
	 * the set holder who can then fold them into the lock-free set
	 */
	spinlock_t		set_new_req_lock;
	/** List of new yet unsent requests. Only used with ptlrpcd now. */
	struct list_head	set_new_requests;

	/** rq_status of requests that have been freed already */
	int			set_rc;
	/** Additional fields used by the flow control extension */
	/** Maximum number of RPCs in flight */
	int			set_max_inflight;
	/** Callback function used to generate RPCs */
	set_producer_func	set_producer;
	/** opaq argument passed to the producer callback */
	void			*set_producer_arg;
	unsigned int		 set_allow_intr:1;
};

struct ptlrpc_bulk_desc;
struct ptlrpc_service_part;
struct ptlrpc_service;

/**
 * ptlrpc callback & work item stuff
 */
struct ptlrpc_cb_id {
	void (*cbid_fn)(struct lnet_event *ev);	/* specific callback fn */
	void *cbid_arg;				/* additional arg */
};

/** Maximum number of locks to fit into reply state */
#define RS_MAX_LOCKS 8
#define RS_DEBUG     0

/**
 * Structure to define reply state on the server
 * Reply state holds various reply message information. Also for "difficult"
 * replies (rep-ack case) we store the state after sending reply and wait
 * for the client to acknowledge the reception. In these cases locks could be
 * added to the state for replay/failover consistency guarantees.
 */
struct ptlrpc_reply_state {
	/** Callback description */
	struct ptlrpc_cb_id	rs_cb_id;
	/** Linkage for list of all reply states in a system */
	struct list_head	rs_list;
	/** Linkage for list of all reply states on same export */
	struct list_head	rs_exp_list;
	/** Linkage for list of all reply states for same obd */
	struct list_head	rs_obd_list;
#if RS_DEBUG
	struct list_head	rs_debug_list;
#endif
	/** A spinlock to protect the reply state flags */
	spinlock_t		rs_lock;
	/** Reply state flags */
        unsigned long          rs_difficult:1;     /* ACK/commit stuff */
        unsigned long          rs_no_ack:1;    /* no ACK, even for
                                                  difficult requests */
        unsigned long          rs_scheduled:1;     /* being handled? */
        unsigned long          rs_scheduled_ever:1;/* any schedule attempts? */
        unsigned long          rs_handled:1;  /* been handled yet? */
        unsigned long          rs_on_net:1;   /* reply_out_callback pending? */
        unsigned long          rs_prealloc:1; /* rs from prealloc list */
        unsigned long          rs_committed:1;/* the transaction was committed
                                                 and the rs was dispatched
                                                 by ptlrpc_commit_replies */
	unsigned long		rs_convert_lock:1; /* need to convert saved
						    * locks to COS mode */
	atomic_t		rs_refcount;	/* number of users */
	/** Number of locks awaiting client ACK */
	int			rs_nlocks;

        /** Size of the state */
        int                    rs_size;
        /** opcode */
        __u32                  rs_opc;
        /** Transaction number */
        __u64                  rs_transno;
        /** xid */
        __u64                  rs_xid;
	struct obd_export     *rs_export;
	struct ptlrpc_service_part *rs_svcpt;
	/** Lnet metadata handle for the reply */
	struct lnet_handle_md	rs_md_h;

	/** Context for the sevice thread */
	struct ptlrpc_svc_ctx	*rs_svc_ctx;
	/** Reply buffer (actually sent to the client), encoded if needed */
	struct lustre_msg	*rs_repbuf;	/* wrapper */
	/** Size of the reply buffer */
	int			rs_repbuf_len;	/* wrapper buf length */
	/** Size of the reply message */
	int			rs_repdata_len;	/* wrapper msg length */
	/**
	 * Actual reply message. Its content is encrupted (if needed) to
	 * produce reply buffer for actual sending. In simple case
	 * of no network encryption we jus set \a rs_repbuf to \a rs_msg
	 */
	struct lustre_msg	*rs_msg;	/* reply message */

	/** Handles of locks awaiting client reply ACK */
	struct lustre_handle	rs_locks[RS_MAX_LOCKS];
	/** Lock modes of locks in \a rs_locks */
	enum ldlm_mode		rs_modes[RS_MAX_LOCKS];
};

struct ptlrpc_thread;

/** RPC stages */
enum rq_phase {
	RQ_PHASE_NEW            = 0xebc0de00,
	RQ_PHASE_RPC            = 0xebc0de01,
	RQ_PHASE_BULK           = 0xebc0de02,
	RQ_PHASE_INTERPRET      = 0xebc0de03,
	RQ_PHASE_COMPLETE       = 0xebc0de04,
	RQ_PHASE_UNREG_RPC      = 0xebc0de05,
	RQ_PHASE_UNREG_BULK     = 0xebc0de06,
	RQ_PHASE_UNDEFINED      = 0xebc0de07
};

/** Type of request interpreter call-back */
typedef int (*ptlrpc_interpterer_t)(const struct lu_env *env,
                                    struct ptlrpc_request *req,
                                    void *arg, int rc);
/** Type of request resend call-back */
typedef void (*ptlrpc_resend_cb_t)(struct ptlrpc_request *req,
				   void *arg);

/**
 * Definition of request pool structure.
 * The pool is used to store empty preallocated requests for the case
 * when we would actually need to send something without performing
 * any allocations (to avoid e.g. OOM).
 */
struct ptlrpc_request_pool {
	/** Locks the list */
	spinlock_t		prp_lock;
	/** list of ptlrpc_request structs */
	struct list_head	prp_req_list;
	/** Maximum message size that would fit into a rquest from this pool */
	int			prp_rq_size;
	/** Function to allocate more requests for this pool */
	int (*prp_populate)(struct ptlrpc_request_pool *, int);
};

struct lu_context;
struct lu_env;

struct ldlm_lock;

#include <lustre_nrs.h>

/**
 * Basic request prioritization operations structure.
 * The whole idea is centered around locks and RPCs that might affect locks.
 * When a lock is contended we try to give priority to RPCs that might lead
 * to fastest release of that lock.
 * Currently only implemented for OSTs only in a way that makes all
 * IO and truncate RPCs that are coming from a locked region where a lock is
 * contended a priority over other requests.
 */
struct ptlrpc_hpreq_ops {
        /**
         * Check if the lock handle of the given lock is the same as
         * taken from the request.
         */
        int  (*hpreq_lock_match)(struct ptlrpc_request *, struct ldlm_lock *);
        /**
         * Check if the request is a high priority one.
         */
        int  (*hpreq_check)(struct ptlrpc_request *);
        /**
         * Called after the request has been handled.
         */
        void (*hpreq_fini)(struct ptlrpc_request *);
};

struct ptlrpc_cli_req {
	/** For bulk requests on client only: bulk descriptor */
	struct ptlrpc_bulk_desc		*cr_bulk;
	/** optional time limit for send attempts. This is a timeout
	 *  not a timestamp so timeout_t (s32) is used instead of time64_t
	 */
	timeout_t			 cr_delay_limit;
	/** time request was first queued */
	time64_t			 cr_queued_time;
	/** request sent in nanoseconds */
	ktime_t				 cr_sent_ns;
	/** time for request really sent out */
	time64_t			 cr_sent_out;
	/** when req reply unlink must finish. */
	time64_t			 cr_reply_deadline;
	/** when req bulk unlink must finish. */
	time64_t			 cr_bulk_deadline;
	/** when req unlink must finish. */
	time64_t			 cr_req_deadline;
	/** Portal to which this request would be sent */
	short				 cr_req_ptl;
	/** Portal where to wait for reply and where reply would be sent */
	short				 cr_rep_ptl;
	/** request resending number */
	unsigned int			 cr_resend_nr;
	/** What was import generation when this request was sent */
	int				 cr_imp_gen;
	enum lustre_imp_state		 cr_send_state;
	/** Per-request waitq introduced by bug 21938 for recovery waiting */
	wait_queue_head_t		 cr_set_waitq;
	/** Link item for request set lists */
	struct list_head		 cr_set_chain;
	/** link to waited ctx */
	struct list_head		 cr_ctx_chain;

	/** client's half ctx */
	struct ptlrpc_cli_ctx		*cr_cli_ctx;
	/** Link back to the request set */
	struct ptlrpc_request_set	*cr_set;
	/** outgoing request MD handle */
	struct lnet_handle_md		 cr_req_md_h;
	/** request-out callback parameter */
	struct ptlrpc_cb_id		 cr_req_cbid;
	/** incoming reply MD handle */
	struct lnet_handle_md		 cr_reply_md_h;
	wait_queue_head_t		 cr_reply_waitq;
	/** reply callback parameter */
	struct ptlrpc_cb_id		 cr_reply_cbid;
	/** Async completion handler, called when reply is received */
	ptlrpc_interpterer_t		 cr_reply_interp;
	/** Resend handler, called when request is resend to update RPC data */
	ptlrpc_resend_cb_t		 cr_resend_cb;
	/** Async completion context */
	union ptlrpc_async_args		 cr_async_args;
	/** Opaq data for replay and commit callbacks. */
	void				*cr_cb_data;
	/** Link to the imp->imp_unreplied_list */
	struct list_head		 cr_unreplied_list;
	/**
	 * Commit callback, called when request is committed and about to be
	 * freed.
	 */
	void (*cr_commit_cb)(struct ptlrpc_request *);
	/** Replay callback, called after request is replayed at recovery */
	void (*cr_replay_cb)(struct ptlrpc_request *);
};

/** client request member alias */
/* NB: these alias should NOT be used by any new code, instead they should
 * be removed step by step to avoid potential abuse */
#define rq_bulk			rq_cli.cr_bulk
#define rq_delay_limit		rq_cli.cr_delay_limit
#define rq_queued_time		rq_cli.cr_queued_time
#define rq_sent_ns		rq_cli.cr_sent_ns
#define rq_real_sent		rq_cli.cr_sent_out
#define rq_reply_deadline	rq_cli.cr_reply_deadline
#define rq_bulk_deadline	rq_cli.cr_bulk_deadline
#define rq_req_deadline		rq_cli.cr_req_deadline
#define rq_nr_resend		rq_cli.cr_resend_nr
#define rq_request_portal	rq_cli.cr_req_ptl
#define rq_reply_portal		rq_cli.cr_rep_ptl
#define rq_import_generation	rq_cli.cr_imp_gen
#define rq_send_state		rq_cli.cr_send_state
#define rq_set_chain		rq_cli.cr_set_chain
#define rq_ctx_chain		rq_cli.cr_ctx_chain
#define rq_set			rq_cli.cr_set
#define rq_set_waitq		rq_cli.cr_set_waitq
#define rq_cli_ctx		rq_cli.cr_cli_ctx
#define rq_req_md_h		rq_cli.cr_req_md_h
#define rq_req_cbid		rq_cli.cr_req_cbid
#define rq_reply_md_h		rq_cli.cr_reply_md_h
#define rq_reply_waitq		rq_cli.cr_reply_waitq
#define rq_reply_cbid		rq_cli.cr_reply_cbid
#define rq_interpret_reply	rq_cli.cr_reply_interp
#define rq_resend_cb		rq_cli.cr_resend_cb
#define rq_async_args		rq_cli.cr_async_args
#define rq_cb_data		rq_cli.cr_cb_data
#define rq_unreplied_list	rq_cli.cr_unreplied_list
#define rq_commit_cb		rq_cli.cr_commit_cb
#define rq_replay_cb		rq_cli.cr_replay_cb

struct ptlrpc_srv_req {
	/** initial thread servicing this request */
	struct ptlrpc_thread		*sr_svc_thread;
	/**
	 * Server side list of incoming unserved requests sorted by arrival
	 * time.  Traversed from time to time to notice about to expire
	 * requests and sent back "early replies" to clients to let them
	 * know server is alive and well, just very busy to service their
	 * requests in time
	 */
	struct list_head		 sr_timed_list;
	/** server-side per-export list */
	struct list_head		 sr_exp_list;
	/** server-side history, used for debuging purposes. */
	struct list_head		 sr_hist_list;
	/** history sequence # */
	__u64				 sr_hist_seq;
	/** the index of service's srv_at_array into which request is linked */
	__u32				 sr_at_index;
	/** authed uid */
	uid_t				 sr_auth_uid;
	/** authed uid mapped to */
	uid_t				 sr_auth_mapped_uid;
	/** RPC is generated from what part of Lustre */
	enum lustre_sec_part		 sr_sp_from;
	/** request session context */
	struct lu_context		 sr_ses;
	/** \addtogroup  nrs
	 * @{
	 */
	/** stub for NRS request */
	struct ptlrpc_nrs_request	 sr_nrq;
	/** @} nrs */
	/** request arrival time */
	struct timespec64		 sr_arrival_time;
	/** server's half ctx */
	struct ptlrpc_svc_ctx		*sr_svc_ctx;
	/** (server side), pointed directly into req buffer */
	struct ptlrpc_user_desc		*sr_user_desc;
	/** separated reply state, may be vmalloc'd */
	struct ptlrpc_reply_state	*sr_reply_state;
	/** server-side hp handlers */
	struct ptlrpc_hpreq_ops		*sr_ops;
	/** incoming request buffer */
	struct ptlrpc_request_buffer_desc *sr_rqbd;
};

/** server request member alias */
/* NB: these alias should NOT be used by any new code, instead they should
 * be removed step by step to avoid potential abuse */
#define rq_svc_thread		rq_srv.sr_svc_thread
#define rq_timed_list		rq_srv.sr_timed_list
#define rq_exp_list		rq_srv.sr_exp_list
#define rq_history_list		rq_srv.sr_hist_list
#define rq_history_seq		rq_srv.sr_hist_seq
#define rq_at_index		rq_srv.sr_at_index
#define rq_auth_uid		rq_srv.sr_auth_uid
#define rq_auth_mapped_uid	rq_srv.sr_auth_mapped_uid
#define rq_sp_from		rq_srv.sr_sp_from
#define rq_session		rq_srv.sr_ses
#define rq_nrq			rq_srv.sr_nrq
#define rq_arrival_time		rq_srv.sr_arrival_time
#define rq_reply_state		rq_srv.sr_reply_state
#define rq_svc_ctx		rq_srv.sr_svc_ctx
#define rq_user_desc		rq_srv.sr_user_desc
#define rq_ops			rq_srv.sr_ops
#define rq_rqbd			rq_srv.sr_rqbd
#define rq_reqmsg		rq_pill.rc_reqmsg
#define rq_repmsg		rq_pill.rc_repmsg
#define rq_req_swab_mask	rq_pill.rc_req_swab_mask
#define rq_rep_swab_mask	rq_pill.rc_rep_swab_mask

/**
 * Represents remote procedure call.
 *
 * This is a staple structure used by everybody wanting to send a request
 * in Lustre.
 */
struct ptlrpc_request {
	/* Request type: one of PTL_RPC_MSG_* */
	int				 rq_type;
	/** Result of request processing */
	int				 rq_status;
	/**
	 * Linkage item through which this request is included into
	 * sending/delayed lists on client and into rqbd list on server
	 */
	struct list_head		 rq_list;
	/** Lock to protect request flags and some other important bits, like
	 * rq_list
	 */
	spinlock_t			 rq_lock;
	spinlock_t			 rq_early_free_lock;
	/** client-side flags are serialized by rq_lock @{ */
	unsigned int rq_intr:1, rq_replied:1, rq_err:1,
                rq_timedout:1, rq_resend:1, rq_restart:1,
                /**
                 * when ->rq_replay is set, request is kept by the client even
                 * after server commits corresponding transaction. This is
                 * used for operations that require sequence of multiple
                 * requests to be replayed. The only example currently is file
                 * open/close. When last request in such a sequence is
                 * committed, ->rq_replay is cleared on all requests in the
                 * sequence.
                 */
                rq_replay:1,
                rq_no_resend:1, rq_waiting:1, rq_receiving_reply:1,
                rq_no_delay:1, rq_net_err:1, rq_wait_ctx:1,
		rq_early:1,
		rq_req_unlinked:1,	/* unlinked request buffer from lnet */
		rq_reply_unlinked:1,	/* unlinked reply buffer from lnet */
		rq_memalloc:1,      /* req originated from "kswapd" */
		rq_committed:1,
		rq_reply_truncated:1,
		/** whether the "rq_set" is a valid one */
		rq_invalid_rqset:1,
		rq_generation_set:1,
		/** do not resend request on -EINPROGRESS */
		rq_no_retry_einprogress:1,
		/* allow the req to be sent if the import is in recovery
		 * status */
		rq_allow_replay:1,
		/* bulk request, sent to server, but uncommitted */
		rq_unstable:1,
		rq_early_free_repbuf:1, /* free reply buffer in advance */
		rq_allow_intr:1;
	/** @} */

	/** server-side flags @{ */
	unsigned int
		rq_hp:1,		/**< high priority RPC */
		rq_at_linked:1,		/**< link into service's srv_at_array */
		rq_packed_final:1,	/**< packed final reply */
		rq_obsolete:1;		/* aborted by a signal on a client */
	/** @} */

	/** one of RQ_PHASE_* */
	enum rq_phase			 rq_phase;
	/** one of RQ_PHASE_* to be used next */
	enum rq_phase			 rq_next_phase;
	/**
	 * client-side refcount for SENT race, server-side refcounf
	 * for multiple replies
	 */
	atomic_t			 rq_refcount;
        /**
         * client-side:
         * !rq_truncate : # reply bytes actually received,
         *  rq_truncate : required repbuf_len for resend
         */
        int rq_nob_received;
        /** Request length */
        int rq_reqlen;
        /** Reply length */
        int rq_replen;
	/** Pool if request is from preallocated list */
	struct ptlrpc_request_pool	*rq_pool;
        /** Transaction number */
        __u64 rq_transno;
        /** xid */
        __u64				 rq_xid;
	/** bulk match bits */
	__u64				 rq_mbits;
	/** reply match bits */
	__u64				 rq_rep_mbits;
	/**
	 * List item to for replay list. Not yet committed requests get linked
	 * there.
	 * Also see \a rq_replay comment above.
	 * It's also link chain on obd_export::exp_req_replay_queue
	 */
	struct list_head		 rq_replay_list;
	/** non-shared members for client & server request*/
	union {
		struct ptlrpc_cli_req	 rq_cli;
		struct ptlrpc_srv_req	 rq_srv;
	};
	/**
	 * security and encryption data
	 * @{ */
	/** description of flavors for client & server */
	struct sptlrpc_flavor		 rq_flvr;

	/**
	 * SELinux policy info at the time of the request
	 * sepol string format is:
	 * <mode>:<policy name>:<policy version>:<policy hash>
	 */
	char rq_sepol[LUSTRE_NODEMAP_SEPOL_LENGTH + 1];

	/* client/server security flags */
	unsigned int
                                 rq_ctx_init:1,      /* context initiation */
                                 rq_ctx_fini:1,      /* context destroy */
                                 rq_bulk_read:1,     /* request bulk read */
                                 rq_bulk_write:1,    /* request bulk write */
                                 /* server authentication flags */
                                 rq_auth_gss:1,      /* authenticated by gss */
                                 rq_auth_usr_root:1, /* authed as root */
                                 rq_auth_usr_mdt:1,  /* authed as mdt */
                                 rq_auth_usr_ost:1,  /* authed as ost */
                                 /* security tfm flags */
                                 rq_pack_udesc:1,
                                 rq_pack_bulk:1,
                                 /* doesn't expect reply FIXME */
                                 rq_no_reply:1,
				 rq_pill_init:1, /* pill initialized */
				 rq_srv_req:1; /* server request */


	/** various buffer pointers */
	struct lustre_msg		*rq_reqbuf;  /**< req wrapper, vmalloc*/
	char				*rq_repbuf;  /**< rep buffer, vmalloc */
	struct lustre_msg		*rq_repdata; /**< rep wrapper msg */
	/** only in priv mode */
	struct lustre_msg		*rq_clrbuf;
        int                      rq_reqbuf_len;  /* req wrapper buf len */
        int                      rq_reqdata_len; /* req wrapper msg len */
        int                      rq_repbuf_len;  /* rep buffer len */
        int                      rq_repdata_len; /* rep wrapper msg len */
        int                      rq_clrbuf_len;  /* only in priv mode */
        int                      rq_clrdata_len; /* only in priv mode */

	/** early replies go to offset 0, regular replies go after that */
	unsigned int			 rq_reply_off;
	/** @} */

	/** how many early replies (for stats) */
	int				 rq_early_count;
	/** Server-side, export on which request was received */
	struct obd_export		*rq_export;
	/** import where request is being sent */
	struct obd_import		*rq_import;
	/** our LNet NID */
	lnet_nid_t			 rq_self;
	/** Peer description (the other side) */
	struct lnet_process_id		 rq_peer;
	/** Descriptor for the NID from which the peer sent the request. */
	struct lnet_process_id		 rq_source;
	/**
	 * service time estimate (secs)
	 * If the request is not served by this time, it is marked as timed out.
	 * Do not change to time64_t since this is transmitted over the wire.
	 *
	 * The linux kernel handles timestamps with time64_t and timeouts
	 * are normally done with jiffies. Lustre shares the rq_timeout between
	 * nodes. Since jiffies can vary from node to node Lustre instead
	 * will express the timeout value in seconds. To avoid confusion with
	 * timestamps (time64_t) and jiffy timeouts (long) Lustre timeouts
	 * are expressed in s32 (timeout_t). Also what is transmitted over
	 * the wire is 32 bits.
	 */
	timeout_t			 rq_timeout;
	/**
	 * when request/reply sent (secs), or time when request should be sent
	 */
	time64_t			 rq_sent;
	/** when request must finish. */
	time64_t			 rq_deadline;
	/** request format description */
	struct req_capsule		 rq_pill;
};

/**
 * Call completion handler for rpc if any, return it's status or original
 * rc if there was no handler defined for this request.
 */
static inline int ptlrpc_req_interpret(const struct lu_env *env,
                                       struct ptlrpc_request *req, int rc)
{
	if (req->rq_interpret_reply != NULL) {
		req->rq_status = req->rq_interpret_reply(env, req,
							 &req->rq_async_args,
							 rc);
		return req->rq_status;
	}

	return rc;
}

/** \addtogroup  nrs
 * @{
 */
void ptlrpc_nrs_req_hp_move(struct ptlrpc_request *req);

/*
 * Can the request be moved from the regular NRS head to the high-priority NRS
 * head (of the same PTLRPC service partition), if any?
 *
 * For a reliable result, this should be checked under svcpt->scp_req lock.
 */
static inline bool ptlrpc_nrs_req_can_move(struct ptlrpc_request *req)
{
	struct ptlrpc_nrs_request *nrq = &req->rq_nrq;

	/**
	 * LU-898: Check ptlrpc_nrs_request::nr_enqueued to make sure the
	 * request has been enqueued first, and ptlrpc_nrs_request::nr_started
	 * to make sure it has not been scheduled yet (analogous to previous
	 * (non-NRS) checking of !list_empty(&ptlrpc_request::rq_list).
	 */
	return nrq->nr_enqueued && !nrq->nr_started && !req->rq_hp;
}
/** @} nrs */

static inline bool req_capsule_ptlreq(struct req_capsule *pill)
{
	struct ptlrpc_request *req = pill->rc_req;

	return req != NULL && pill == &req->rq_pill;
}

static inline bool req_capsule_subreq(struct req_capsule *pill)
{
	struct ptlrpc_request *req = pill->rc_req;

	return req == NULL || pill != &req->rq_pill;
}

/**
 * Returns true if request needs to be swabbed into local cpu byteorder
 */
static inline bool req_capsule_req_need_swab(struct req_capsule *pill)
{
	struct ptlrpc_request *req = pill->rc_req;

	return req && req_capsule_req_swabbed(&req->rq_pill,
					      MSG_PTLRPC_HEADER_OFF);
}

/**
 * Returns true if request reply needs to be swabbed into local cpu byteorder
 */
static inline bool req_capsule_rep_need_swab(struct req_capsule *pill)
{
	struct ptlrpc_request *req = pill->rc_req;

	return req && req_capsule_rep_swabbed(&req->rq_pill,
					      MSG_PTLRPC_HEADER_OFF);
}

/**
 * Convert numerical request phase value \a phase into text string description
 */
static inline const char *
ptlrpc_phase2str(enum rq_phase phase)
{
	switch (phase) {
	case RQ_PHASE_NEW:
		return "New";
	case RQ_PHASE_RPC:
		return "Rpc";
	case RQ_PHASE_BULK:
		return "Bulk";
	case RQ_PHASE_INTERPRET:
		return "Interpret";
	case RQ_PHASE_COMPLETE:
		return "Complete";
	case RQ_PHASE_UNREG_RPC:
		return "UnregRPC";
	case RQ_PHASE_UNREG_BULK:
		return "UnregBULK";
	default:
		return "?Phase?";
	}
}

/**
 * Convert numerical request phase of the request \a req into text stringi
 * description
 */
static inline const char *
ptlrpc_rqphase2str(struct ptlrpc_request *req)
{
        return ptlrpc_phase2str(req->rq_phase);
}

/**
 * Debugging functions and helpers to print request structure into debug log
 * @{
 */
/* Spare the preprocessor, spoil the bugs. */
#define FLAG(field, str) (field ? str : "")

/** Convert bit flags into a string */
#define DEBUG_REQ_FLAGS(req)                                                   \
	ptlrpc_rqphase2str(req),                                               \
	FLAG(req->rq_intr, "I"), FLAG(req->rq_replied, "R"),                   \
	FLAG(req->rq_err, "E"), FLAG(req->rq_net_err, "e"),                    \
	FLAG(req->rq_timedout, "X") /* eXpired */, FLAG(req->rq_resend, "S"),  \
	FLAG(req->rq_restart, "T"), FLAG(req->rq_replay, "P"),                 \
	FLAG(req->rq_no_resend, "N"), FLAG(req->rq_no_reply, "n"),            \
	FLAG(req->rq_waiting, "W"),                                            \
	FLAG(req->rq_wait_ctx, "C"), FLAG(req->rq_hp, "H"),                    \
	FLAG(req->rq_committed, "M"),                                          \
	FLAG(req->rq_req_unlinked, "Q"),                                       \
	FLAG(req->rq_reply_unlinked, "U"),                                     \
	FLAG(req->rq_receiving_reply, "r")

#define REQ_FLAGS_FMT "%s:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s"

void _debug_req(struct ptlrpc_request *req,
                struct libcfs_debug_msg_data *data, const char *fmt, ...)
        __attribute__ ((format (printf, 3, 4)));

/**
 * Helper that decides if we need to print request accordig to current debug
 * level settings
 */
#define debug_req(msgdata, mask, cdls, req, fmt, a...)                        \
do {                                                                          \
        CFS_CHECK_STACK(msgdata, mask, cdls);                                 \
                                                                              \
        if (((mask) & D_CANTMASK) != 0 ||                                     \
            ((libcfs_debug & (mask)) != 0 &&                                  \
             (libcfs_subsystem_debug & DEBUG_SUBSYSTEM) != 0))                \
                _debug_req((req), msgdata, fmt, ##a);                         \
} while(0)

/**
 * This is the debug print function you need to use to print request sturucture
 * content into lustre debug log.
 * for most callers (level is a constant) this is resolved at compile time */
#define DEBUG_REQ(level, req, fmt, args...)                                   \
do {                                                                          \
        if ((level) & (D_ERROR | D_WARNING)) {                                \
		static struct cfs_debug_limit_state cdls;		      \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, level, &cdls);            \
                debug_req(&msgdata, level, &cdls, req, "@@@ "fmt" ", ## args);\
        } else {                                                              \
                LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, level, NULL);             \
                debug_req(&msgdata, level, NULL, req, "@@@ "fmt" ", ## args); \
        }                                                                     \
} while (0)
/** @} */

enum ptlrpc_bulk_op_type {
	PTLRPC_BULK_OP_ACTIVE =	 0x00000001,
	PTLRPC_BULK_OP_PASSIVE = 0x00000002,
	PTLRPC_BULK_OP_PUT =	 0x00000004,
	PTLRPC_BULK_OP_GET =	 0x00000008,
	PTLRPC_BULK_GET_SOURCE = PTLRPC_BULK_OP_PASSIVE | PTLRPC_BULK_OP_GET,
	PTLRPC_BULK_PUT_SINK =	 PTLRPC_BULK_OP_PASSIVE | PTLRPC_BULK_OP_PUT,
	PTLRPC_BULK_GET_SINK =	 PTLRPC_BULK_OP_ACTIVE | PTLRPC_BULK_OP_GET,
	PTLRPC_BULK_PUT_SOURCE = PTLRPC_BULK_OP_ACTIVE | PTLRPC_BULK_OP_PUT,
};

static inline bool ptlrpc_is_bulk_op_get(enum ptlrpc_bulk_op_type type)
{
	return (type & PTLRPC_BULK_OP_GET) == PTLRPC_BULK_OP_GET;
}

static inline bool ptlrpc_is_bulk_get_source(enum ptlrpc_bulk_op_type type)
{
	return (type & PTLRPC_BULK_GET_SOURCE) == PTLRPC_BULK_GET_SOURCE;
}

static inline bool ptlrpc_is_bulk_put_sink(enum ptlrpc_bulk_op_type type)
{
	return (type & PTLRPC_BULK_PUT_SINK) == PTLRPC_BULK_PUT_SINK;
}

static inline bool ptlrpc_is_bulk_get_sink(enum ptlrpc_bulk_op_type type)
{
	return (type & PTLRPC_BULK_GET_SINK) == PTLRPC_BULK_GET_SINK;
}

static inline bool ptlrpc_is_bulk_put_source(enum ptlrpc_bulk_op_type type)
{
	return (type & PTLRPC_BULK_PUT_SOURCE) == PTLRPC_BULK_PUT_SOURCE;
}

static inline bool ptlrpc_is_bulk_op_active(enum ptlrpc_bulk_op_type type)
{
	return ((type & PTLRPC_BULK_OP_ACTIVE) |
		(type & PTLRPC_BULK_OP_PASSIVE))
			== PTLRPC_BULK_OP_ACTIVE;
}

static inline bool ptlrpc_is_bulk_op_passive(enum ptlrpc_bulk_op_type type)
{
	return ((type & PTLRPC_BULK_OP_ACTIVE) |
		(type & PTLRPC_BULK_OP_PASSIVE))
			== PTLRPC_BULK_OP_PASSIVE;
}

struct ptlrpc_bulk_frag_ops {
	/**
	 * Add a page \a page to the bulk descriptor \a desc
	 * Data to transfer in the page starts at offset \a pageoffset and
	 * amount of data to transfer from the page is \a len
	 */
	void (*add_kiov_frag)(struct ptlrpc_bulk_desc *desc,
			      struct page *page, int pageoffset, int len);

	/*
	 * Add a \a fragment to the bulk descriptor \a desc.
	 * Data to transfer in the fragment is pointed to by \a frag
	 * The size of the fragment is \a len
	 */
	int (*add_iov_frag)(struct ptlrpc_bulk_desc *desc, void *frag, int len);

	/**
	 * Uninitialize and free bulk descriptor \a desc.
	 * Works on bulk descriptors both from server and client side.
	 */
	void (*release_frags)(struct ptlrpc_bulk_desc *desc);
};

extern const struct ptlrpc_bulk_frag_ops ptlrpc_bulk_kiov_pin_ops;
extern const struct ptlrpc_bulk_frag_ops ptlrpc_bulk_kiov_nopin_ops;

/*
 * Definition of bulk descriptor.
 * Bulks are special "Two phase" RPCs where initial request message
 * is sent first and it is followed bt a transfer (o receiving) of a large
 * amount of data to be settled into pages referenced from the bulk descriptors.
 * Bulks transfers (the actual data following the small requests) are done
 * on separate LNet portals.
 * In lustre we use bulk transfers for READ and WRITE transfers from/to OSTs.
 *  Another user is readpage for MDT.
 */
struct ptlrpc_bulk_desc {
	unsigned int	bd_refs; /* number MD's assigned including zero-sends */
	/** completed with failure */
	unsigned long bd_failure:1;
	/** client side */
	unsigned long bd_registered:1;
	/** For serialization with callback */
	spinlock_t bd_lock;
	/** {put,get}{source,sink}{kvec,kiov} */
	enum ptlrpc_bulk_op_type bd_type;
	/** LNet portal for this bulk */
	__u32 bd_portal;
	/** Server side - export this bulk created for */
	struct obd_export *bd_export;
	/** Client side - import this bulk was sent on */
	struct obd_import *bd_import;
	/** Back pointer to the request */
	struct ptlrpc_request *bd_req;
	const struct ptlrpc_bulk_frag_ops *bd_frag_ops;
	wait_queue_head_t      bd_waitq;        /* server side only WQ */
	int                    bd_iov_count;    /* # entries in bd_iov */
	int                    bd_max_iov;      /* allocated size of bd_iov */
	int                    bd_nob;          /* # bytes covered */
	int                    bd_nob_transferred; /* # bytes GOT/PUT */
	unsigned int		bd_nob_last;	/* # bytes in last MD */

	__u64                  bd_last_mbits;

	struct ptlrpc_cb_id    bd_cbid;         /* network callback info */
	lnet_nid_t             bd_sender;       /* stash event::sender */
	int			bd_md_count;	/* # valid entries in bd_mds */
	int			bd_md_max_brw;	/* max entries in bd_mds */

	/** array of offsets for each MD */
	unsigned int		bd_mds_off[PTLRPC_BULK_OPS_COUNT];
	/** array of associated MDs */
	struct lnet_handle_md	bd_mds[PTLRPC_BULK_OPS_COUNT];

	/* encrypted iov, size is either 0 or bd_iov_count. */
	struct bio_vec *bd_enc_vec;
	struct bio_vec *bd_vec;
};

enum {
	SVC_INIT	= 0,
	SVC_STOPPED	= BIT(0),
	SVC_STOPPING	= BIT(1),
	SVC_STARTING	= BIT(2),
	SVC_RUNNING	= BIT(3),
};

#define PTLRPC_THR_NAME_LEN		32
/**
 * Definition of server service thread structure
 */
struct ptlrpc_thread {
	/**
	 * List of active threads in svcpt->scp_threads
	 */
	struct list_head t_link;
	/**
	 * thread-private data (preallocated vmalloc'd memory)
	 */
	void *t_data;
	__u32 t_flags;
	/**
	 * service thread index, from ptlrpc_start_threads
	 */
	unsigned int t_id;
	/**
	 * service thread
	 */
	struct task_struct *t_task;
	pid_t t_pid;
	ktime_t t_touched;
	/**
	 * put watchdog in the structure per thread b=14840
	 */
	struct delayed_work t_watchdog;
	/**
	 * the svc this thread belonged to b=18582
	 */
	struct ptlrpc_service_part	*t_svcpt;
	wait_queue_head_t		t_ctl_waitq;
	struct lu_env			*t_env;
	char				t_name[PTLRPC_THR_NAME_LEN];
};

static inline int thread_is_init(struct ptlrpc_thread *thread)
{
	return thread->t_flags == 0;
}

static inline int thread_is_stopped(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_STOPPED);
}

static inline int thread_is_stopping(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_STOPPING);
}

static inline int thread_is_starting(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_STARTING);
}

static inline int thread_is_running(struct ptlrpc_thread *thread)
{
        return !!(thread->t_flags & SVC_RUNNING);
}

static inline void thread_clear_flags(struct ptlrpc_thread *thread, __u32 flags)
{
        thread->t_flags &= ~flags;
}

static inline void thread_set_flags(struct ptlrpc_thread *thread, __u32 flags)
{
        thread->t_flags = flags;
}

static inline void thread_add_flags(struct ptlrpc_thread *thread, __u32 flags)
{
        thread->t_flags |= flags;
}

static inline int thread_test_and_clear_flags(struct ptlrpc_thread *thread,
                                              __u32 flags)
{
        if (thread->t_flags & flags) {
                thread->t_flags &= ~flags;
                return 1;
        }
        return 0;
}

/**
 * Request buffer descriptor structure.
 * This is a structure that contains one posted request buffer for service.
 * Once data land into a buffer, event callback creates actual request and
 * notifies wakes one of the service threads to process new incoming request.
 * More than one request can fit into the buffer.
 */
struct ptlrpc_request_buffer_desc {
	/** Link item for rqbds on a service */
	struct list_head		rqbd_list;
	/** History of requests for this buffer */
	struct list_head		rqbd_reqs;
	/** Back pointer to service for which this buffer is registered */
	struct ptlrpc_service_part	*rqbd_svcpt;
	/** LNet descriptor */
	struct lnet_handle_md		rqbd_md_h;
	int				rqbd_refcount;
	/** The buffer itself */
	char				*rqbd_buffer;
	struct ptlrpc_cb_id		rqbd_cbid;
	/**
	 * This "embedded" request structure is only used for the
	 * last request to fit into the buffer
	 */
	struct ptlrpc_request		rqbd_req;
};

typedef int  (*svc_handler_t)(struct ptlrpc_request *req);

struct ptlrpc_service_ops {
	/**
	 * if non-NULL called during thread creation (ptlrpc_start_thread())
	 * to initialize service specific per-thread state.
	 */
	int		(*so_thr_init)(struct ptlrpc_thread *thr);
	/**
	 * if non-NULL called during thread shutdown (ptlrpc_main()) to
	 * destruct state created by ->srv_init().
	 */
	void		(*so_thr_done)(struct ptlrpc_thread *thr);
	/**
	 * Handler function for incoming requests for this service
	 */
	int		(*so_req_handler)(struct ptlrpc_request *req);
	/**
	 * function to determine priority of the request, it's called
	 * on every new request
	 */
	int		(*so_hpreq_handler)(struct ptlrpc_request *);
	/**
	 * service-specific print fn
	 */
	void		(*so_req_printer)(void *, struct ptlrpc_request *);
};

#ifndef __cfs_cacheline_aligned
/* NB: put it here for reducing patche dependence */
# define __cfs_cacheline_aligned
#endif

/**
 * How many high priority requests to serve before serving one normal
 * priority request
 */
#define PTLRPC_SVC_HP_RATIO 10

/**
 * Definition of PortalRPC service.
 * The service is listening on a particular portal (like tcp port)
 * and perform actions for a specific server like IO service for OST
 * or general metadata service for MDS.
 */
struct ptlrpc_service {
	/** serialize /proc operations */
	spinlock_t			srv_lock;
	/** most often accessed fields */
	/** chain thru all services */
	struct list_head		srv_list;
	/** service operations table */
	struct ptlrpc_service_ops	srv_ops;
        /** only statically allocated strings here; we don't clean them */
        char                           *srv_name;
        /** only statically allocated strings here; we don't clean them */
        char                           *srv_thread_name;
	/** threads # should be created for each partition on initializing */
	int				srv_nthrs_cpt_init;
	/** limit of threads number for each partition */
	int				srv_nthrs_cpt_limit;
	/** Root of debugfs dir tree for this service */
	struct dentry		       *srv_debugfs_entry;
        /** Pointer to statistic data for this service */
        struct lprocfs_stats           *srv_stats;
        /** # hp per lp reqs to handle */
        int                             srv_hpreq_ratio;
        /** biggest request to receive */
        int                             srv_max_req_size;
        /** biggest reply to send */
        int                             srv_max_reply_size;
        /** size of individual buffers */
        int                             srv_buf_size;
        /** # buffers to allocate in 1 group */
        int                             srv_nbuf_per_group;
        /** Local portal on which to receive requests */
        __u32                           srv_req_portal;
        /** Portal on the client to send replies to */
        __u32                           srv_rep_portal;
        /**
         * Tags for lu_context associated with this thread, see struct
         * lu_context.
         */
        __u32                           srv_ctx_tags;
        /** soft watchdog timeout multiplier */
        int                             srv_watchdog_factor;
        /** under unregister_service */
        unsigned                        srv_is_stopping:1;
	/** Whether or not to restrict service threads to CPUs in this CPT */
	unsigned			srv_cpt_bind:1;

	/** max # request buffers */
	int				srv_nrqbds_max;
	/** max # request buffers in history per partition */
	int				srv_hist_nrqbds_cpt_max;
	/** number of CPTs this service associated with */
	int				srv_ncpts;
	/** CPTs array this service associated with */
	__u32				*srv_cpts;
	/** 2^srv_cptab_bits >= cfs_cpt_numbert(srv_cptable) */
	int				srv_cpt_bits;
	/** CPT table this service is running over */
	struct cfs_cpt_table		*srv_cptable;

	/* sysfs object */
	struct kobject			srv_kobj;
	struct completion		srv_kobj_unregister;
	/**
	 * partition data for ptlrpc service
	 */
	struct ptlrpc_service_part	*srv_parts[0];
};

/**
 * Definition of PortalRPC service partition data.
 * Although a service only has one instance of it right now, but we
 * will have multiple instances very soon (instance per CPT).
 *
 * it has four locks:
 * \a scp_lock
 *    serialize operations on rqbd and requests waiting for preprocess
 * \a scp_req_lock
 *    serialize operations active requests sent to this portal
 * \a scp_at_lock
 *    serialize adaptive timeout stuff
 * \a scp_rep_lock
 *    serialize operations on RS list (reply states)
 *
 * We don't have any use-case to take two or more locks at the same time
 * for now, so there is no lock order issue.
 */
struct ptlrpc_service_part {
	/** back reference to owner */
	struct ptlrpc_service		*scp_service __cfs_cacheline_aligned;
	/* CPT id, reserved */
	int				scp_cpt;
	/** always increasing number */
	int				scp_thr_nextid;
	/** # of starting threads */
	int				scp_nthrs_starting;
	/** # running threads */
	int				scp_nthrs_running;
	/** service threads list */
	struct list_head		scp_threads;

	/**
	 * serialize the following fields, used for protecting
	 * rqbd list and incoming requests waiting for preprocess,
	 * threads starting & stopping are also protected by this lock.
	 */
	spinlock_t			scp_lock  __cfs_cacheline_aligned;
	/** userland serialization */
	struct mutex			scp_mutex;
	/** total # req buffer descs allocated */
	int				scp_nrqbds_total;
	/** # posted request buffers for receiving */
	int				scp_nrqbds_posted;
	/** in progress of allocating rqbd */
	int				scp_rqbd_allocating;
	/** # incoming reqs */
	int				scp_nreqs_incoming;
	/** request buffers to be reposted */
	struct list_head		scp_rqbd_idle;
	/** req buffers receiving */
	struct list_head		scp_rqbd_posted;
	/** incoming reqs */
	struct list_head		scp_req_incoming;
	/** timeout before re-posting reqs, in jiffies */
	long				scp_rqbd_timeout;
	/**
	 * all threads sleep on this. This wait-queue is signalled when new
	 * incoming request arrives and when difficult reply has to be handled.
	 */
	wait_queue_head_t		scp_waitq;

	/** request history */
	struct list_head		scp_hist_reqs;
	/** request buffer history */
	struct list_head		scp_hist_rqbds;
	/** # request buffers in history */
	int				scp_hist_nrqbds;
	/** sequence number for request */
	__u64				scp_hist_seq;
	/** highest seq culled from history */
	__u64				scp_hist_seq_culled;

	/**
	 * serialize the following fields, used for processing requests
	 * sent to this portal
	 */
	spinlock_t			scp_req_lock __cfs_cacheline_aligned;
	/** # reqs in either of the NRS heads below */
	/** # reqs being served */
	int				scp_nreqs_active;
	/** # HPreqs being served */
	int				scp_nhreqs_active;
	/** # hp requests handled */
	int				scp_hreq_count;

	/** NRS head for regular requests */
	struct ptlrpc_nrs		scp_nrs_reg;
	/** NRS head for HP requests; this is only valid for services that can
	 *  handle HP requests */
	struct ptlrpc_nrs	       *scp_nrs_hp;

	/** AT stuff */
	/** @{ */
	/**
	 * serialize the following fields, used for changes on
	 * adaptive timeout
	 */
	spinlock_t			scp_at_lock __cfs_cacheline_aligned;
	/** estimated rpc service time */
	struct adaptive_timeout		scp_at_estimate;
	/** reqs waiting for replies */
	struct ptlrpc_at_array		scp_at_array;
	/** early reply timer */
	struct timer_list		scp_at_timer;
	/** debug */
	ktime_t				scp_at_checktime;
	/** check early replies */
	unsigned			scp_at_check;
	/** @} */

	/**
	 * serialize the following fields, used for processing
	 * replies for this portal
	 */
	spinlock_t			scp_rep_lock __cfs_cacheline_aligned;
	/** all the active replies */
	struct list_head		scp_rep_active;
	/** List of free reply_states */
	struct list_head		scp_rep_idle;
	/** waitq to run, when adding stuff to srv_free_rs_list */
	wait_queue_head_t		scp_rep_waitq;
	/** # 'difficult' replies */
	atomic_t			scp_nreps_difficult;
};

#define ptlrpc_service_for_each_part(part, i, svc)			\
	for (i = 0;							\
	     i < (svc)->srv_ncpts &&					\
	     (svc)->srv_parts != NULL &&				\
	     ((part) = (svc)->srv_parts[i]) != NULL; i++)

/**
 * Declaration of ptlrpcd control structure
 */
struct ptlrpcd_ctl {
	/**
	 * Ptlrpc thread control flags (LIOD_START, LIOD_STOP, LIOD_FORCE)
	 */
	unsigned long			pc_flags;
	/**
	 * Thread lock protecting structure fields.
	 */
	spinlock_t			pc_lock;
	/**
	 * Start completion.
	 */
	struct completion		pc_starting;
	/**
	 * Stop completion.
	 */
	struct completion		pc_finishing;
	/**
	 * Thread requests set.
	 */
	struct ptlrpc_request_set	*pc_set;
	/**
	 * Thread name used in kthread_run()
	 */
	char				pc_name[16];
	/**
	 * CPT the thread is bound on.
	 */
	int				pc_cpt;
        /**
         * Index of ptlrpcd thread in the array.
         */
	int				pc_index;
	/**
	 * Pointer to the array of partners' ptlrpcd_ctl structure.
	 */
	struct ptlrpcd_ctl		**pc_partners;
	/**
	 * Number of the ptlrpcd's partners.
	 */
	int				pc_npartners;
	/**
	 * Record the partner index to be processed next.
	 */
	int				pc_cursor;
	/**
	 * Error code if the thread failed to fully start.
	 */
	int				pc_error;
};

/* Bits for pc_flags */
enum ptlrpcd_ctl_flags {
	/**
	 * Ptlrpc thread start flag.
	 */
	LIOD_START	= BIT(0),
	/**
	 * Ptlrpc thread stop flag.
	 */
	LIOD_STOP	= BIT(1),
	/**
	 * Ptlrpc thread force flag (only stop force so far).
	 * This will cause aborting any inflight rpcs handled
	 * by thread if LIOD_STOP is specified.
	 */
	LIOD_FORCE	= BIT(2),
	/**
	 * This is a recovery ptlrpc thread.
	 */
	LIOD_RECOVERY	= BIT(3),
};

/**
 * \addtogroup nrs
 * @{
 *
 * Service compatibility function; the policy is compatible with all services.
 *
 * \param[in] svc  The service the policy is attempting to register with.
 * \param[in] desc The policy descriptor
 *
 * \retval true The policy is compatible with the service
 *
 * \see ptlrpc_nrs_pol_desc::pd_compat()
 */
static inline bool nrs_policy_compat_all(const struct ptlrpc_service *svc,
					 const struct ptlrpc_nrs_pol_desc *desc)
{
	return true;
}

/**
 * Service compatibility function; the policy is compatible with only a specific
 * service which is identified by its human-readable name at
 * ptlrpc_service::srv_name.
 *
 * \param[in] svc  The service the policy is attempting to register with.
 * \param[in] desc The policy descriptor
 *
 * \retval false The policy is not compatible with the service
 * \retval true	 The policy is compatible with the service
 *
 * \see ptlrpc_nrs_pol_desc::pd_compat()
 */
static inline bool nrs_policy_compat_one(const struct ptlrpc_service *svc,
					 const struct ptlrpc_nrs_pol_desc *desc)
{
	LASSERT(desc->pd_compat_svc_name != NULL);
	return strcmp(svc->srv_name, desc->pd_compat_svc_name) == 0;
}

/** @} nrs */

/* ptlrpc/events.c */
extern int ptlrpc_uuid_to_peer(struct obd_uuid *uuid,
			       struct lnet_process_id *peer, lnet_nid_t *self);
/**
 * These callbacks are invoked by LNet when something happened to
 * underlying buffer
 * @{
 */
extern void request_out_callback(struct lnet_event *ev);
extern void reply_in_callback(struct lnet_event *ev);
extern void client_bulk_callback(struct lnet_event *ev);
extern void request_in_callback(struct lnet_event *ev);
extern void reply_out_callback(struct lnet_event *ev);
#ifdef HAVE_SERVER_SUPPORT
extern void server_bulk_callback(struct lnet_event *ev);
#endif
/** @} */

/* ptlrpc/connection.c */
struct ptlrpc_connection *ptlrpc_connection_get(struct lnet_process_id peer,
                                                lnet_nid_t self,
                                                struct obd_uuid *uuid);

static inline void  ptlrpc_connection_put(struct ptlrpc_connection *conn)
{
	if (!conn)
		return;

	LASSERT(atomic_read(&conn->c_refcount) > 0);

	/*
	 * We do not remove connection from hashtable and
	 * do not free it even if last caller released ref,
	 * as we want to have it cached for the case it is
	 * needed again.
	 *
	 * Deallocating it and later creating new connection
	 * again would be wastful. This way we also avoid
	 * expensive locking to protect things from get/put
	 * race when found cached connection is freed by
	 * ptlrpc_connection_put().
	 *
	 * It will be freed later in module unload time,
	 * when ptlrpc_connection_fini()->lh_exit->conn_exit()
	 * path is called.
	 */
	atomic_dec(&conn->c_refcount);

	CDEBUG(D_INFO, "PUT conn=%p refcount %d to %s\n",
	       conn, atomic_read(&conn->c_refcount),
	       libcfs_nid2str(conn->c_peer.nid));
}

struct ptlrpc_connection *ptlrpc_connection_addref(struct ptlrpc_connection *);
int ptlrpc_connection_init(void);
void ptlrpc_connection_fini(void);
extern lnet_pid_t ptl_get_pid(void);

/*
 * Check if the peer connection is on the local node.  We need to use GFP_NOFS
 * for requests from a local client to avoid recursing into the filesystem
 * as we might end up waiting on a page sent in the request we're serving.
 *
 * Use __GFP_HIGHMEM so that the pages can use all of the available memory
 * on 32-bit machines.  Use more aggressive GFP_HIGHUSER flags from non-local
 * clients to be able to generate more memory pressure on the OSS and allow
 * inactive pages to be reclaimed, since it doesn't have any other processes
 * or allocations that generate memory reclaim pressure.
 *
 * See b=17576 (bdf50dc9) and b=19529 (3dcf18d3) for details.
 */
static inline bool ptlrpc_connection_is_local(struct ptlrpc_connection *conn)
{
	if (!conn)
		return false;

	if (conn->c_peer.nid == conn->c_self)
		return true;

	RETURN(LNetIsPeerLocal(conn->c_peer.nid));
}

/* ptlrpc/niobuf.c */
/**
 * Actual interfacing with LNet to put/get/register/unregister stuff
 * @{
 */
#ifdef HAVE_SERVER_SUPPORT
struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_exp(struct ptlrpc_request *req,
					      unsigned nfrags, unsigned max_brw,
					      unsigned int type,
					      unsigned portal,
					      const struct ptlrpc_bulk_frag_ops
						*ops);
int ptlrpc_start_bulk_transfer(struct ptlrpc_bulk_desc *desc);
void ptlrpc_abort_bulk(struct ptlrpc_bulk_desc *desc);

static inline int ptlrpc_server_bulk_active(struct ptlrpc_bulk_desc *desc)
{
	int rc;

	LASSERT(desc != NULL);

	spin_lock(&desc->bd_lock);
	rc = desc->bd_refs;
	spin_unlock(&desc->bd_lock);
	return rc;
}
#endif

int ptlrpc_register_bulk(struct ptlrpc_request *req);
int ptlrpc_unregister_bulk(struct ptlrpc_request *req, int async);

static inline int ptlrpc_client_bulk_active(struct ptlrpc_request *req)
{
	struct ptlrpc_bulk_desc *desc;
	int rc;

	LASSERT(req != NULL);
	desc = req->rq_bulk;

	if (!desc)
		return 0;

	if (req->rq_bulk_deadline > ktime_get_real_seconds())
		return 1;


	spin_lock(&desc->bd_lock);
	rc = desc->bd_refs;
	spin_unlock(&desc->bd_lock);
	return rc;
}

#define PTLRPC_REPLY_MAYBE_DIFFICULT 0x01
#define PTLRPC_REPLY_EARLY           0x02
int ptlrpc_send_reply(struct ptlrpc_request *req, int flags);
int ptlrpc_reply(struct ptlrpc_request *req);
int ptlrpc_send_error(struct ptlrpc_request *req, int difficult);
int ptlrpc_error(struct ptlrpc_request *req);
int ptlrpc_at_get_net_latency(struct ptlrpc_request *req);
int ptl_send_rpc(struct ptlrpc_request *request, int noreply);
int ptlrpc_register_rqbd(struct ptlrpc_request_buffer_desc *rqbd);
/** @} */

/* ptlrpc/client.c */
/**
 * Client-side portals API. Everything to send requests, receive replies,
 * request queues, request management, etc.
 * @{
 */
void ptlrpc_request_committed(struct ptlrpc_request *req, int force);

void ptlrpc_init_client(int req_portal, int rep_portal, const char *name,
                        struct ptlrpc_client *);
void ptlrpc_cleanup_client(struct obd_import *imp);
struct ptlrpc_connection *ptlrpc_uuid_to_connection(struct obd_uuid *uuid,
						    lnet_nid_t nid4refnet);

int ptlrpc_queue_wait(struct ptlrpc_request *req);
int ptlrpc_replay_req(struct ptlrpc_request *req);
void ptlrpc_restart_req(struct ptlrpc_request *req);
void ptlrpc_abort_inflight(struct obd_import *imp);
void ptlrpc_cleanup_imp(struct obd_import *imp);
void ptlrpc_abort_set(struct ptlrpc_request_set *set);

struct ptlrpc_request_set *ptlrpc_prep_set(void);
struct ptlrpc_request_set *ptlrpc_prep_fcset(int max, set_producer_func func,
					     void *arg);
int ptlrpc_check_set(const struct lu_env *env, struct ptlrpc_request_set *set);
int ptlrpc_set_wait(const struct lu_env *env, struct ptlrpc_request_set *);
void ptlrpc_set_destroy(struct ptlrpc_request_set *);
void ptlrpc_set_add_req(struct ptlrpc_request_set *, struct ptlrpc_request *);
#define PTLRPCD_SET ((struct ptlrpc_request_set *)1)

void ptlrpc_free_rq_pool(struct ptlrpc_request_pool *pool);
int ptlrpc_add_rqs_to_pool(struct ptlrpc_request_pool *pool, int num_rq);

struct ptlrpc_request_pool *
ptlrpc_init_rq_pool(int, int,
		    int (*populate_pool)(struct ptlrpc_request_pool *, int));

void ptlrpc_at_set_req_timeout(struct ptlrpc_request *req);
struct ptlrpc_request *ptlrpc_request_alloc(struct obd_import *imp,
                                            const struct req_format *format);
struct ptlrpc_request *ptlrpc_request_alloc_pool(struct obd_import *imp,
                                            struct ptlrpc_request_pool *,
                                            const struct req_format *format);
void ptlrpc_request_free(struct ptlrpc_request *request);
int ptlrpc_request_pack(struct ptlrpc_request *request,
                        __u32 version, int opcode);
struct ptlrpc_request *ptlrpc_request_alloc_pack(struct obd_import *imp,
                                                const struct req_format *format,
                                                __u32 version, int opcode);
int ptlrpc_request_bufs_pack(struct ptlrpc_request *request,
                             __u32 version, int opcode, char **bufs,
                             struct ptlrpc_cli_ctx *ctx);
void ptlrpc_req_finished(struct ptlrpc_request *request);
void ptlrpc_req_finished_with_imp_lock(struct ptlrpc_request *request);
struct ptlrpc_request *ptlrpc_request_addref(struct ptlrpc_request *req);
struct ptlrpc_bulk_desc *ptlrpc_prep_bulk_imp(struct ptlrpc_request *req,
					      unsigned nfrags, unsigned max_brw,
					      unsigned int type,
					      unsigned portal,
					      const struct ptlrpc_bulk_frag_ops
						*ops);

void __ptlrpc_prep_bulk_page(struct ptlrpc_bulk_desc *desc,
			     struct page *page, int pageoffset, int len,
			     int pin);

void ptlrpc_free_bulk(struct ptlrpc_bulk_desc *bulk);

static inline void ptlrpc_release_bulk_noop(struct ptlrpc_bulk_desc *desc)
{
}

void ptlrpc_retain_replayable_request(struct ptlrpc_request *req,
                                      struct obd_import *imp);
__u64 ptlrpc_next_xid(void);
__u64 ptlrpc_sample_next_xid(void);
__u64 ptlrpc_req_xid(struct ptlrpc_request *request);
void ptlrpc_get_mod_rpc_slot(struct ptlrpc_request *req);
void ptlrpc_put_mod_rpc_slot(struct ptlrpc_request *req);

/* Set of routines to run a function in ptlrpcd context */
void *ptlrpcd_alloc_work(struct obd_import *imp,
                         int (*cb)(const struct lu_env *, void *), void *data);
void ptlrpcd_destroy_work(void *handler);
int ptlrpcd_queue_work(void *handler);

/** @} */
struct ptlrpc_service_buf_conf {
	/* nbufs is buffers # to allocate when growing the pool */
	unsigned int			bc_nbufs;
	/* buffer size to post */
	unsigned int			bc_buf_size;
	/* portal to listed for requests on */
	unsigned int			bc_req_portal;
	/* portal of where to send replies to */
	unsigned int			bc_rep_portal;
	/* maximum request size to be accepted for this service */
	unsigned int			bc_req_max_size;
	/* maximum reply size this service can ever send */
	unsigned int			bc_rep_max_size;
};

struct ptlrpc_service_thr_conf {
	/* threadname should be 8 characters or less - 6 will be added on */
	char				*tc_thr_name;
	/* threads increasing factor for each CPU */
	unsigned int			tc_thr_factor;
	/* service threads # to start on each partition while initializing */
	unsigned int			tc_nthrs_init;
	/*
	 * low water of threads # upper-limit on each partition while running,
	 * service availability may be impacted if threads number is lower
	 * than this value. It can be ZERO if the service doesn't require
	 * CPU affinity or there is only one partition.
	 */
	unsigned int			tc_nthrs_base;
	/* "soft" limit for total threads number */
	unsigned int			tc_nthrs_max;
	/* user specified threads number, it will be validated due to
	 * other members of this structure. */
	unsigned int			tc_nthrs_user;
	/* bind service threads to only CPUs in their associated CPT */
	unsigned int			tc_cpu_bind;
	/* Tags for lu_context associated with service thread */
	__u32				tc_ctx_tags;
};

struct ptlrpc_service_cpt_conf {
	struct cfs_cpt_table		*cc_cptable;
	/* string pattern to describe CPTs for a service */
	char				*cc_pattern;
	/* whether or not to have per-CPT service partitions */
	bool				cc_affinity;
};

struct ptlrpc_service_conf {
	/* service name */
	char				*psc_name;
	/* soft watchdog timeout multiplifier to print stuck service traces */
	unsigned int			psc_watchdog_factor;
	/* buffer information */
	struct ptlrpc_service_buf_conf	psc_buf;
	/* thread information */
	struct ptlrpc_service_thr_conf	psc_thr;
	/* CPU partition information */
	struct ptlrpc_service_cpt_conf	psc_cpt;
	/* function table */
	struct ptlrpc_service_ops	psc_ops;
};

/* ptlrpc/service.c */
/**
 * Server-side services API. Register/unregister service, request state
 * management, service thread management
 *
 * @{
 */
void ptlrpc_save_lock(struct ptlrpc_request *req, struct lustre_handle *lock,
		      int mode, bool no_ack, bool convert_lock);
void ptlrpc_commit_replies(struct obd_export *exp);
void ptlrpc_dispatch_difficult_reply(struct ptlrpc_reply_state *rs);
void ptlrpc_schedule_difficult_reply(struct ptlrpc_reply_state *rs);
int ptlrpc_hpreq_handler(struct ptlrpc_request *req);
struct ptlrpc_service *ptlrpc_register_service(
				struct ptlrpc_service_conf *conf,
				struct kset *parent,
				struct dentry *debugfs_entry);

int ptlrpc_unregister_service(struct ptlrpc_service *service);
int ptlrpc_service_health_check(struct ptlrpc_service *);
void ptlrpc_server_drop_request(struct ptlrpc_request *req);
void ptlrpc_request_change_export(struct ptlrpc_request *req,
				  struct obd_export *export);
void ptlrpc_update_export_timer(struct obd_export *exp,
				time64_t extra_delay);

int ptlrpc_hr_init(void);
void ptlrpc_hr_fini(void);

void ptlrpc_watchdog_init(struct delayed_work *work, timeout_t timeout);
void ptlrpc_watchdog_disable(struct delayed_work *work);
void ptlrpc_watchdog_touch(struct delayed_work *work, timeout_t timeout);

/** @} */

/* ptlrpc/import.c */
/**
 * Import API
 * @{
 */
int ptlrpc_connect_import(struct obd_import *imp);
int ptlrpc_connect_import_locked(struct obd_import *imp);
int ptlrpc_init_import(struct obd_import *imp);
int ptlrpc_disconnect_import(struct obd_import *imp, int noclose);
int ptlrpc_disconnect_and_idle_import(struct obd_import *imp);
int ptlrpc_import_recovery_state_machine(struct obd_import *imp);
void deuuidify(char *uuid, const char *prefix, char **uuid_start,
	       int *uuid_len);
void ptlrpc_import_enter_resend(struct obd_import *imp);
/* ptlrpc/pack_generic.c */
int ptlrpc_reconnect_import(struct obd_import *imp);
/** @} */

/**
 * ptlrpc msg buffer and swab interface
 *
 * @{
 */
#define PTLRPC_MAX_BUFCOUNT \
	(sizeof(((struct ptlrpc_request *)0)->rq_req_swab_mask) * 8)
#define MD_MAX_BUFLEN		(MDS_REG_MAXREQSIZE > OUT_MAXREQSIZE ? \
				 MDS_REG_MAXREQSIZE : OUT_MAXREQSIZE)
#define PTLRPC_MAX_BUFLEN	(OST_IO_MAXREQSIZE > MD_MAX_BUFLEN ? \
				 OST_IO_MAXREQSIZE : MD_MAX_BUFLEN)
int ptlrpc_unpack_rep_msg(struct ptlrpc_request *req, int len);
int ptlrpc_unpack_req_msg(struct ptlrpc_request *req, int len);

int lustre_msg_check_version(struct lustre_msg *msg, __u32 version);
void lustre_init_msg_v2(struct lustre_msg_v2 *msg, int count, __u32 *lens,
                        char **bufs);
int lustre_pack_request(struct ptlrpc_request *, __u32 magic, int count,
                        __u32 *lens, char **bufs);
int lustre_pack_reply(struct ptlrpc_request *, int count, __u32 *lens,
                      char **bufs);
int lustre_pack_reply_v2(struct ptlrpc_request *req, int count,
                         __u32 *lens, char **bufs, int flags);
#define LPRFL_EARLY_REPLY 1
int lustre_pack_reply_flags(struct ptlrpc_request *, int count, __u32 *lens,
                            char **bufs, int flags);
int lustre_shrink_msg(struct lustre_msg *msg, int segment,
                      unsigned int newlen, int move_data);
int lustre_grow_msg(struct lustre_msg *msg, int segment, unsigned int newlen);
void lustre_free_reply_state(struct ptlrpc_reply_state *rs);
int __lustre_unpack_msg(struct lustre_msg *m, int len);
__u32 lustre_msg_hdr_size(__u32 magic, __u32 count);
__u32 lustre_msg_size(__u32 magic, int count, __u32 *lengths);
__u32 lustre_msg_size_v2(int count, __u32 *lengths);
__u32 lustre_packed_msg_size(struct lustre_msg *msg);
__u32 lustre_msg_early_size(void);
void *lustre_msg_buf_v2(struct lustre_msg_v2 *m, __u32 n, __u32 min_size);
void *lustre_msg_buf(struct lustre_msg *m, __u32 n, __u32 minlen);
__u32 lustre_msg_buflen(struct lustre_msg *m, __u32 n);
void lustre_msg_set_buflen(struct lustre_msg *m, __u32 n, __u32 len);
__u32 lustre_msg_bufcount(struct lustre_msg *m);
char *lustre_msg_string(struct lustre_msg *m, __u32 n, __u32 max_len);
__u32 lustre_msghdr_get_flags(struct lustre_msg *msg);
void lustre_msghdr_set_flags(struct lustre_msg *msg, __u32 flags);
__u32 lustre_msg_get_flags(struct lustre_msg *msg);
void lustre_msg_add_flags(struct lustre_msg *msg, __u32 flags);
void lustre_msg_set_flags(struct lustre_msg *msg, __u32 flags);
void lustre_msg_clear_flags(struct lustre_msg *msg, __u32 flags);
__u32 lustre_msg_get_op_flags(struct lustre_msg *msg);
void lustre_msg_add_op_flags(struct lustre_msg *msg, __u32 flags);
struct lustre_handle *lustre_msg_get_handle(struct lustre_msg *msg);
__u32 lustre_msg_get_type(struct lustre_msg *msg);
enum lustre_msg_version lustre_msg_get_version(struct lustre_msg *msg);
void lustre_msg_add_version(struct lustre_msg *msg, __u32 version);
__u32 lustre_msg_get_opc(struct lustre_msg *msg);
__u64 lustre_msg_get_last_xid(struct lustre_msg *msg);
__u16 lustre_msg_get_tag(struct lustre_msg *msg);
__u64 lustre_msg_get_last_committed(struct lustre_msg *msg);
__u64 *lustre_msg_get_versions(struct lustre_msg *msg);
__u64 lustre_msg_get_transno(struct lustre_msg *msg);
__u64 lustre_msg_get_slv(struct lustre_msg *msg);
__u32 lustre_msg_get_limit(struct lustre_msg *msg);
void lustre_msg_set_slv(struct lustre_msg *msg, __u64 slv);
void lustre_msg_set_limit(struct lustre_msg *msg, __u64 limit);
int lustre_msg_get_status(struct lustre_msg *msg);
__u32 lustre_msg_get_conn_cnt(struct lustre_msg *msg);
__u32 lustre_msg_get_magic(struct lustre_msg *msg);
timeout_t lustre_msg_get_timeout(struct lustre_msg *msg);
timeout_t lustre_msg_get_service_timeout(struct lustre_msg *msg);
char *lustre_msg_get_jobid(struct lustre_msg *msg);
__u32 lustre_msg_get_cksum(struct lustre_msg *msg);
__u64 lustre_msg_get_mbits(struct lustre_msg *msg);
__u32 lustre_msg_calc_cksum(struct lustre_msg *msg, __u32 buf);
void lustre_msg_set_handle(struct lustre_msg *msg,struct lustre_handle *handle);
void lustre_msg_set_type(struct lustre_msg *msg, __u32 type);
void lustre_msg_set_opc(struct lustre_msg *msg, __u32 opc);
void lustre_msg_set_last_xid(struct lustre_msg *msg, __u64 last_xid);
void lustre_msg_set_tag(struct lustre_msg *msg, __u16 tag);
void lustre_msg_set_last_committed(struct lustre_msg *msg,__u64 last_committed);
void lustre_msg_set_versions(struct lustre_msg *msg, __u64 *versions);
void lustre_msg_set_transno(struct lustre_msg *msg, __u64 transno);
void lustre_msg_set_status(struct lustre_msg *msg, __u32 status);
void lustre_msg_set_conn_cnt(struct lustre_msg *msg, __u32 conn_cnt);
void ptlrpc_req_set_repsize(struct ptlrpc_request *req, int count, __u32 *sizes);
void ptlrpc_request_set_replen(struct ptlrpc_request *req);
void lustre_msg_set_timeout(struct lustre_msg *msg, timeout_t timeout);
void lustre_msg_set_service_timeout(struct lustre_msg *msg,
				    timeout_t service_timeout);
void lustre_msg_set_jobid(struct lustre_msg *msg, char *jobid);
void lustre_msg_set_cksum(struct lustre_msg *msg, __u32 cksum);
void lustre_msg_set_mbits(struct lustre_msg *msg, __u64 mbits);

static inline void
lustre_shrink_reply(struct ptlrpc_request *req, int segment,
                    unsigned int newlen, int move_data)
{
        LASSERT(req->rq_reply_state);
        LASSERT(req->rq_repmsg);
        req->rq_replen = lustre_shrink_msg(req->rq_repmsg, segment,
                                           newlen, move_data);
}

#ifdef LUSTRE_TRANSLATE_ERRNOS

static inline int ptlrpc_status_hton(int h)
{
	/*
	 * Positive errnos must be network errnos, such as LUSTRE_EDEADLK,
	 * ELDLM_LOCK_ABORTED, etc.
	 */
	if (h < 0)
		return -lustre_errno_hton(-h);
	else
		return h;
}

static inline int ptlrpc_status_ntoh(int n)
{
	/*
	 * See the comment in ptlrpc_status_hton().
	 */
	if (n < 0)
		return -lustre_errno_ntoh(-n);
	else
		return n;
}

#else

#define ptlrpc_status_hton(h) (h)
#define ptlrpc_status_ntoh(n) (n)

#endif
/** @} */

/** Change request phase of \a req to \a new_phase */
static inline void
ptlrpc_rqphase_move(struct ptlrpc_request *req, enum rq_phase new_phase)
{
	if (req->rq_phase == new_phase)
		return;

	if (new_phase == RQ_PHASE_UNREG_RPC ||
	    new_phase == RQ_PHASE_UNREG_BULK) {
		/* No embedded unregistering phases */
		if (req->rq_phase == RQ_PHASE_UNREG_RPC ||
		    req->rq_phase == RQ_PHASE_UNREG_BULK)
			return;

		req->rq_next_phase = req->rq_phase;
		if (req->rq_import)
			atomic_inc(&req->rq_import->imp_unregistering);
	}

	if (req->rq_phase == RQ_PHASE_UNREG_RPC ||
	    req->rq_phase == RQ_PHASE_UNREG_BULK) {
		if (req->rq_import)
			atomic_dec(&req->rq_import->imp_unregistering);
	}

	DEBUG_REQ(D_INFO, req, "move request phase from %s to %s",
		  ptlrpc_rqphase2str(req), ptlrpc_phase2str(new_phase));

	req->rq_phase = new_phase;
}

/**
 * Returns true if request \a req got early reply and hard deadline is not met
 */
static inline int
ptlrpc_client_early(struct ptlrpc_request *req)
{
        return req->rq_early;
}

/**
 * Returns true if we got real reply from server for this request
 */
static inline int
ptlrpc_client_replied(struct ptlrpc_request *req)
{
	if (req->rq_reply_deadline > ktime_get_real_seconds())
		return 0;
	return req->rq_replied;
}

/** Returns true if request \a req is in process of receiving server reply */
static inline int
ptlrpc_client_recv(struct ptlrpc_request *req)
{
	if (req->rq_reply_deadline > ktime_get_real_seconds())
		return 1;
	return req->rq_receiving_reply;
}

#define ptlrpc_cli_wait_unlink(req) __ptlrpc_cli_wait_unlink(req, NULL)

static inline int
__ptlrpc_cli_wait_unlink(struct ptlrpc_request *req, bool *discard)
{
	int rc;

	spin_lock(&req->rq_lock);
	if (req->rq_reply_deadline > ktime_get_real_seconds()) {
		spin_unlock(&req->rq_lock);
		return 1;
	}
	if (req->rq_req_deadline > ktime_get_real_seconds()) {
		spin_unlock(&req->rq_lock);
		return 1;
	}

	if (discard) {
		*discard = false;
		if (req->rq_reply_unlinked && req->rq_req_unlinked == 0) {
			*discard = true;
			spin_unlock(&req->rq_lock);
			return 1; /* Should call again after LNetMDUnlink */
		}
	}

	rc = !req->rq_req_unlinked || !req->rq_reply_unlinked ||
	     req->rq_receiving_reply;
	spin_unlock(&req->rq_lock);
	return rc;
}

static inline void
ptlrpc_client_wake_req(struct ptlrpc_request *req)
{
	smp_mb();
	if (req->rq_set == NULL)
		wake_up(&req->rq_reply_waitq);
	else
		wake_up(&req->rq_set->set_waitq);
}

static inline void
ptlrpc_rs_addref(struct ptlrpc_reply_state *rs)
{
	LASSERT(atomic_read(&rs->rs_refcount) > 0);
	atomic_inc(&rs->rs_refcount);
}

static inline void
ptlrpc_rs_decref(struct ptlrpc_reply_state *rs)
{
	LASSERT(atomic_read(&rs->rs_refcount) > 0);
	if (atomic_dec_and_test(&rs->rs_refcount))
		lustre_free_reply_state(rs);
}

/* Should only be called once per req */
static inline void ptlrpc_req_drop_rs(struct ptlrpc_request *req)
{
        if (req->rq_reply_state == NULL)
                return; /* shouldn't occur */
        ptlrpc_rs_decref(req->rq_reply_state);
        req->rq_reply_state = NULL;
        req->rq_repmsg = NULL;
}

static inline __u32 lustre_request_magic(struct ptlrpc_request *req)
{
        return lustre_msg_get_magic(req->rq_reqmsg);
}

static inline int ptlrpc_req_get_repsize(struct ptlrpc_request *req)
{
        switch (req->rq_reqmsg->lm_magic) {
        case LUSTRE_MSG_MAGIC_V2:
                return req->rq_reqmsg->lm_repsize;
        default:
                LASSERTF(0, "incorrect message magic: %08x\n",
                         req->rq_reqmsg->lm_magic);
                return -EFAULT;
        }
}

static inline int ptlrpc_send_limit_expired(struct ptlrpc_request *req)
{
        if (req->rq_delay_limit != 0 &&
	    req->rq_queued_time + req->rq_delay_limit < ktime_get_seconds())
                return 1;
        return 0;
}

static inline int ptlrpc_no_resend(struct ptlrpc_request *req)
{
	if (!req->rq_no_resend && ptlrpc_send_limit_expired(req)) {
		spin_lock(&req->rq_lock);
		req->rq_no_resend = 1;
		spin_unlock(&req->rq_lock);
	}
	return req->rq_no_resend;
}

static inline int
ptlrpc_server_get_timeout(struct ptlrpc_service_part *svcpt)
{
	int at = AT_OFF ? 0 : at_get(&svcpt->scp_at_estimate);

	return svcpt->scp_service->srv_watchdog_factor *
	       max_t(int, at, obd_timeout);
}

/**
 * Calculate the amount of time for lock prolongation.
 *
 * This is helper function to get the timeout extra time.
 *
 * @req		current request
 *
 * Return:	amount of time to extend the timeout with
 */
static inline timeout_t prolong_timeout(struct ptlrpc_request *req)
{
	struct ptlrpc_service_part *svcpt = req->rq_rqbd->rqbd_svcpt;
	timeout_t req_timeout = 0;

	if (AT_OFF)
		return obd_timeout / 2;

	if (req->rq_deadline > req->rq_arrival_time.tv_sec)
		req_timeout = req->rq_deadline - req->rq_arrival_time.tv_sec;

	return max(req_timeout,
		   at_est2timeout(at_get(&svcpt->scp_at_estimate)));
}

static inline struct ptlrpc_service *
ptlrpc_req2svc(struct ptlrpc_request *req)
{
	LASSERT(req->rq_rqbd != NULL);
	return req->rq_rqbd->rqbd_svcpt->scp_service;
}

/* ldlm/ldlm_lib.c */
/**
 * Target client logic
 * @{
 */
int client_obd_setup(struct obd_device *obd, struct lustre_cfg *lcfg);
int client_obd_cleanup(struct obd_device *obd);
int client_connect_import(const struct lu_env *env,
                          struct obd_export **exp, struct obd_device *obd,
                          struct obd_uuid *cluuid, struct obd_connect_data *,
                          void *localdata);
int client_disconnect_export(struct obd_export *exp);
int client_import_add_conn(struct obd_import *imp, struct obd_uuid *uuid,
                           int priority);
int client_import_dyn_add_conn(struct obd_import *imp, struct obd_uuid *uuid,
			       lnet_nid_t prim_nid, int priority);
int client_import_add_nids_to_conn(struct obd_import *imp, lnet_nid_t *nids,
				   int nid_count, struct obd_uuid *uuid);
int client_import_del_conn(struct obd_import *imp, struct obd_uuid *uuid);
int client_import_find_conn(struct obd_import *imp, lnet_nid_t peer,
                            struct obd_uuid *uuid);
int import_set_conn_priority(struct obd_import *imp, struct obd_uuid *uuid);
void client_destroy_import(struct obd_import *imp);
/** @} */

#ifdef HAVE_SERVER_SUPPORT
int server_disconnect_export(struct obd_export *exp);
#endif

/* ptlrpc/pinger.c */
/**
 * Pinger API (client side only)
 * @{
 */
enum timeout_event {
        TIMEOUT_GRANT = 1
};
struct timeout_item;
typedef int (*timeout_cb_t)(struct timeout_item *, void *);
int ptlrpc_pinger_add_import(struct obd_import *imp);
int ptlrpc_pinger_del_import(struct obd_import *imp);
struct ptlrpc_request * ptlrpc_prep_ping(struct obd_import *imp);
int ptlrpc_obd_ping(struct obd_device *obd);
void ping_evictor_start(void);
void ping_evictor_stop(void);
void ptlrpc_pinger_ir_up(void);
void ptlrpc_pinger_ir_down(void);
/** @} */
int ptlrpc_pinger_suppress_pings(void);

/* ptlrpc/ptlrpcd.c */
void ptlrpcd_stop(struct ptlrpcd_ctl *pc, int force);
void ptlrpcd_free(struct ptlrpcd_ctl *pc);
void ptlrpcd_wake(struct ptlrpc_request *req);
void ptlrpcd_add_req(struct ptlrpc_request *req);
void ptlrpcd_add_rqset(struct ptlrpc_request_set *set);
int ptlrpcd_addref(void);
void ptlrpcd_decref(void);

/* ptlrpc/lproc_ptlrpc.c */
/**
 * procfs output related functions
 * @{
 */
const char* ll_opcode2str(__u32 opcode);
const int ll_str2opcode(const char *ops);
#ifdef CONFIG_PROC_FS
void ptlrpc_lprocfs_register_obd(struct obd_device *obd);
void ptlrpc_lprocfs_unregister_obd(struct obd_device *obd);
void ptlrpc_lprocfs_brw(struct ptlrpc_request *req, int bytes);
#else
static inline void ptlrpc_lprocfs_register_obd(struct obd_device *obd) {}
static inline void ptlrpc_lprocfs_unregister_obd(struct obd_device *obd) {}
static inline void ptlrpc_lprocfs_brw(struct ptlrpc_request *req, int bytes) {}
#endif
/** @} */

/* ptlrpc/llog_server.c */
int llog_origin_handle_open(struct ptlrpc_request *req);
int llog_origin_handle_prev_block(struct ptlrpc_request *req);
int llog_origin_handle_next_block(struct ptlrpc_request *req);
int llog_origin_handle_read_header(struct ptlrpc_request *req);

/* ptlrpc/llog_client.c */
extern const struct llog_operations llog_client_ops;
/** @} net */

#endif
/** @} PtlRPC */
