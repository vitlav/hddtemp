/*
 * Copyright (C) 2025  Vitaly Lipatov <lav@etersoft.ru>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * CCISS/HPSA support for HP Smart Array controllers
 * Uses lunid from sysfs to address physical disks
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if ENABLE_NLS
#include <libintl.h>
#define _(String) gettext (String)
#else
#define _(String) (String)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/cciss_ioctl.h>

#include "cciss.h"
#include "hddtemp.h"

/* ATA commands */
#define ATA_SMART            0xb0
#define SMART_READ_VALUES    0xd0

/* SAT ATA PASS-THROUGH (16) command */
#define SAT_ATA_PASSTHROUGH_16  0x85

/* Temperature attribute IDs */
#define ATTR_TEMPERATURE     194
#define ATTR_TEMPERATURE2    190

/* Storage for current disk's lunid */
static unsigned char current_lunid[8];
static int lunid_valid = 0;

/*
 * Get device name from file descriptor via /proc/self/fd
 */
static const char *get_device_name(int fd)
{
    static char devname[32];
    char procpath[64];
    char linkpath[256];
    ssize_t len;
    char *p;

    snprintf(procpath, sizeof(procpath), "/proc/self/fd/%d", fd);
    len = readlink(procpath, linkpath, sizeof(linkpath) - 1);
    if (len < 0)
        return NULL;
    linkpath[len] = '\0';

    p = strrchr(linkpath, '/');
    if (p == NULL)
        return NULL;
    p++;

    strncpy(devname, p, sizeof(devname) - 1);
    devname[sizeof(devname) - 1] = '\0';

    /* Strip partition numbers */
    for (p = devname; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            *p = '\0';
            break;
        }
    }

    return devname;
}

/*
 * Read lunid from sysfs (big-endian format)
 */
static int read_lunid(int fd, unsigned char *lunid)
{
    char path[256];
    char buf[32];
    FILE *f;
    const char *devname;
    unsigned long long val;

    devname = get_device_name(fd);
    if (devname == NULL)
        return -1;

    snprintf(path, sizeof(path), "/sys/block/%s/device/lunid", devname);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;

    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Parse hex value like 0x000000c000000101 */
    val = strtoull(buf, NULL, 16);

    /* Store as big-endian byte array (network order) */
    lunid[0] = (val >> 56) & 0xff;
    lunid[1] = (val >> 48) & 0xff;
    lunid[2] = (val >> 40) & 0xff;
    lunid[3] = (val >> 32) & 0xff;
    lunid[4] = (val >> 24) & 0xff;
    lunid[5] = (val >> 16) & 0xff;
    lunid[6] = (val >> 8) & 0xff;
    lunid[7] = val & 0xff;

    return 0;
}

/*
 * Send passthrough command using physical LUN
 */
static int cciss_sendpassthru(int fd, unsigned char *cdb, int cdb_len,
                              unsigned char *buf, int buf_len, int direction)
{
    IOCTL_Command_struct iocommand;

    if (!lunid_valid)
        return -1;

    memset(&iocommand, 0, sizeof(iocommand));

    /* Use physical LUN from sysfs - copy bytes then set Mode=0 for physical device */
    memcpy(iocommand.LUN_info.LunAddrBytes, current_lunid, 8);
    iocommand.LUN_info.LogDev.Mode = 0;  /* Physical device mode */

    iocommand.Request.CDBLen = cdb_len;
    memcpy(iocommand.Request.CDB, cdb, cdb_len);

    iocommand.Request.Type.Type = 0;  /* TYPE_CMD */
    iocommand.Request.Type.Attribute = 4;  /* ATTR_SIMPLE */
    iocommand.Request.Type.Direction = direction;
    iocommand.Request.Timeout = 0;

    iocommand.buf_size = buf_len;
    iocommand.buf = buf;

    if (ioctl(fd, CCISS_PASSTHRU, &iocommand) < 0) {
        fprintf(stderr, "CCISS ioctl failed: %s\n", strerror(errno));
        return -1;
    }

    if (iocommand.error_info.CommandStatus != CMD_SUCCESS) {
        fprintf(stderr, "CCISS cmd failed: status=%d scsi=0x%02x\n",
                iocommand.error_info.CommandStatus,
                iocommand.error_info.ScsiStatus);
        return -1;
    }

    return 0;
}

/*
 * Send ATA SMART READ DATA command via SAT passthrough
 */
