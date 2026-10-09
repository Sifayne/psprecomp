TARGET = modprobe
OBJS = main.o probe.o mods/modulemgr.o
VPATH = ../common
INCDIR = ../common
CFLAGS = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
PSP_LARGE_MEMORY = 0

LIBDIR =
LDFLAGS =
LIBS =

# The test modules too: each a PRX of its own, copied beside the EBOOT.
EXTRA_TARGETS = EBOOT.PBP modules
PSP_EBOOT_TITLE = Module probe v$(shell sed -n 's/^.define PROBE_VERSION //p' main.c)

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

MODULES = a b c d e f
modules:
	for m in $(MODULES); do $(MAKE) -C mods/$$m || exit 1; done
.PHONY: modules

# The title carries PROBE_VERSION: remake it when main.c changes.
PARAM.SFO: main.c
