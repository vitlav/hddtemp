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
/* 
 * Adapted from a patch sended by : Frederic LOCHON <lochon@roulaise.net>
 */

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/hdreg.h>
#include <scsi/scsi.h>

#include "scsicmds.h"
#include "hddtemp.h"
#include "i18n.h"

#define GBUF_SIZE 65535

static char gBuf[GBUF_SIZE];

static int scsi_probe(int device) {
  int bus_num;

  if(ioctl(device, SCSI_IOCTL_GET_BUS_NUMBER, &bus_num))
    return 0;
  else
    return 1;
}

static const char *scsi_model (int device) {
  if (stdinquiry (device, (char *) &gBuf) != 0)
    return strdup(_("unknown"));
  else {
    gBuf[16] = '\0';
    return strdup((char*) ((u16*)gBuf + 8 ));
  }
}

/*
static int scsi_enable_smart(int device)
{
  UINT8 smartsupport;
  return scsiSmartSupport(device, (UINT8 *) &smartsupport);
}
*/

/*
static int scsi_get_smart_values(int device, unsigned char* buff)
{
  UINT8 tBuf[1024];
  unsigned short pagesize;
  int ret;

  ret = logsense ( device , SMART_PAGE, (UINT8 *) &tBuf);
  if(ret)
    return ret;

  pagesize = (unsigned short) (tBuf[2] << 8) | tBuf[3];
  if ( !pagesize ) return 1;
  if ( pagesize == 8 )
  {
    buff[2]=194;
    buff[7]=tBuf[10];
  }
  return 0;
}
*/


static enum e_gettemp scsi_get_temperature(struct disk *dsk) {
  int              i;
  int              tempPage = 0, smartPage = 0;
  UINT8            smartsupport;

  /*
  if(dsk->db_entry && dsk->db_entry->attribute_id == 0) {
    close(dsk->fd);
    dsk->fd = -1;
    return GETTEMP_NOSENSOR;
  }
  */

  if ( scsiSmartSupport(dsk->fd, (UINT8 *) &smartsupport) != 0 && (gBuf[0] & 0x1f) == 0 ) {
    snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE, _("%s: %s:  S.M.A.R.T. not available\n"), dsk->drive, dsk->model);
    close(dsk->fd);
    dsk->fd = -1;
    return GETTEMP_NOT_APPLICABLE;
  }

  /*
    Enable SMART
  */
  if (scsiSmartDEXCPTDisable(dsk->fd) != 0) {
    snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE, "%s", strerror(errno));
    close(dsk->fd);
    dsk->fd = -1;
    return GETTEMP_ERROR;
  }

  /*
  if (scsiSmartEWASCEnable(dsk->device) != 0) {
    printf("Temperature Warning not Supported\n");
  }
  else {
    printf("Temperature Warning Enabled\n");  
  }
  */


  /*
    Temp. capable 
  */  
  if (logsense ( dsk->fd , SUPPORT_LOG_PAGES, (UINT8 *) &gBuf) != 0) {
    snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE, _("log sense failed : %s"), strerror(errno));
    close(dsk->fd);
    dsk->fd = -1;
    return GETTEMP_ERROR;
   }

   for ( i = 4; i < gBuf[3] + LOGPAGEHDRSIZE ; i++) {
     switch ( gBuf[i]) {
     case TEMPERATURE_PAGE:
       tempPage = 1;
       break;
     /*
     case STARTSTOP_CYCLE_COUNTER_PAGE:
       gStartStopPage = 1;
       break;
     case SELFTEST_RESULTS_PAGE:
       gSelfTestPage = 1;
       break;
     */
     case SMART_PAGE:
       smartPage = 1;
       break;
     /*
     case TAPE_ALERTS_PAGE:
       gTapeAlertsPage = 1;
       break;
     */
     default:
       break;
     }
   }

   if(tempPage) {
      /* 
	 get temperature (from scsiGetTemp (scsicmd.c))
      */

      UINT8 tBuf[1024];

      if (logsense ( dsk->fd , TEMPERATURE_PAGE, (UINT8 *) &tBuf) != 0) {
	snprintf(dsk->errormsg, MAX_ERRORMSG_SIZE, _("log sense failed : %s"), strerror(errno));
	close(dsk->fd);
	dsk->fd = -1;
	return GETTEMP_ERROR;
      }

      dsk->value = tBuf[9];

      return GETTEMP_KNOWN;
   } else {
     return GETTEMP_NOSENSOR;
   }
}

/*******************************
 *******************************/

struct bustype scsi_bus = {
  scsi_probe,
  scsi_model,
  scsi_get_temperature
};
