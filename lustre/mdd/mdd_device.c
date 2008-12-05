/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
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
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright  2008 Sun Microsystems, Inc. All rights reserved
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdd/mdd_device.c
 *
 * Lustre Metadata Server (mdd) routines
 *
 * Author: Wang Di <wangdi@clusterfs.com>
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_MDS

#include <linux/module.h>
#include <linux/jbd.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_ver.h>
#include <obd_support.h>
#include <lprocfs_status.h>

#include <lustre_disk.h>
#include <lustre_fid.h>
#include <linux/ldiskfs_fs.h>
#include <lustre_mds.h>
#include <lustre/lustre_idl.h>
#include <lustre_disk.h>      /* for changelogs */
#include <lustre_param.h>

#include "mdd_internal.h"

const struct md_device_operations mdd_ops;

static const char mdd_root_dir_name[] = "ROOT";

static int mdd_device_init(const struct lu_env *env, struct lu_device *d,
                           const char *name, struct lu_device *next)
{
        struct mdd_device *mdd = lu2mdd_dev(d);
        int rc;
        ENTRY;

        mdd->mdd_child = lu2dt_dev(next);

        /* Prepare transactions callbacks. */
        mdd->mdd_txn_cb.dtc_txn_start = mdd_txn_start_cb;
        mdd->mdd_txn_cb.dtc_txn_stop = mdd_txn_stop_cb;
        mdd->mdd_txn_cb.dtc_txn_commit = mdd_txn_commit_cb;
        mdd->mdd_txn_cb.dtc_cookie = mdd;
        CFS_INIT_LIST_HEAD(&mdd->mdd_txn_cb.dtc_linkage);
        mdd->mdd_atime_diff = MAX_ATIME_DIFF;

        rc = mdd_procfs_init(mdd, name);
        RETURN(rc);
}

static struct lu_device *mdd_device_fini(const struct lu_env *env,
                                         struct lu_device *d)
{
        struct mdd_device *mdd = lu2mdd_dev(d);
        struct lu_device *next = &mdd->mdd_child->dd_lu_dev;
        int rc;

        rc = mdd_procfs_fini(mdd);
        if (rc) {
                CERROR("proc fini error %d \n", rc);
                return ERR_PTR(rc);
        }
        return next;
}

static void mdd_changelog_fini(const struct lu_env *env,
                               struct mdd_device *mdd);

static void mdd_device_shutdown(const struct lu_env *env,
                                struct mdd_device *m, struct lustre_cfg *cfg)
{
        ENTRY;
        mdd_changelog_fini(env, m);
        dt_txn_callback_del(m->mdd_child, &m->mdd_txn_cb);
        if (m->mdd_obd_dev)
                mdd_fini_obd(env, m, cfg);
        orph_index_fini(env, m);
        /* remove upcall device*/
        md_upcall_fini(&m->mdd_md_dev);
        EXIT;
}

static int changelog_init_cb(struct llog_handle *llh, struct llog_rec_hdr *hdr,
                             void *data)
{
        struct mdd_device *mdd = (struct mdd_device *)data;
        struct llog_changelog_rec *rec = (struct llog_changelog_rec *)hdr;
        ENTRY;

        if (!(llh->lgh_hdr->llh_flags & LLOG_F_IS_PLAIN)) {
                CERROR("log is not plain\n");
                RETURN(-EINVAL);
        }
        if (rec->cr_hdr.lrh_type != CHANGELOG_REC) {
                CERROR("Not a changelog rec? %d\n", rec->cr_hdr.lrh_type);
                RETURN(-EINVAL);
        }

        CDEBUG(D_INODE,
               "seeing record at index %d/%d/"LPU64" t=%x %.*s in log "LPX64"\n",
               hdr->lrh_index, rec->cr_hdr.lrh_index, rec->cr_index,
               rec->cr_type, rec->cr_namelen, rec->cr_name,
               llh->lgh_id.lgl_oid);

        mdd->mdd_cl.mc_index = rec->cr_index;
        RETURN(LLOG_PROC_BREAK);
}

