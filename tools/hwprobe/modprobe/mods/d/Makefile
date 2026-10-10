TARGET = mod_d
OBJS = d.o ../probelib_none.o
INCDIR = ..
CFLAGS = -O2 -G0 -Wall -std=gnu99
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PRX_EXPORTS = exports.exp
# module_start is the module's own: no C runtime, so none of its startup.
LDFLAGS = -nostartfiles
LIBS =

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
