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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/delay.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <lustre_dlm.h>
#include "llite_internal.h"

#define SA_OMITTED_ENTRY_MAX 8ULL

typedef enum {
	/** negative values are for error cases */
	SA_ENTRY_INIT = 0,      /** init entry */
	SA_ENTRY_SUCC = 1,      /** stat succeed */
	SA_ENTRY_INVA = 2,      /** invalid entry */
} se_state_t;

/*
 * sa_entry is not refcounted: statahead thread allocates it and do async stat,
 * and in async stat callback ll_statahead_interpret() will prepare the inode
 * and set lock data in the ptlrpcd context. Then the scanner process will be
 * woken up if this entry is the waiting one, can access and free it.
 */
struct sa_entry {
	/* link into sai_entries */
	struct list_head	se_list;
	/* link into sai hash table locally */
	struct list_head	se_hash;
	/* entry index in the sai */
	__u64			se_index;
	/* low layer ldlm lock handle */
	__u64			se_handle;
	/* entry status */
	se_state_t		se_state;
	/* entry size, contains name */
	int			se_size;
	/* pointer to the target inode */
	struct inode	       *se_inode;
	/* entry name */
	struct qstr		se_qstr;
	/* entry fid */
	struct lu_fid		se_fid;
};

static unsigned int sai_generation;
static DEFINE_SPINLOCK(sai_generation_lock);

static inline int sa_unhashed(struct sa_entry *entry)
{
	return list_empty(&entry->se_hash);
}

/* sa_entry is ready to use */
static inline int sa_ready(struct sa_entry *entry)
{
	/* Make sure sa_entry is updated and ready to use */
	smp_rmb();
	return (entry->se_state != SA_ENTRY_INIT);
}

/* hash value to put in sai_cache */
static inline int sa_hash(int val)
{
	return val & LL_SA_CACHE_MASK;
}

/* hash entry into sai_cache */
static inline void
sa_rehash(struct ll_statahead_info *sai, struct sa_entry *entry)
{
	int i = sa_hash(entry->se_qstr.hash);

	spin_lock(&sai->sai_cache_lock[i]);
	list_add_tail(&entry->se_hash, &sai->sai_cache[i]);
	spin_unlock(&sai->sai_cache_lock[i]);
}

/* unhash entry from sai_cache */
static inline void
sa_unhash(struct ll_statahead_info *sai, struct sa_entry *entry)
{
	int i = sa_hash(entry->se_qstr.hash);

	spin_lock(&sai->sai_cache_lock[i]);
	list_del_init(&entry->se_hash);
	spin_unlock(&sai->sai_cache_lock[i]);
}

static inline int agl_should_run(struct ll_statahead_info *sai,
				 struct inode *inode)
{
	return inode && S_ISREG(inode->i_mode) && sai->sai_agl_task;
}

static inline struct ll_inode_info *
agl_first_entry(struct ll_statahead_info *sai)
{
	return list_entry(sai->sai_agls.next, struct ll_inode_info,
			  lli_agl_list);
}

/* statahead window is full */
static inline int sa_sent_full(struct ll_statahead_info *sai)
{
	return atomic_read(&sai->sai_cache_count) >= sai->sai_max;
}

/* Batch metadata handle */
static inline bool sa_has_batch_handle(struct ll_statahead_info *sai)
{
	return sai->sai_bh != NULL;
}

static inline void ll_statahead_flush_nowait(struct ll_statahead_info *sai)
{
	if (sa_has_batch_handle(sai)) {
		sai->sai_index_end = sai->sai_index - 1;
		(void) md_batch_flush(ll_i2mdexp(sai->sai_dentry->d_inode),
				      sai->sai_bh, false);
	}
}

static inline int agl_list_empty(struct ll_statahead_info *sai)
{
	return list_empty(&sai->sai_agls);
}

/**
 * (1) hit ratio less than 80%
 * or
 * (2) consecutive miss more than 8
 * then means low hit.
 */
static inline int sa_low_hit(struct ll_statahead_info *sai)
{
	return ((sai->sai_hit > 7 && sai->sai_hit < 4 * sai->sai_miss) ||
		(sai->sai_consecutive_miss > 8));
}

/*
 * if the given index is behind of statahead window more than
 * SA_OMITTED_ENTRY_MAX, then it is old.
 */
static inline int is_omitted_entry(struct ll_statahead_info *sai, __u64 index)
{
	return ((__u64)sai->sai_max + index + SA_OMITTED_ENTRY_MAX <
		sai->sai_index);
}

/* allocate sa_entry and hash it to allow scanner process to find it */
static struct sa_entry *
sa_alloc(struct dentry *parent, struct ll_statahead_info *sai, __u64 index,
	 const char *name, int len, const struct lu_fid *fid)
{
	struct ll_inode_info *lli;
	struct sa_entry *entry;
	int entry_size;
	char *dname;

	ENTRY;

	entry_size = sizeof(struct sa_entry) + (len & ~3) + 4;
	OBD_ALLOC(entry, entry_size);
	if (unlikely(!entry))
		RETURN(ERR_PTR(-ENOMEM));

	CDEBUG(D_READA, "alloc sa entry %.*s(%p) index %llu\n",
	       len, name, entry, index);

	entry->se_index = index;

	entry->se_state = SA_ENTRY_INIT;
	entry->se_size = entry_size;
	dname = (char *)entry + sizeof(struct sa_entry);
	memcpy(dname, name, len);
	dname[len] = 0;
	entry->se_qstr.hash = ll_full_name_hash(parent, name, len);
	entry->se_qstr.len = len;
	entry->se_qstr.name = dname;
	entry->se_fid = *fid;

	lli = ll_i2info(sai->sai_dentry->d_inode);

	spin_lock(&lli->lli_sa_lock);
	INIT_LIST_HEAD(&entry->se_list);
	sa_rehash(sai, entry);
	spin_unlock(&lli->lli_sa_lock);

	atomic_inc(&sai->sai_cache_count);

	RETURN(entry);
}

/* free sa_entry, which should have been unhashed and not in any list */
static void sa_free(struct ll_statahead_info *sai, struct sa_entry *entry)
{
	CDEBUG(D_READA, "free sa entry %.*s(%p) index %llu\n",
	       entry->se_qstr.len, entry->se_qstr.name, entry,
	       entry->se_index);

	LASSERT(list_empty(&entry->se_list));
	LASSERT(sa_unhashed(entry));

	OBD_FREE(entry, entry->se_size);
	atomic_dec(&sai->sai_cache_count);
}

/*
 * find sa_entry by name, used by directory scanner, lock is not needed because
 * only scanner can remove the entry from cache.
 */
