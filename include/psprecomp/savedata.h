/* Savedata host bridge. All snapshots own their data; never guest pointers.
 * The HLE owns selection, confirmation and I/O. Host calls are thread-safe.
 * Register/unregister the redraw callback only while guest execution is stopped. */
#ifndef PSPRECOMP_SAVEDATA_H
#define PSPRECOMP_SAVEDATA_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PSP_SAVEDATA_MAX_SLOTS 256
#define PSP_SAVEDATA_ICON_MAX (128 * 1024)
#define PSP_SAVEDATA_PIC1_MAX (512 * 1024)
enum psp_savedata_stage {
    PSP_SAVEDATA_LIST = 1, PSP_SAVEDATA_CONFIRM, PSP_SAVEDATA_DONE, PSP_SAVEDATA_ERROR
};
enum psp_savedata_action {
    PSP_SAVEDATA_SELECT = 1, PSP_SAVEDATA_ACCEPT, PSP_SAVEDATA_CANCEL
};
typedef struct {
    char name[64];                 /* saveName, or full directory for mode 7 */
    char title[129], detail[1025];
    char icon_path[1024];          /* resolved host path, read-only */
    char pic1_path[1024];          /* resolved host path of PIC1.PNG, or empty */
    int64_t modified;
    uint64_t bytes;
    int exists, broken;
} psp_savedata_slot;
typedef struct {
    uint64_t session, revision;
    uint32_t mode, result;
    int active, stage, count, selected, confirm_circle;
    char game[14], title[129], message[256];
    uint32_t new_icon_size;
    unsigned char new_icon[PSP_SAVEDATA_ICON_MAX];
    uint32_t new_pic1_size;        /* the request's own PIC1: artwork behind an unused slot */
    unsigned char new_pic1[PSP_SAVEDATA_PIC1_MAX];
    psp_savedata_slot slots[PSP_SAVEDATA_MAX_SLOTS];
} psp_savedata_view;
/* Copy if changed: returns 0 inactive, 1 unchanged, 2 copied. Zero-init out. */
int psp_savedata_snapshot(psp_savedata_view *out);
/* One outstanding response; stale revision/session responses are rejected. */
int psp_savedata_respond(uint64_t session, uint64_t revision, int action, int index);
void psp_savedata_set_host(int available);
/* Called from UtilityUpdate on a guest thread. Must not wait for UI input.
 * Return negative if presentation cannot run on that thread; HLE cancels. */
void psp_savedata_set_redraw(int (*redraw)(void));
/* Script lines: ordinal mode game saveName action. action = select/accept/cancel.
 * '-' matches an empty name. One line per Update, independent of pad polling.
 * NULL closes the script. Returns -1 on open failure. Set before execution. */
int psp_savedata_set_script(const char *path);
#ifdef __cplusplus
}
#endif
#endif
