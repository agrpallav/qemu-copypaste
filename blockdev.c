/*
 * QEMU host block devices
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "sysemu/blockdev.h"
#include "hw/block/block.h"
#include "block/blockjob.h"
#include "monitor/monitor.h"
#include "qapi/qmp/qerror.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "qapi/qmp/types.h"
#include "sysemu/sysemu.h"
#include "block/block_int.h"
#include "qmp-commands.h"
#include "trace.h"
#include "sysemu/arch_init.h"

static QTAILQ_HEAD(drivelist, DriveInfo) drives = QTAILQ_HEAD_INITIALIZER(drives);
extern QemuOptsList qemu_common_drive_opts;
extern QemuOptsList qemu_old_drive_opts;

static const char *const if_name[IF_COUNT] = {
    [IF_NONE] = "none",
    [IF_IDE] = "ide",
    [IF_SCSI] = "scsi",
    [IF_FLOPPY] = "floppy",
    [IF_PFLASH] = "pflash",
    [IF_MTD] = "mtd",
    [IF_SD] = "sd",
    [IF_VIRTIO] = "virtio",
    [IF_XEN] = "xen",
};

static const int if_max_devs[IF_COUNT] = {
    /*
     * Do not change these numbers!  They govern how drive option
     * index maps to unit and bus.  That mapping is ABI.
     *
     * All controllers used to imlement if=T drives need to support
     * if_max_devs[T] units, for any T with if_max_devs[T] != 0.
     * Otherwise, some index values map to "impossible" bus, unit
     * values.
     *
     * For instance, if you change [IF_SCSI] to 255, -drive
     * if=scsi,index=12 no longer means bus=1,unit=5, but
     * bus=0,unit=12.  With an lsi53c895a controller (7 units max),
     * the drive can't be set up.  Regression.
     */
    [IF_IDE] = 2,
    [IF_SCSI] = 7,
};

/*
 * We automatically delete the drive when a device using it gets
 * unplugged.  Questionable feature, but we can't just drop it.
 * Device models call blockdev_mark_auto_del() to schedule the
 * automatic deletion, and generic qdev code calls blockdev_auto_del()
 * when deletion is actually safe.
 */
void blockdev_mark_auto_del(BlockDriverState *bs)
{
    DriveInfo *dinfo = drive_get_by_blockdev(bs);

    if (bs->job) {
        block_job_cancel(bs->job);
    }
    if (dinfo) {
        dinfo->auto_del = 1;
    }
}

void blockdev_auto_del(BlockDriverState *bs)
{
    DriveInfo *dinfo = drive_get_by_blockdev(bs);

    if (dinfo && dinfo->auto_del) {
        drive_put_ref(dinfo);
    }
}

static int drive_index_to_bus_id(BlockInterfaceType type, int index)
{
    int max_devs = if_max_devs[type];
    return max_devs ? index / max_devs : 0;
}

static int drive_index_to_unit_id(BlockInterfaceType type, int index)
{
    int max_devs = if_max_devs[type];
    return max_devs ? index % max_devs : index;
}

QemuOpts *drive_def(const char *optstr)
{
    return qemu_opts_parse(qemu_find_opts("drive"), optstr, 0);
}

QemuOpts *drive_add(BlockInterfaceType type, int index, const char *file,
                    const char *optstr)
{
    QemuOpts *opts;
    char buf[32];

    opts = drive_def(optstr);
    if (!opts) {
        return NULL;
    }
    if (type != IF_DEFAULT) {
        qemu_opt_set(opts, "if", if_name[type]);
    }
    if (index >= 0) {
        snprintf(buf, sizeof(buf), "%d", index);
        qemu_opt_set(opts, "index", buf);
    }
    if (file)
        qemu_opt_set(opts, "file", file);
    return opts;
}

DriveInfo *drive_get(BlockInterfaceType type, int bus, int unit)
{
    DriveInfo *dinfo;

    /* seek interface, bus and unit */

    QTAILQ_FOREACH(dinfo, &drives, next) {
        if (dinfo->type == type &&
	    dinfo->bus == bus &&
	    dinfo->unit == unit)
            return dinfo;
    }

    return NULL;
}

DriveInfo *drive_get_by_index(BlockInterfaceType type, int index)
{
    return drive_get(type,
                     drive_index_to_bus_id(type, index),
                     drive_index_to_unit_id(type, index));
}

int drive_get_max_bus(BlockInterfaceType type)
{
    int max_bus;
    DriveInfo *dinfo;

    max_bus = -1;
    QTAILQ_FOREACH(dinfo, &drives, next) {
        if(dinfo->type == type &&
           dinfo->bus > max_bus)
            max_bus = dinfo->bus;
    }
    return max_bus;
}

/* Get a block device.  This should only be used for single-drive devices
   (e.g. SD/Floppy/MTD).  Multi-disk devices (scsi/ide) should use the
   appropriate bus.  */
DriveInfo *drive_get_next(BlockInterfaceType type)
{
    static int next_block_unit[IF_COUNT];

    return drive_get(type, 0, next_block_unit[type]++);
}

DriveInfo *drive_get_by_blockdev(BlockDriverState *bs)
{
    DriveInfo *dinfo;

    QTAILQ_FOREACH(dinfo, &drives, next) {
        if (dinfo->bdrv == bs) {
            return dinfo;
        }
    }
    return NULL;
}

static void bdrv_format_print(void *opaque, const char *name)
{
    error_printf(" %s", name);
}

static void drive_uninit(DriveInfo *dinfo)
{
    qemu_opts_del(dinfo->opts);
    bdrv_delete(dinfo->bdrv);
    g_free(dinfo->id);
    QTAILQ_REMOVE(&drives, dinfo, next);
    g_free(dinfo->serial);
    g_free(dinfo);
}

void drive_put_ref(DriveInfo *dinfo)
{
    assert(dinfo->refcount);
    if (--dinfo->refcount == 0) {
        drive_uninit(dinfo);
    }
}

void drive_get_ref(DriveInfo *dinfo)
{
    dinfo->refcount++;
}

typedef struct {
    QEMUBH *bh;
    DriveInfo *dinfo;
} DrivePutRefBH;

static void drive_put_ref_bh(void *opaque)
{
    DrivePutRefBH *s = opaque;

    drive_put_ref(s->dinfo);
    qemu_bh_delete(s->bh);
    g_free(s);
}

/*
 * Release a drive reference in a BH
 *
 * It is not possible to use drive_put_ref() from a callback function when the
 * callers still need the drive.  In such cases we schedule a BH to release the
 * reference.
 */
static void drive_put_ref_bh_schedule(DriveInfo *dinfo)
{
    DrivePutRefBH *s;

    s = g_new(DrivePutRefBH, 1);
    s->bh = qemu_bh_new(drive_put_ref_bh, s);
    s->dinfo = dinfo;
    qemu_bh_schedule(s->bh);
}

static int parse_block_error_action(const char *buf, bool is_read)
{
    if (!strcmp(buf, "ignore")) {
        return BLOCKDEV_ON_ERROR_IGNORE;
    } else if (!is_read && !strcmp(buf, "enospc")) {
        return BLOCKDEV_ON_ERROR_ENOSPC;
    } else if (!strcmp(buf, "stop")) {
        return BLOCKDEV_ON_ERROR_STOP;
    } else if (!strcmp(buf, "report")) {
        return BLOCKDEV_ON_ERROR_REPORT;
    } else {
        error_report("'%s' invalid %s error action",
                     buf, is_read ? "read" : "write");
        return -1;
    }
}

