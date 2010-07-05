/*
 * Copyright (C) 2002  Emmanuel VARAGNAT <coredump@free.fr>
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/hdreg.h>

#include "hddtemp.h"
#include "i18n.h"

#define SBUFF_SIZE 512
#define swapb(x) \
({ \
        u16 __x = (x); \
        (x) = ((u16)( \
                (((u16)(__x) & (u16)0x00ffU) << 8) | \
                (((u16)(__x) & (u16)0xff00U) >> 8) )); \
})

static char sbuff[SBUFF_SIZE];

static int ata_probe(int device) {
  if(device == -1 || ioctl(device, HDIO_GET_IDENTITY, sbuff))
    return 0;
  else
    return 1;
}

static const char *ata_model (int device) {
  if(device == -1 || ioctl(device, HDIO_GET_IDENTITY, sbuff))
    return strdup(_("unknown"));
  else
    return strdup((char*) ((u16*)sbuff + 27));
}


static int ata_enable_smart(int device) {
  unsigned char cmd[4] = { WIN_SMART, 0, SMART_ENABLE, 0 };

  return ioctl(device, HDIO_DRIVE_CMD, cmd);
}


static int ata_get_smart_values(int device, unsigned char* buff) {
  unsigned char  cmd[516] = { WIN_SMART, 0, SMART_READ_VALUES, 1 };
  int            ret;

  ret = ioctl(device, HDIO_DRIVE_CMD, cmd);
  if(ret)
    return ret;
  memcpy(buff, cmd+4, 512);
  return 0;
}


static unsigned char* ata_search_temperature(const unsigned char* smart_data, int attribute_id) {
  int i, n;

  n = 3;
  i = 0;
  while((debug || *(smart_data + n) != attribute_id) && i < 30) {
    if(debug && *(smart_data + n))
      printf(_("field(%d)\t = %d\n"), *(smart_data + n), *(smart_data + n + 3));

    n += 12;
    i++;
  }

  if(i >= 30)
    return NULL;
  else
    return (unsigned char*)(smart_data + n);
}


static enum e_gettemp ata_get_temperature(struct disk *dsk) {   
  unsigned char    values[512]/*, thresholds[512]*/;
  unsigned char *  field;
  int              i;
  u16 *            p;

  if(dsk->db_entry && dsk->db_entry->attribute_id == 0) {
    close(dsk->fd);
    dsk->fd = -1;
    return GETTEMP_NOSENSOR;
  }

  /* get SMART values */
  if(ata_enable_smart(dsk->fd) != 0) {
    snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE, _("S.M.A.R.T. not available"));
    close(dsk->fd);
    dsk->fd = -1;
    return GETTEMP_NOT_APPLICABLE;
  }

  if(ata_get_smart_values(dsk->fd, values)) {
    snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE, "%s", strerror(errno));
    close(dsk->fd);
    dsk->fd = -1;
    return GETTEMP_ERROR;
  }

  p = (u16*)values;
  for(i = 0; i < 256; i++) {
    swapb(*(p+i));
  }

  /* get SMART threshold values */
  /*
  if(get_smart_threshold_values(fd, thresholds)) {
    perror("ioctl");
    exit(3);
  }

  p = (u16*)thresholds;
  for(i = 0; i < 256; i++) {
    swapb(*(p+i));
  }
  */

  /* temperature */
  if(dsk->db_entry && dsk->db_entry->attribute_id > 0)
    field = ata_search_temperature(values, dsk->db_entry->attribute_id);
  else
    field = ata_search_temperature(values, DEFAULT_ATTRIBUTE_ID);

  if(field)
    dsk->value = *(field+3);

  if(dsk->db_entry && dsk->value != -1)
    return GETTEMP_KNOWN;
  else {
    if(dsk->value != -1) {
      return GETTEMP_GUESS;
    }
    else {
      return GETTEMP_UNKNOWN;
    }
  }

  /* never reached */
}



/*******************************
 *******************************/

struct bustype ata_bus = {
  ata_probe,
  ata_model,
  ata_get_temperature
};
