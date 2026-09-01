/* psprecomp — sceUmdUser.
 *
 * The UMD drive. There is no drive: `sceIoOpen` reads from a host directory, so
 * by the time a game asks whether the disc is ready, it already is.
 *
 * That makes every call here trivial, and the triviality is the point. A game's
 * startup usually runs
 *
 *     sceUmdCheckMedium();                  is there a disc?
 *     sceUmdActivate(1, "disc0:");          spin it up
 *     sceUmdWaitDriveStat(PSP_UMD_READY);   block until it is ready
 *
 * and an unimplemented firmware call returns zero, which for CheckMedium means
 * "no disc" and for WaitDriveStat means the wait never ends. Armored Core sat
 * in exactly that loop -- alternating sceUmdWaitDriveStat and
 * sceKernelCheckCallback -- because the drive it was waiting for could never
 * become ready.
 *
 * Only the functions this class of module actually imports are here. Adding
 * more would be guessing at semantics with nothing to check them against.
 */

#include "psprecomp/hle.h"

#include <stdio.h>

/* Drive state bits, as pspumd.h defines them. */
#define PSP_UMD_NOT_PRESENT 0x01
#define PSP_UMD_PRESENT     0x02
#define PSP_UMD_CHANGED     0x04
#define PSP_UMD_INITING     0x08
#define PSP_UMD_INITED      0x10
#define PSP_UMD_READY       0x20

/* A disc that is present, initialised and ready, permanently. Nothing here can
 * eject it or spin it down. */
#define DRIVE_STATE (PSP_UMD_PRESENT | PSP_UMD_INITED | PSP_UMD_READY)

static uint32_t g_callback_id;

void psp_umd_reset(void) { g_callback_id = 0; }
void psp_umd_init(void)  { psp_umd_reset(); }

/* Returns 1 when a disc is inserted. Zero -- what an unimplemented call
 * returns -- means "no disc", which is why leaving this out stops a game
 * before it reads anything. */
static void hle_CheckMedium(void) { psp_ret(1); }

/* Spinning up a drive that is already spinning. */
static void hle_Activate(void)   { psp_ret(SCE_KERNEL_ERROR_OK); }
static void hle_Deactivate(void) { psp_ret(SCE_KERNEL_ERROR_OK); }

/* Wait until the drive reaches `state`.
 *
 * It cannot block: there is no drive to change state and no scheduler to yield
 * to, so a wait that did not return immediately would never return at all.
 * Success is therefore reported whatever is asked for -- including, dishonestly,
 * a wait for PSP_UMD_NOT_PRESENT, which will never become true here. A game
 * that ejects and waits for the absence of a disc would be told it happened.
 * That is worth knowing about and not worth modelling until something needs
 * it. */
static void hle_WaitDriveStat(void) {
    const uint32_t want = psp_arg(0);
    if (want & PSP_UMD_NOT_PRESENT) {
        static int complained;
        if (!complained++)
            fprintf(stderr, "psprecomp: sceUmdWaitDriveStat waited for the disc to be "
                            "absent; reporting success, which is a lie\n");
    }
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* The drive raises a callback on insert/eject. Neither happens, so the id is
 * recorded and never fired -- registering is accepted so the game's bookkeeping
 * stays consistent, and unregistering finds what it expects. */
static void hle_RegisterCallback(void) {
    g_callback_id = psp_arg(0);
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_UnRegisterCallback(void) {
    g_callback_id = 0;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_umd_register(void) {
    psp_hle_register(0x46EBB729, "sceUmdUser", "sceUmdCheckMedium",           hle_CheckMedium);
    psp_hle_register(0xC6183D47, "sceUmdUser", "sceUmdActivate",              hle_Activate);
    psp_hle_register(0xE83742BA, "sceUmdUser", "sceUmdDeactivate",            hle_Deactivate);
    psp_hle_register(0x8EF08FCE, "sceUmdUser", "sceUmdWaitDriveStat",         hle_WaitDriveStat);
    psp_hle_register(0x4A9E5E29, "sceUmdUser", "sceUmdWaitDriveStatCB",       hle_WaitDriveStat);
    psp_hle_register(0xAEE7404D, "sceUmdUser", "sceUmdRegisterUMDCallBack",   hle_RegisterCallback);
    psp_hle_register(0xBD2BDE07, "sceUmdUser", "sceUmdUnRegisterUMDCallBack", hle_UnRegisterCallback);
}