static bool do_check_io_limits(BlockIOLimit *io_limits, Error **errp)
{
    bool bps_flag;
    bool iops_flag;

    assert(io_limits);

    bps_flag  = (io_limits->bps[BLOCK_IO_LIMIT_TOTAL] != 0)
                 && ((io_limits->bps[BLOCK_IO_LIMIT_READ] != 0)
                 || (io_limits->bps[BLOCK_IO_LIMIT_WRITE] != 0));
    iops_flag = (io_limits->iops[BLOCK_IO_LIMIT_TOTAL] != 0)
                 && ((io_limits->iops[BLOCK_IO_LIMIT_READ] != 0)
                 || (io_limits->iops[BLOCK_IO_LIMIT_WRITE] != 0));
    if (bps_flag || iops_flag) {
        error_setg(errp, "bps(iops) and bps_rd/bps_wr(iops_rd/iops_wr) "
                         "cannot be used at the same time");
        return false;
    }

    if (io_limits->bps[BLOCK_IO_LIMIT_TOTAL] < 0 ||
        io_limits->bps[BLOCK_IO_LIMIT_WRITE] < 0 ||
        io_limits->bps[BLOCK_IO_LIMIT_READ] < 0 ||
        io_limits->iops[BLOCK_IO_LIMIT_TOTAL] < 0 ||
        io_limits->iops[BLOCK_IO_LIMIT_WRITE] < 0 ||
        io_limits->iops[BLOCK_IO_LIMIT_READ] < 0) {
        error_setg(errp, "bps and iops values must be 0 or greater");
        return false;
    }

    return true;
}

