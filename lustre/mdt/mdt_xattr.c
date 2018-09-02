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
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/mdt/mdt_xattr.c
 *
 * Lustre Metadata Target (mdt) extended attributes management.
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Huang Hua <huanghua@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <linux/xattr.h>
#include <obd_class.h>
#include <lustre_nodemap.h>
#include <lustre_acl.h>
#include "mdt_internal.h"


/* return EADATA length to the caller. negative value means error */
static int mdt_getxattr_pack_reply(struct mdt_thread_info * info)
{
	struct req_capsule *pill = info->mti_pill;
	struct ptlrpc_request *req = mdt_info_req(info);
	const char *xattr_name;
	u64 valid;
	static const char user_string[] = "user.";
	int size;
	int rc = 0;
	int rc2;
	ENTRY;

	valid = info->mti_body->mbo_valid & (OBD_MD_FLXATTR | OBD_MD_FLXATTRLS);

	/* Determine how many bytes we need */
        if (valid == OBD_MD_FLXATTR) {
		xattr_name = req_capsule_client_get(pill, &RMF_NAME);
		if (!xattr_name)
			RETURN(-EFAULT);

		if (!(exp_connect_flags(req->rq_export) & OBD_CONNECT_XATTR) &&
		    !strncmp(xattr_name, user_string, sizeof(user_string) - 1))
			RETURN(-EOPNOTSUPP);

		size = mo_xattr_get(info->mti_env,
				    mdt_object_child(info->mti_object),
				    &LU_BUF_NULL, xattr_name);
		if (size == -ENODATA) {
			/* XXX: Some client code will not handle -ENODATA
			 * for XATTR_NAME_LOV (trusted.lov) properly. */
			if (strcmp(xattr_name, XATTR_NAME_LOV) == 0)
				rc = 0;
			else
				rc = -ENODATA;

			size = 0;
		}
	} else if (valid == OBD_MD_FLXATTRLS) {
		xattr_name = "list";
		size = mo_xattr_list(info->mti_env,
				     mdt_object_child(info->mti_object),
				     &LU_BUF_NULL);
	} else if (valid == OBD_MD_FLXATTRALL) {
		xattr_name = "all";
		/* N.B. eadatasize = 0 is not valid for FLXATTRALL */
		/* We could calculate accurate sizes, but this would
		 * introduce a lot of overhead, let's do it later... */
		size = info->mti_body->mbo_eadatasize;
		req_capsule_set_size(pill, &RMF_EAVALS, RCL_SERVER, size);
		req_capsule_set_size(pill, &RMF_EAVALS_LENS, RCL_SERVER, size);
	} else {
		CDEBUG(D_INFO, "Valid bits: %#llx\n",
		       info->mti_body->mbo_valid);
		RETURN(-EINVAL);
	}

	if (size < 0) {
		if (size != -EOPNOTSUPP && size != -ENOENT)
			CERROR("%s: error geting EA size for '%s': rc = %d\n",
			       mdt_obd_name(info->mti_mdt), xattr_name, size);
		RETURN(size);
	}

	if (req_capsule_has_field(pill, &RMF_ACL, RCL_SERVER))
		req_capsule_set_size(pill, &RMF_ACL, RCL_SERVER,
				     LUSTRE_POSIX_ACL_MAX_SIZE_OLD);

	req_capsule_set_size(pill, &RMF_EADATA, RCL_SERVER,
			     info->mti_body->mbo_eadatasize == 0 ? 0 : size);

	rc2 = req_capsule_server_pack(pill);
	if (rc2 < 0)
		RETURN(rc2);

	if (OBD_FAIL_CHECK(OBD_FAIL_MDS_GETXATTR_PACK))
		RETURN(-ENOMEM);

	RETURN(rc < 0 ? rc : size);
}

static int mdt_nodemap_map_acl(struct mdt_thread_info *info, void *buf,
			       size_t size, const char *name,
			       enum nodemap_tree_type tree_type)
{
	struct lu_nodemap      *nodemap;
	struct obd_export      *exp = info->mti_exp;
	int			rc = size;

	ENTRY;

