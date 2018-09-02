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
 * Copyright (c) 2004, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/seq_file.h>
#include <linux/statfs.h>
#include <lprocfs_status.h>
#include <obd_class.h>

#include "lmv_internal.h"

static ssize_t numobd_show(struct kobject *kobj, struct attribute *attr,
			   char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lmv_desc *desc;

        desc = &dev->u.lmv.desc;
	return sprintf(buf, "%u\n", desc->ld_tgt_count);
}
LUSTRE_RO_ATTR(numobd);

static ssize_t activeobd_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lmv_desc *desc;

        desc = &dev->u.lmv.desc;
	return sprintf(buf, "%u\n", desc->ld_active_tgt_count);
}
LUSTRE_RO_ATTR(activeobd);

static ssize_t desc_uuid_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lmv_desc *desc;

	desc = &dev->u.lmv.desc;
	return sprintf(buf, "%s\n", desc->ld_uuid.uuid);
}
LUSTRE_RO_ATTR(desc_uuid);

#ifdef CONFIG_PROC_FS
static void *lmv_tgt_seq_start(struct seq_file *p, loff_t *pos)
{
	struct obd_device       *dev = p->private;
	struct lmv_obd          *lmv = &dev->u.lmv;

	while (*pos < lmv->tgts_size) {
		if (lmv->tgts[*pos])
			return lmv->tgts[*pos];
		++*pos;
	}

	return  NULL;
}

static void lmv_tgt_seq_stop(struct seq_file *p, void *v)
{
}

static void *lmv_tgt_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
	struct obd_device       *dev = p->private;
	struct lmv_obd          *lmv = &dev->u.lmv;

	++*pos;
	while (*pos < lmv->tgts_size) {
		if (lmv->tgts[*pos])
			return lmv->tgts[*pos];
		++*pos;
	}

	return  NULL;
}

static int lmv_tgt_seq_show(struct seq_file *p, void *v)
{
	struct lmv_tgt_desc     *tgt = v;

	if (!tgt)
		return 0;

	seq_printf(p, "%u: %s %sACTIVE\n",
		   tgt->ltd_idx, tgt->ltd_uuid.uuid,
		   tgt->ltd_active ? "" : "IN");
	return 0;
}

static const struct seq_operations lmv_tgt_sops = {
        .start                 = lmv_tgt_seq_start,
        .stop                  = lmv_tgt_seq_stop,
        .next                  = lmv_tgt_seq_next,
        .show                  = lmv_tgt_seq_show,
};

static int lmv_target_seq_open(struct inode *inode, struct file *file)
{
        struct seq_file         *seq;
        int                     rc;

        rc = seq_open(file, &lmv_tgt_sops);
        if (rc)
                return rc;

	seq = file->private_data;
	seq->private = PDE_DATA(inode);
	return 0;
}

static const struct file_operations lmv_proc_target_fops = {
        .owner                = THIS_MODULE,
        .open                 = lmv_target_seq_open,
        .read                 = seq_read,
        .llseek               = seq_lseek,
        .release              = seq_release,
};
#endif /* CONFIG_PROC_FS */

static struct attribute *lmv_attrs[] = {
	&lustre_attr_activeobd.attr,
	&lustre_attr_desc_uuid.attr,
	&lustre_attr_numobd.attr,
	NULL,
};

int lmv_tunables_init(struct obd_device *obd)
{
	int rc;

	obd->obd_ktype.default_attrs = lmv_attrs;
	rc = lprocfs_obd_setup(obd, true);
	if (rc)
		goto out_failed;
#ifdef CONFIG_PROC_FS
	rc = lprocfs_alloc_md_stats(obd, 0);
	if (rc) {
		lprocfs_obd_cleanup(obd);
		goto out_failed;
	}

	rc = lprocfs_seq_create(obd->obd_proc_entry, "target_obd",
				0444, &lmv_proc_target_fops, obd);
	if (rc) {
		lprocfs_free_md_stats(obd);
		lprocfs_obd_cleanup(obd);
		CWARN("%s: error adding LMV target_obd file: rc = %d\n",
		      obd->obd_name, rc);
		rc = 0;
	}
#endif /* CONFIG_PROC_FS */
out_failed:
	return rc;
}
