## Process this file with automake to produce Makefile.in
CLEANFILES = *~

sbin_PROGRAMS = hddtemp

hddtemp_SOURCES = ata.c ata.h \
		  atacmds.c atacmds.h \
		  daemon.c daemon.h \
                  db.c db.h \
		  hddtemp.c hddtemp.h \
		  sata.c sata.h \
		  satacmds.c statcmds.h \
		  scsi.c scsi.h \
		  scsicmds.c scsicmds.h \
		  backtrace.c backtrace.h \
		  utf8.c utf8.h \
		  nvme.c nvme.h

hddtemp_CFLAGS = -Wall -W -rdynamic

localedir = $(datadir)/locale

INCLUDES = -I. -I$(srcdir) -I..
DEFS = -DLOCALEDIR=\"$(localedir)\" @DEFS@