static DriveInfo *blockdev_init(QemuOpts *all_opts,
                                BlockInterfaceType block_default_type)
{
    const char *buf;
    const char *file = NULL;
    const char *serial;
    const char *mediastr = "";
    BlockInterfaceType type;
    enum { MEDIA_DISK, MEDIA_CDROM } media;
    int bus_id, unit_id;
    int cyls, heads, secs, translation;
    int max_devs;
    int index;
    int ro = 0;
    int bdrv_flags = 0;
    int on_read_error, on_write_error;
    const char *devaddr;
    DriveInfo *dinfo;
    BlockIOLimit io_limits;
    int snapshot = 0;
    bool copy_on_read;
    int ret;
    Error *error = NULL;
    QemuOpts *opts;
    QDict *bs_opts;
    const char *id;
    bool has_driver_specific_opts;

    translation = BIOS_ATA_TRANSLATION_AUTO;
    media = MEDIA_DISK;

    /* Check common options by copying from all_opts to opts, all other options
     * are stored in bs_opts. */
    id = qemu_opts_id(all_opts);
    opts = qemu_opts_create(&qemu_common_drive_opts, id, 1, &error);
    if (error_is_set(&error)) {
        qerror_report_err(error);
        error_free(error);
        return NULL;
    }

    bs_opts = qdict_new();
    qemu_opts_to_qdict(all_opts, bs_opts);
    qemu_opts_absorb_qdict(opts, bs_opts, &error);
    if (error_is_set(&error)) {
        qerror_report_err(error);
        error_free(error);
        return NULL;
    }

    if (id) {
        qdict_del(bs_opts, "id");
    }

    has_driver_specific_opts = !!qdict_size(bs_opts);

    /* extract parameters */
    bus_id  = qemu_opt_get_number(opts, "bus", 0);
    unit_id = qemu_opt_get_number(opts, "unit", -1);
    index   = qemu_opt_get_number(opts, "index", -1);

    cyls  = qemu_opt_get_number(opts, "cyls", 0);
    heads = qemu_opt_get_number(opts, "heads", 0);
    secs  = qemu_opt_get_number(opts, "secs", 0);

    snapshot = qemu_opt_get_bool(opts, "snapshot", 0);
    ro = qemu_opt_get_bool(opts, "read-only", 0);
    copy_on_read = qemu_opt_get_bool(opts, "copy-on-read", false);

    file = qemu_opt_get(opts, "file");
    serial = qemu_opt_get(opts, "serial");

    if ((buf = qemu_opt_get(opts, "if")) != NULL) {
        for (type = 0; type < IF_COUNT && strcmp(buf, if_name[type]); type++)
            ;
        if (type == IF_COUNT) {
            error_report("unsupported bus type '%s'", buf);
            return NULL;
	}
    } else {
        type = block_default_type;
    }

    max_devs = if_max_devs[type];

    if (cyls || heads || secs) {
        if (cyls < 1) {
            error_report("invalid physical cyls number");
	    return NULL;
	}
        if (heads < 1) {
            error_report("invalid physical heads number");
	    return NULL;
	}
        if (secs < 1) {
            error_report("invalid physical secs number");
	    return NULL;
	}
    }

    if ((buf = qemu_opt_get(opts, "trans")) != NULL) {
        if (!cyls) {
            error_report("'%s' trans must be used with cyls, heads and secs",
                         buf);
            return NULL;
        }
        if (!strcmp(buf, "none"))
            translation = BIOS_ATA_TRANSLATION_NONE;
        else if (!strcmp(buf, "lba"))
            translation = BIOS_ATA_TRANSLATION_LBA;
        else if (!strcmp(buf, "auto"))
            translation = BIOS_ATA_TRANSLATION_AUTO;
	else {
            error_report("'%s' invalid translation type", buf);
	    return NULL;
	}
    }

    if ((buf = qemu_opt_get(opts, "media")) != NULL) {
        if (!strcmp(buf, "disk")) {
	    media = MEDIA_DISK;
	} else if (!strcmp(buf, "cdrom")) {
            if (cyls || secs || heads) {
                error_report("CHS can't be set with media=%s", buf);
	        return NULL;
            }
	    media = MEDIA_CDROM;
	} else {
	    error_report("'%s' invalid media", buf);
	    return NULL;
	}
    }

    if ((buf = qemu_opt_get(opts, "discard")) != NULL) {
        if (bdrv_parse_discard_flags(buf, &bdrv_flags) != 0) {
            error_report("invalid discard option");
            return NULL;
        }
    }

    bdrv_flags = 0;
    if (qemu_opt_get_bool(opts, "cache.writeback", true)) {
        bdrv_flags |= BDRV_O_CACHE_WB;
    }
    if (qemu_opt_get_bool(opts, "cache.direct", false)) {
        bdrv_flags |= BDRV_O_NOCACHE;
    }
    if (qemu_opt_get_bool(opts, "cache.no-flush", true)) {
        bdrv_flags |= BDRV_O_NO_FLUSH;
    }

#ifdef CONFIG_LINUX_AIO
    if ((buf = qemu_opt_get(opts, "aio")) != NULL) {
        if (!strcmp(buf, "native")) {
            bdrv_flags |= BDRV_O_NATIVE_AIO;
        } else if (!strcmp(buf, "threads")) {
            /* this is the default */
        } else {
           error_report("invalid aio option");
           return NULL;
        }
    }
#endif

    if ((buf = qemu_opt_get(opts, "format")) != NULL) {
        if (is_help_option(buf)) {
            error_printf("Supported formats:");
            bdrv_iterate_format(bdrv_format_print, NULL);
            error_printf("\n");
            return NULL;
        }

        qdict_put(bs_opts, "driver", qstring_from_str(buf));
    }

    /* disk I/O throttling */
    io_limits.bps[BLOCK_IO_LIMIT_TOTAL]  =
        qemu_opt_get_number(opts, "throttling.bps-total", 0);
    io_limits.bps[BLOCK_IO_LIMIT_READ]   =
        qemu_opt_get_number(opts, "throttling.bps-read", 0);
    io_limits.bps[BLOCK_IO_LIMIT_WRITE]  =
        qemu_opt_get_number(opts, "throttling.bps-write", 0);
    io_limits.iops[BLOCK_IO_LIMIT_TOTAL] =
        qemu_opt_get_number(opts, "throttling.iops-total", 0);
    io_limits.iops[BLOCK_IO_LIMIT_READ]  =
        qemu_opt_get_number(opts, "throttling.iops-read", 0);
    io_limits.iops[BLOCK_IO_LIMIT_WRITE] =
        qemu_opt_get_number(opts, "throttling.iops-write", 0);

    if (!do_check_io_limits(&io_limits, &error)) {
        error_report("%s", error_get_pretty(error));
        error_free(error);
        return NULL;
    }

    if (qemu_opt_get(opts, "boot") != NULL) {
        fprintf(stderr, "qemu-kvm: boot=on|off is deprecated and will be "
                "ignored. Future versions will reject this parameter. Please "
                "update your scripts.\n");
    }

    on_write_error = BLOCKDEV_ON_ERROR_ENOSPC;
    if ((buf = qemu_opt_get(opts, "werror")) != NULL) {
        if (type != IF_IDE && type != IF_SCSI && type != IF_VIRTIO && type != IF_NONE) {
            error_report("werror is not supported by this bus type");
            return NULL;
        }

        on_write_error = parse_block_error_action(buf, 0);
        if (on_write_error < 0) {
            return NULL;
        }
    }

    on_read_error = BLOCKDEV_ON_ERROR_REPORT;
    if ((buf = qemu_opt_get(opts, "rerror")) != NULL) {
        if (type != IF_IDE && type != IF_VIRTIO && type != IF_SCSI && type != IF_NONE) {
            error_report("rerror is not supported by this bus type");
            return NULL;
        }

        on_read_error = parse_block_error_action(buf, 1);
        if (on_read_error < 0) {
            return NULL;
        }
    }

    if ((devaddr = qemu_opt_get(opts, "addr")) != NULL) {
        if (type != IF_VIRTIO) {
            error_report("addr is not supported by this bus type");
            return NULL;
        }
    }

    /* compute bus and unit according index */

    if (index != -1) {
        if (bus_id != 0 || unit_id != -1) {
            error_report("index cannot be used with bus and unit");
            return NULL;
        }
        bus_id = drive_index_to_bus_id(type, index);
        unit_id = drive_index_to_unit_id(type, index);
    }

    /* if user doesn't specify a unit_id,
     * try to find the first free
     */

    if (unit_id == -1) {
       unit_id = 0;
       while (drive_get(type, bus_id, unit_id) != NULL) {
           unit_id++;
           if (max_devs && unit_id >= max_devs) {
               unit_id -= max_devs;
               bus_id++;
           }
       }
    }

    /* check unit id */

    if (max_devs && unit_id >= max_devs) {
        error_report("unit %d too big (max is %d)",
                     unit_id, max_devs - 1);
        return NULL;
    }

    /*
     * catch multiple definitions
     */

    if (drive_get(type, bus_id, unit_id) != NULL) {
        error_report("drive with bus=%d, unit=%d (index=%d) exists",
                     bus_id, unit_id, index);
        return NULL;
    }

    /* init */

    dinfo = g_malloc0(sizeof(*dinfo));
    if ((buf = qemu_opts_id(opts)) != NULL) {
        dinfo->id = g_strdup(buf);
    } else {
        /* no id supplied -> create one */
        dinfo->id = g_malloc0(32);
        if (type == IF_IDE || type == IF_SCSI)
            mediastr = (media == MEDIA_CDROM) ? "-cd" : "-hd";
        if (max_devs)
            snprintf(dinfo->id, 32, "%s%i%s%i",
                     if_name[type], bus_id, mediastr, unit_id);
        else
            snprintf(dinfo->id, 32, "%s%s%i",
                     if_name[type], mediastr, unit_id);
    }
    dinfo->bdrv = bdrv_new(dinfo->id);
    dinfo->bdrv->open_flags = snapshot ? BDRV_O_SNAPSHOT : 0;
    dinfo->bdrv->read_only = ro;
    dinfo->devaddr = devaddr;
    dinfo->type = type;
    dinfo->bus = bus_id;
    dinfo->unit = unit_id;
    dinfo->cyls = cyls;
    dinfo->heads = heads;
    dinfo->secs = secs;
    dinfo->trans = translation;
    dinfo->opts = all_opts;
    dinfo->refcount = 1;
    if (serial != NULL) {
        dinfo->serial = g_strdup(serial);
    }
    QTAILQ_INSERT_TAIL(&drives, dinfo, next);

    bdrv_set_on_error(dinfo->bdrv, on_read_error, on_write_error);

    /* disk I/O throttling */
    bdrv_set_io_limits(dinfo->bdrv, &io_limits);

    switch(type) {
    case IF_IDE:
    case IF_SCSI:
    case IF_XEN:
    case IF_NONE:
        dinfo->media_cd = media == MEDIA_CDROM;
        break;
    case IF_SD:
    case IF_FLOPPY:
    case IF_PFLASH:
    case IF_MTD:
        break;
    case IF_VIRTIO:
    {
        /* add virtio block device */
        QemuOpts *devopts;
        devopts = qemu_opts_create_nofail(qemu_find_opts("device"));
        if (arch_type == QEMU_ARCH_S390X) {
            qemu_opt_set(devopts, "driver", "virtio-blk-s390");
        } else {
            qemu_opt_set(devopts, "driver", "virtio-blk-pci");
        }
        qemu_opt_set(devopts, "drive", dinfo->id);
        if (devaddr)
            qemu_opt_set(devopts, "addr", devaddr);
        break;
    }
    default:
        abort();
    }
    if (!file || !*file) {
        if (has_driver_specific_opts) {
            file = NULL;
        } else {
            return dinfo;
        }
    }
    if (snapshot) {
        /* always use cache=unsafe with snapshot */
        bdrv_flags &= ~BDRV_O_CACHE_MASK;
        bdrv_flags |= (BDRV_O_SNAPSHOT|BDRV_O_CACHE_WB|BDRV_O_NO_FLUSH);
    }

    if (copy_on_read) {
        bdrv_flags |= BDRV_O_COPY_ON_READ;
    }

    if (runstate_check(RUN_STATE_INMIGRATE)) {
        bdrv_flags |= BDRV_O_INCOMING;
    }

    if (media == MEDIA_CDROM) {
        /* CDROM is fine for any interface, don't check.  */
        ro = 1;
    } else if (ro == 1) {
        if (type != IF_SCSI && type != IF_VIRTIO && type != IF_FLOPPY &&
            type != IF_NONE && type != IF_PFLASH) {
            error_report("read-only not supported by this bus type");
            goto err;
        }
    }

    bdrv_flags |= ro ? 0 : BDRV_O_RDWR;

    if (ro && copy_on_read) {
        error_report("warning: disabling copy_on_read on read-only drive");
    }

    QINCREF(bs_opts);
    ret = bdrv_open(dinfo->bdrv, file, bs_opts, bdrv_flags, NULL);

    if (ret < 0) {
        if (ret == -EMEDIUMTYPE) {
            error_report("could not open disk image %s: not in %s format",
                         file ?: dinfo->id, qdict_get_str(bs_opts, "driver"));
        } else {
            error_report("could not open disk image %s: %s",
                         file ?: dinfo->id, strerror(-ret));
        }
        goto err;
    }

    if (bdrv_key_required(dinfo->bdrv))
        autostart = 0;

    QDECREF(bs_opts);
    qemu_opts_del(opts);

    return dinfo;

err:
    qemu_opts_del(opts);
    QDECREF(bs_opts);
    bdrv_delete(dinfo->bdrv);
    g_free(dinfo->id);
    QTAILQ_REMOVE(&drives, dinfo, next);
    g_free(dinfo);
    return NULL;
}

static void qemu_opt_rename(QemuOpts *opts, const char *from, const char *to)
{
    const char *value;

    value = qemu_opt_get(opts, from);
    if (value) {
        qemu_opt_set(opts, to, value);
        qemu_opt_unset(opts, from);
    }
}

