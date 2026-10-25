	.set noreorder

#include "pspimport.s"

// Build files
// kcall_0000.o kcall_0001.o kcall_0002.o kcall_0003.o 

#ifdef F_kcall_0000
	IMPORT_START "kcall",0x40090000
#endif
#ifdef F_kcall_0001
	IMPORT_FUNC  "kcall",0x75F43FF0,kcall_2
#endif
#ifdef F_kcall_0002
	IMPORT_FUNC  "kcall",0xBADD8D3B,kcall_3
#endif
#ifdef F_kcall_0003
	IMPORT_FUNC  "kcall",0x2BB46CB6,kinit
#endif
