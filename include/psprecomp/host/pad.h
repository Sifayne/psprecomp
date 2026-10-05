#ifndef PSPRECOMP_HOST_PAD_H
#define PSPRECOMP_HOST_PAD_H
/* Carrier bits: the extra physical controls of a modern controller, in bits
 * of the sceCtrl button word that no PSP button uses.
 *
 * They pass through sceCtrl, and therefore through the recorder and the
 * replayer, and a title's replacement of its button converter reads them and
 * strips them before the game sees its buttons. A title without one never
 * gets them: the host sends them only to a title with
 * PSP_TITLE_MODERN_CONTROLS (psprecomp/host/title.h). HOME (0x10000) and
 * HOLD (0x20000) are skipped deliberately, because games read those two
 * before any converter. Both games defined these values identically as
 * LR_PAD_*. */
enum {
    PSP_PAD_A     = 0x00000400u,
    PSP_PAD_B     = 0x00000800u,
    PSP_PAD_X     = 0x00040000u,
    PSP_PAD_Y     = 0x00080000u,
    PSP_PAD_LB    = 0x00100000u,
    PSP_PAD_RB    = 0x00200000u,
    PSP_PAD_LT    = 0x00400000u,
    PSP_PAD_RT    = 0x00800000u,
    PSP_PAD_L3    = 0x01000000u,
    PSP_PAD_R3    = 0x02000000u,
    PSP_PAD_EXTRA = 0x03FC0C00u,
};
#endif