DriveInfo *drive_init(QemuOpts *all_opts, BlockInterfaceType block_default_type)
{
    const char *value;

    /*
     * Check that only old options are used by copying into a QemuOpts with
     * stricter checks. Going through a QDict seems to be the easiest way to
     * achieve this...
     */
    QemuOpts* check_opts;
    QDict *qdict;
    Error *local_err = NULL;

    qdict = qemu_opts_to_qdict(all_opts, NULL);
    check_opts = qemu_opts_from_qdict(&qemu_old_drive_opts, qdict, &local_err);
    QDECREF(qdict);

    if (error_is_set(&local_err)) {
        qerror_report_err(local_err);
        error_free(local_err);
        return NULL;
    }
    qemu_opts_del(check_opts);

    /* Change legacy command line options into QMP ones */
    qemu_opt_rename(all_opts, "iops", "throttling.iops-total");
    qemu_opt_rename(all_opts, "iops_rd", "throttling.iops-read");
    qemu_opt_rename(all_opts, "iops_wr", "throttling.iops-write");

    qemu_opt_rename(all_opts, "bps", "throttling.bps-total");
    qemu_opt_rename(all_opts, "bps_rd", "throttling.bps-read");
    qemu_opt_rename(all_opts, "bps_wr", "throttling.bps-write");

    qemu_opt_rename(all_opts, "readonly", "read-only");

    value = qemu_opt_get(all_opts, "cache");
    if (value) {
        int flags = 0;

        if (bdrv_parse_cache_flags(value, &flags) != 0) {
            error_report("invalid cache option");
            return NULL;
        }

        /* Specific options take precedence */
        if (!qemu_opt_get(all_opts, "cache.writeback")) {
            qemu_opt_set_bool(all_opts, "cache.writeback",
                              !!(flags & BDRV_O_CACHE_WB));
        }
        if (!qemu_opt_get(all_opts, "cache.direct")) {
            qemu_opt_set_bool(all_opts, "cache.direct",
                              !!(flags & BDRV_O_NOCACHE));
        }
        if (!qemu_opt_get(all_opts, "cache.no-flush")) {
            qemu_opt_set_bool(all_opts, "cache.no-flush",
                              !!(flags & BDRV_O_NO_FLUSH));
        }
        qemu_opt_unset(all_opts, "cache");
    }

    return blockdev_init(all_opts, block_default_type);
}

void do_commit(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    BlockDriverState *bs;
    int ret;

    if (!strcmp(device, "all")) {
        ret = bdrv_commit_all();
    } else {
        bs = bdrv_find(device);
        if (!bs) {
            monitor_printf(mon, "Device '%s' not found\n", device);
            return;
        }
        ret = bdrv_commit(bs);
    }
    if (ret < 0) {
        monitor_printf(mon, "'commit' error for '%s': %s\n", device,
                       strerror(-ret));
    }
}

static void blockdev_do_action(int kind, void *data, Error **errp)
{
    TransactionAction action;
    TransactionActionList list;

    action.kind = kind;
    action.data = data;
    list.value = &action;
    list.next = NULL;
    qmp_transaction(&list, errp);
}

void qmp_blockdev_snapshot_sync(const char *device, const char *snapshot_file,
                                bool has_format, const char *format,
                                bool has_mode, enum NewImageMode mode,
                                Error **errp)
{
    BlockdevSnapshot snapshot = {
        .device = (char *) device,
        .snapshot_file = (char *) snapshot_file,
        .has_format = has_format,
        .format = (char *) format,
        .has_mode = has_mode,
        .mode = mode,
    };
    blockdev_do_action(TRANSACTION_ACTION_KIND_BLOCKDEV_SNAPSHOT_SYNC,
                       &snapshot, errp);
}


/* New and old BlockDriverState structs for group snapshots */

typedef struct BlkTransactionState BlkTransactionState;

/* Only prepare() may fail. In a single transaction, only one of commit() or
   abort() will be called, clean() will always be called if it present. */
typedef struct BdrvActionOps {
    /* Size of state struct, in bytes. */
    size_t instance_size;
    /* Prepare the work, must NOT be NULL. */
    void (*prepare)(BlkTransactionState *common, Error **errp);
    /* Commit the changes, can be NULL. */
    void (*commit)(BlkTransactionState *common);
    /* Abort the changes on fail, can be NULL. */
    void (*abort)(BlkTransactionState *common);
    /* Clean up resource in the end, can be NULL. */
    void (*clean)(BlkTransactionState *common);
} BdrvActionOps;

/*
 * This structure must be arranged as first member in child type, assuming
 * that compiler will also arrange it to the same address with parent instance.
 * Later it will be used in free().
 */
struct BlkTransactionState {
    TransactionAction *action;
    const BdrvActionOps *ops;
    QSIMPLEQ_ENTRY(BlkTransactionState) entry;
};

/* external snapshot private data */
typedef struct ExternalSnapshotState {
    BlkTransactionState common;
    BlockDriverState *old_bs;
    BlockDriverState *new_bs;
} ExternalSnapshotState;

static void external_snapshot_prepare(BlkTransactionState *common,
                                      Error **errp)
{
    BlockDriver *drv;
    int flags, ret;
    Error *local_err = NULL;
    const char *device;
    const char *new_image_file;
    const char *format = "qcow2";
    enum NewImageMode mode = NEW_IMAGE_MODE_ABSOLUTE_PATHS;
    ExternalSnapshotState *state =
                             DO_UPCAST(ExternalSnapshotState, common, common);
    TransactionAction *action = common->action;

    /* get parameters */
    g_assert(action->kind == TRANSACTION_ACTION_KIND_BLOCKDEV_SNAPSHOT_SYNC);

    device = action->blockdev_snapshot_sync->device;
    new_image_file = action->blockdev_snapshot_sync->snapshot_file;
    if (action->blockdev_snapshot_sync->has_format) {
        format = action->blockdev_snapshot_sync->format;
    }
    if (action->blockdev_snapshot_sync->has_mode) {
        mode = action->blockdev_snapshot_sync->mode;
    }

    /* start processing */
    drv = bdrv_find_format(format);
    if (!drv) {
        error_set(errp, QERR_INVALID_BLOCK_FORMAT, format);
        return;
    }

    state->old_bs = bdrv_find(device);
    if (!state->old_bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (!bdrv_is_inserted(state->old_bs)) {
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        return;
    }

    if (bdrv_in_use(state->old_bs)) {
        error_set(errp, QERR_DEVICE_IN_USE, device);
        return;
    }

    if (!bdrv_is_read_only(state->old_bs)) {
        if (bdrv_flush(state->old_bs)) {
            error_set(errp, QERR_IO_ERROR);
            return;
        }
    }

    flags = state->old_bs->open_flags;

    /* create new image w/backing file */
    if (mode != NEW_IMAGE_MODE_EXISTING) {
        bdrv_img_create(new_image_file, format,
                        state->old_bs->filename,
                        state->old_bs->drv->format_name,
                        NULL, -1, flags, &local_err, false);
        if (error_is_set(&local_err)) {
            error_propagate(errp, local_err);
            return;
        }
    }

    /* We will manually add the backing_hd field to the bs later */
    state->new_bs = bdrv_new("");
    /* TODO Inherit bs->options or only take explicit options with an
     * extended QMP command? */
    ret = bdrv_open(state->new_bs, new_image_file, NULL,
                    flags | BDRV_O_NO_BACKING, drv);
    if (ret != 0) {
        error_setg_file_open(errp, -ret, new_image_file);
    }
}

static void external_snapshot_commit(BlkTransactionState *common)
{
    ExternalSnapshotState *state =
                             DO_UPCAST(ExternalSnapshotState, common, common);

    /* This removes our old bs and adds the new bs */
    bdrv_append(state->new_bs, state->old_bs);
    /* We don't need (or want) to use the transactional
     * bdrv_reopen_multiple() across all the entries at once, because we
     * don't want to abort all of them if one of them fails the reopen */
    bdrv_reopen(state->new_bs, state->new_bs->open_flags & ~BDRV_O_RDWR,
                NULL);
}

static void external_snapshot_abort(BlkTransactionState *common)
{
    ExternalSnapshotState *state =
                             DO_UPCAST(ExternalSnapshotState, common, common);
    if (state->new_bs) {
        bdrv_delete(state->new_bs);
    }
}

typedef struct DriveBackupState {
    BlkTransactionState common;
    BlockDriverState *bs;
    BlockJob *job;
} DriveBackupState;

static void drive_backup_prepare(BlkTransactionState *common, Error **errp)
{
    DriveBackupState *state = DO_UPCAST(DriveBackupState, common, common);
    DriveBackup *backup;
    Error *local_err = NULL;

    assert(common->action->kind == TRANSACTION_ACTION_KIND_DRIVE_BACKUP);
    backup = common->action->drive_backup;

    qmp_drive_backup(backup->device, backup->target,
                     backup->has_format, backup->format,
                     backup->sync,
                     backup->has_mode, backup->mode,
                     backup->has_speed, backup->speed,
                     backup->has_on_source_error, backup->on_source_error,
                     backup->has_on_target_error, backup->on_target_error,
                     &local_err);
    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        state->bs = NULL;
        state->job = NULL;
        return;
    }

    state->bs = bdrv_find(backup->device);
    state->job = state->bs->job;
}

