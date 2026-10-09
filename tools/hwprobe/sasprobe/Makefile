TARGET = sasprobe
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
# psputility comes in with the default libraries; naming it here splits its
# import stubs and psp-fixup-imports cannot join them.
LIBS = -lpspsascore

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = SasCore probe v$(shell sed -n 's/^.define PROBE_VERSION //p' main.c)

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# The title carries PROBE_VERSION: remake it when main.c changes.
PARAM.SFO: main.c
