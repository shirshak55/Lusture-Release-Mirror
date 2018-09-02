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
 * Copyright (c) 2012, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */
#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/version.h>
#include <asm/statfs.h>
#include <lprocfs_status.h>
#include <obd_class.h>
#include <uapi/linux/lustre/lustre_param.h>
#include "lov_internal.h"

static int lov_stripesize_seq_show(struct seq_file *m, void *v)
{
	struct obd_device *dev = (struct obd_device *)m->private;
	struct lov_desc *desc;

	LASSERT(dev != NULL);
	desc = &dev->u.lov.desc;

	seq_printf(m, "%llu\n", desc->ld_default_stripe_size);
	return 0;
}

static ssize_t lov_stripesize_seq_write(struct file *file,
					const char __user *buffer,
					size_t count, loff_t *off)
{
	struct obd_device *dev = ((struct seq_file *)file->private_data)->private;
	struct lov_desc *desc;
	s64 val;
	int rc;

	LASSERT(dev != NULL);
	desc = &dev->u.lov.desc;
	rc = lprocfs_str_with_units_to_s64(buffer, count, &val, '1');
	if (rc)
		return rc;
	if (val < 0)
		return -ERANGE;

	lov_fix_desc_stripe_size(&val);
	desc->ld_default_stripe_size = val;

	return count;
}
LPROC_SEQ_FOPS(lov_stripesize);

static ssize_t stripeoffset_show(struct kobject *kobj, struct attribute *attr,
				 char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;

	return sprintf(buf, "%lld\n", desc->ld_default_stripe_offset);
}

static ssize_t stripeoffset_store(struct kobject *kobj, struct attribute *attr,
				  const char *buf, size_t count)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;
	long val;
	int rc;

	rc = kstrtol(buf, 0, &val);
	if (rc)
		return rc;
	if (val < -1 || val > LOV_MAX_STRIPE_COUNT)
		return -ERANGE;

	desc->ld_default_stripe_offset = val;

	return count;
}
LUSTRE_RW_ATTR(stripeoffset);

static ssize_t stripetype_show(struct kobject *kobj, struct attribute *attr,
			       char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;

	return sprintf(buf, "%u\n", desc->ld_pattern);
}

static ssize_t stripetype_store(struct kobject *kobj, struct attribute *attr,
				const char *buffer, size_t count)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;
	u32 pattern;
	int rc;

	rc = kstrtouint(buffer, 0, &pattern);
	if (rc)
		return rc;

	lov_fix_desc_pattern(&pattern);
	desc->ld_pattern = pattern;

	return count;
}
LUSTRE_RW_ATTR(stripetype);

static ssize_t stripecount_show(struct kobject *kobj, struct attribute *attr,
				char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;

	return sprintf(buf, "%d\n",
		       (__s16)(desc->ld_default_stripe_count + 1) - 1);
}

static ssize_t stripecount_store(struct kobject *kobj, struct attribute *attr,
				 const char *buffer, size_t count)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;
	int stripe_count;
	int rc;

	rc = kstrtoint(buffer, 0, &stripe_count);
	if (rc)
		return rc;

	if (stripe_count < -1)
		return -ERANGE;

	lov_fix_desc_stripe_count(&stripe_count);
	desc->ld_default_stripe_count = stripe_count;

	return count;
}
LUSTRE_RW_ATTR(stripecount);

static ssize_t numobd_show(struct kobject *kobj, struct attribute *attr,
			   char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;

	return sprintf(buf, "%u\n", desc->ld_tgt_count);
}
LUSTRE_RO_ATTR(numobd);

static ssize_t activeobd_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;

	return sprintf(buf, "%u\n", desc->ld_active_tgt_count);
}
LUSTRE_RO_ATTR(activeobd);

static ssize_t desc_uuid_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct obd_device *dev = container_of(kobj, struct obd_device,
					      obd_kset.kobj);
	struct lov_desc *desc = &dev->u.lov.desc;

	return sprintf(buf, "%s\n", desc->ld_uuid.uuid);
}
LUSTRE_RO_ATTR(desc_uuid);

#ifdef CONFIG_PROC_FS
static void *lov_tgt_seq_start(struct seq_file *p, loff_t *pos)
{
        struct obd_device *dev = p->private;
        struct lov_obd *lov = &dev->u.lov;

        while (*pos < lov->desc.ld_tgt_count) {
                if (lov->lov_tgts[*pos])
                        return lov->lov_tgts[*pos];
                ++*pos;
        }
        return NULL;
}