static void drive_backup_abort(BlkTransactionState *common)
{
    DriveBackupState *state = DO_UPCAST(DriveBackupState, common, common);
    BlockDriverState *bs = state->bs;

    /* Only cancel if it's the job we started */
    if (bs && bs->job && bs->job == state->job) {
        block_job_cancel_sync(bs->job);
    }
}

static void abort_prepare(BlkTransactionState *common, Error **errp)
{
    error_setg(errp, "Transaction aborted using Abort action");
}

static void abort_commit(BlkTransactionState *common)
{
    g_assert_not_reached(); /* this action never succeeds */
}

static const BdrvActionOps actions[] = {
    [TRANSACTION_ACTION_KIND_BLOCKDEV_SNAPSHOT_SYNC] = {
        .instance_size = sizeof(ExternalSnapshotState),
        .prepare  = external_snapshot_prepare,
        .commit   = external_snapshot_commit,
        .abort = external_snapshot_abort,
    },
    [TRANSACTION_ACTION_KIND_DRIVE_BACKUP] = {
        .instance_size = sizeof(DriveBackupState),
        .prepare = drive_backup_prepare,
        .abort = drive_backup_abort,
    },
    [TRANSACTION_ACTION_KIND_ABORT] = {
        .instance_size = sizeof(BlkTransactionState),
        .prepare = abort_prepare,
        .commit = abort_commit,
    },
};

/*
 * 'Atomic' group snapshots.  The snapshots are taken as a set, and if any fail
 *  then we do not pivot any of the devices in the group, and abandon the
 *  snapshots
 */
void qmp_transaction(TransactionActionList *dev_list, Error **errp)
{
    TransactionActionList *dev_entry = dev_list;
    BlkTransactionState *state, *next;
    Error *local_err = NULL;

    QSIMPLEQ_HEAD(snap_bdrv_states, BlkTransactionState) snap_bdrv_states;
    QSIMPLEQ_INIT(&snap_bdrv_states);

    /* drain all i/o before any snapshots */
    bdrv_drain_all();

    /* We don't do anything in this loop that commits us to the snapshot */
    while (NULL != dev_entry) {
        TransactionAction *dev_info = NULL;
        const BdrvActionOps *ops;

        dev_info = dev_entry->value;
        dev_entry = dev_entry->next;

        assert(dev_info->kind < ARRAY_SIZE(actions));

        ops = &actions[dev_info->kind];
        state = g_malloc0(ops->instance_size);
        state->ops = ops;
        state->action = dev_info;
        QSIMPLEQ_INSERT_TAIL(&snap_bdrv_states, state, entry);

        state->ops->prepare(state, &local_err);
        if (error_is_set(&local_err)) {
            error_propagate(errp, local_err);
            goto delete_and_fail;
        }
    }

    QSIMPLEQ_FOREACH(state, &snap_bdrv_states, entry) {
        if (state->ops->commit) {
            state->ops->commit(state);
        }
    }

    /* success */
    goto exit;

delete_and_fail:
    /*
    * failure, and it is all-or-none; abandon each new bs, and keep using
    * the original bs for all images
    */
    QSIMPLEQ_FOREACH(state, &snap_bdrv_states, entry) {
        if (state->ops->abort) {
            state->ops->abort(state);
        }
    }
exit:
    QSIMPLEQ_FOREACH_SAFE(state, &snap_bdrv_states, entry, next) {
        if (state->ops->clean) {
            state->ops->clean(state);
        }
        g_free(state);
    }
}


static void eject_device(BlockDriverState *bs, int force, Error **errp)
{
    if (bdrv_in_use(bs)) {
        error_set(errp, QERR_DEVICE_IN_USE, bdrv_get_device_name(bs));
        return;
    }
    if (!bdrv_dev_has_removable_media(bs)) {
        error_set(errp, QERR_DEVICE_NOT_REMOVABLE, bdrv_get_device_name(bs));
        return;
    }

    if (bdrv_dev_is_medium_locked(bs) && !bdrv_dev_is_tray_open(bs)) {
        bdrv_dev_eject_request(bs, force);
        if (!force) {
            error_set(errp, QERR_DEVICE_LOCKED, bdrv_get_device_name(bs));
            return;
        }
    }

    bdrv_close(bs);
}

void qmp_eject(const char *device, bool has_force, bool force, Error **errp)
{
    BlockDriverState *bs;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    eject_device(bs, force, errp);
}

void qmp_block_passwd(const char *device, const char *password, Error **errp)
{
    BlockDriverState *bs;
    int err;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    err = bdrv_set_key(bs, password);
    if (err == -EINVAL) {
        error_set(errp, QERR_DEVICE_NOT_ENCRYPTED, bdrv_get_device_name(bs));
        return;
    } else if (err < 0) {
        error_set(errp, QERR_INVALID_PASSWORD);
        return;
    }
}

static void qmp_bdrv_open_encrypted(BlockDriverState *bs, const char *filename,
                                    int bdrv_flags, BlockDriver *drv,
                                    const char *password, Error **errp)
{
    int ret;

    ret = bdrv_open(bs, filename, NULL, bdrv_flags, drv);
    if (ret < 0) {
        error_setg_file_open(errp, -ret, filename);
        return;
    }

    if (bdrv_key_required(bs)) {
        if (password) {
            if (bdrv_set_key(bs, password) < 0) {
                error_set(errp, QERR_INVALID_PASSWORD);
            }
        } else {
            error_set(errp, QERR_DEVICE_ENCRYPTED, bdrv_get_device_name(bs),
                      bdrv_get_encrypted_filename(bs));
        }
    } else if (password) {
        error_set(errp, QERR_DEVICE_NOT_ENCRYPTED, bdrv_get_device_name(bs));
    }
}

void qmp_change_blockdev(const char *device, const char *filename,
                         bool has_format, const char *format, Error **errp)
{
    BlockDriverState *bs;
    BlockDriver *drv = NULL;
    int bdrv_flags;
    Error *err = NULL;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (format) {
        drv = bdrv_find_whitelisted_format(format, bs->read_only);
        if (!drv) {
            error_set(errp, QERR_INVALID_BLOCK_FORMAT, format);
            return;
        }
    }

    eject_device(bs, 0, &err);
    if (error_is_set(&err)) {
        error_propagate(errp, err);
        return;
    }

    bdrv_flags = bdrv_is_read_only(bs) ? 0 : BDRV_O_RDWR;
    bdrv_flags |= bdrv_is_snapshot(bs) ? BDRV_O_SNAPSHOT : 0;

    qmp_bdrv_open_encrypted(bs, filename, bdrv_flags, drv, NULL, errp);
}

