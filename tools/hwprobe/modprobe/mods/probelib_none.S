/* probelib_none.S -- an import of a library nothing exports, for mod_d. */
	.set noreorder

#include "pspstub.s"

	STUB_START "ProbeLibNone",0x00090000,0x00010005
	STUB_FUNC  0xF945FDB0,probe_none
	STUB_END