static int mdd_changelog_llog_init(struct mdd_device *mdd)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_ctxt *ctxt;
        int rc;

        ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
        if (ctxt == NULL) {
                CERROR("no context\n");
                return -EINVAL;
        }
        if (!ctxt->loc_handle) {
                CERROR("no handle\n");
                return -EINVAL;
        }
        rc = llog_cat_reverse_process(ctxt->loc_handle, changelog_init_cb, mdd);
        llog_ctxt_put(ctxt);

        if (rc < 0)
                CERROR("changelog init failed: %d\n", rc);
        else
                rc = 0; /* llog_proc_break is ok */

        CDEBUG(D_INODE, "changelog_init index="LPU64"\n", mdd->mdd_cl.mc_index);

        return rc;
}

static int mdd_changelog_init(const struct lu_env *env, struct mdd_device *mdd)
{
        int rc;

        mdd->mdd_cl.mc_index = 0;
        spin_lock_init(&mdd->mdd_cl.mc_lock);
        cfs_waitq_init(&mdd->mdd_cl.mc_waitq);

        mdd->mdd_cl.mc_starttime = cfs_time_current_64();
        mdd->mdd_cl.mc_flags = 0; /* off by default */
        mdd->mdd_cl.mc_mask = CL_DEFMASK;
        rc = mdd_changelog_llog_init(mdd);
        if (rc) {
                CERROR("Changelog setup during init failed %d\n", rc);
                mdd->mdd_cl.mc_flags |= CLM_ERR;
        }
        return rc;
}

static void mdd_changelog_fini(const struct lu_env *env, struct mdd_device *mdd)
{
        mdd->mdd_cl.mc_flags = 0;
}

/** Add a changelog entry \a rec to the changelog llog
 * \param mdd
 * \param rec
 * \param handle - currently ignored since llogs start their own transaction;
 *                 this will hopefully be fixed in llog rewrite
 * \retval 0 ok
 */
int mdd_changelog_llog_write(struct mdd_device         *mdd,
                             struct llog_changelog_rec *rec,
                             struct thandle            *handle)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_ctxt *ctxt;
        int rc;

        if ((mdd->mdd_cl.mc_mask & (1 << rec->cr_type)) == 0)
                return 0;

        rec->cr_hdr.lrh_len = llog_data_len(sizeof(*rec) + rec->cr_namelen);
        /* llog_lvfs_write_rec sets the llog tail len */
        rec->cr_hdr.lrh_type = CHANGELOG_REC;
        rec->cr_time = cfs_time_current_64();
        spin_lock(&mdd->mdd_cl.mc_lock);
        /* NB: I suppose it's possible llog_add adds out of order wrt cr_index,
           but as long as the MDD transactions are ordered correctly for e.g.
           rename conflicts, I don't think this should matter. */
        rec->cr_index = ++mdd->mdd_cl.mc_index;
        spin_unlock(&mdd->mdd_cl.mc_lock);
        ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
        if (ctxt == NULL)
                return -ENXIO;

        /* nested journal transaction */
        rc = llog_add(ctxt, &rec->cr_hdr, NULL, NULL, 0);
        llog_ctxt_put(ctxt);

        cfs_waitq_signal(&mdd->mdd_cl.mc_waitq);

        return rc;
}

/** Remove entries with indicies up to and including \a endrec from the
 *  changelog
 * \param mdd
 * \param endrec
 * \retval 0 ok
 */
int mdd_changelog_llog_cancel(struct mdd_device *mdd, long long endrec)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_ctxt *ctxt;
        int rc;

        ctxt = llog_get_context(obd, LLOG_CHANGELOG_ORIG_CTXT);
        if (ctxt == NULL)
                return -ENXIO;

        /* Some records purged; reset repeat-access time */
        mdd->mdd_cl.mc_starttime = cfs_time_current_64();

        rc = llog_cancel(ctxt, NULL, 1, (struct llog_cookie *)&endrec, 0);

        llog_ctxt_put(ctxt);

        return rc;
}

