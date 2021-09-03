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
 * Copyright (c) 2021, DDN/Whamcloud Storage Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */
/*
 * lustre/mdd/mdd_remove.c
 *
 * Subtree removal policy used for WBC.
 *
 * Author: Qian Yingjin <qian@ddn.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <obd.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_fid.h>
#include "mdd_internal.h"

static int mdd_unpack_ent(struct lu_dirent *ent, __u16 *type)
{
	struct luda_type *lt;
	int align = sizeof(*lt) - 1;
	int len;

	fid_le_to_cpu(&ent->lde_fid, &ent->lde_fid);
	ent->lde_reclen = le16_to_cpu(ent->lde_reclen);
	ent->lde_namelen = le16_to_cpu(ent->lde_namelen);
	ent->lde_attrs = le32_to_cpu(ent->lde_attrs);

	if (unlikely(!(ent->lde_attrs & LUDA_TYPE)))
		return -EINVAL;

	len = (ent->lde_namelen + align) & ~align;
	lt = (struct luda_type *)(ent->lde_name + len);
	*type = le16_to_cpu(lt->lt_type);

	/*
	 * Make sure the name is terminated with '\0'. The data (object type)
	 * after ent::lde_name maybe broken, but we have stored such data in
	 * the output parameter @type as above.
	 */
	ent->lde_name[ent->lde_namelen] = '\0';
	return 0;
}

static int mdd_tree_remove_one(const struct lu_env *env, struct mdd_device *mdd,
			       struct mdd_object *pobj,
			       const struct lu_fid *fid,
			       const char *name, bool is_dir)
{
	struct mdd_object *obj;
	struct thandle *th;
	int rc;

	ENTRY;

	obj = mdd_object_find(env, mdd, fid);
	if (IS_ERR(obj))
		RETURN(PTR_ERR(obj));

	th = mdd_trans_create(env, mdd);
	if (IS_ERR(th)) {
		rc = PTR_ERR(th);
		if (rc != -EINPROGRESS)
			CERROR("%s: cannot get orphan thandle: rc = %d\n",
			       mdd2obd_dev(mdd)->obd_name, rc);
		GOTO(out_put, rc);
	}

	rc = mdo_declare_index_delete(env, pobj, name, th);
	if (rc)
		GOTO(out, rc);

	if (!mdd_object_exists(obj))
		GOTO(out, rc = -ENOENT);

	rc = mdo_declare_ref_del(env, obj, th);
	if (rc)
		GOTO(out, rc);

	if (S_ISDIR(mdd_object_type(obj))) {
		rc = mdo_declare_ref_del(env, obj, th);
		if (rc)
			GOTO(out, rc);

		rc = mdo_declare_ref_del(env, pobj, th);
		if (rc)
			GOTO(out, rc);
	}

	rc = mdo_declare_destroy(env, obj, th);
	if (rc)
		GOTO(out, rc);

	rc = mdd_trans_start(env, mdd, th);
	if (rc)
		GOTO(out, rc);

	mdd_write_lock(env, obj, DT_TGT_CHILD);
	rc = __mdd_index_delete(env, pobj, name, is_dir, th);
	/* FIXME: Should we remove object even dt_delete failed? */
	if (rc)
		GOTO(cleanup, rc);

	rc = mdo_ref_del(env, obj, th);
	if (rc)
		GOTO(cleanup, rc);

	if (is_dir)
		/* unlink dot */
		mdo_ref_del(env, obj, th);

	rc = mdo_destroy(env, obj, th);
cleanup:
	mdd_write_unlock(env, obj);
out:
	mdd_trans_stop(env, mdd, 0, th);
out_put:
	mdd_object_put(env, obj);
	RETURN(rc);
}

int mdd_tree_remove(const struct lu_env *env, struct mdd_object *mdd_pobj,
		    struct mdd_object *mdd_cobj, const char *name)
{
	struct mdd_object *mdd_obj;
	struct mdd_thread_info *info = mdd_env_info(env);
	//struct mdd_object *mdd_pobj = md2mdd_obj(pobj);
	//struct mdd_object *mdd_cobj = md2mdd_obj(cobj);
	struct mdd_device *mdd = mdd_obj2mdd_dev(mdd_pobj);
	struct lu_dirent *ent = &mdd_env_info(env)->mti_ent;
	struct linkea_data *ldata = &info->mti_link_data;
	const struct dt_it_ops *iops;
	const struct lu_fid *root;
	struct lu_fid fid;
	struct dt_object *obj;
	struct dt_it *it;
	__u16 type;
	int rc;

	ENTRY;

	CDEBUG(D_INODE, "Subtree removal for directory '%s'\n", name);
	root = mdd_object_fid(mdd_cobj);
	fid = *root;
	mdd_obj = mdd_cobj;
repeat:
	obj = mdd_object_child(mdd_obj);
	if (!dt_try_as_dir(env, obj))
		RETURN(-ENOTDIR);