static struct sa_entry *
sa_get(struct ll_statahead_info *sai, const struct qstr *qstr)
{
	struct sa_entry *entry;
	int i = sa_hash(qstr->hash);

	list_for_each_entry(entry, &sai->sai_cache[i], se_hash) {
		if (entry->se_qstr.hash == qstr->hash &&
		    entry->se_qstr.len == qstr->len &&
		    memcmp(entry->se_qstr.name, qstr->name, qstr->len) == 0)
			return entry;
	}
	return NULL;
}

/* unhash and unlink sa_entry, and then free it */
static inline void
sa_kill(struct ll_statahead_info *sai, struct sa_entry *entry)
{
	struct ll_inode_info *lli = ll_i2info(sai->sai_dentry->d_inode);

	LASSERT(!sa_unhashed(entry));
	LASSERT(!list_empty(&entry->se_list));
	LASSERT(sa_ready(entry));

	sa_unhash(sai, entry);

	spin_lock(&lli->lli_sa_lock);
	list_del_init(&entry->se_list);
	spin_unlock(&lli->lli_sa_lock);

	iput(entry->se_inode);

	sa_free(sai, entry);
}

/* called by scanner after use, sa_entry will be killed */
static void
sa_put(struct inode *dir, struct ll_statahead_info *sai, struct sa_entry *entry)
{
	struct ll_inode_info *lli = ll_i2info(dir);
	struct sa_entry *tmp, *next;
	bool wakeup = false;

	if (entry && entry->se_state == SA_ENTRY_SUCC) {
		struct ll_sb_info *sbi = ll_i2sbi(sai->sai_dentry->d_inode);

		sai->sai_hit++;
		sai->sai_consecutive_miss = 0;
		if (sai->sai_max < sbi->ll_sa_max) {
			sai->sai_max = min(2 * sai->sai_max, sbi->ll_sa_max);
			wakeup = true;
		} else if (sai->sai_max_batch_count > 0) {
			if (sai->sai_max >= sai->sai_max_batch_count &&
			   (sai->sai_index_end - entry->se_index) %
			   sai->sai_max_batch_count == 0) {
				wakeup = true;
			} else if (entry->se_index == sai->sai_index_end) {
				wakeup = true;
			}
		} else {
			wakeup = true;
		}
	} else {
		sai->sai_miss++;
		sai->sai_consecutive_miss++;
		wakeup = true;
	}

	if (entry)
		sa_kill(sai, entry);

	/*
	 * kill old completed entries, only scanner process does this, no need
	 * to lock
	 */
	list_for_each_entry_safe(tmp, next, &sai->sai_entries, se_list) {
		if (!is_omitted_entry(sai, tmp->se_index))
			break;
		sa_kill(sai, tmp);
	}

	spin_lock(&lli->lli_sa_lock);
	if (wakeup && sai->sai_task)
		wake_up_process(sai->sai_task);
	spin_unlock(&lli->lli_sa_lock);
}

/*
 * update state and sort add entry to sai_entries by index, return true if
 * scanner is waiting on this entry.
 */
static bool
__sa_make_ready(struct ll_statahead_info *sai, struct sa_entry *entry, int ret)
{
	struct sa_entry *se;
	struct list_head *pos = &sai->sai_entries;
	__u64 index = entry->se_index;

	LASSERT(!sa_ready(entry));
	LASSERT(list_empty(&entry->se_list));

	list_for_each_entry_reverse(se, &sai->sai_entries, se_list) {
		if (se->se_index < entry->se_index) {
			pos = &se->se_list;
			break;
		}
	}
	list_add(&entry->se_list, pos);
	/*
	 * LU-9210: ll_statahead_interpet must be able to see this before
	 * we wake it up
	 */
	smp_store_release(&entry->se_state,
			  ret < 0 ? SA_ENTRY_INVA : SA_ENTRY_SUCC);

	return (index == sai->sai_index_wait);
	//return (index == sai->sai_batch_end);
}

/* finish async stat RPC arguments */
static void sa_fini_data(struct md_op_item *item)
{
	ll_unlock_md_op_lsm(&item->mop_data);
	iput(item->mop_dir);
	OBD_FREE_PTR(item);
}

static int ll_statahead_interpret(struct req_capsule *pill,
				  struct md_op_item *item, int rc);

/*
 * prepare arguments for async stat RPC.
 */
static struct md_op_item *
sa_prep_data(struct inode *dir, struct inode *child, struct sa_entry *entry)
{
	struct md_op_item *item;
	struct ldlm_enqueue_info *einfo;
	struct md_op_data *op_data;

	OBD_ALLOC_PTR(item);
	if (!item)
		return ERR_PTR(-ENOMEM);

	op_data = ll_prep_md_op_data(&item->mop_data, dir, child,
				     entry->se_qstr.name, entry->se_qstr.len, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data)) {
		OBD_FREE_PTR(item);
		return (struct md_op_item *)op_data;
	}

	if (!child)
		op_data->op_fid2 = entry->se_fid;

	item->mop_opc = MD_OP_GETATTR;
	item->mop_it.it_op = IT_GETATTR;
	item->mop_dir = igrab(dir);
	item->mop_cb = ll_statahead_interpret;
	item->mop_cbdata = entry;

	einfo = &item->mop_einfo;
	einfo->ei_type = LDLM_IBITS;
	einfo->ei_mode = it_to_lock_mode(&item->mop_it);
	einfo->ei_cb_bl = ll_md_blocking_ast;
	einfo->ei_cb_cp = ldlm_completion_ast;
	einfo->ei_cb_gl = NULL;
	einfo->ei_cbdata = NULL;

	return item;
}

/*
 * release resources used in async stat RPC, update entry state and wakeup if
 * scanner process it waiting on this entry.
 */
static void
sa_make_ready(struct ll_statahead_info *sai, struct sa_entry *entry, int ret)
{
	struct ll_inode_info *lli = ll_i2info(sai->sai_dentry->d_inode);
	bool wakeup;

	spin_lock(&lli->lli_sa_lock);
	wakeup = __sa_make_ready(sai, entry, ret);
	spin_unlock(&lli->lli_sa_lock);

	if (wakeup)
		wake_up(&sai->sai_waitq);
}

/* insert inode into the list of sai_agls */
static void ll_agl_add(struct ll_statahead_info *sai,
		       struct inode *inode, int index)
{
	struct ll_inode_info *child  = ll_i2info(inode);
	struct ll_inode_info *parent = ll_i2info(sai->sai_dentry->d_inode);

