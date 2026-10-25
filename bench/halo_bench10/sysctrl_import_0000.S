	.set noreorder
#include "pspimport.s"
# ARK-4's SystemCtrlForUser library (libs/SystemCtrlForUser/SystemCtrlForUser.S
# in ARK-4): the library entry. Weak (0x0009 in the flags): without ARK the
# program still loads, and the call returns an error
	IMPORT_START "SystemCtrlForUser",0x40090000
