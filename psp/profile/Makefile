TARGET = halo_profile
OBJS = main.o
CFLAGS = -O2 -G0 -Wall
BUILD_PRX = 1
USE_KERNEL_LIBC = 1
USE_KERNEL_LIBS = 1
LIBS =
PRX_EXPORTS = exports.exp
PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