	if (strcmp(name, XATTR_NAME_ACL_ACCESS) == 0 ||
	    strcmp(name, XATTR_NAME_ACL_DEFAULT) == 0) {
		if (size > info->mti_mdt->mdt_max_ea_size ||
		     (!exp_connect_large_acl(exp) &&
		      size > LUSTRE_POSIX_ACL_MAX_SIZE_OLD))
			GOTO(out, rc = -ERANGE);

		nodemap = nodemap_get_from_exp(exp);
		if (IS_ERR(nodemap))
			GOTO(out, rc = PTR_ERR(nodemap));

		rc = nodemap_map_acl(nodemap, buf, size, tree_type);
		nodemap_putref(nodemap);
		if (rc < 0)
			GOTO(out, rc);
	}
out:
	RETURN(rc);
}

static int mdt_getxattr_all(struct mdt_thread_info *info,
			    struct mdt_body *reqbody, struct mdt_body *repbody,
			    struct lu_buf *buf, struct md_object *next)
{
	const struct lu_env *env = info->mti_env;
	char *v, *b, *eadatahead, *eadatatail;
	__u32 *sizes;
	int eadatasize, eavallen = 0, eavallens = 0, rc;

	ENTRY;

	/*
	 * The format of the pill is the following:
	 * EADATA:      attr1\0attr2\0...attrn\0
	 * EAVALS:      val1val2...valn
	 * EAVALS_LENS: 4,4,...4
	 */

	eadatahead = buf->lb_buf;

	/* Fill out EADATA first */
	rc = mo_xattr_list(env, next, buf);
	if (rc < 0)
		GOTO(out_shrink, rc);

	eadatasize = rc;
	eadatatail = eadatahead + eadatasize;

	v = req_capsule_server_get(info->mti_pill, &RMF_EAVALS);
	sizes = req_capsule_server_get(info->mti_pill, &RMF_EAVALS_LENS);

	/* Fill out EAVALS and EAVALS_LENS */
	for (b = eadatahead; b < eadatatail; b += strlen(b) + 1, v += rc) {
		buf->lb_buf = v;
		buf->lb_len = reqbody->mbo_eadatasize - eavallen;
		rc = mo_xattr_get(env, next, buf, b);
		if (rc < 0)
			GOTO(out_shrink, rc);
		rc = mdt_nodemap_map_acl(info, buf->lb_buf, rc, b,
					 NODEMAP_FS_TO_CLIENT);
		if (rc < 0)
			GOTO(out_shrink, rc);
		sizes[eavallens] = rc;
		eavallens++;
		eavallen += rc;
	}

out_shrink:
	if (rc < 0) {
		eadatasize = 0;
		eavallens = 0;
		eavallen = 0;
	}
	repbody->mbo_aclsize = eavallen;
	repbody->mbo_max_mdsize = eavallens;

	req_capsule_shrink(info->mti_pill, &RMF_EAVALS, eavallen, RCL_SERVER);
	req_capsule_shrink(info->mti_pill, &RMF_EAVALS_LENS,
			   eavallens * sizeof(__u32), RCL_SERVER);
	req_capsule_shrink(info->mti_pill, &RMF_EADATA, eadatasize, RCL_SERVER);

	if (rc >= 0)
		RETURN(eadatasize);
	return rc;
}

int mdt_getxattr(struct mdt_thread_info *info)
{
	struct ptlrpc_request  *req = mdt_info_req(info);
        struct mdt_body        *reqbody;
        struct mdt_body        *repbody = NULL;
        struct md_object       *next;
        struct lu_buf          *buf;
        int                     easize, rc;
	u64			valid;
        ENTRY;

        LASSERT(info->mti_object != NULL);
	LASSERT(lu_object_assert_exists(&info->mti_object->mot_obj));

	CDEBUG(D_INODE, "getxattr "DFID"\n", PFID(&info->mti_body->mbo_fid1));

        reqbody = req_capsule_client_get(info->mti_pill, &RMF_MDT_BODY);
        if (reqbody == NULL)
                RETURN(err_serious(-EFAULT));

	rc = mdt_init_ucred(info, reqbody);
        if (rc)
                RETURN(err_serious(rc));

        next = mdt_object_child(info->mti_object);
        easize = mdt_getxattr_pack_reply(info);
	if (easize == -ENODATA)
		GOTO(out, rc = easize);
	else if (easize < 0)
		GOTO(out, rc = err_serious(easize));

        repbody = req_capsule_server_get(info->mti_pill, &RMF_MDT_BODY);
        LASSERT(repbody != NULL);

        /* No need further getxattr. */
	if (easize == 0 || reqbody->mbo_eadatasize == 0)
		GOTO(out, rc = easize);

	buf = &info->mti_buf;
	buf->lb_buf = req_capsule_server_get(info->mti_pill, &RMF_EADATA);
	buf->lb_len = easize;

	valid = info->mti_body->mbo_valid & (OBD_MD_FLXATTR | OBD_MD_FLXATTRLS);

	if (valid == OBD_MD_FLXATTR) {
		const char *xattr_name = req_capsule_client_get(info->mti_pill,
								&RMF_NAME);
		rc = mo_xattr_get(info->mti_env, next, buf, xattr_name);
		if (rc < 0)
			GOTO(out, rc);

		rc = mdt_nodemap_map_acl(info, buf->lb_buf, rc, xattr_name,
					 NODEMAP_FS_TO_CLIENT);
	} else if (valid == OBD_MD_FLXATTRLS) {
		CDEBUG(D_INODE, "listxattr\n");

		rc = mo_xattr_list(info->mti_env, next, buf);
		if (rc < 0)
			CDEBUG(D_INFO, "listxattr failed: %d\n", rc);
	} else if (valid == OBD_MD_FLXATTRALL) {
		rc = mdt_getxattr_all(info, reqbody, repbody,
				      buf, next);
	} else
		LBUG();

	EXIT;
out:
	if (rc >= 0) {
		mdt_counter_incr(req, LPROC_MDT_GETXATTR);
		/* LU-11109: Set OBD_MD_FLXATTR on success so that
		 * newer clients can distinguish between nonexistent
		 * xattrs and zero length values. */
		repbody->mbo_valid |= OBD_MD_FLXATTR;
		repbody->mbo_eadatasize = rc;
		rc = 0;
	}
	mdt_exit_ucred(info);
	return rc;
}