static void lov_tgt_seq_stop(struct seq_file *p, void *v)
{
}

static void *lov_tgt_seq_next(struct seq_file *p, void *v, loff_t *pos)
{
        struct obd_device *dev = p->private;
        struct lov_obd *lov = &dev->u.lov;

        while (++*pos < lov->desc.ld_tgt_count) {
                if (lov->lov_tgts[*pos])
                        return lov->lov_tgts[*pos];
        }
        return NULL;
}

static int lov_tgt_seq_show(struct seq_file *p, void *v)
{
        struct lov_tgt_desc *tgt = v;

	seq_printf(p, "%d: %s %sACTIVE\n", tgt->ltd_index,
		   obd_uuid2str(&tgt->ltd_uuid),
		   tgt->ltd_active ? "" : "IN");
	return 0;
}

static const struct seq_operations lov_tgt_sops = {
        .start = lov_tgt_seq_start,
        .stop = lov_tgt_seq_stop,
        .next = lov_tgt_seq_next,
        .show = lov_tgt_seq_show,
};

static int lov_target_seq_open(struct inode *inode, struct file *file)
{
	struct seq_file *seq;
	int rc;

	rc = seq_open(file, &lov_tgt_sops);
	if (rc)
		return rc;

	seq = file->private_data;
	seq->private = PDE_DATA(inode);
	return 0;
}

struct lprocfs_vars lprocfs_lov_obd_vars[] = {
	{ .name =	"stripesize",
	  .fops =	&lov_stripesize_fops	},
	{ NULL }
};

const struct file_operations lov_proc_target_fops = {
        .owner   = THIS_MODULE,
        .open    = lov_target_seq_open,
        .read    = seq_read,
        .llseek  = seq_lseek,
        .release = lprocfs_seq_release,
};
#endif /* CONFIG_PROC_FS */

static struct attribute *lov_attrs[] = {
	&lustre_attr_activeobd.attr,
	&lustre_attr_numobd.attr,
	&lustre_attr_desc_uuid.attr,
	&lustre_attr_stripeoffset.attr,
	&lustre_attr_stripetype.attr,
	&lustre_attr_stripecount.attr,
	NULL,
};

int lov_tunables_init(struct obd_device *obd)
{
	struct lov_obd *lov = &obd->u.lov;
#if defined(CONFIG_PROC_FS) && defined(HAVE_SERVER_SUPPORT)
	struct obd_type *type;
#endif
	int rc;

	obd->obd_vars = lprocfs_lov_obd_vars;
#if defined(CONFIG_PROC_FS) && defined(HAVE_SERVER_SUPPORT)
	/* If this is true then both client (lov) and server
	 * (lod) are on the same node. The lod layer if loaded
	 * first will register the lov proc directory. In that
	 * case obd->obd_type->typ_procroot will be not set.
	 * Instead we use type->typ_procsym as the parent.
	 */
	type = class_search_type(LUSTRE_LOD_NAME);
	if (type && type->typ_procsym) {
		obd->obd_proc_entry = lprocfs_register(obd->obd_name,
						       type->typ_procsym,
						       obd->obd_vars, obd);
		if (IS_ERR(obd->obd_proc_entry)) {
			rc = PTR_ERR(obd->obd_proc_entry);
			CERROR("error %d setting up lprocfs for %s\n", rc,
			       obd->obd_name);
			obd->obd_proc_entry = NULL;
		}
	}
#endif
	obd->obd_ktype.default_attrs = lov_attrs;
	rc = lprocfs_obd_setup(obd, false);
	if (rc)
		GOTO(out, rc);

#ifdef CONFIG_PROC_FS
	rc = lprocfs_seq_create(obd->obd_proc_entry, "target_obd", 0444,
				&lov_proc_target_fops, obd);
	if (rc)
		CWARN("%s: Error adding the target_obd file : rc %d\n",
		      obd->obd_name, rc);

	lov->lov_pool_proc_entry = lprocfs_register("pools",
						    obd->obd_proc_entry,
						    NULL, NULL);
	if (IS_ERR(lov->lov_pool_proc_entry)) {
		rc = PTR_ERR(lov->lov_pool_proc_entry);
		CERROR("%s: error setting up debugfs for pools : rc %d\n",
		       obd->obd_name, rc);
		lov->lov_pool_proc_entry = NULL;
	}
#endif /* CONFIG_FS_PROC */
out:
	return rc;
}
