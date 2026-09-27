/* probe.h -- shared plumbing for the hardware probes in tools/hwprobe.
 *
 * Written against PSPSDK (BSD) only. Every probe here is a plain homebrew
 * EBOOT that runs its tests without asking for input and writes what it sees
 * to <name>.txt beside the EBOOT. The same PRX also runs under psprecomp
 * (`allegrexrecomp interp <name>.prx --dispatch`), where the log goes to the
 * host's stderr and to ./ms/PSP/GAME/<name>/, so the two logs can be diffed
 * line by line. That only works if a log line holds what the firmware
 * *decided* -- return codes, out-parameters, orderings -- and never what
 * differs between two correct runs: addresses, UIDs, host timing. Log a UID
 * as whether it is valid, a pointer as an offset from something the probe
 * owns, a duration only when the duration is the question.
 *
 * Lines collect in memory and reach the file on probe_flush(), step(),
 * section(), or when the buffer fills. Each write opens, appends and closes,
 * so what was flushed survives the PSP switching itself off, and the last
 * line names the step that did it. */
#ifndef HWPROBE_PROBE_H
#define HWPROBE_PROBE_H

#include <stdarg.h>

/* Start a run: find the EBOOT's directory, open the log, write the header
 * (probe name, version, firmware). `argc`/`argv` are main()'s. */
void probe_init(const char *name, int version, int argc, char **argv);

/* The probe's directory, "ms0:/PSP/GAME/<name>/" or wherever it was run from,
 * with a trailing slash. */
const char *probe_dir(void);

/* To the log (and stdout). */
void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* To the log and the screen. */
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Name the call about to be made and put the log on the memory stick first. */
void step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* "---- name ----", flushed. */
void section(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void probe_flush(void);

/* Screen output can be turned off while a probe owns the framebuffer. */
void probe_screen(int on);

/* Log `nwords` words as "+off value" rows, four per line. */
void dump_words(const char *label, const void *p, int nwords);

/* Log an sce return: "= 00000000" or "= 80020190". */
void ret(int r);

/* Write a whole file beside the EBOOT. Returns bytes written or <0. */
int probe_write_file(const char *name, const void *data, int len);
/* Read a file from beside the EBOOT into buf. Returns bytes read or <0. */
int probe_read_file(const char *name, void *buf, int cap);

/* Finish: flush, say so, wait a few seconds so the screen can be read, and
 * return to the XMB. */
void probe_done(void) __attribute__((noreturn));

#endif
