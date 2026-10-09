/* probelib_a.S -- imports of mod_a's ProbeLibA, for mod_c and mod_f. NIDs are
 * SHA-1 of the names, as psp-build-exports makes them for
 * PSP_EXPORT_FUNC_HASH. */
	.set noreorder

#include "pspstub.s"

	STUB_START "ProbeLibA",0x00090000,0x00020005
	STUB_FUNC  0x1FE4906D,probe_a_add
	STUB_FUNC  0x4DA49945,probe_a_id
	STUB_END