int mdt_reint_setxattr(struct mdt_thread_info *info,
                       struct mdt_lock_handle *unused)
{
	struct ptlrpc_request	*req = mdt_info_req(info);
	struct mdt_lock_handle	*lh;
	const struct lu_env	*env  = info->mti_env;
	struct lu_buf		*buf  = &info->mti_buf;
	struct mdt_reint_record	*rr   = &info->mti_rr;
	struct md_attr		*ma = &info->mti_attr;
	struct lu_attr		*attr = &info->mti_attr.ma_attr;
	struct mdt_object	*obj;
	struct md_object	*child;
	__u64			 valid = attr->la_valid;
	const char		*xattr_name = rr->rr_name.ln_name;
	int			 xattr_len = rr->rr_eadatalen;
	__u64			 lockpart = MDS_INODELOCK_UPDATE;
	int			 rc;
	ENTRY;

	CDEBUG(D_INODE, "setxattr for "DFID": %s %s\n", PFID(rr->rr_fid1),
	       valid & OBD_MD_FLXATTR ? "set" : "remove", xattr_name);

	if (info->mti_dlm_req)
		ldlm_request_cancel(req, info->mti_dlm_req, 0, LATF_SKIP);

        if (OBD_FAIL_CHECK(OBD_FAIL_MDS_SETXATTR))
                RETURN(err_serious(-ENOMEM));

	rc = mdt_init_ucred_reint(info);
        if (rc != 0)
                RETURN(rc);