static int cciss_ata_smart_read(int fd, unsigned char *buf)
{
    unsigned char cdb[16];

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SAT_ATA_PASSTHROUGH_16;
    cdb[1] = (4 << 1);      /* PROTOCOL: PIO Data-In */
    cdb[2] = 0x0e;          /* CK_COND=0, T_DIR=1, BYT_BLOK=1, T_LENGTH=2 */
    cdb[4] = SMART_READ_VALUES;  /* FEATURES */
    cdb[6] = 1;             /* SECTOR_COUNT */
    cdb[8] = 0;             /* LBA_LOW */
    cdb[10] = 0x4f;         /* LBA_MID (SMART signature) */
    cdb[12] = 0xc2;         /* LBA_HIGH (SMART signature) */
    cdb[13] = 0;            /* DEVICE */
    cdb[14] = ATA_SMART;    /* COMMAND */

    return cciss_sendpassthru(fd, cdb, sizeof(cdb), buf, 512, XFER_READ);
}

/*
 * Search for temperature in SMART data
 */
static int find_temperature(const unsigned char *smart_data, int attr_id)
{
    int i;
    const unsigned char *p;

    /* SMART data format: offset 2 starts attribute entries, each 12 bytes */
    for (i = 0; i < 30; i++) {
        p = smart_data + 2 + (i * 12);
        if (p[0] == attr_id) {
            return p[5];  /* Raw value low byte */
        }
    }
    return -1;
}

static int cciss_probe(int device)
{
    const char *devname;
    char path[256];
    struct stat st;
    unsigned char cdb[6];
    unsigned char buf[36];

    /* Check if device has lunid in sysfs (HP Smart Array indicator) */
    devname = get_device_name(device);
    if (devname == NULL)
        return 0;

    snprintf(path, sizeof(path), "/sys/block/%s/device/lunid", devname);
    if (stat(path, &st) != 0)
        return 0;

    /* Read lunid and send INQUIRY to check if it's SATA (vendor="ATA") */
    if (read_lunid(device, current_lunid) != 0)
        return 0;
    lunid_valid = 1;

    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;  /* INQUIRY */
    cdb[4] = 36;

    if (cciss_sendpassthru(device, cdb, sizeof(cdb), buf, 36, XFER_READ) != 0)
        return 0;

    /* Check vendor field (bytes 8-15) for "ATA" - indicates SATA disk */
    return (strncmp((char *)(buf + 8), "ATA", 3) == 0);
}

static const char *cciss_model(int device)
{
    unsigned char cdb[6];
    unsigned char buf[36];
    char *model;
    int i;

    /* Read and cache lunid */
    if (read_lunid(device, current_lunid) != 0) {
        lunid_valid = 0;
        return strdup(_("unknown (no lunid)"));
    }
    lunid_valid = 1;

    /* SCSI INQUIRY */
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;  /* INQUIRY */
    cdb[4] = 36;

    if (cciss_sendpassthru(device, cdb, sizeof(cdb), buf, 36, XFER_READ) != 0)
        return strdup(_("unknown"));

    /* INQUIRY: bytes 16-31 = Product Identification (skip vendor at 8-15) */
    buf[32] = '\0';
    for (i = 31; i >= 16 && buf[i] == ' '; i--)
        buf[i] = '\0';

    model = strdup((char *)(buf + 16));
    return model ? model : strdup(_("unknown"));
}

static enum e_gettemp cciss_get_temperature(struct disk *dsk)
{
    unsigned char smart_data[512];
    int temp;

    if (dsk->db_entry == NULL) {
        dsk->db_entry = (struct harddrive_entry *)malloc(sizeof(struct harddrive_entry));
        if (dsk->db_entry == NULL) {
            perror("malloc");
            return GETTEMP_ERROR;
        }
        dsk->db_entry->regexp = "";
        dsk->db_entry->description = "";
        dsk->db_entry->attribute_id = ATTR_TEMPERATURE;
        dsk->db_entry->attribute_id2 = ATTR_TEMPERATURE2;
        dsk->db_entry->unit = 'C';
        dsk->db_entry->next = NULL;
    }

    /* Always read lunid for current disk (can't cache across multiple disks) */
    if (read_lunid(dsk->fd, current_lunid) != 0) {
        snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE,
                 _("cannot read lunid from sysfs"));
        return GETTEMP_ERROR;
    }
    lunid_valid = 1;

    if (cciss_ata_smart_read(dsk->fd, smart_data) != 0) {
        snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE,
                 _("SMART read failed"));
        return GETTEMP_ERROR;
    }

    temp = find_temperature(smart_data, dsk->db_entry->attribute_id);
    if (temp < 0 && dsk->db_entry->attribute_id2)
        temp = find_temperature(smart_data, dsk->db_entry->attribute_id2);

    if (temp < 0)
        return GETTEMP_NOSENSOR;

    dsk->value = temp;
    return GETTEMP_KNOWN;
}

struct bustype cciss_bus = {
    "CCISS",
    cciss_probe,
    cciss_model,
    cciss_get_temperature
};
