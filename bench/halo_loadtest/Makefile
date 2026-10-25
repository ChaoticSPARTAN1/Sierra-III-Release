TARGET = halo_loadtest
OBJS = main.o
CFLAGS = -O2 -G0 -Wall -fno-pic
BUILD_PRX = 1
LIBS = -lpspctrl -lpspdebug
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Halo PSP Load Test

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