/* throttling disk I/O limits */
void qmp_block_set_io_throttle(const char *device, int64_t bps, int64_t bps_rd,
                               int64_t bps_wr, int64_t iops, int64_t iops_rd,
                               int64_t iops_wr, Error **errp)
{
    BlockIOLimit io_limits;
    BlockDriverState *bs;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    io_limits.bps[BLOCK_IO_LIMIT_TOTAL] = bps;
    io_limits.bps[BLOCK_IO_LIMIT_READ]  = bps_rd;
    io_limits.bps[BLOCK_IO_LIMIT_WRITE] = bps_wr;
    io_limits.iops[BLOCK_IO_LIMIT_TOTAL]= iops;
    io_limits.iops[BLOCK_IO_LIMIT_READ] = iops_rd;
    io_limits.iops[BLOCK_IO_LIMIT_WRITE]= iops_wr;

    if (!do_check_io_limits(&io_limits, errp)) {
        return;
    }

    bs->io_limits = io_limits;

    if (!bs->io_limits_enabled && bdrv_io_limits_enabled(bs)) {
        bdrv_io_limits_enable(bs);
    } else if (bs->io_limits_enabled && !bdrv_io_limits_enabled(bs)) {
        bdrv_io_limits_disable(bs);
    } else {
        if (bs->block_timer) {
            qemu_mod_timer(bs->block_timer, qemu_get_clock_ns(vm_clock));
        }
    }
}

int do_drive_del(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *id = qdict_get_str(qdict, "id");
    BlockDriverState *bs;

    bs = bdrv_find(id);
    if (!bs) {
        qerror_report(QERR_DEVICE_NOT_FOUND, id);
        return -1;
    }
    if (bdrv_in_use(bs)) {
        qerror_report(QERR_DEVICE_IN_USE, id);
        return -1;
    }

    /* quiesce block driver; prevent further io */
    bdrv_drain_all();
    bdrv_flush(bs);
    bdrv_close(bs);

    /* if we have a device attached to this BlockDriverState
     * then we need to make the drive anonymous until the device
     * can be removed.  If this is a drive with no device backing
     * then we can just get rid of the block driver state right here.
     */
    if (bdrv_get_attached_dev(bs)) {
        bdrv_make_anon(bs);

        /* Further I/O must not pause the guest */
        bdrv_set_on_error(bs, BLOCKDEV_ON_ERROR_REPORT,
                          BLOCKDEV_ON_ERROR_REPORT);
    } else {
        drive_uninit(drive_get_by_blockdev(bs));
    }

    return 0;
}

void qmp_block_resize(const char *device, int64_t size, Error **errp)
{
    BlockDriverState *bs;
    int ret;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (size < 0) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE, "size", "a >0 size");
        return;
    }

    /* complete all in-flight operations before resizing the device */
    bdrv_drain_all();

    ret = bdrv_truncate(bs, size);
    switch (ret) {
    case 0:
        break;
    case -ENOMEDIUM:
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        break;
    case -ENOTSUP:
        error_set(errp, QERR_UNSUPPORTED);
        break;
    case -EACCES:
        error_set(errp, QERR_DEVICE_IS_READ_ONLY, device);
        break;
    case -EBUSY:
        error_set(errp, QERR_DEVICE_IN_USE, device);
        break;
    default:
        error_setg_errno(errp, -ret, "Could not resize");
        break;
    }
}

static void block_job_cb(void *opaque, int ret)
{
    BlockDriverState *bs = opaque;
    QObject *obj;

    trace_block_job_cb(bs, bs->job, ret);

    assert(bs->job);
    obj = qobject_from_block_job(bs->job);
    if (ret < 0) {
        QDict *dict = qobject_to_qdict(obj);
        qdict_put(dict, "error", qstring_from_str(strerror(-ret)));
    }

    if (block_job_is_cancelled(bs->job)) {
        monitor_protocol_event(QEVENT_BLOCK_JOB_CANCELLED, obj);
    } else {
        monitor_protocol_event(QEVENT_BLOCK_JOB_COMPLETED, obj);
    }
    qobject_decref(obj);

    drive_put_ref_bh_schedule(drive_get_by_blockdev(bs));
}

void qmp_block_stream(const char *device, bool has_base,
                      const char *base, bool has_speed, int64_t speed,
                      bool has_on_error, BlockdevOnError on_error,
                      Error **errp)
{
    BlockDriverState *bs;
    BlockDriverState *base_bs = NULL;
    Error *local_err = NULL;

    if (!has_on_error) {
        on_error = BLOCKDEV_ON_ERROR_REPORT;
    }

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (base) {
        base_bs = bdrv_find_backing_image(bs, base);
        if (base_bs == NULL) {
            error_set(errp, QERR_BASE_NOT_FOUND, base);
            return;
        }
    }

    stream_start(bs, base_bs, base, has_speed ? speed : 0,
                 on_error, block_job_cb, bs, &local_err);
    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        return;
    }

    /* Grab a reference so hotplug does not delete the BlockDriverState from
     * underneath us.
     */
    drive_get_ref(drive_get_by_blockdev(bs));

    trace_qmp_block_stream(bs, bs->job);
}

void qmp_block_commit(const char *device,
                      bool has_base, const char *base, const char *top,
                      bool has_speed, int64_t speed,
                      Error **errp)
{
    BlockDriverState *bs;
    BlockDriverState *base_bs, *top_bs;
    Error *local_err = NULL;
    /* This will be part of the QMP command, if/when the
     * BlockdevOnError change for blkmirror makes it in
     */
    BlockdevOnError on_error = BLOCKDEV_ON_ERROR_REPORT;

    /* drain all i/o before commits */
    bdrv_drain_all();

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    /* default top_bs is the active layer */
    top_bs = bs;

    if (top) {
        if (strcmp(bs->filename, top) != 0) {
            top_bs = bdrv_find_backing_image(bs, top);
        }
    }

    if (top_bs == NULL) {
        error_setg(errp, "Top image file %s not found", top ? top : "NULL");
        return;
    }

    if (has_base && base) {
        base_bs = bdrv_find_backing_image(top_bs, base);
    } else {
        base_bs = bdrv_find_base(top_bs);
    }

    if (base_bs == NULL) {
        error_set(errp, QERR_BASE_NOT_FOUND, base ? base : "NULL");
        return;
    }

    commit_start(bs, base_bs, top_bs, speed, on_error, block_job_cb, bs,
                &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }
    /* Grab a reference so hotplug does not delete the BlockDriverState from
     * underneath us.
     */
    drive_get_ref(drive_get_by_blockdev(bs));
}