	spin_lock(&child->lli_agl_lock);
	if (child->lli_agl_index == 0) {
		child->lli_agl_index = index;
		spin_unlock(&child->lli_agl_lock);

		LASSERT(list_empty(&child->lli_agl_list));

		spin_lock(&parent->lli_agl_lock);
		/* Re-check under the lock */
		if (agl_should_run(sai, inode)) {
			if (agl_list_empty(sai))
				wake_up_process(sai->sai_agl_task);
			igrab(inode);
			list_add_tail(&child->lli_agl_list, &sai->sai_agls);
		} else
			child->lli_agl_index = 0;
		spin_unlock(&parent->lli_agl_lock);
	} else {
		spin_unlock(&child->lli_agl_lock);
	}
}

/* allocate sai */
static struct ll_statahead_info *ll_sai_alloc(struct dentry *dentry)
{
	struct ll_statahead_info *sai;
	struct ll_inode_info *lli = ll_i2info(dentry->d_inode);
	int i;

	ENTRY;

	OBD_ALLOC_PTR(sai);
	if (!sai)
		RETURN(NULL);

	sai->sai_dentry = dget(dentry);
	atomic_set(&sai->sai_refcount, 1);
	sai->sai_max = 8; //LL_SA_RPC_MIN;
	sai->sai_index = 1;
	init_waitqueue_head(&sai->sai_waitq);

	INIT_LIST_HEAD(&sai->sai_entries);
	INIT_LIST_HEAD(&sai->sai_agls);

	for (i = 0; i < LL_SA_CACHE_SIZE; i++) {
		INIT_LIST_HEAD(&sai->sai_cache[i]);
		spin_lock_init(&sai->sai_cache_lock[i]);
	}
	atomic_set(&sai->sai_cache_count, 0);

	spin_lock(&sai_generation_lock);
	lli->lli_sa_generation = ++sai_generation;
	if (unlikely(sai_generation == 0))
		lli->lli_sa_generation = ++sai_generation;
	spin_unlock(&sai_generation_lock);

	RETURN(sai);
}

/* free sai */
static inline void ll_sai_free(struct ll_statahead_info *sai)
{
	LASSERT(sai->sai_dentry != NULL);
	dput(sai->sai_dentry);
	OBD_FREE_PTR(sai);
}

/*
 * take refcount of sai if sai for @dir exists, which means statahead is on for
 * this directory.
 */
static inline struct ll_statahead_info *ll_sai_get(struct inode *dir)
{
	struct ll_inode_info *lli = ll_i2info(dir);
	struct ll_statahead_info *sai = NULL;

	spin_lock(&lli->lli_sa_lock);
	sai = lli->lli_sai;
	if (sai)
		atomic_inc(&sai->sai_refcount);
	spin_unlock(&lli->lli_sa_lock);

	return sai;
}

/*
 * put sai refcount after use, if refcount reaches zero, free sai and sa_entries
 * attached to it.
 */
static void ll_sai_put(struct ll_statahead_info *sai)
{
	struct ll_inode_info *lli = ll_i2info(sai->sai_dentry->d_inode);

	if (atomic_dec_and_lock(&sai->sai_refcount, &lli->lli_sa_lock)) {
		struct sa_entry *entry, *next;
		struct ll_sb_info *sbi = ll_i2sbi(sai->sai_dentry->d_inode);

		lli->lli_sai = NULL;
		spin_unlock(&lli->lli_sa_lock);

		LASSERT(!sai->sai_task);
		LASSERT(!sai->sai_agl_task);
		LASSERT(sai->sai_sent == sai->sai_replied);

		list_for_each_entry_safe(entry, next, &sai->sai_entries,
					 se_list)
			sa_kill(sai, entry);

		LASSERT(atomic_read(&sai->sai_cache_count) == 0);
		LASSERT(agl_list_empty(sai));

		ll_sai_free(sai);
		atomic_dec(&sbi->ll_sa_running);
	}
}

/* Do NOT forget to drop inode refcount when into sai_agls. */
static void ll_agl_trigger(struct inode *inode, struct ll_statahead_info *sai)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	u64 index = lli->lli_agl_index;
	ktime_t expire;
	int rc;

	ENTRY;

	LASSERT(list_empty(&lli->lli_agl_list));

	/* AGL maybe fall behind statahead with one entry */
	if (is_omitted_entry(sai, index + 1)) {
		lli->lli_agl_index = 0;
		iput(inode);
		RETURN_EXIT;
	}

	/*
	 * In case of restore, the MDT has the right size and has already
	 * sent it back without granting the layout lock, inode is up-to-date.
	 * Then AGL (async glimpse lock) is useless.
	 * Also to glimpse we need the layout, in case of a runninh restore
	 * the MDT holds the layout lock so the glimpse will block up to the
	 * end of restore (statahead/agl will block)
	 */
	if (test_bit(LLIF_FILE_RESTORING, &lli->lli_flags)) {
		lli->lli_agl_index = 0;
		iput(inode);
		RETURN_EXIT;
	}

	/* Someone is in glimpse (sync or async), do nothing. */
	rc = down_write_trylock(&lli->lli_glimpse_sem);
	if (rc == 0) {
		lli->lli_agl_index = 0;
		iput(inode);
		RETURN_EXIT;
	}

	/*
	 * Someone triggered glimpse within 1 sec before.
	 * 1) The former glimpse succeeded with glimpse lock granted by OST, and
	 *    if the lock is still cached on client, AGL needs to do nothing. If
	 *    it is cancelled by other client, AGL maybe cannot obtaion new lock
	 *    for no glimpse callback triggered by AGL.
	 * 2) The former glimpse succeeded, but OST did not grant glimpse lock.
	 *    Under such case, it is quite possible that the OST will not grant
	 *    glimpse lock for AGL also.
	 * 3) The former glimpse failed, compared with other two cases, it is
	 *    relative rare. AGL can ignore such case, and it will not muchly
	 *    affect the performance.
	 */
	expire = ktime_sub_ns(ktime_get(), NSEC_PER_SEC);
	if (ktime_to_ns(lli->lli_glimpse_time) &&
	    ktime_before(expire, lli->lli_glimpse_time)) {
		up_write(&lli->lli_glimpse_sem);
		lli->lli_agl_index = 0;
		iput(inode);
		RETURN_EXIT;
	}

	CDEBUG(D_READA,
	       "Handling (init) async glimpse: inode = " DFID", idx = %llu\n",
	       PFID(&lli->lli_fid), index);

	cl_agl(inode);
	lli->lli_agl_index = 0;
	lli->lli_glimpse_time = ktime_get();
	up_write(&lli->lli_glimpse_sem);

	CDEBUG(D_READA,
	       "Handled (init) async glimpse: inode= " DFID", idx = %llu, rc = %d\n",
	       PFID(&lli->lli_fid), index, rc);

	iput(inode);

	EXIT;
}

