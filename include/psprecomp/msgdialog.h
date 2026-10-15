/* Message dialog host bridge (sceUtilityMsgDialog, src/hle/utility.c).
 *
 * The dialog reads the pad itself, as the PSP's does, so the host only draws
 * it: snapshots own their data, never guest pointers, and are thread-safe. */
#ifndef PSPRECOMP_MSGDIALOG_H
#define PSPRECOMP_MSGDIALOG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint64_t session, revision;
    int active;                    /* on screen: the host draws it */
    int error_mode;                /* show `error`, not `message` */
    uint32_t error;
    int yes_no;                    /* Yes and No, rather than one button */
    int selected_no;               /* the cursor is on No */
    int confirm_circle;            /* circle confirms, cross cancels */
    char message[513];
    char ok_label[65], cancel_label[65];  /* the request's own, or empty */
} psp_msgdialog_view;
/* Copy if changed: returns 0 inactive, 1 unchanged, 2 copied. */
int psp_msgdialog_snapshot(psp_msgdialog_view *out);
#ifdef __cplusplus
}
#endif
#endif
