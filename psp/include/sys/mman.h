/* PSP: no virtual memory. The platform layer's users of this header only
need the names; psp_memory.c replaces the code that maps memory. */
#ifndef __PSP_SYS_MMAN_H
#define __PSP_SYS_MMAN_H
#include <stddef.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED 0x10
#define MAP_FAILED ((void *)-1)
static inline int mprotect(void *address, size_t length, int protection) { (void)address; (void)length; (void)protection; return 0; }
#endif