/*
 * Callback for async stat RPC, this is called in ptlrpcd context. It prepares
 * the inode and set lock data directly in the ptlrpcd context. It will wake up
 * the directory listing process if the dentry is the waiting one.
 */
static int ll_statahead_interpret(struct req_capsule *pill,
				  struct md_op_item *item, int rc)
{
	struct lookup_intent *it = &item->mop_it;
	struct inode *dir = item->mop_dir;
	struct ll_inode_info *lli = ll_i2info(dir);
	struct ll_statahead_info *sai = lli->lli_sai;
	struct sa_entry *entry = (struct sa_entry *)item->mop_cbdata;
	struct mdt_body *body;
	struct inode *child;
	__u64 handle = 0;

	ENTRY;

	if (it_disposition(it, DISP_LOOKUP_NEG))
		rc = -ENOENT;

	/*
	 * because statahead thread will wait for all inflight RPC to finish,
	 * sai should be always valid, no need to refcount
	 */
	LASSERT(sai != NULL);
	LASSERT(entry != NULL);

	CDEBUG(D_READA, "sa_entry %.*s rc %d\n",
	       entry->se_qstr.len, entry->se_qstr.name, rc);

	if (rc != 0) {
		ll_intent_release(it);
		sa_fini_data(item);
	} else {
		/*
		 * release ibits lock ASAP to avoid deadlock when statahead
		 * thread enqueues lock on parent in readdir and another
		 * process enqueues lock on child with parent lock held, eg.
		 * unlink.
		 */
		handle = it->it_lock_handle;
		ll_intent_drop_lock(it);
		ll_unlock_md_op_lsm(&item->mop_data);
	}

	if (rc != 0) {
		spin_lock(&lli->lli_sa_lock);
		if (__sa_make_ready(sai, entry, rc))
			wake_up(&sai->sai_waitq);

		sai->sai_replied++;
		spin_unlock(&lli->lli_sa_lock);

		RETURN(rc);
	}

	entry->se_handle = handle;
	body = req_capsule_server_get(pill, &RMF_MDT_BODY);
	if (!body)
		GOTO(out, rc = -EFAULT);

	child = entry->se_inode;
	/* revalidate; unlinked and re-created with the same name */
	if (unlikely(!lu_fid_eq(&item->mop_data.op_fid2, &body->mbo_fid1))) {
		if (child) {
			entry->se_inode = NULL;
			iput(child);
		}
		/* The mdt_body is invalid. Skip this entry */
		GOTO(out, rc = -EAGAIN);
	}

	it->it_lock_handle = entry->se_handle;
	rc = md_revalidate_lock(ll_i2mdexp(dir), it, ll_inode2fid(dir), NULL);
	if (rc != 1)
		GOTO(out, rc = -EAGAIN);

	rc = ll_prep_inode(&child, pill, dir->i_sb, it);
	if (rc) {
		CERROR("getattr callback for %.*s "DFID" - rc = %d\n",
		       entry->se_qstr.len, entry->se_qstr.name,
		       PFID(&entry->se_fid), rc);
		GOTO(out, rc);
	}

	CDEBUG(D_READA, "%s: setting %.*s"DFID" l_data to inode %p\n",
	       ll_i2sbi(dir)->ll_fsname, entry->se_qstr.len,
	       entry->se_qstr.name, PFID(ll_inode2fid(child)), child);
	ll_set_lock_data(ll_i2sbi(dir)->ll_md_exp, child, it, NULL);

	entry->se_inode = child;

	if (agl_should_run(sai, child))
		ll_agl_add(sai, child, entry->se_index);

out:
	/*
	 * First it will drop ldlm ibits lock refcount by calling
	 * ll_intent_drop_lock() in spite of failures. Do not worry about
	 * calling ll_intent_drop_lock() more than once.
	 */
	ll_intent_release(&item->mop_it);
	sa_fini_data(item);
	sa_make_ready(sai, entry, rc);

	spin_lock(&lli->lli_sa_lock);
	sai->sai_replied++;
	spin_unlock(&lli->lli_sa_lock);

	RETURN(rc);
}

static inline int sa_getattr(struct inode *dir, struct md_op_item *item)
{
	struct ll_statahead_info *sai = ll_i2info(dir)->lli_sai;
	int rc;

	if (sa_has_batch_handle(sai))
		rc = md_batch_add(ll_i2mdexp(dir), sai->sai_bh, item);
	else
		rc = md_intent_getattr_async(ll_i2mdexp(dir), item);

	return rc;
}

/* async stat for file not found in dcache */
static int sa_lookup(struct inode *dir, struct sa_entry *entry)
{
	struct md_op_item *item;
	int rc;

	ENTRY;

	item = sa_prep_data(dir, NULL, entry);
	if (IS_ERR(item))
		RETURN(PTR_ERR(item));

	rc = sa_getattr(dir, item);
	if (rc < 0)
		sa_fini_data(item);

	RETURN(rc);
}

/**
 * async stat for file found in dcache, similar to .revalidate
 *
 * \retval	1 dentry valid, no RPC sent
 * \retval	0 dentry invalid, will send async stat RPC
 * \retval	negative number upon error
 */
static int sa_revalidate(struct inode *dir, struct sa_entry *entry,
			 struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct lookup_intent it = { .it_op = IT_GETATTR,
				    .it_lock_handle = 0 };
	struct md_op_item *item;
	int rc;

	ENTRY;

	if (unlikely(!inode))
		RETURN(1);

	if (d_mountpoint(dentry))
		RETURN(1);

	item = sa_prep_data(dir, inode, entry);
	if (IS_ERR(item))
		RETURN(PTR_ERR(item));

	entry->se_inode = igrab(inode);
	rc = md_revalidate_lock(ll_i2mdexp(dir), &it, ll_inode2fid(inode),
				NULL);
	if (rc == 1) {
		entry->se_handle = it.it_lock_handle;
		ll_intent_release(&it);
		sa_fini_data(item);
		RETURN(1);
	}

	rc = sa_getattr(dir, item);
	if (rc < 0) {
		entry->se_inode = NULL;
		iput(inode);
		sa_fini_data(item);
	}

	RETURN(rc);
}

/* async stat for file with @name */
static void sa_statahead(struct dentry *parent, const char *name, int len,
			 const struct lu_fid *fid)
{
	struct inode *dir = parent->d_inode;
	struct ll_inode_info *lli = ll_i2info(dir);
	struct ll_statahead_info *sai = lli->lli_sai;
	struct dentry *dentry = NULL;
	struct sa_entry *entry;
	int rc;

	ENTRY;

	entry = sa_alloc(parent, sai, sai->sai_index, name, len, fid);
	if (IS_ERR(entry))
		RETURN_EXIT;

	dentry = d_lookup(parent, &entry->se_qstr);
	if (!dentry) {
		rc = sa_lookup(dir, entry);
	} else {
		rc = sa_revalidate(dir, entry, dentry);
		if (rc == 1 && agl_should_run(sai, dentry->d_inode))
			ll_agl_add(sai, dentry->d_inode, entry->se_index);
	}

	if (dentry)
		dput(dentry);

	if (rc != 0)
		sa_make_ready(sai, entry, rc);
	else
		sai->sai_sent++;

	sai->sai_index++;

	if (sa_sent_full(sai))
		ll_statahead_flush_nowait(sai);

	EXIT;
}

