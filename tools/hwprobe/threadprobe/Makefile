TARGET = threadprobe
OBJS = main.o probe.o stubs.o
VPATH = ../common
INCDIR = ../common
CFLAGS = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PSP_LARGE_MEMORY = 0
LIBDIR =
LDFLAGS =
# psprtc comes in with the default libraries.
LIBS = -lpsppower
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Thread probe v$(shell sed -n 's/^.define PROBE_VERSION //p' main.c)
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
