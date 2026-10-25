	.set noreorder
#include "pspimport.s"
# sctrlSEGetConfigEx (the NID from ARK-4's SystemCtrlForUser.S)
	IMPORT_FUNC  "SystemCtrlForUser",0x8E426F09,sctrlSEGetConfigEx