/* async glimpse (agl) thread main function */
static int ll_agl_thread(void *arg)
{
	struct dentry *parent = (struct dentry *)arg;
	struct inode *dir = parent->d_inode;
	struct ll_inode_info *plli = ll_i2info(dir);
	struct ll_inode_info *clli;
	/*
	 * We already own this reference, so it is safe to take it
	 * without a lock.
	 */
	struct ll_statahead_info *sai = plli->lli_sai;

	ENTRY;

	CDEBUG(D_READA, "agl thread started: sai %p, parent %pd\n",
	       sai, parent);

	while (({set_current_state(TASK_IDLE);
		 !kthread_should_stop(); })) {
		spin_lock(&plli->lli_agl_lock);
		if (!agl_list_empty(sai)) {
			__set_current_state(TASK_RUNNING);
			clli = agl_first_entry(sai);
			list_del_init(&clli->lli_agl_list);
			spin_unlock(&plli->lli_agl_lock);
			ll_agl_trigger(&clli->lli_vfs_inode, sai);
			cond_resched();
		} else {
			spin_unlock(&plli->lli_agl_lock);
			schedule();
		}
	}
	__set_current_state(TASK_RUNNING);
	RETURN(0);
}

static void ll_stop_agl(struct ll_statahead_info *sai)
{
	struct dentry *parent = sai->sai_dentry;
	struct ll_inode_info *plli = ll_i2info(parent->d_inode);
	struct ll_inode_info *clli;
	struct task_struct *agl_task;

	spin_lock(&plli->lli_agl_lock);
	agl_task = sai->sai_agl_task;
	sai->sai_agl_task = NULL;
	spin_unlock(&plli->lli_agl_lock);
	if (!agl_task)
		return;

	CDEBUG(D_READA, "stop agl thread: sai %p pid %u\n",
	       sai, (unsigned int)agl_task->pid);
	kthread_stop(agl_task);

	spin_lock(&plli->lli_agl_lock);
	while (!agl_list_empty(sai)) {
		clli = agl_first_entry(sai);
		list_del_init(&clli->lli_agl_list);
		spin_unlock(&plli->lli_agl_lock);
		clli->lli_agl_index = 0;
		iput(&clli->lli_vfs_inode);
		spin_lock(&plli->lli_agl_lock);
	}
	spin_unlock(&plli->lli_agl_lock);
	CDEBUG(D_READA, "agl thread stopped: sai %p, parent %pd\n",
	       sai, parent);
	ll_sai_put(sai);
}

/* start agl thread */
static void ll_start_agl(struct dentry *parent, struct ll_statahead_info *sai)
{
	int node = cfs_cpt_spread_node(cfs_cpt_tab, CFS_CPT_ANY);
	struct ll_inode_info *plli;
	struct task_struct *task;

	ENTRY;

	CDEBUG(D_READA, "start agl thread: sai %p, parent %pd\n",
	       sai, parent);

	plli = ll_i2info(parent->d_inode);
	task = kthread_create_on_node(ll_agl_thread, parent, node, "ll_agl_%d",
				      plli->lli_opendir_pid);
	if (IS_ERR(task)) {
		CERROR("can't start ll_agl thread, rc: %ld\n", PTR_ERR(task));
		RETURN_EXIT;
	}
	sai->sai_agl_task = task;
	atomic_inc(&ll_i2sbi(d_inode(parent))->ll_agl_total);
	/* Get an extra reference that the thread holds */
	ll_sai_get(d_inode(parent));

	wake_up_process(task);

	EXIT;
}