void qmp_drive_backup(const char *device, const char *target,
                      bool has_format, const char *format,
                      enum MirrorSyncMode sync,
                      bool has_mode, enum NewImageMode mode,
                      bool has_speed, int64_t speed,
                      bool has_on_source_error, BlockdevOnError on_source_error,
                      bool has_on_target_error, BlockdevOnError on_target_error,
                      Error **errp)
{
    BlockDriverState *bs;
    BlockDriverState *target_bs;
    BlockDriverState *source = NULL;
    BlockDriver *drv = NULL;
    Error *local_err = NULL;
    int flags;
    int64_t size;
    int ret;

    if (!has_speed) {
        speed = 0;
    }
    if (!has_on_source_error) {
        on_source_error = BLOCKDEV_ON_ERROR_REPORT;
    }
    if (!has_on_target_error) {
        on_target_error = BLOCKDEV_ON_ERROR_REPORT;
    }
    if (!has_mode) {
        mode = NEW_IMAGE_MODE_ABSOLUTE_PATHS;
    }

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (!bdrv_is_inserted(bs)) {
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        return;
    }

    if (!has_format) {
        format = mode == NEW_IMAGE_MODE_EXISTING ? NULL : bs->drv->format_name;
    }
    if (format) {
        drv = bdrv_find_format(format);
        if (!drv) {
            error_set(errp, QERR_INVALID_BLOCK_FORMAT, format);
            return;
        }
    }

    if (bdrv_in_use(bs)) {
        error_set(errp, QERR_DEVICE_IN_USE, device);
        return;
    }

    flags = bs->open_flags | BDRV_O_RDWR;

    /* See if we have a backing HD we can use to create our new image
     * on top of. */
    if (sync == MIRROR_SYNC_MODE_TOP) {
        source = bs->backing_hd;
        if (!source) {
            sync = MIRROR_SYNC_MODE_FULL;
        }
    }
    if (sync == MIRROR_SYNC_MODE_NONE) {
        source = bs;
    }

    size = bdrv_getlength(bs);
    if (size < 0) {
        error_setg_errno(errp, -size, "bdrv_getlength failed");
        return;
    }

    if (mode != NEW_IMAGE_MODE_EXISTING) {
        assert(format && drv);
        if (source) {
            bdrv_img_create(target, format, source->filename,
                            source->drv->format_name, NULL,
                            size, flags, &local_err, false);
        } else {
            bdrv_img_create(target, format, NULL, NULL, NULL,
                            size, flags, &local_err, false);
        }
    }

    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        return;
    }

    target_bs = bdrv_new("");
    ret = bdrv_open(target_bs, target, NULL, flags, drv);
    if (ret < 0) {
        bdrv_delete(target_bs);
        error_setg_file_open(errp, -ret, target);
        return;
    }

    backup_start(bs, target_bs, speed, sync, on_source_error, on_target_error,
                 block_job_cb, bs, &local_err);
    if (local_err != NULL) {
        bdrv_delete(target_bs);
        error_propagate(errp, local_err);
        return;
    }

    /* Grab a reference so hotplug does not delete the BlockDriverState from
     * underneath us.
     */
    drive_get_ref(drive_get_by_blockdev(bs));
}

#define DEFAULT_MIRROR_BUF_SIZE   (10 << 20)

void qmp_drive_mirror(const char *device, const char *target,
                      bool has_format, const char *format,
                      enum MirrorSyncMode sync,
                      bool has_mode, enum NewImageMode mode,
                      bool has_speed, int64_t speed,
                      bool has_granularity, uint32_t granularity,
                      bool has_buf_size, int64_t buf_size,
                      bool has_on_source_error, BlockdevOnError on_source_error,
                      bool has_on_target_error, BlockdevOnError on_target_error,
                      Error **errp)
{
    BlockDriverState *bs;
    BlockDriverState *source, *target_bs;
    BlockDriver *drv = NULL;
    Error *local_err = NULL;
    int flags;
    int64_t size;
    int ret;

    if (!has_speed) {
        speed = 0;
    }
    if (!has_on_source_error) {
        on_source_error = BLOCKDEV_ON_ERROR_REPORT;
    }
    if (!has_on_target_error) {
        on_target_error = BLOCKDEV_ON_ERROR_REPORT;
    }
    if (!has_mode) {
        mode = NEW_IMAGE_MODE_ABSOLUTE_PATHS;
    }
    if (!has_granularity) {
        granularity = 0;
    }
    if (!has_buf_size) {
        buf_size = DEFAULT_MIRROR_BUF_SIZE;
    }

    if (granularity != 0 && (granularity < 512 || granularity > 1048576 * 64)) {
        error_set(errp, QERR_INVALID_PARAMETER, device);
        return;
    }
    if (granularity & (granularity - 1)) {
        error_set(errp, QERR_INVALID_PARAMETER, device);
        return;
    }

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (!bdrv_is_inserted(bs)) {
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        return;
    }

    if (!has_format) {
        format = mode == NEW_IMAGE_MODE_EXISTING ? NULL : bs->drv->format_name;
    }
    if (format) {
        drv = bdrv_find_format(format);
        if (!drv) {
            error_set(errp, QERR_INVALID_BLOCK_FORMAT, format);
            return;
        }
    }

    if (bdrv_in_use(bs)) {
        error_set(errp, QERR_DEVICE_IN_USE, device);
        return;
    }

    flags = bs->open_flags | BDRV_O_RDWR;
    source = bs->backing_hd;
    if (!source && sync == MIRROR_SYNC_MODE_TOP) {
        sync = MIRROR_SYNC_MODE_FULL;
    }

    size = bdrv_getlength(bs);
    if (size < 0) {
        error_setg_errno(errp, -size, "bdrv_getlength failed");
        return;
    }

    if (sync == MIRROR_SYNC_MODE_FULL && mode != NEW_IMAGE_MODE_EXISTING) {
        /* create new image w/o backing file */
        assert(format && drv);
        bdrv_img_create(target, format,
                        NULL, NULL, NULL, size, flags, &local_err, false);
    } else {
        switch (mode) {
        case NEW_IMAGE_MODE_EXISTING:
            ret = 0;
            break;
        case NEW_IMAGE_MODE_ABSOLUTE_PATHS:
            /* create new image with backing file */
            bdrv_img_create(target, format,
                            source->filename,
                            source->drv->format_name,
                            NULL, size, flags, &local_err, false);
            break;
        default:
            abort();
        }
    }

    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        return;
    }

    /* Mirroring takes care of copy-on-write using the source's backing
     * file.
     */
    target_bs = bdrv_new("");
    ret = bdrv_open(target_bs, target, NULL, flags | BDRV_O_NO_BACKING, drv);
    if (ret < 0) {
        bdrv_delete(target_bs);
        error_setg_file_open(errp, -ret, target);
        return;
    }

    mirror_start(bs, target_bs, speed, granularity, buf_size, sync,
                 on_source_error, on_target_error,
                 block_job_cb, bs, &local_err);
    if (local_err != NULL) {
        bdrv_delete(target_bs);
        error_propagate(errp, local_err);
        return;
    }

    /* Grab a reference so hotplug does not delete the BlockDriverState from
     * underneath us.
     */
    drive_get_ref(drive_get_by_blockdev(bs));
}

static BlockJob *find_block_job(const char *device)
{
    BlockDriverState *bs;

    bs = bdrv_find(device);
    if (!bs || !bs->job) {
        return NULL;
    }
    return bs->job;
}

void qmp_block_job_set_speed(const char *device, int64_t speed, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    block_job_set_speed(job, speed, errp);
}

void qmp_block_job_cancel(const char *device,
                          bool has_force, bool force, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!has_force) {
        force = false;
    }

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }
    if (job->paused && !force) {
        error_set(errp, QERR_BLOCK_JOB_PAUSED, device);
        return;
    }

    trace_qmp_block_job_cancel(job);
    block_job_cancel(job);
}

void qmp_block_job_pause(const char *device, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    trace_qmp_block_job_pause(job);
    block_job_pause(job);
}

void qmp_block_job_resume(const char *device, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    trace_qmp_block_job_resume(job);
    block_job_resume(job);
}

void qmp_block_job_complete(const char *device, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    trace_qmp_block_job_complete(job);
    block_job_complete(job, errp);
}

static void do_qmp_query_block_jobs_one(void *opaque, BlockDriverState *bs)
{
    BlockJobInfoList **prev = opaque;
    BlockJob *job = bs->job;

    if (job) {
        BlockJobInfoList *elem = g_new0(BlockJobInfoList, 1);
        elem->value = block_job_query(bs->job);
        (*prev)->next = elem;
        *prev = elem;
    }
}

BlockJobInfoList *qmp_query_block_jobs(Error **errp)
{
    /* Dummy is a fake list element for holding the head pointer */
    BlockJobInfoList dummy = {};
    BlockJobInfoList *prev = &dummy;
    bdrv_iterate(do_qmp_query_block_jobs_one, &prev);
    return dummy.next;
}

