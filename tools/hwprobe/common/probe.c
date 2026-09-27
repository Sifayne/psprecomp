/* probe.c -- see probe.h. PSPSDK (BSD) only. */
#include "probe.h"

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <pspdisplay.h>

#include <stdio.h>
#include <string.h>

static char g_name[32];
static char g_dir[256];
static char g_logpath[300];
static char g_pending[16384];
static int  g_pending_len;
static int  g_step;
static int  g_screen = 1;
static int  g_screen_ready;

const char *probe_dir(void) { return g_dir; }

void probe_flush(void) {
    if (!g_pending_len) return;
    SceUID fd = sceIoOpen(g_logpath, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, g_pending, g_pending_len);
        sceIoClose(fd);
    }
    g_pending_len = 0;
}

void probe_screen(int on) {
    g_screen = on;
    if (on && g_screen_ready) pspDebugScreenInit();
}

static void emit(int screen, const char *buf, int n) {
    if (n <= 0) return;
    /* stdout is where psprecomp shows a guest's writes; on a PSP it goes
     * nowhere, which costs nothing. */
    sceIoWrite(1, buf, n);
    if (g_pending_len + n > (int)sizeof g_pending) probe_flush();
    if (n > (int)sizeof g_pending) n = sizeof g_pending;
    memcpy(g_pending + g_pending_len, buf, n);
    g_pending_len += n;
    if (screen && g_screen && g_screen_ready) pspDebugScreenPrintf("%s", buf);
}

static void vemit(int screen, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    emit(screen, buf, n);
}

void out(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vemit(0, fmt, ap); va_end(ap);
}

void say(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vemit(1, fmt, ap); va_end(ap);
}

void step(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    g_step++;
    if (g_screen && g_screen_ready) pspDebugScreenPrintf("%3d %s\n", g_step, buf);
    out("[%d] %s\n", g_step, buf);
    probe_flush();
}

void section(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    say("\n---- %s ----\n", buf);
    probe_flush();
}

void ret(int r) { out("  = %08X\n", (unsigned)r); }

void dump_words(const char *label, const void *p, int nwords) {
    const volatile unsigned int *w = p;
    out("%s\n", label);
    for (int i = 0; i < nwords; i += 4) {
        out("   ");
        for (int j = i; j < nwords && j < i + 4; j++)
            out("  +%02X %08X", j * 4, w[j]);
        out("\n");
    }
}

static void path_of(const char *name, char *outp, int cap) {
    snprintf(outp, cap, "%s%s", g_dir, name);
}

int probe_write_file(const char *name, const void *data, int len) {
    char p[300];
    path_of(name, p, sizeof p);
    probe_flush();
    SceUID fd = sceIoOpen(p, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return fd;
    int n = sceIoWrite(fd, data, len);
    sceIoClose(fd);
    return n;
}

int probe_read_file(const char *name, void *buf, int cap) {
    char p[300];
    path_of(name, p, sizeof p);
    SceUID fd = sceIoOpen(p, PSP_O_RDONLY, 0);
    if (fd < 0) return fd;
    int n = sceIoRead(fd, buf, cap);
    sceIoClose(fd);
    return n;
}

/* ---- exit callback -------------------------------------------------------- */

static int exit_cb(int a, int b, void *c) {
    (void)a; (void)b; (void)c;
    probe_flush();
    sceKernelExitGame();
    return 0;
}

static int cb_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int id = sceKernelCreateCallback("exit", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}

void probe_init(const char *name, int version, int argc, char **argv) {
    snprintf(g_name, sizeof g_name, "%s", name);

    /* The EBOOT's own directory when the firmware says where it is; otherwise
     * the conventional one, made if missing (psprecomp starts with an empty
     * memory stick). */
    g_dir[0] = '\0';
    if (argc > 0 && argv && argv[0] && strchr(argv[0], '/')) {
        snprintf(g_dir, sizeof g_dir, "%s", argv[0]);
        char *slash = strrchr(g_dir, '/');
        slash[1] = '\0';
    } else {
        sceIoMkdir("ms0:/PSP", 0777);
        sceIoMkdir("ms0:/PSP/GAME", 0777);
        snprintf(g_dir, sizeof g_dir, "ms0:/PSP/GAME/%s/", name);
        sceIoMkdir(g_dir, 0777);
    }
    snprintf(g_logpath, sizeof g_logpath, "%s%s.txt", g_dir, name);

    SceUID th = sceKernelCreateThread("exit_cb", cb_thread, 0x11, 0x1000, 0, NULL);
    if (th >= 0) sceKernelStartThread(th, 0, NULL);

    pspDebugScreenInit();
    g_screen_ready = 1;

    say("\n==== %s %d, firmware (sceKernelDevkitVersion) %08X ====\n",
        g_name, version, (unsigned)sceKernelDevkitVersion());
    say("log: %s\n", g_logpath);
    probe_flush();
}

void probe_done(void) {
    say("\n==== %s done ====\n", g_name);
    probe_flush();
    if (g_screen_ready && g_screen)
        pspDebugScreenPrintf("\nThe log is saved. Back to the XMB in 5 seconds.\n");
    sceKernelDelayThread(5 * 1000 * 1000);
    sceKernelExitGame();
    for (;;) sceKernelSleepThread();
}