/* statahead thread main function */
static int ll_statahead_thread(void *arg)
{
	struct dentry *parent = (struct dentry *)arg;
	struct inode *dir = parent->d_inode;
	struct ll_inode_info *lli = ll_i2info(dir);
	struct ll_sb_info *sbi = ll_i2sbi(dir);
	struct ll_statahead_info *sai = lli->lli_sai;
	int first = 0;
	struct md_op_data *op_data;
	struct page *page = NULL;
	struct lu_batch *bh = NULL;
	__u64 pos = 0;
	int rc = 0;

	ENTRY;

	CDEBUG(D_READA, "statahead thread starting: sai %p, parent %pd\n",
	       sai, parent);

	sai->sai_max_batch_count = sbi->ll_sa_batch_max;
	if (sai->sai_max_batch_count) {
		bh = md_batch_create(ll_i2mdexp(dir), BATCH_FL_RDONLY,
				     sai->sai_max_batch_count);
		if (IS_ERR(bh))
			GOTO(out_stop_agl, rc = PTR_ERR(bh));
	}

	sai->sai_bh = bh;
	OBD_ALLOC_PTR(op_data);
	if (!op_data)
		GOTO(out, rc = -ENOMEM);

	while (pos != MDS_DIR_END_OFF && sai->sai_task) {
		struct lu_dirpage *dp;
		struct lu_dirent  *ent;

		op_data = ll_prep_md_op_data(op_data, dir, dir, NULL, 0, 0,
					     LUSTRE_OPC_ANY, dir);
		if (IS_ERR(op_data)) {
			rc = PTR_ERR(op_data);
			break;
		}

		page = ll_get_dir_page(dir, op_data, pos);
		ll_unlock_md_op_lsm(op_data);
		if (IS_ERR(page)) {
			rc = PTR_ERR(page);
			CDEBUG(D_READA,
			       "error reading dir "DFID" at %llu /%llu opendir_pid = %u: rc = %d\n",
			       PFID(ll_inode2fid(dir)), pos, sai->sai_index,
			       lli->lli_opendir_pid, rc);
			break;
		}

		dp = page_address(page);
		for (ent = lu_dirent_start(dp);
		     ent != NULL && sai->sai_task &&
		     !sa_low_hit(sai);
		     ent = lu_dirent_next(ent)) {
			__u64 hash;
			int namelen;
			char *name;
			struct lu_fid fid;

			hash = le64_to_cpu(ent->lde_hash);
			if (unlikely(hash < pos))
				/*
				 * Skip until we find target hash value.
				 */
				continue;

			namelen = le16_to_cpu(ent->lde_namelen);
			if (unlikely(namelen == 0))
				/*
				 * Skip dummy record.
				 */
				continue;

			name = ent->lde_name;
			if (name[0] == '.') {
				if (namelen == 1) {
					/*
					 * skip "."
					 */
					continue;
				} else if (name[1] == '.' && namelen == 2) {
					/*
					 * skip ".."
					 */
					continue;
				} else if (!sai->sai_ls_all) {
					/*
					 * skip hidden files.
					 */
					sai->sai_skip_hidden++;
					continue;
				}
			}

			/*
			 * don't stat-ahead first entry.
			 */
			if (unlikely(++first == 1))
				continue;

			fid_le_to_cpu(&fid, &ent->lde_fid);

			while (({set_current_state(TASK_IDLE);
				 sai->sai_task; })) {
				spin_lock(&lli->lli_agl_lock);
				while (sa_sent_full(sai) &&
				       !agl_list_empty(sai)) {
					struct ll_inode_info *clli;

					__set_current_state(TASK_RUNNING);
					clli = agl_first_entry(sai);
					list_del_init(&clli->lli_agl_list);
					spin_unlock(&lli->lli_agl_lock);

					ll_agl_trigger(&clli->lli_vfs_inode,
						       sai);
					cond_resched();
					spin_lock(&lli->lli_agl_lock);
				}
				spin_unlock(&lli->lli_agl_lock);

				if (!sa_sent_full(sai))
					break;
				schedule();
			}
			__set_current_state(TASK_RUNNING);

			sa_statahead(parent, name, namelen, &fid);
		}

		pos = le64_to_cpu(dp->ldp_hash_end);
		ll_release_page(dir, page,
				le32_to_cpu(dp->ldp_flags) & LDF_COLLIDE);

		if (sa_low_hit(sai)) {
			rc = -EFAULT;
			atomic_inc(&sbi->ll_sa_wrong);
			CDEBUG(D_READA,
			       "Statahead for dir "DFID" hit ratio too low: hit/miss %llu/%llu, sent/replied %llu/%llu, stoppingstatahead thread: pid %d\n",
			       PFID(&lli->lli_fid), sai->sai_hit,
			       sai->sai_miss, sai->sai_sent,
			       sai->sai_replied, current->pid);
			break;
		}
	}
	ll_finish_md_op_data(op_data);

	if (rc < 0) {
		spin_lock(&lli->lli_sa_lock);
		sai->sai_task = NULL;
		lli->lli_sa_enabled = 0;
		spin_unlock(&lli->lli_sa_lock);
	}

	ll_statahead_flush_nowait(sai);

	/*
	 * statahead is finished, but statahead entries need to be cached, wait
	 * for file release closedir() call to stop me.
	 */
	while (({set_current_state(TASK_IDLE);
		 sai->sai_task; })) {
		schedule();
	}
	__set_current_state(TASK_RUNNING);

	EXIT;
out:
	if (bh) {
		rc = md_batch_stop(ll_i2mdexp(dir), sai->sai_bh);
		sai->sai_bh = NULL;
	}

out_stop_agl:
	ll_stop_agl(sai);

	/*
	 * wait for inflight statahead RPCs to finish, and then we can free sai
	 * safely because statahead RPC will access sai data
	 */
	while (sai->sai_sent != sai->sai_replied)
		/* in case we're not woken up, timeout wait */
		msleep(125);

	CDEBUG(D_READA, "%s: statahead thread stopped: sai %p, parent %pd\n",
	       sbi->ll_fsname, sai, parent);

	spin_lock(&lli->lli_sa_lock);
	sai->sai_task = NULL;
	spin_unlock(&lli->lli_sa_lock);
	wake_up(&sai->sai_waitq);

	ll_sai_put(sai);

	return rc;
}

/* authorize opened dir handle @key to statahead */
void ll_authorize_statahead(struct inode *dir, void *key)
{
	struct ll_inode_info *lli = ll_i2info(dir);

	spin_lock(&lli->lli_sa_lock);
	if (!lli->lli_opendir_key && !lli->lli_sai) {
		/*
		 * if lli_sai is not NULL, it means previous statahead is not
		 * finished yet, we'd better not start a new statahead for now.
		 */
		LASSERT(lli->lli_opendir_pid == 0);
		lli->lli_opendir_key = key;
		lli->lli_opendir_pid = current->pid;
		lli->lli_sa_enabled = 1;
	}
	spin_unlock(&lli->lli_sa_lock);
}

/*
 * deauthorize opened dir handle @key to statahead, and notify statahead thread
 * to quit if it's running.
 */
void ll_deauthorize_statahead(struct inode *dir, void *key)
{
	struct ll_inode_info *lli = ll_i2info(dir);
	struct ll_statahead_info *sai;

	LASSERT(lli->lli_opendir_key == key);
	LASSERT(lli->lli_opendir_pid != 0);

	CDEBUG(D_READA, "deauthorize statahead for "DFID"\n",
	       PFID(&lli->lli_fid));

	spin_lock(&lli->lli_sa_lock);
	lli->lli_opendir_key = NULL;
	lli->lli_opendir_pid = 0;
	lli->lli_sa_enabled = 0;
	sai = lli->lli_sai;
	if (sai && sai->sai_task) {
		/*
		 * statahead thread may not have quit yet because it needs to
		 * cache entries, now it's time to tell it to quit.
		 *
		 * wake_up_process() provides the necessary barriers
		 * to pair with set_current_state().
		 */
		struct task_struct *task = sai->sai_task;

		sai->sai_task = NULL;
		wake_up_process(task);
	}
	spin_unlock(&lli->lli_sa_lock);
}

enum {
	/**
	 * not first dirent, or is "."
	 */
	LS_NOT_FIRST_DE = 0,
	/**
	 * the first non-hidden dirent
	 */
	LS_FIRST_DE,
	/**
	 * the first hidden dirent, that is "."
	 */
	LS_FIRST_DOT_DE
};