QemuOptsList qemu_common_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_common_drive_opts.head),
    .desc = {
        {
            .name = "bus",
            .type = QEMU_OPT_NUMBER,
            .help = "bus number",
        },{
            .name = "unit",
            .type = QEMU_OPT_NUMBER,
            .help = "unit number (i.e. lun for scsi)",
        },{
            .name = "if",
            .type = QEMU_OPT_STRING,
            .help = "interface (ide, scsi, sd, mtd, floppy, pflash, virtio)",
        },{
            .name = "index",
            .type = QEMU_OPT_NUMBER,
            .help = "index number",
        },{
            .name = "cyls",
            .type = QEMU_OPT_NUMBER,
            .help = "number of cylinders (ide disk geometry)",
        },{
            .name = "heads",
            .type = QEMU_OPT_NUMBER,
            .help = "number of heads (ide disk geometry)",
        },{
            .name = "secs",
            .type = QEMU_OPT_NUMBER,
            .help = "number of sectors (ide disk geometry)",
        },{
            .name = "trans",
            .type = QEMU_OPT_STRING,
            .help = "chs translation (auto, lba. none)",
        },{
            .name = "media",
            .type = QEMU_OPT_STRING,
            .help = "media type (disk, cdrom)",
        },{
            .name = "snapshot",
            .type = QEMU_OPT_BOOL,
            .help = "enable/disable snapshot mode",
        },{
            .name = "file",
            .type = QEMU_OPT_STRING,
            .help = "disk image",
        },{
            .name = "discard",
            .type = QEMU_OPT_STRING,
            .help = "discard operation (ignore/off, unmap/on)",
        },{
            .name = "cache.writeback",
            .type = QEMU_OPT_BOOL,
            .help = "enables writeback mode for any caches",
        },{
            .name = "cache.direct",
            .type = QEMU_OPT_BOOL,
            .help = "enables use of O_DIRECT (bypass the host page cache)",
        },{
            .name = "cache.no-flush",
            .type = QEMU_OPT_BOOL,
            .help = "ignore any flush requests for the device",
        },{
            .name = "aio",
            .type = QEMU_OPT_STRING,
            .help = "host AIO implementation (threads, native)",
        },{
            .name = "format",
            .type = QEMU_OPT_STRING,
            .help = "disk format (raw, qcow2, ...)",
        },{
            .name = "serial",
            .type = QEMU_OPT_STRING,
            .help = "disk serial number",
        },{
            .name = "rerror",
            .type = QEMU_OPT_STRING,
            .help = "read error action",
        },{
            .name = "werror",
            .type = QEMU_OPT_STRING,
            .help = "write error action",
        },{
            .name = "addr",
            .type = QEMU_OPT_STRING,
            .help = "pci address (virtio only)",
        },{
            .name = "read-only",
            .type = QEMU_OPT_BOOL,
            .help = "open drive file as read-only",
        },{
            .name = "throttling.iops-total",
            .type = QEMU_OPT_NUMBER,
            .help = "limit total I/O operations per second",
        },{
            .name = "throttling.iops-read",
            .type = QEMU_OPT_NUMBER,
            .help = "limit read operations per second",
        },{
            .name = "throttling.iops-write",
            .type = QEMU_OPT_NUMBER,
            .help = "limit write operations per second",
        },{
            .name = "throttling.bps-total",
            .type = QEMU_OPT_NUMBER,
            .help = "limit total bytes per second",
        },{
            .name = "throttling.bps-read",
            .type = QEMU_OPT_NUMBER,
            .help = "limit read bytes per second",
        },{
            .name = "throttling.bps-write",
            .type = QEMU_OPT_NUMBER,
            .help = "limit write bytes per second",
        },{
            .name = "copy-on-read",
            .type = QEMU_OPT_BOOL,
            .help = "copy read data from backing file into image file",
        },{
            .name = "boot",
            .type = QEMU_OPT_BOOL,
            .help = "(deprecated, ignored)",
        },
        { /* end of list */ }
    },
};

QemuOptsList qemu_old_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_old_drive_opts.head),
    .desc = {
        {
            .name = "bus",
            .type = QEMU_OPT_NUMBER,
            .help = "bus number",
        },{
            .name = "unit",
            .type = QEMU_OPT_NUMBER,
            .help = "unit number (i.e. lun for scsi)",
        },{
            .name = "if",
            .type = QEMU_OPT_STRING,
            .help = "interface (ide, scsi, sd, mtd, floppy, pflash, virtio)",
        },{
            .name = "index",
            .type = QEMU_OPT_NUMBER,
            .help = "index number",
        },{
            .name = "cyls",
            .type = QEMU_OPT_NUMBER,
            .help = "number of cylinders (ide disk geometry)",
        },{
            .name = "heads",
            .type = QEMU_OPT_NUMBER,
            .help = "number of heads (ide disk geometry)",
        },{
            .name = "secs",
            .type = QEMU_OPT_NUMBER,
            .help = "number of sectors (ide disk geometry)",
        },{
            .name = "trans",
            .type = QEMU_OPT_STRING,
            .help = "chs translation (auto, lba. none)",
        },{
            .name = "media",
            .type = QEMU_OPT_STRING,
            .help = "media type (disk, cdrom)",
        },{
            .name = "snapshot",
            .type = QEMU_OPT_BOOL,
            .help = "enable/disable snapshot mode",
        },{
            .name = "file",
            .type = QEMU_OPT_STRING,
            .help = "disk image",
        },{
            .name = "discard",
            .type = QEMU_OPT_STRING,
            .help = "discard operation (ignore/off, unmap/on)",
        },{
            .name = "cache",
            .type = QEMU_OPT_STRING,
            .help = "host cache usage (none, writeback, writethrough, "
                    "directsync, unsafe)",
        },{
            .name = "aio",
            .type = QEMU_OPT_STRING,
            .help = "host AIO implementation (threads, native)",
        },{
            .name = "format",
            .type = QEMU_OPT_STRING,
            .help = "disk format (raw, qcow2, ...)",
        },{
            .name = "serial",
            .type = QEMU_OPT_STRING,
            .help = "disk serial number",
        },{
            .name = "rerror",
            .type = QEMU_OPT_STRING,
            .help = "read error action",
        },{
            .name = "werror",
            .type = QEMU_OPT_STRING,
            .help = "write error action",
        },{
            .name = "addr",
            .type = QEMU_OPT_STRING,
            .help = "pci address (virtio only)",
        },{
            .name = "readonly",
            .type = QEMU_OPT_BOOL,
            .help = "open drive file as read-only",
        },{
            .name = "iops",
            .type = QEMU_OPT_NUMBER,
            .help = "limit total I/O operations per second",
        },{
            .name = "iops_rd",
            .type = QEMU_OPT_NUMBER,
            .help = "limit read operations per second",
        },{
            .name = "iops_wr",
            .type = QEMU_OPT_NUMBER,
            .help = "limit write operations per second",
        },{
            .name = "bps",
            .type = QEMU_OPT_NUMBER,
            .help = "limit total bytes per second",
        },{
            .name = "bps_rd",
            .type = QEMU_OPT_NUMBER,
            .help = "limit read bytes per second",
        },{
            .name = "bps_wr",
            .type = QEMU_OPT_NUMBER,
            .help = "limit write bytes per second",
        },{
            .name = "copy-on-read",
            .type = QEMU_OPT_BOOL,
            .help = "copy read data from backing file into image file",
        },{
            .name = "boot",
            .type = QEMU_OPT_BOOL,
            .help = "(deprecated, ignored)",
        },
        { /* end of list */ }
    },
};

QemuOptsList qemu_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_drive_opts.head),
    .desc = {
        /*
         * no elements => accept any params
         * validation will happen later
         */
        { /* end of list */ }
    },
};