/** Add a CL_MARK record to the changelog
 * \param mdd
 * \param markerflags - CLM_*
 * \retval 0 ok
 */
int mdd_changelog_write_header(struct mdd_device *mdd, int markerflags)
{
        struct obd_device *obd = mdd2obd_dev(mdd);
        struct llog_changelog_rec *rec;
        int reclen;
        int len = strlen(obd->obd_name);
        int rc;
        ENTRY;

        reclen = llog_data_len(sizeof(*rec) + len);
        OBD_ALLOC(rec, reclen);
        if (rec == NULL)
                RETURN(-ENOMEM);

        rec->cr_flags = CLF_VERSION;
        rec->cr_type = CL_MARK;
        rec->cr_namelen = len;
        memcpy(rec->cr_name, obd->obd_name, rec->cr_namelen);
        /* Status and action flags */
        rec->cr_markerflags = mdd->mdd_cl.mc_flags | markerflags;

        rc = mdd_changelog_llog_write(mdd, rec, NULL);

        /* assume on or off event; reset repeat-access time */
        mdd->mdd_cl.mc_starttime = rec->cr_time;

        OBD_FREE(rec, reclen);
        RETURN(rc);
}

static int mdd_process_config(const struct lu_env *env,
                              struct lu_device *d, struct lustre_cfg *cfg)
{
        struct mdd_device *m    = lu2mdd_dev(d);
        struct dt_device  *dt   = m->mdd_child;
        struct lu_device  *next = &dt->dd_lu_dev;
        int rc;
        ENTRY;

        switch (cfg->lcfg_command) {
        case LCFG_PARAM: {
                struct lprocfs_static_vars lvars;

                lprocfs_mdd_init_vars(&lvars);
                rc = class_process_proc_param(PARAM_MDD, lvars.obd_vars, cfg,m);
                if (rc > 0 || rc == -ENOSYS)
                        /* we don't understand; pass it on */
                        rc = next->ld_ops->ldo_process_config(env, next, cfg);
                break;
        }
        case LCFG_SETUP:
                rc = next->ld_ops->ldo_process_config(env, next, cfg);
                if (rc)
                        GOTO(out, rc);
                dt->dd_ops->dt_conf_get(env, dt, &m->mdd_dt_conf);

                rc = mdd_init_obd(env, m, cfg);
                if (rc) {
                        CERROR("lov init error %d \n", rc);
                        GOTO(out, rc);
                }
                rc = mdd_txn_init_credits(env, m);
                if (rc)
                        break;

                mdd_changelog_init(env, m);
                break;
        case LCFG_CLEANUP:
                mdd_device_shutdown(env, m, cfg);
        default:
                rc = next->ld_ops->ldo_process_config(env, next, cfg);
                break;
        }
out:
        RETURN(rc);
}

#if 0
static int mdd_lov_set_nextid(const struct lu_env *env,
                              struct mdd_device *mdd)
{
        struct mds_obd *mds = &mdd->mdd_obd_dev->u.mds;
        int rc;
        ENTRY;

        LASSERT(mds->mds_lov_objids != NULL);
        rc = obd_set_info_async(mds->mds_osc_exp, strlen(KEY_NEXT_ID),
                                KEY_NEXT_ID, mds->mds_lov_desc.ld_tgt_count,
                                mds->mds_lov_objids, NULL);

        RETURN(rc);
}

static int mdd_cleanup_unlink_llog(const struct lu_env *env,
                                   struct mdd_device *mdd)
{
        /* XXX: to be implemented! */
        return 0;
}
#endif

static int mdd_recovery_complete(const struct lu_env *env,
                                 struct lu_device *d)
{
        struct mdd_device *mdd = lu2mdd_dev(d);
        struct lu_device *next = &mdd->mdd_child->dd_lu_dev;
        struct obd_device *obd = mdd2obd_dev(mdd);
        int rc;
        ENTRY;