	iops = &obj->do_index_ops->dio_it;
	it = iops->init(env, obj, LUDA_64BITHASH | LUDA_TYPE);
	if (IS_ERR(it)) {
		rc = PTR_ERR(it);
		GOTO(out, rc);
	}

	rc = iops->load(env, it, 0);
	if (rc < 0)
		GOTO(out_put, rc);
	if (rc == 0) {
		CERROR("%s: loading iterator to remove the subtree, rc = 0\n",
		       mdd2obd_dev(mdd)->obd_name);
		rc = iops->next(env, it);
	}

	do {
		rc = iops->rec(env, it, (struct dt_rec *)ent,
			       LUDA_64BITHASH | LUDA_TYPE);
		if (rc == 0)
			rc = mdd_unpack_ent(ent, &type);
		if (rc) {
			CERROR("%s: failed to iterate backend: rc = %d\n",
			       mdd2obd_dev(mdd)->obd_name, rc);
			goto next;
		}

		/* skip dot and dotdot entries */
		if (name_is_dot_or_dotdot(ent->lde_name, ent->lde_namelen))
			goto next;

		if (!fid_seq_in_fldb(fid_seq(&ent->lde_fid)))
			goto next;

		if (S_ISDIR(type)) {
			iops->put(env, it);
			iops->fini(env, it);

			if (!lu_fid_eq(root, &fid))
				mdd_object_put(env, mdd_obj);

			fid = ent->lde_fid;
			CDEBUG(D_INODE, "Sink to next level '%s' "DFID"\n",
			       ent->lde_name, PFID(&fid));
			mdd_obj = mdd_object_find(env, mdd, &fid);
			if (IS_ERR(mdd_obj))
				RETURN(PTR_ERR(mdd_obj));

			/* Sink down to next level of the directory. */
			GOTO(repeat, rc);
		}

		CDEBUG(D_INODE, "Removing entry: '%s' "DFID"\n",
		       ent->lde_name, PFID(&ent->lde_fid));
		rc = mdd_tree_remove_one(env, mdd, mdd_obj, &ent->lde_fid,
					 ent->lde_name, S_ISDIR(type));
		if (rc)
			CERROR("%s: failed to remove '%s': rc = %d\n",
			       mdd2obd_dev(mdd)->obd_name, ent->lde_name, rc);
next:
		rc = iops->next(env, it);
	} while (rc == 0);

out_put:
	iops->put(env, it);
	iops->fini(env, it);

	/* Finish to iterator over a children subtree. */
	if (!lu_fid_eq(root, &fid)) {
		char *filename = info->mti_name;
		struct mdd_object *parent;
		struct lu_name lname;
		struct lu_fid pfid;

		rc = mdd_links_read_with_rec(env, mdd_obj, ldata);
		mdd_object_put(env, mdd_obj);
		if (rc)
			GOTO(out, rc);

		LASSERT(ldata->ld_leh != NULL);
		/* Directory should only have one parent. */
		if (ldata->ld_leh->leh_reccount > 1)
			GOTO(out, rc = -EINVAL);

		ldata->ld_lee = (struct link_ea_entry *)(ldata->ld_leh + 1);
		linkea_entry_unpack(ldata->ld_lee, &ldata->ld_reclen,
				    &lname, &pfid);

		/* Note: lname might miss \0 at the end */
		snprintf(filename, sizeof(info->mti_name), "%.*s",
			 lname.ln_namelen, lname.ln_name);
		if (lu_fid_eq(root, &pfid)) {
			CDEBUG(D_INODE, "Top level '%s':"DFID" root="DFID"\n",
			       filename, PFID(&fid), PFID(&pfid));
			rc = mdd_tree_remove_one(env, mdd, mdd_cobj, &fid,
						 filename, true);
			if (rc)
				GOTO(out, rc);

			mdd_obj = mdd_cobj;
			fid = pfid;
		} else {
			parent = mdd_object_find(env, mdd, &pfid);
			if (IS_ERR(parent))
				RETURN(PTR_ERR(parent));

			CDEBUG(D_INODE, "Iter over '%s':"DFID" root="DFID"\n",
			       filename, PFID(&fid), PFID(root));
			rc = mdd_tree_remove_one(env, mdd, parent, &fid,
						 filename, true);
			if (rc)
				GOTO(out, rc);

			mdd_obj = parent;
			fid = pfid;
		}
		GOTO(repeat, rc);
	}

	rc = mdd_tree_remove_one(env, mdd, mdd_pobj, root, name, true);
out:
	RETURN(rc);
}

void mdd_remove_item_add(struct mdd_object *obj)
{
	struct mdd_device *mdd = mdd_obj2mdd_dev(obj);
	struct mdd_dir_remover *remover = &mdd->mdd_remover;

	ENTRY;

	LASSERT(obj->mod_flags & REMOVED_OBJ);

	spin_lock(&remover->mrm_lock);
	mdd_object_get(obj);
	LASSERT(list_empty(&obj->mod_remove_item));
	list_add_tail(&obj->mod_remove_item, &remover->mrm_list);
	CDEBUG(D_INFO, "add "DFID" into asynchonous removal list.\n",
	       PFID(mdd_object_fid(obj)));
	spin_unlock(&remover->mrm_lock);

	wake_up_process(remover->mrm_task);
}

