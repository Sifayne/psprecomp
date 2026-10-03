TARGET = geprobe
OBJS = main.o probe.o
VPATH = ../common
INCDIR = ../common
CFLAGS = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PSP_LARGE_MEMORY = 0

LIBDIR =
LDFLAGS =
LIBS = -lpspgum -lpspgu

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = GE probe v$(shell sed -n 's/^.define PROBE_VERSION //p' main.c)

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# main.c includes the generated streams (patch13.py emit); rebuild when they change.
main.o: patch13_data.inc c14_data.inc