        LASSERT(mdd != NULL);
        LASSERT(obd != NULL);
#if 0
        /* XXX: Do we need this in new stack? */
        rc = mdd_lov_set_nextid(env, mdd);
        if (rc) {
                CERROR("mdd_lov_set_nextid() failed %d\n",
                       rc);
                RETURN(rc);
        }

        /* XXX: cleanup unlink. */
        rc = mdd_cleanup_unlink_llog(env, mdd);
        if (rc) {
                CERROR("mdd_cleanup_unlink_llog() failed %d\n",
                       rc);
                RETURN(rc);
        }
#endif
        /* Call that with obd_recovering = 1 just to update objids */
        obd_notify(obd->u.mds.mds_osc_obd, NULL, (obd->obd_async_recov ?
                    OBD_NOTIFY_SYNC_NONBLOCK : OBD_NOTIFY_SYNC), NULL);

        /* Drop obd_recovering to 0 and call o_postrecov to recover mds_lov */
        obd->obd_recovering = 0;
        obd->obd_type->typ_dt_ops->o_postrecov(obd);

        /* XXX: orphans handling. */
        __mdd_orphan_cleanup(env, mdd);
        rc = next->ld_ops->ldo_recovery_complete(env, next);

        RETURN(rc);
}

static int mdd_prepare(const struct lu_env *env,
                       struct lu_device *pdev,
                       struct lu_device *cdev)
{
        struct mdd_device *mdd = lu2mdd_dev(cdev);
        struct lu_device *next = &mdd->mdd_child->dd_lu_dev;
        struct dt_object *root;
        int rc;

        ENTRY;
        rc = next->ld_ops->ldo_prepare(env, cdev, next);
        if (rc)
                GOTO(out, rc);

        dt_txn_callback_add(mdd->mdd_child, &mdd->mdd_txn_cb);
        root = dt_store_open(env, mdd->mdd_child, "", mdd_root_dir_name,
                             &mdd->mdd_root_fid);
        if (!IS_ERR(root)) {
                LASSERT(root != NULL);
                lu_object_put(env, &root->do_lu);
                rc = orph_index_init(env, mdd);
        } else
                rc = PTR_ERR(root);

out:
        RETURN(rc);
}

const struct lu_device_operations mdd_lu_ops = {
        .ldo_object_alloc      = mdd_object_alloc,
        .ldo_process_config    = mdd_process_config,
        .ldo_recovery_complete = mdd_recovery_complete,
        .ldo_prepare           = mdd_prepare,
};

/*
 * No permission check is needed.
 */
static int mdd_root_get(const struct lu_env *env,
                        struct md_device *m, struct lu_fid *f)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);

        ENTRY;
        *f = mdd->mdd_root_fid;
        RETURN(0);
}

/*
 * No permission check is needed.
 */
static int mdd_statfs(const struct lu_env *env, struct md_device *m,
                      struct kstatfs *sfs)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        int rc;

        ENTRY;

        rc = mdd_child_ops(mdd)->dt_statfs(env, mdd->mdd_child, sfs);

        RETURN(rc);
}

/*
 * No permission check is needed.
 */
static int mdd_maxsize_get(const struct lu_env *env, struct md_device *m,
                           int *md_size, int *cookie_size)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        ENTRY;

        *md_size = mdd_lov_mdsize(env, mdd);
        *cookie_size = mdd_lov_cookiesize(env, mdd);

        RETURN(0);
}

static int mdd_init_capa_ctxt(const struct lu_env *env, struct md_device *m,
                              int mode, unsigned long timeout, __u32 alg,
                              struct lustre_capa_key *keys)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        struct mds_obd    *mds = &mdd2obd_dev(mdd)->u.mds;
        int rc;
        ENTRY;

        mds->mds_capa_keys = keys;
        rc = mdd_child_ops(mdd)->dt_init_capa_ctxt(env, mdd->mdd_child, mode,
                                                   timeout, alg, keys);
        RETURN(rc);
}