/* file is first dirent under @dir */
static int is_first_dirent(struct inode *dir, struct dentry *dentry)
{
	struct qstr          *target = &dentry->d_name;
	struct md_op_data    *op_data;
	int                   dot_de;
	struct page	     *page = NULL;
	int                   rc = LS_NOT_FIRST_DE;
	__u64		      pos = 0;

	ENTRY;

	op_data = ll_prep_md_op_data(NULL, dir, dir, NULL, 0, 0,
				     LUSTRE_OPC_ANY, dir);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));
	/**
	 *FIXME choose the start offset of the readdir
	 */

	page = ll_get_dir_page(dir, op_data, 0);

	while (1) {
		struct lu_dirpage *dp;
		struct lu_dirent  *ent;

		if (IS_ERR(page)) {
			struct ll_inode_info *lli = ll_i2info(dir);

			rc = PTR_ERR(page);
			CERROR("%s: reading dir "DFID" at %llu opendir_pid = %u : rc = %d\n",
			       ll_i2sbi(dir)->ll_fsname,
			       PFID(ll_inode2fid(dir)), pos,
			       lli->lli_opendir_pid, rc);
			break;
		}

		dp = page_address(page);
		for (ent = lu_dirent_start(dp); ent != NULL;
		     ent = lu_dirent_next(ent)) {
			__u64 hash;
			int namelen;
			char *name;

			hash = le64_to_cpu(ent->lde_hash);
			/*
			 * The ll_get_dir_page() can return any page containing
			 * the given hash which may be not the start hash.
			 */
			if (unlikely(hash < pos))
				continue;

			namelen = le16_to_cpu(ent->lde_namelen);
			if (unlikely(namelen == 0))
				/*
				 * skip dummy record.
				 */
				continue;

			name = ent->lde_name;
			if (name[0] == '.') {
				if (namelen == 1)
					/*
					 * skip "."
					 */
					continue;
				else if (name[1] == '.' && namelen == 2)
					/*
					 * skip ".."
					 */
					continue;
				else
					dot_de = 1;
			} else {
				dot_de = 0;
			}

			if (dot_de && target->name[0] != '.') {
				CDEBUG(D_READA, "%.*s skip hidden file %.*s\n",
				       target->len, target->name,
				       namelen, name);
				continue;
			}

			if (target->len != namelen ||
			    memcmp(target->name, name, namelen) != 0)
				rc = LS_NOT_FIRST_DE;
			else if (!dot_de)
				rc = LS_FIRST_DE;
			else
				rc = LS_FIRST_DOT_DE;

			ll_release_page(dir, page, false);
			GOTO(out, rc);
		}
		pos = le64_to_cpu(dp->ldp_hash_end);
		if (pos == MDS_DIR_END_OFF) {
			/*
			 * End of directory reached.
			 */
			ll_release_page(dir, page, false);
			GOTO(out, rc);
		} else {
			/*
			 * chain is exhausted
			 * Normal case: continue to the next page.
			 */
			ll_release_page(dir, page, le32_to_cpu(dp->ldp_flags) &
					      LDF_COLLIDE);
			page = ll_get_dir_page(dir, op_data, pos);
		}
	}
	EXIT;
out:
	ll_finish_md_op_data(op_data);

	return rc;
}

/**
 * revalidate @dentryp from statahead cache
 *
 * \param[in] dir	parent directory
 * \param[in] sai	sai structure
 * \param[out] dentryp	pointer to dentry which will be revalidated
 * \param[in] unplug	unplug statahead window only (normally for negative
 *			dentry)
 * \retval		1 on success, dentry is saved in @dentryp
 * \retval		0 if revalidation failed (no proper lock on client)
 * \retval		negative number upon error
 */
static int revalidate_statahead_dentry(struct inode *dir,
				       struct ll_statahead_info *sai,
				       struct dentry **dentryp,
				       bool unplug)
{
	struct sa_entry *entry = NULL;
	struct ll_dentry_data *ldd;
	struct ll_inode_info *lli = ll_i2info(dir);
	int rc = 0;

	ENTRY;

	if ((*dentryp)->d_name.name[0] == '.') {
		if (sai->sai_ls_all ||
		    sai->sai_miss_hidden >= sai->sai_skip_hidden) {
			/*
			 * Hidden dentry is the first one, or statahead
			 * thread does not skip so many hidden dentries
			 * before "sai_ls_all" enabled as below.
			 */
		} else {
			if (!sai->sai_ls_all)
				/*
				 * It maybe because hidden dentry is not
				 * the first one, "sai_ls_all" was not
				 * set, then "ls -al" missed. Enable
				 * "sai_ls_all" for such case.
				 */
				sai->sai_ls_all = 1;

			/*
			 * Such "getattr" has been skipped before
			 * "sai_ls_all" enabled as above.
			 */
			sai->sai_miss_hidden++;
			RETURN(-EAGAIN);
		}
	}

	if (unplug)
		GOTO(out, rc = 1);

	entry = sa_get(sai, &(*dentryp)->d_name);
	if (!entry)
		GOTO(out, rc = -EAGAIN);

	if (!sa_ready(entry)) {
		spin_lock(&lli->lli_sa_lock);
		sai->sai_index_wait = entry->se_index;
		spin_unlock(&lli->lli_sa_lock);
		rc = wait_event_idle_timeout(sai->sai_waitq, sa_ready(entry),
					     cfs_time_seconds(30));
		if (rc == 0) {
			/*
			 * entry may not be ready, so it may be used by inflight
			 * statahead RPC, don't free it.
			 */
			entry = NULL;
			GOTO(out, rc = -EAGAIN);
		}
	}

	/*
	 * We need to see the value that was set immediately before we
	 * were woken up.
	 */
	if (smp_load_acquire(&entry->se_state) == SA_ENTRY_SUCC &&
	    entry->se_inode) {
		struct inode *inode = entry->se_inode;
		struct lookup_intent it = { .it_op = IT_GETATTR,
					    .it_lock_handle =
						entry->se_handle };
		__u64 bits;

		rc = md_revalidate_lock(ll_i2mdexp(dir), &it,
					ll_inode2fid(inode), &bits);
		if (rc == 1) {
			if (!(*dentryp)->d_inode) {
				struct dentry *alias;

				alias = ll_splice_alias(inode, *dentryp);
				if (IS_ERR(alias)) {
					ll_intent_release(&it);
					GOTO(out, rc = PTR_ERR(alias));
				}
				*dentryp = alias;
				/*
				 * statahead prepared this inode, transfer inode
				 * refcount from sa_entry to dentry
				 */
				entry->se_inode = NULL;
			} else if ((*dentryp)->d_inode != inode) {
				/* revalidate, but inode is recreated */
				CDEBUG(D_READA,
				       "%s: stale dentry %pd inode " DFID", statahead inode "DFID "\n",
				       ll_i2sbi(inode)->ll_fsname, *dentryp,
				       PFID(ll_inode2fid((*dentryp)->d_inode)),
				       PFID(ll_inode2fid(inode)));
				ll_intent_release(&it);
				GOTO(out, rc = -ESTALE);
			}

			if ((bits & MDS_INODELOCK_LOOKUP) &&
			    d_lustre_invalid(*dentryp))
				d_lustre_revalidate(*dentryp);
			ll_intent_release(&it);
		}
	}
out:
	/*
	 * statahead cached sa_entry can be used only once, and will be killed
	 * right after use, so if lookup/revalidate accessed statahead cache,
	 * set dentry ldd_sa_generation to parent lli_sa_generation, later if we
	 * stat this file again, we know we've done statahead before, see
	 * dentry_may_statahead().
	 */
	ldd = ll_d2d(*dentryp);
	/* ldd can be NULL if llite lookup failed. */
	if (ldd)
		ldd->lld_sa_generation = lli->lli_sa_generation;
	sa_put(dir, sai, entry);

	RETURN(rc);
}