static int mdd_tree_remove_background(struct lu_env *env,
				      struct mdd_device *mdd,
				      struct mdd_dir_remover *remover)
{
	struct mdd_object *obj = NULL;
	struct mdd_thread_info *info = mdd_env_info(env);
	char *name = info->mti_fidname;
	int rc;

	ENTRY;

	spin_lock(&remover->mrm_lock);
	if (!list_empty(&remover->mrm_list)) {
		obj = list_entry(remover->mrm_list.next, typeof(*obj),
				 mod_remove_item);
		list_del_init(&obj->mod_remove_item);
	}
	spin_unlock(&remover->mrm_lock);

	if (!obj)
		RETURN(0);

	LASSERT(obj->mod_flags & REMOVED_OBJ);
	snprintf(name, sizeof(info->mti_fidname), DFID_NOBRACE,
		 PFID(mdd_object_fid(obj)));
	rc = mdd_tree_remove(env, remover->mrm_root, obj, name);
	if (rc)
		CERROR("%s: failed to remove subtree FID %s: rc = %d\n",
		       mdd2obd_dev(mdd)->obd_name, name, rc);

	mdd_object_put(env, obj);
	RETURN(rc);
}

static int mdd_remover_main(void *args)
{
	struct mdd_device *mdd = (struct mdd_device *)args;
	struct mdd_dir_remover *remover = &mdd->mdd_remover;
	struct lu_env *env = &remover->mrm_env;

	ENTRY;

	while (({set_current_state(TASK_IDLE);
		 !kthread_should_stop(); })) {
		if (!list_empty(&remover->mrm_list)) {
			__set_current_state(TASK_RUNNING);
			mdd_tree_remove_background(env, mdd, remover);
			cond_resched();
		} else {
			schedule();
		}
	}
	__set_current_state(TASK_RUNNING);

	RETURN(0);
}

int mdd_remover_init(const struct lu_env *env, struct mdd_device *mdd)
{
	struct mdd_dir_remover *remover = &mdd->mdd_remover;
	struct task_struct *task;
	struct md_object *mdo;
	char *name = NULL;
	int rc;

	ENTRY;

	spin_lock_init(&remover->mrm_lock);
	INIT_LIST_HEAD(&remover->mrm_list);

	rc = lu_env_init(&remover->mrm_env, LCT_MD_THREAD);
	if (rc)
		RETURN(rc);

	OBD_ALLOC(name, MTI_NAME_MAXLEN);
	if (name == NULL)
		GOTO(err, rc = -ENOMEM);

	snprintf(name, MTI_NAME_MAXLEN, "mdd_remover_%s",
		 mdd2obd_dev(mdd)->obd_name);

	mdo = mdo_locate(env, &mdd->mdd_md_dev,
			 lu_object_fid(&mdd->mdd_orphans->do_lu));
	if (IS_ERR(mdo)) {
		rc = PTR_ERR(mdo);
		CERROR("%s: cannot locate Orphan object: rc = %d.\n",
		       mdd2obd_dev(mdd)->obd_name, rc);
		GOTO(err, rc);
	}

	LASSERT(lu_object_exists(&mdo->mo_lu));
	remover->mrm_root = md2mdd_obj(mdo);
	task = kthread_create(mdd_remover_main, mdd, name);
	if (IS_ERR(task)) {
		rc = PTR_ERR(task);
		CERROR("%s: cannot start dir subtree remove thread: rc = %d\n",
		       mdd2obd_dev(mdd)->obd_name, rc);
		GOTO(err_put, rc);
	}
	remover->mrm_task = task;
	wake_up_process(task);

	OBD_FREE(name, MTI_NAME_MAXLEN);
	RETURN(0);

err_put:
	mdd_object_put(env, remover->mrm_root);
	remover->mrm_root = NULL;
err:
	if (name)
		OBD_FREE(name, MTI_NAME_MAXLEN);
	lu_env_fini(&remover->mrm_env);
	RETURN(rc);
}

void mdd_remover_fini(const struct lu_env *env, struct mdd_device *mdd)
{
	struct mdd_dir_remover *remover = &mdd->mdd_remover;
	struct mdd_object *obj, *next;

	if (!remover->mrm_task)
		return;

	kthread_stop(remover->mrm_task);
	remover->mrm_task = NULL;

	list_for_each_entry_safe(obj, next, &remover->mrm_list,
				 mod_remove_item) {
		list_del_init(&obj->mod_remove_item);
		mdd_object_put(env, obj);
	}

	lu_env_fini(&remover->mrm_env);

	if (remover->mrm_root) {
		printk("PUT root\n");
		mdd_object_put(env, remover->mrm_root);
		remover->mrm_root = NULL;
	}
}