static int mdd_update_capa_key(const struct lu_env *env,
                               struct md_device *m,
                               struct lustre_capa_key *key)
{
        struct mdd_device *mdd = lu2mdd_dev(&m->md_lu_dev);
        struct obd_export *lov_exp = mdd2obd_dev(mdd)->u.mds.mds_osc_exp;
        int rc;
        ENTRY;

        rc = obd_set_info_async(lov_exp, sizeof(KEY_CAPA_KEY), KEY_CAPA_KEY,
                                sizeof(*key), key, NULL);
        RETURN(rc);
}

static struct lu_device *mdd_device_alloc(const struct lu_env *env,
                                          struct lu_device_type *t,
                                          struct lustre_cfg *lcfg)
{
        struct lu_device  *l;
        struct mdd_device *m;

        OBD_ALLOC_PTR(m);
        if (m == NULL) {
                l = ERR_PTR(-ENOMEM);
        } else {
                md_device_init(&m->mdd_md_dev, t);
                l = mdd2lu_dev(m);
                l->ld_ops = &mdd_lu_ops;
                m->mdd_md_dev.md_ops = &mdd_ops;
                md_upcall_init(&m->mdd_md_dev, NULL);
        }

        return l;
}

static struct lu_device *mdd_device_free(const struct lu_env *env,
                                         struct lu_device *lu)
{
        struct mdd_device *m = lu2mdd_dev(lu);
        struct lu_device  *next = &m->mdd_child->dd_lu_dev;
        ENTRY;

        LASSERT(atomic_read(&lu->ld_ref) == 0);
        md_device_fini(&m->mdd_md_dev);
        OBD_FREE_PTR(m);
        RETURN(next);
}

static struct obd_ops mdd_obd_device_ops = {
        .o_owner = THIS_MODULE
};

/* context key constructor/destructor: mdd_ucred_key_init, mdd_ucred_key_fini */
LU_KEY_INIT_FINI(mdd_ucred, struct md_ucred);

static struct lu_context_key mdd_ucred_key = {
        .lct_tags = LCT_SESSION,
        .lct_init = mdd_ucred_key_init,
        .lct_fini = mdd_ucred_key_fini
};

struct md_ucred *md_ucred(const struct lu_env *env)
{
        LASSERT(env->le_ses != NULL);
        return lu_context_key_get(env->le_ses, &mdd_ucred_key);
}
EXPORT_SYMBOL(md_ucred);

/*
 * context key constructor/destructor:
 * mdd_capainfo_key_init, mdd_capainfo_key_fini
 */
LU_KEY_INIT_FINI(mdd_capainfo, struct md_capainfo);

struct lu_context_key mdd_capainfo_key = {
        .lct_tags = LCT_SESSION,
        .lct_init = mdd_capainfo_key_init,
        .lct_fini = mdd_capainfo_key_fini
};

struct md_capainfo *md_capainfo(const struct lu_env *env)
{
        /* NB, in mdt_init0 */
        if (env->le_ses == NULL)
                return NULL;
        return lu_context_key_get(env->le_ses, &mdd_capainfo_key);
}
EXPORT_SYMBOL(md_capainfo);

/* type constructor/destructor: mdd_type_init, mdd_type_fini */
LU_TYPE_INIT_FINI(mdd, &mdd_thread_key, &mdd_ucred_key, &mdd_capainfo_key);