	if (strncmp(xattr_name, XATTR_USER_PREFIX,
		    sizeof(XATTR_USER_PREFIX) - 1) == 0) {
		if (!(exp_connect_flags(req->rq_export) & OBD_CONNECT_XATTR))
			GOTO(out, rc = -EOPNOTSUPP);
	} else if (strncmp(xattr_name, XATTR_TRUSTED_PREFIX,
		    sizeof(XATTR_TRUSTED_PREFIX) - 1) == 0) {

		if (!md_capable(mdt_ucred(info), CFS_CAP_SYS_ADMIN))
			GOTO(out, rc = -EPERM);

		if (strcmp(xattr_name, XATTR_NAME_LOV) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_LMA) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_LMV) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_LINK) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_FID) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_VERSION) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_SOM) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_HSM) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_LFSCK_NAMESPACE) == 0)
			GOTO(out, rc = 0);
	} else if ((valid & OBD_MD_FLXATTR) &&
		   (strcmp(xattr_name, XATTR_NAME_ACL_ACCESS) == 0 ||
		    strcmp(xattr_name, XATTR_NAME_ACL_DEFAULT) == 0)) {
		rc = mdt_nodemap_map_acl(info, rr->rr_eadata, xattr_len,
					 xattr_name, NODEMAP_CLIENT_TO_FS);
		if (rc < 0)
			GOTO(out, rc);
		/* ACLs were mapped out, return an error so the user knows */
		if (rc != xattr_len)
			GOTO(out, rc = -EPERM);
	} else if ((strlen(xattr_name) > strlen(XATTR_LUSTRE_LOV) + 1) &&
		   strncmp(xattr_name, XATTR_LUSTRE_LOV,
			   strlen(XATTR_LUSTRE_LOV)) == 0) {

		if (strncmp(xattr_name, XATTR_LUSTRE_LOV".add",
			    strlen(XATTR_LUSTRE_LOV".add")) &&
		    strncmp(xattr_name, XATTR_LUSTRE_LOV".set",
			    strlen(XATTR_LUSTRE_LOV".set")) &&
		    strncmp(xattr_name, XATTR_LUSTRE_LOV".del",
			    strlen(XATTR_LUSTRE_LOV".del"))) {
			CERROR("%s: invalid xattr name: %s\n",
			       mdt_obd_name(info->mti_mdt), xattr_name);
			GOTO(out, rc = -EINVAL);
		}

		lockpart |= MDS_INODELOCK_LAYOUT;
	}

        /* Revoke all clients' lookup lock, since the access
         * permissions for this inode is changed when ACL_ACCESS is
         * set. This isn't needed for ACL_DEFAULT, since that does
         * not change the access permissions of this inode, nor any
         * other existing inodes. It is setting the ACLs inherited
         * by new directories/files at create time. */
	/* We need revoke both LOOKUP|PERM lock here, see mdt_attr_set. */
        if (!strcmp(xattr_name, XATTR_NAME_ACL_ACCESS))
		lockpart |= MDS_INODELOCK_PERM | MDS_INODELOCK_LOOKUP;
	/* We need to take the lock on behalf of old clients so that newer
	 * clients flush their xattr caches */
	else
		lockpart |= MDS_INODELOCK_XATTR;

        lh = &info->mti_lh[MDT_LH_PARENT];
        /* ACLs were sent to clients under LCK_CR locks, so taking LCK_EX
         * to cancel them. */
        mdt_lock_reg_init(lh, LCK_EX);
        obj = mdt_object_find_lock(info, rr->rr_fid1, lh, lockpart);
	if (IS_ERR(obj))
		GOTO(out, rc = PTR_ERR(obj));

	tgt_vbr_obj_set(env, mdt_obj2dt(obj));
	rc = mdt_version_get_check_save(info, obj, 0);
	if (rc)
		GOTO(out_unlock, rc);

	if (unlikely(!(valid & OBD_MD_FLCTIME))) {
		/* This isn't strictly an error, but all current clients
		 * should set OBD_MD_FLCTIME when setting attributes. */
		CWARN("%s: client miss to set OBD_MD_FLCTIME when "
		      "setxattr %s: [object "DFID"] [valid %llu]\n",
		      mdt_obd_name(info->mti_mdt), xattr_name,
		      PFID(rr->rr_fid1), valid);
		attr->la_ctime = ktime_get_real_seconds();
	}
	attr->la_valid = LA_CTIME;
	child = mdt_object_child(obj);
	if (valid & OBD_MD_FLXATTR) {
		int	flags = 0;

		if (attr->la_flags & XATTR_REPLACE)
			flags |= LU_XATTR_REPLACE;

		if (attr->la_flags & XATTR_CREATE)
			flags |= LU_XATTR_CREATE;

		mdt_fail_write(env, info->mti_mdt->mdt_bottom,
			       OBD_FAIL_MDS_SETXATTR_WRITE);

		buf->lb_buf = rr->rr_eadata;
		buf->lb_len = xattr_len;
		rc = mo_xattr_set(env, child, buf, xattr_name, flags);
		/* update ctime after xattr changed */
		if (rc == 0) {
			ma->ma_attr_flags |= MDS_PERM_BYPASS;
			mo_attr_set(env, child, ma);
		}
        } else if (valid & OBD_MD_FLXATTRRM) {
                rc = mo_xattr_del(env, child, xattr_name);
                /* update ctime after xattr changed */
                if (rc == 0) {
                        ma->ma_attr_flags |= MDS_PERM_BYPASS;
                        mo_attr_set(env, child, ma);
                }
	} else {
		CDEBUG(D_INFO, "valid bits: %#llx\n", valid);
		rc = -EINVAL;
	}

	if (rc == 0)
		mdt_counter_incr(req, LPROC_MDT_SETXATTR);

        EXIT;
out_unlock:
        mdt_object_unlock_put(info, obj, lh, rc);
out:
	mdt_exit_ucred(info);
	return rc;
}
