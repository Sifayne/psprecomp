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
main.o: patch13_data.inc c14_data.inc c15_data.inc c16_data.inc l17_data.inc l18_data.inc l19_data.inc c20_data.inc c21_data.inc c21p_data.inc \
        c22e_data.inc c22t_data.inc c22n_data.inc c22m_data.inc c22c_data.inc \
        c23c_data.inc c23a_data.inc c23b_data.inc c23z_data.inc
