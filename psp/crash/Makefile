TARGET = halo_crash
OBJS = main.o crash_asm.o
CFLAGS = -O2 -G0 -Wall
BUILD_PRX = 1
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1
LIBS = -lpspdebug -lpspge_driver -lpspdisplay_driver
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