/**
 * start statahead thread
 *
 * \param[in] dir	parent directory
 * \param[in] dentry	dentry that triggers statahead, normally the first
 *			dirent under @dir
 * \param[in] agl	indicate whether AGL is needed
 * \retval		-EAGAIN on success, because when this function is
 *			called, it's already in lookup call, so client should
 *			do it itself instead of waiting for statahead thread
 *			to do it asynchronously.
 * \retval		negative number upon error
 */
static int start_statahead_thread(struct inode *dir, struct dentry *dentry,
				  bool agl)
{
	int node = cfs_cpt_spread_node(cfs_cpt_tab, CFS_CPT_ANY);
	struct ll_inode_info *lli = ll_i2info(dir);
	struct ll_statahead_info *sai = NULL;
	struct dentry *parent = dentry->d_parent;
	struct task_struct *task;
	struct ll_sb_info *sbi = ll_i2sbi(parent->d_inode);
	int first = LS_FIRST_DE;
	int rc = 0;

	ENTRY;

	/* I am the "lli_opendir_pid" owner, only me can set "lli_sai". */
	first = is_first_dirent(dir, dentry);
	if (first == LS_NOT_FIRST_DE)
		/* It is not "ls -{a}l" operation, no need statahead for it. */
		GOTO(out, rc = -EFAULT);

	if (unlikely(atomic_inc_return(&sbi->ll_sa_running) >
				       sbi->ll_sa_running_max)) {
		CDEBUG(D_READA,
		       "Too many concurrent statahead instances, avoid new statahead instance temporarily.\n");
		GOTO(out, rc = -EMFILE);
	}

	sai = ll_sai_alloc(parent);
	if (!sai)
		GOTO(out, rc = -ENOMEM);

	sai->sai_ls_all = (first == LS_FIRST_DOT_DE);

	/*
	 * if current lli_opendir_key was deauthorized, or dir re-opened by
	 * another process, don't start statahead, otherwise the newly spawned
	 * statahead thread won't be notified to quit.
	 */
	spin_lock(&lli->lli_sa_lock);
	if (unlikely(lli->lli_sai || !lli->lli_opendir_key ||
		     lli->lli_opendir_pid != current->pid)) {
		spin_unlock(&lli->lli_sa_lock);
		GOTO(out, rc = -EPERM);
	}
	lli->lli_sai = sai;
	spin_unlock(&lli->lli_sa_lock);

	CDEBUG(D_READA, "start statahead thread: [pid %d] [parent %pd]\n",
	       current->pid, parent);

	task = kthread_create_on_node(ll_statahead_thread, parent, node,
				      "ll_sa_%u", lli->lli_opendir_pid);
	if (IS_ERR(task)) {
		spin_lock(&lli->lli_sa_lock);
		lli->lli_sai = NULL;
		spin_unlock(&lli->lli_sa_lock);
		rc = PTR_ERR(task);
		CERROR("can't start ll_sa thread, rc: %d\n", rc);
		GOTO(out, rc);
	}

	if (ll_i2sbi(parent->d_inode)->ll_flags & LL_SBI_AGL_ENABLED && agl)
		ll_start_agl(parent, sai);

	atomic_inc(&ll_i2sbi(parent->d_inode)->ll_sa_total);
	sai->sai_task = task;

	wake_up_process(task);
	/*
	 * We don't stat-ahead for the first dirent since we are already in
	 * lookup.
	 */
	RETURN(-EAGAIN);

out:
	/*
	 * once we start statahead thread failed, disable statahead so that
	 * subsequent stat won't waste time to try it.
	 */
	spin_lock(&lli->lli_sa_lock);
	if (lli->lli_opendir_pid == current->pid)
		lli->lli_sa_enabled = 0;
	spin_unlock(&lli->lli_sa_lock);

	if (sai)
		ll_sai_free(sai);
	if (first != LS_NOT_FIRST_DE)
		atomic_dec(&sbi->ll_sa_running);

	RETURN(rc);
}

/*
 * Check whether statahead for @dir was started.
 */
static inline bool ll_statahead_started(struct inode *dir, bool agl)
{
	struct ll_inode_info *lli = ll_i2info(dir);
	struct ll_statahead_info *sai;

	spin_lock(&lli->lli_sa_lock);
	sai = lli->lli_sai;
	if (sai && (sai->sai_agl_task != NULL) != agl)
		CDEBUG(D_READA,
		       "%s: Statahead AGL hint changed from %d to %d\n",
		       ll_i2sbi(dir)->ll_fsname,
		       sai->sai_agl_task != NULL, agl);
	spin_unlock(&lli->lli_sa_lock);

	return !!sai;
}

/**
 * statahead entry function, this is called when client getattr on a file, it
 * will start statahead thread if this is the first dir entry, else revalidate
 * dentry from statahead cache.
 *
 * \param[in]  dir	parent directory
 * \param[out] dentryp	dentry to getattr
 * \param[in]  agl	whether start the agl thread
 *
 * \retval		1 on success
 * \retval		0 revalidation from statahead cache failed, caller needs
 *			to getattr from server directly
 * \retval		negative number on error, caller often ignores this and
 *			then getattr from server
 */
int ll_start_statahead(struct inode *dir, struct dentry *dentry, bool agl)
{
	if (!ll_statahead_started(dir, agl))
		return start_statahead_thread(dir, dentry, agl);
	return 0;
}

/**
 * revalidate dentry from statahead cache.
 *
 * \param[in]  dir	parent directory
 * \param[out] dentryp	dentry to getattr
 * \param[in]  unplug	unplug statahead window only (normally for negative
 *			dentry)
 * \retval		1 on success
 * \retval		0 revalidation from statahead cache failed, caller needs
 *			to getattr from server directly
 * \retval		negative number on error, caller often ignores this and
 *			then getattr from server
 */
int ll_revalidate_statahead(struct inode *dir, struct dentry **dentryp,
			    bool unplug)
{
	struct ll_statahead_info *sai;
	int rc = 0;

	sai = ll_sai_get(dir);
	if (sai) {
		rc = revalidate_statahead_dentry(dir, sai, dentryp, unplug);
		CDEBUG(D_READA, "revalidate statahead %pd: rc = %d.\n",
		       *dentryp, rc);
		ll_sai_put(sai);
	}
	return rc;
}
