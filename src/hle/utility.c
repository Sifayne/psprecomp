/* psprecomp — the utility dialogs, which are not implemented.
 *
 * Registered anyway, and that is the whole point of the file. An unimplemented
 * firmware call returns zero (see psp_hle_call), which for a dialog reads as
 * "started successfully" -- so the caller settles into its poll loop and waits
 * for a status that nothing will ever set. pspautotests' savedata harness polls
 * four hundred thousand times with a 2ms delay between attempts; eight of its
 * tests produced no output at all, hit the thread-drain timeout and dumped
 * core, and cost the sweep about three and a half minutes between them.
 *
 * sceMpeg already made this call and states the reasoning: refuse outright
 * rather than report nothing, because a caller that waits out "nothing" never
 * stops. A dialog has a documented way of ending without doing anything, which
 * is to be cancelled, so that is what these report.
 *
 * Only the states are modelled, and only the ones PSPSDK names. The dialog
 * protocol is InitStart, then poll GetStatus while calling Update, then
 * ShutdownStart once the status says the dialog is done with -- so a dialog
 * that goes straight to QUIT is a dialog the user dismissed before it did
 * anything, which is a sequence a real caller must already handle. Nothing here
 * touches ms0:, PARAM.SFO or the save layout: this is the shape of the
 * conversation, not savedata. */

#include "psprecomp/hle.h"
#include "psprecomp/cpu.h"

#include <stdio.h>

/* PspUtilityDialogState, from PSPSDK's psputility.h -- the values a caller
 * compares GetStatus against. NONE is "no dialog is currently active" and QUIT
 * is "the dialog has been cancelled and should be shut down"; those two are the
 * only ones a dialog that never appears can honestly be in. INIT, VISIBLE and
 * FINISHED are named here because the enum is only half an answer without them.
 * https://pspdev.github.io/pspsdk/psputility_8h_source.html */
#define PSP_UTILITY_DIALOG_NONE     0
#define PSP_UTILITY_DIALOG_INIT     1
#define PSP_UTILITY_DIALOG_VISIBLE  2
#define PSP_UTILITY_DIALOG_QUIT     3
#define PSP_UTILITY_DIALOG_FINISHED 4

static int g_savedata_state;

void psp_utility_init(void) { g_savedata_state = PSP_UTILITY_DIALOG_NONE; }

/* Said once. A title that offers to load a save on every screen would otherwise
 * repeat this for as long as it runs. */
static void no_savedata(void) {
    static int said;
    if (!said++)
        fprintf(stderr,
            "psprecomp: sceUtilitySavedata is not implemented. The dialog is\n"
            "  reported as cancelled rather than left pending, because a caller\n"
            "  polls a pending dialog forever. Saves cannot be loaded or\n"
            "  written; everything else continues.\n");
}

/* Accepted. Returning an error here would end the conversation sooner, but
 * every error this could return is a claim about *why* -- that the data is
 * corrupt, or the memory stick is absent -- and none of those is true. What is
 * true is that the dialog opens and closes without the user seeing it. */
static void hle_SavedataInitStart(void) {
    no_savedata();
    g_savedata_state = PSP_UTILITY_DIALOG_INIT;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

/* Reports where the dialog is, then moves it on.
 *
 * The lifecycle is a ratchet rather than a state machine with inputs, and it
 * has to be: the only thing that could drive it here is being asked. A caller
 * polls until the dialog is done with, so a status that only changed when
 * something else changed it would never change at all -- the same trade
 * hle_GetVcount already makes for the scanline counter, and for the same
 * reason.
 *
 * The sequence is hardware's, read off savedata/autosave: INIT, then VISIBLE
 * while the caller drives it, then QUIT to say it is finished with, and after
 * ShutdownStart a FINISHED that settles to NONE. A dialog that does nothing
 * still passes through all of them, which is why they are all here -- stopping
 * at QUIT skipped three of the statuses the tests print. */
static void hle_SavedataGetStatus(void) {
    const int now = g_savedata_state;
    switch (now) {
    case PSP_UTILITY_DIALOG_INIT:     g_savedata_state = PSP_UTILITY_DIALOG_VISIBLE; break;
    case PSP_UTILITY_DIALOG_VISIBLE:  g_savedata_state = PSP_UTILITY_DIALOG_QUIT;    break;
    case PSP_UTILITY_DIALOG_FINISHED: g_savedata_state = PSP_UTILITY_DIALOG_NONE;    break;
    /* QUIT waits for ShutdownStart, and NONE is the resting state. */
    default: break;
    }
    psp_ret((uint32_t)now);
}

/* The caller drives the dialog a frame at a time. There is no dialog to drive,
 * but the call has to succeed: a caller that sees Update fail reports it and
 * keeps polling, which is the loop this file exists to end. */
static void hle_SavedataUpdate(void) {
    psp_ret(SCE_KERNEL_ERROR_OK);
}

static void hle_SavedataShutdownStart(void) {
    g_savedata_state = PSP_UTILITY_DIALOG_FINISHED;
    psp_ret(SCE_KERNEL_ERROR_OK);
}

void psp_utility_register(void) {
    psp_hle_register(0x50C4CD57, "sceUtility", "sceUtilitySavedataInitStart",
                     hle_SavedataInitStart);
    psp_hle_register(0x8874DBE0, "sceUtility", "sceUtilitySavedataGetStatus",
                     hle_SavedataGetStatus);
    psp_hle_register(0xD4B95FFB, "sceUtility", "sceUtilitySavedataUpdate",
                     hle_SavedataUpdate);
    psp_hle_register(0x9790B33C, "sceUtility", "sceUtilitySavedataShutdownStart",
                     hle_SavedataShutdownStart);
}