const struct md_device_operations mdd_ops = {
        .mdo_statfs         = mdd_statfs,
        .mdo_root_get       = mdd_root_get,
        .mdo_maxsize_get    = mdd_maxsize_get,
        .mdo_init_capa_ctxt = mdd_init_capa_ctxt,
        .mdo_update_capa_key= mdd_update_capa_key,
#ifdef HAVE_QUOTA_SUPPORT
        .mdo_quota          = {
                .mqo_notify      = mdd_quota_notify,
                .mqo_setup       = mdd_quota_setup,
                .mqo_cleanup     = mdd_quota_cleanup,
                .mqo_recovery    = mdd_quota_recovery,
                .mqo_check       = mdd_quota_check,
                .mqo_on          = mdd_quota_on,
                .mqo_off         = mdd_quota_off,
                .mqo_setinfo     = mdd_quota_setinfo,
                .mqo_getinfo     = mdd_quota_getinfo,
                .mqo_setquota    = mdd_quota_setquota,
                .mqo_getquota    = mdd_quota_getquota,
                .mqo_getoinfo    = mdd_quota_getoinfo,
                .mqo_getoquota   = mdd_quota_getoquota,
                .mqo_invalidate  = mdd_quota_invalidate,
                .mqo_finvalidate = mdd_quota_finvalidate
        }
#endif
};

static struct lu_device_type_operations mdd_device_type_ops = {
        .ldto_init = mdd_type_init,
        .ldto_fini = mdd_type_fini,

        .ldto_start = mdd_type_start,
        .ldto_stop  = mdd_type_stop,

        .ldto_device_alloc = mdd_device_alloc,
        .ldto_device_free  = mdd_device_free,

        .ldto_device_init    = mdd_device_init,
        .ldto_device_fini    = mdd_device_fini
};

static struct lu_device_type mdd_device_type = {
        .ldt_tags     = LU_DEVICE_MD,
        .ldt_name     = LUSTRE_MDD_NAME,
        .ldt_ops      = &mdd_device_type_ops,
        .ldt_ctx_tags = LCT_MD_THREAD
};

/* context key constructor: mdd_key_init */
LU_KEY_INIT(mdd, struct mdd_thread_info);

static void mdd_key_fini(const struct lu_context *ctx,
                         struct lu_context_key *key, void *data)
{
        struct mdd_thread_info *info = data;
        if (info->mti_max_lmm != NULL)
                OBD_FREE(info->mti_max_lmm, info->mti_max_lmm_size);
        if (info->mti_max_cookie != NULL)
                OBD_FREE(info->mti_max_cookie, info->mti_max_cookie_size);
        mdd_buf_put(&info->mti_big_buf);

        OBD_FREE_PTR(info);
}

/* context key: mdd_thread_key */
LU_CONTEXT_KEY_DEFINE(mdd, LCT_MD_THREAD);

static struct lu_local_obj_desc llod_capa_key = {
        .llod_name      = CAPA_KEYS,
        .llod_oid       = MDD_CAPA_KEYS_OID,
        .llod_is_index  = 0,
};

static struct lu_local_obj_desc llod_mdd_orphan = {
        .llod_name      = orph_index_name,
        .llod_oid       = MDD_ORPHAN_OID,
        .llod_is_index  = 1,
        .llod_feat      = &dt_directory_features,
};

static struct lu_local_obj_desc llod_mdd_root = {
        .llod_name      = mdd_root_dir_name,
        .llod_oid       = MDD_ROOT_INDEX_OID,
        .llod_is_index  = 1,
        .llod_feat      = &dt_directory_features,
};

static int __init mdd_mod_init(void)
{
        struct lprocfs_static_vars lvars;
        lprocfs_mdd_init_vars(&lvars);

        llo_local_obj_register(&llod_capa_key);
        llo_local_obj_register(&llod_mdd_orphan);
        llo_local_obj_register(&llod_mdd_root);

        return class_register_type(&mdd_obd_device_ops, NULL, lvars.module_vars,
                                   LUSTRE_MDD_NAME, &mdd_device_type);
}

static void __exit mdd_mod_exit(void)
{
        llo_local_obj_unregister(&llod_capa_key);
        llo_local_obj_unregister(&llod_mdd_orphan);
        llo_local_obj_unregister(&llod_mdd_root);

        class_unregister_type(LUSTRE_MDD_NAME);
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Meta-data Device Prototype ("LUSTRE_MDD_NAME")");
MODULE_LICENSE("GPL");

cfs_module(mdd, "0.1.0", mdd_mod_init, mdd_mod_exit);
