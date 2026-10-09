/* The pack list of a launcher holding one pack: the pack's own
 * psp_launcher_info and psp_title_settings (psprecomp/host/launcher.h). */
#include "psprecomp/host/launcher.h"

const psp_pack psp_packs[] = { { &psp_launcher_info, &psp_title_settings } };
const int psp_pack_count = 1;
