TARGET = mpegprobe
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall -std=gnu99
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

BUILD_PRX = 1
# Same memory layout as the game: no expanded user memory.
PSP_LARGE_MEMORY = 0

LIBDIR =
LDFLAGS =
# psputility comes in with the default libraries; naming it here splits its
# import stubs and psp-fixup-imports cannot join them.
LIBS = -lpspgu -lpspmpeg

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = MPEG probe

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
