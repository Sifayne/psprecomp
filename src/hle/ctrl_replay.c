/* psprecomp — scripted pad input: a deterministic player, and a recorder.
 *
 * PSPRECOMP_PAD holds buttons for a whole run and PSPRECOMP_PAD_PRESS presses
 * one button once. Neither can express "skip the intro, then pick New Game",
 * which is the shape of every question worth asking past the title screen: a
 * bug three menus in is only reproducible if the three menus are.
 *
 * Two halves that must agree:
 *
 *   the player    reads a scenario file and drives the script lane
 *   the recorder  writes one out from what the guest actually saw
 *
 * Both live at the guest's pad read (src/hle/misc.c, hle_ReadBufferPositive),
 * and that is the whole design. The recorder's timestamp *is* the player's
 * cursor -- same counter, same thread, same function -- so a recording cannot
 * describe a session the player is unable to reproduce. Recording in the SDL
 * layer instead would timestamp against a different clock on a different
 * thread, and would faithfully record presses that fell between two polls,
 * which the guest never saw and the player cannot deliver.
 *
 * It also means the recorder is input-source agnostic: SDL keys, a gamepad,
 * PSPRECOMP_PAD, a press, or another scenario all record identically, because
 * they have already been merged by the time it looks.
 *
 * THE TIMEBASE. Events are keyed on the pad-poll count (@N) or on guest
 * microseconds (T.Ts). @N is the portable one. PSPRECOMP_WINDOW forces the
 * clock to wall time (host/present.c calls psp_clock_realtime), so seconds
 * mean different things in a window and headless, while a poll count is a
 * count of things the guest did either way. The recorder emits @N and puts
 * the seconds in a comment.
 */

#include "psprecomp/hle.h"
#include "psprecomp/sched.h"
#include "psprecomp/clock.h"
#include "psprecomp/state.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    EV_DOWN,        /* buttons |= mask                                     */
    EV_UP,          /* buttons &= ~mask                                    */
    EV_STATE,       /* buttons = mask, and adopt ax/ay -- the recorder's   */
    EV_ANALOG,      /* claim the stick at ax/ay                            */
    EV_NEUTRAL,     /* release everything, hand the stick back             */
    EV_MARK,        /* narrate                                             */
    EV_WAIT,        /* advance the cursor, change nothing                  */
    EV_STOP,        /* end the run at a defined poll                       */
    EV_MOUSE,       /* deliver one poll's worth of mouse travel            */
};

enum { WHEN_POLL, WHEN_US };

/* When an event fires: a poll count or guest microseconds, counted from the
 * start of the run or from where the previous event fired. */
typedef struct {
    int      unit;          /* WHEN_POLL | WHEN_US                         */
    int      rel;           /* relative to where the previous event fired  */
    uint64_t when;
} stamp;

/* The pad as the script lane holds it, or as the recorder last wrote it: the
 * buttons, the stick, the look channel's second stick, and one poll's mouse
 * travel. Neutral is nothing held, both sticks centred, the mouse still. */
typedef struct {
    uint32_t buttons;
    uint8_t  ax, ay, rx, ry;
    int      mdx, mdy;
} pad_state;
#define STICK_CENTRE 128
#define PAD_NEUTRAL { 0, STICK_CENTRE, STICK_CENTRE, STICK_CENTRE, STICK_CENTRE, 0, 0 }

static int look_neutral(const pad_state *p) {
    return p->rx == STICK_CENTRE && p->ry == STICK_CENTRE && !p->mdx && !p->mdy;
}
static int pad_same(const pad_state *a, const pad_state *b) {
    return a->buttons == b->buttons && a->ax == b->ax && a->ay == b->ay &&
           a->rx == b->rx && a->ry == b->ry && a->mdx == b->mdx && a->mdy == b->mdy;
}

typedef struct {
    int      kind;
    stamp    at;
    uint32_t mask;
    uint8_t  ax, ay;
    uint8_t  rx, ry;        /* the look channel: the second stick...       */
    int      mdx, mdy;      /* ...and one poll's mouse travel              */
    int      look;          /* the line gave look values: claim the channel */
    char    *text;          /* EV_MARK only; owned                         */
    int      line;          /* for diagnostics                             */
} ev;

/* The event list grows as a scenario needs. It was a fixed 4096, and a
 * recorded race of 4402 events lost its last 306 -- and its end -- without a
 * word, so the replay left the recorded line near the finish. */
static ev       *g_ev;
static int       g_ev_n, g_ev_cap, g_ev_lost;
static int       g_cursor;
static int       g_minhold = 2;
static int       g_drain;          /* 0 = the scenario did not say         */
static int       g_loaded;
static int       g_finished;
static int       g_tainted;

/* Where the previous event fired. Relative stamps are resolved against this
 * at run time, not at parse time: a `+1` after a `2.0s` has to mean "one poll
 * after that actually happened", and only the run knows when that was. */
static uint32_t  g_prev_poll;
static uint64_t  g_prev_us;

/* The script lane's current value, owned here and published to misc.c, and
 * whether it holds the stick and the look channel; the mouse delta is spent
 * once. */
static pad_state g_lane = PAD_NEUTRAL;
static int       g_analog_owned;
static int       g_look_owned;

/* ---- the recorder -------------------------------------------------------- */

static FILE     *g_rec;
static int       g_rec_first = 1;
static pad_state g_rec_pad = PAD_NEUTRAL;
static uint32_t  g_rec_events;

/* ---- parsing ------------------------------------------------------------- */

static void warn(int line, const char *what, const char *detail) {
    fprintf(stderr, "psprecomp: replay: line %d: %s%s%s\n",
            line, what, detail ? ": " : "", detail ? detail : "");
}

/* A comma- or space-separated button list. Returns 0 and warns on an unknown
 * name; `none` and `-` are the empty set, which is a legal thing to write. */
static uint32_t parse_buttons(const char *s, int line, int *ok) {
    uint32_t mask = 0;
    *ok = 1;
    for (const char *p = s; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t n = 0;
        while (p[n] && p[n] != ',' && p[n] != ' ' && p[n] != '\t') n++;

        if ((n == 4 && !strncasecmp(p, "none", 4)) || (n == 1 && *p == '-')) {
            p += n; continue;
        }
        uint32_t bit = psp_pad_bit(p, n);
        if (!bit) {
            char buf[64];
            snprintf(buf, sizeof buf, "%.*s", (int)(n < 60 ? n : 60), p);
            warn(line, "unknown button", buf);
            *ok = 0;
        }
        mask |= bit;
        p += n;
    }
    return mask;
}

/* `@N` `+N` `T.Ts` `+T.Ts`. Returns 0 if `tok` is not a timestamp at all,
 * which is how an unstamped line is recognised -- it defaults to `+1`. */
static int parse_when(const char *tok, stamp *out) {
    const char *p = tok;
    out->rel = 0;
    if (*p == '@') { p++; }
    else if (*p == '+') { out->rel = 1; p++; }
    else if (!(*p >= '0' && *p <= '9')) return 0;

    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return 0;

    if (*end == 's' || *end == 'S') {
        out->unit = WHEN_US;
        out->when = (uint64_t)(v * 1e6);
        return 1;
    }
    if (*end != '\0') return 0;
    /* A bare number after `@` or `+`, or a bare leading digit, is a poll
     * index. `@` and `+` are what make an unstamped `down cross` legible as
     * unstamped rather than as poll zero. */
    if (tok[0] != '@' && tok[0] != '+') return 0;
    out->unit = WHEN_POLL;
    out->when = (uint64_t)(v < 0 ? 0 : v);
    return 1;
}

static ev *push(int kind, stamp at, int line) {
    if (g_ev_n == g_ev_cap) {
        const int cap = g_ev_cap ? g_ev_cap * 2 : 4096;
        ev *grown = realloc(g_ev, (size_t)cap * sizeof *g_ev);
        if (!grown) { g_ev_lost++; return NULL; }
        g_ev = grown; g_ev_cap = cap;
    }
    ev *e = &g_ev[g_ev_n++];
    memset(e, 0, sizeof *e);
    e->kind = kind; e->at = at; e->line = line;
    return e;
}

/* Strip a trailing comment and trailing whitespace, in place. A `#` inside a
 * mark's quoted text is left alone -- a mark is the one place a scenario
 * carries prose, and prose contains hashes. */
static void strip_comment(char *s) {
    int in_quote = 0;
    for (char *p = s; *p; p++) {
        if (*p == '"') in_quote = !in_quote;
        else if (*p == '#' && !in_quote) { *p = 0; break; }
    }
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = 0;
}

/* The optional look fields after a stick pair: `rx ry [mdx mdy]`, as the
 * recorder writes them. Returns 1 if the stick pair was there, which is what
 * claims the channel for the script; a line without it leaves the channel as
 * it was. */
static int parse_look(char *q, long *rx, long *ry, long *mdx, long *mdy) {
    char *end = q;
    const long v = strtol(q, &end, 10);
    if (end == q) return 0;
    *rx = v;
    *ry = strtol(end, &end, 10);
    char *end2 = end;
    const long d = strtol(end, &end2, 10);
    if (end2 != end) {
        *mdx = d;
        *mdy = strtol(end2, NULL, 10);
    }
    return 1;
}

/* A stick line's numbers, `ax ay [rx ry [mdx mdy]]`, and whether the look
 * pair was among them. What a line leaves out is centred and still. */
typedef struct { long ax, ay, rx, ry, mdx, mdy; int look; } stick_line;
#define STICKS_CENTRED(look) { STICK_CENTRE, STICK_CENTRE, STICK_CENTRE, STICK_CENTRE, 0, 0, look }

static stick_line parse_sticks(char *q) {
    stick_line s = STICKS_CENTRED(0);
    s.ax = strtol(q, &q, 10);
    s.ay = strtol(q, &q, 10);
    s.look = parse_look(q, &s.rx, &s.ry, &s.mdx, &s.mdy);
    return s;
}

static void set_sticks(ev *e, const stick_line *s) {
    e->ax = (uint8_t)s->ax; e->ay = (uint8_t)s->ay;
    e->rx = (uint8_t)s->rx; e->ry = (uint8_t)s->ry;
    e->mdx = (int)s->mdx; e->mdy = (int)s->mdy; e->look = s->look;
}

/* The line's stamp, when its first word is one, and what follows it; else
 * `+1`, the next poll, and the whole line. */
static char *parse_stamp(char *s, stamp *at) {
    size_t n = 0;
    while (s[n] && s[n] != ' ' && s[n] != '\t') n++;
    const char saved = s[n];
    s[n] = 0;
    const int stamped = parse_when(s, at);
    s[n] = saved;
    if (stamped) return s + n;
    at->unit = WHEN_POLL; at->rel = 1; at->when = 1;
    return s;
}

/* End the first word of `s` where it ends; what follows it, past blanks. */
static char *split_word(char *s) {
    size_t n = 0;
    while (s[n] && s[n] != ' ' && s[n] != '\t') n++;
    char *rest = s + n;
    if (*rest) *rest++ = 0;
    while (*rest == ' ' || *rest == '\t') rest++;
    return rest;
}

/* One directive and its argument, stamped `at`. Returns how many errors it
 * found: 0 or 1. */
static int parse_directive(const char *dir, char *arg, stamp at, int line, int *seen_timed) {
    if (!strcasecmp(dir, "drain")) {
        if (*seen_timed) warn(line, "drain must precede the first timed line", NULL);
        g_drain = atoi(arg);
        return *seen_timed;
    }
    if (!strcasecmp(dir, "minhold")) {
        if (*seen_timed) warn(line, "minhold must precede the first timed line", NULL);
        g_minhold = atoi(arg);
        if (g_minhold < 1) g_minhold = 1;
        return *seen_timed;
    }

    /* Facts of the recorded session (psp_ctrl_replay_note): adopted at
     * parse, wherever they stand, since the recorder writes each the
     * first time the guest asks for it. */
    if (!strcasecmp(dir, "date")) {
        psp_rtc_set_date_us(strtoull(arg, NULL, 10));
        return 0;
    }
    if (!strcasecmp(dir, "zone")) {
        char *end;
        const long minutes = strtol(arg, &end, 10);
        psp_sysparam_set_zone((int)minutes, (int)strtol(end, NULL, 10));
        return 0;
    }

    *seen_timed = 1;

    if (!strcasecmp(dir, "down") || !strcasecmp(dir, "up")) {
        int ok; uint32_t m = parse_buttons(arg, line, &ok);
        if (!ok) return 1;
        ev *e = push(!strcasecmp(dir, "down") ? EV_DOWN : EV_UP, at, line);
        if (e) e->mask = m;

    } else if (!strcasecmp(dir, "tap")) {
        /* Expands here rather than at run time, so the player has one
         * kind of thing to do and the hold length is fixed at parse. */
        char *sp = strpbrk(arg, " \t");
        long hold = g_minhold;
        if (sp) { *sp = 0; hold = strtol(sp + 1, NULL, 10); if (hold < 1) hold = g_minhold; }
        int ok; uint32_t m = parse_buttons(arg, line, &ok);
        if (!ok) return 1;
        ev *e = push(EV_DOWN, at, line);
        if (e) e->mask = m;
        e = push(EV_UP, (stamp){ WHEN_POLL, 1, (uint64_t)hold }, line);
        if (e) e->mask = m;

    } else if (!strcasecmp(dir, "state")) {
        /* What the recorder writes: the whole lane in one event, so a
         * poll where two things changed replays as one poll. */
        char *p2 = strpbrk(arg, " \t");
        stick_line s = STICKS_CENTRED(0);
        if (p2) { *p2 = 0; s = parse_sticks(p2 + 1); }
        int ok; uint32_t m = parse_buttons(arg, line, &ok);
        if (!ok) return 1;
        ev *e = push(EV_STATE, at, line);
        if (e) { e->mask = m; set_sticks(e, &s); }

    } else if (!strcasecmp(dir, "analog")) {
        /* `analog x y` is the stick; `analog x y rx ry` both sticks;
         * `center` centres whatever the script holds. */
        stick_line s = STICKS_CENTRED(1);
        if (strncasecmp(arg, "cent", 4)) s = parse_sticks(arg);
        ev *e = push(EV_ANALOG, at, line);
        if (e) set_sticks(e, &s);

    } else if (!strcasecmp(dir, "mouse")) {
        /* One poll's worth of mouse travel. A drag is a run of these,
         * one per poll, which is what the recorder writes for one. */
        char *q = arg;
        const long dx = strtol(q, &q, 10);
        const long dy = strtol(q, NULL, 10);
        ev *e = push(EV_MOUSE, at, line);
        if (e) { e->mdx = (int)dx; e->mdy = (int)dy; e->look = 1; }

    } else if (!strcasecmp(dir, "neutral")) {
        push(EV_NEUTRAL, at, line);

    } else if (!strcasecmp(dir, "wait")) {
        /* `wait 3s` and `wait 40` are stamps in directive position: they
         * move the cursor so the next relative stamp counts from here. */
        stamp w;
        char tmp[64];
        snprintf(tmp, sizeof tmp, "+%s", arg);
        if (!parse_when(tmp, &w)) { warn(line, "wait wants <n> or <n>s", arg); return 1; }
        push(EV_WAIT, w, line);

    } else if (!strcasecmp(dir, "mark")) {
        ev *e = push(EV_MARK, at, line);
        if (e) {
            char *t = arg;
            if (*t == '"') { t++; char *q = strrchr(t, '"'); if (q) *q = 0; }
            e->text = strdup(t);
        }

    } else if (!strcasecmp(dir, "stop")) {
        push(EV_STOP, at, line);

    } else {
        warn(line, "unknown directive", dir);
        return 1;
    }
    return 0;
}

static int parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "psprecomp: replay: cannot open %s\n", path);
        return -1;
    }
    g_ev = NULL; g_ev_n = g_ev_cap = g_ev_lost = 0;

    char raw[512];
    int  line = 0, errors = 0, seen_timed = 0;

    while (fgets(raw, sizeof raw, f)) {
        line++;
        strip_comment(raw);
        char *s = raw;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) continue;

        stamp at;
        char *dir = parse_stamp(s, &at);
        while (*dir == ' ' || *dir == '\t') dir++;
        if (!*dir) { warn(line, "timestamp with no directive", NULL); errors++; continue; }
        char *arg = split_word(dir);
        errors += parse_directive(dir, arg, at, line, &seen_timed);
    }
    fclose(f);

    if (g_ev_lost) {
        fprintf(stderr, "psprecomp: replay: %d event(s) of %s could not be stored\n", g_ev_lost, path);
        errors++;
    }
    if (errors) {
        fprintf(stderr, "psprecomp: replay: %d error(s) in %s -- not loading it\n",
                errors, path);
        return -1;
    }
    return 0;
}

/* ---- the player ---------------------------------------------------------- */

static void tell_lane(void) {
    psp_ctrl_script_set(g_lane.buttons, g_analog_owned, g_lane.ax, g_lane.ay);
    psp_ctrl_script_set_look(g_look_owned, g_lane.rx, g_lane.ry, g_lane.mdx, g_lane.mdy);
}

static void publish(void) {
    tell_lane();
    g_lane.mdx = g_lane.mdy = 0;    /* a delta is delivered once */
}

/* A save state's (psprecomp/state.h): where the scenario is and what it
 * holds down. A run that loads one with the same scenario carries on from
 * there; one with none starts with the script's lanes let go, so a state
 * saved from a scenario never holds a button in someone's hands. */
static struct {
    int32_t  cursor, finished;
    uint32_t prev_poll, buttons;
    uint64_t prev_us;
    int32_t  analog_owned, look_owned, mdx, mdy;
    uint8_t  ax, ay, rx, ry;
} g_saved;

static int replay_save(psp_state_writer *w) {
    g_saved.cursor = g_cursor; g_saved.finished = g_finished;
    g_saved.prev_poll = g_prev_poll; g_saved.prev_us = g_prev_us;
    g_saved.buttons = g_lane.buttons; g_saved.analog_owned = g_analog_owned;
    g_saved.ax = g_lane.ax; g_saved.ay = g_lane.ay;
    g_saved.look_owned = g_look_owned; g_saved.rx = g_lane.rx; g_saved.ry = g_lane.ry;
    g_saved.mdx = g_lane.mdx; g_saved.mdy = g_lane.mdy;
    return psp_state_put(w, "replay", &g_saved, sizeof g_saved);
}

static int replay_load(psp_state_reader *r, char *why, size_t size) {
    (void)why; (void)size;
    size_t n;
    const void *in = psp_state_get(r, "replay", &n);
    if (g_loaded && in && n == sizeof g_saved) {
        memcpy(&g_saved, in, sizeof g_saved);
        g_cursor = g_saved.cursor < g_ev_n ? g_saved.cursor : g_ev_n;
        g_finished = g_saved.finished;
        g_prev_poll = g_saved.prev_poll; g_prev_us = g_saved.prev_us;
        g_lane.buttons = g_saved.buttons; g_analog_owned = g_saved.analog_owned;
        g_lane.ax = g_saved.ax; g_lane.ay = g_saved.ay;
        g_look_owned = g_saved.look_owned; g_lane.rx = g_saved.rx; g_lane.ry = g_saved.ry;
        g_lane.mdx = g_saved.mdx; g_lane.mdy = g_saved.mdy;
    }
    tell_lane();
    return 0;
}

void psp_ctrl_replay_keep(void) {
    static const psp_state_part part = { .name = "replay", .save = replay_save, .load = replay_load };
    psp_state_register(&part);
}

void psp_ctrl_replay_reset(void) {
    for (int i = 0; i < g_ev_n; i++) free(g_ev[i].text);
    free(g_ev);
    g_ev = NULL; g_ev_n = g_ev_cap = 0; g_cursor = 0;
    g_minhold = 2; g_drain = 0;
    g_loaded = g_finished = g_tainted = 0;
    g_prev_poll = 0; g_prev_us = 0;
    g_lane = (pad_state)PAD_NEUTRAL; g_analog_owned = g_look_owned = 0;
    if (g_rec) { fclose(g_rec); g_rec = NULL; }
    g_rec_first = 1; g_rec_pad = (pad_state)PAD_NEUTRAL;
    g_rec_events = 0;
}

void psp_ctrl_replay_init(void) {
    const char *path = getenv("PSPRECOMP_REPLAY");
    const char *rec  = getenv("PSPRECOMP_REPLAY_REC");

    if (path && *path) {
        if (parse_file(path) == 0) {
            g_loaded = 1;
            fprintf(stderr, "psprecomp: replay: %s -- %d event(s), minhold %d, "
                            "clock %s\n",
                    path, g_ev_n, g_minhold,
                    psp_clock_is_realtime() ? "wall (windowed: only @poll stamps reproduce)"
                                            : "virtual (deterministic)");
        }
    }
    if (rec && *rec) {
        g_rec = fopen(rec, "w");
        if (!g_rec) {
            fprintf(stderr, "psprecomp: replay: cannot write %s\n", rec);
        } else {
            fprintf(g_rec,
                "# recorded by psprecomp\n"
                "# clock: %s\n"
                "# @poll stamps reproduce in either clock regime; seconds do not.\n",
                psp_clock_paced_requested() ? "paced (replays exactly in a paced window)"
                : psp_clock_is_realtime() ? "wall (windowed)" : "virtual (headless)");
            fflush(g_rec);
            fprintf(stderr, "psprecomp: replay: recording to %s\n", rec);
        }
    }
}

void psp_ctrl_replay_note(const char *line) {
    if (!g_rec || !line) return;
    fprintf(g_rec, "%s\n", line);
    fflush(g_rec);
}

int psp_ctrl_replay_active(void) { return g_loaded; }
int psp_ctrl_replay_drain(void)  { return g_loaded ? g_drain : 0; }

/* Is event `e` due at (polls, us)? Relative stamps count from where the
 * previous event fired, which is why `g_prev_*` is updated on every fire. */
static int due(const ev *e, uint32_t polls, uint64_t us) {
    if (e->at.unit == WHEN_POLL) {
        uint64_t at = e->at.rel ? (uint64_t)g_prev_poll + e->at.when : e->at.when;
        return (uint64_t)polls >= at;
    }
    uint64_t at = e->at.rel ? g_prev_us + e->at.when : e->at.when;
    return us >= at;
}

/* A state or analog line's stick, and its look channel if it gave one. */
static void take_sticks(const ev *e) {
    g_lane.ax = e->ax; g_lane.ay = e->ay; g_analog_owned = 1;
    if (e->look) {
        g_lane.rx = e->rx; g_lane.ry = e->ry; g_lane.mdx = e->mdx; g_lane.mdy = e->mdy;
        g_look_owned = 1;
    }
}

void psp_ctrl_replay_step(uint32_t polls, uint64_t us) {
    if (!g_loaded || g_finished) return;

    /* Consume everything due, but stop after one event that changes what the
     * guest can see.
     *
     * This is the rule that makes a tap a tap. A scenario's `tap cross` is a
     * down and an up, and when the guest stops polling for a while -- which
     * it does for the whole of the intro movie -- both come due at the same
     * poll. Applying both would hand the guest an unchanged pad: it would
     * never observe the button down, and a press it could not observe is a
     * press that did not happen. So the second edge waits for the next poll.
     *
     * The cost is that a scenario's absolute timeline stretches wherever the
     * guest polls slowly. That is the right way round: the alternative is a
     * timeline that is honoured exactly and delivers nothing. */
    while (g_cursor < g_ev_n && due(&g_ev[g_cursor], polls, us)) {
        const ev *e = &g_ev[g_cursor++];
        g_prev_poll = polls;
        g_prev_us   = us;
        int edge = 0;

        switch (e->kind) {
        case EV_DOWN:    g_lane.buttons |= e->mask;  edge = 1; break;
        case EV_UP:      g_lane.buttons &= ~e->mask; edge = 1; break;
        case EV_STATE:
            g_lane.buttons = e->mask;
            take_sticks(e);
            edge = 1;
            break;
        case EV_ANALOG:
            take_sticks(e);
            edge = 1;
            break;
        case EV_MOUSE:
            g_lane.mdx = e->mdx; g_lane.mdy = e->mdy; g_look_owned = 1;
            edge = 1;
            break;
        case EV_NEUTRAL:
            g_lane = (pad_state)PAD_NEUTRAL;
            g_analog_owned = g_look_owned = 0;
            edge = 1;
            break;
        case EV_MARK:
            fprintf(stderr, "psprecomp: replay: %s (poll %u, t=%.3fs)\n",
                    e->text ? e->text : "", polls, (double)us / 1e6);
            break;
        case EV_WAIT:
            break;
        case EV_STOP:
            fprintf(stderr, "psprecomp: replay: stop at poll %u, t=%.3fs\n",
                    polls, (double)us / 1e6);
            g_finished = 1;
            publish();
            psp_sched_stop_all("replay finished");
            return;
        }

        if (edge) { publish(); break; }
    }

    if (g_cursor >= g_ev_n && !g_finished) {
        g_finished = 1;
        fprintf(stderr, "psprecomp: replay: scenario exhausted at poll %u, t=%.3fs "
                        "(no `stop` -- the run continues to the drain)\n",
                polls, (double)us / 1e6);
    }
}

/* The host lane was non-neutral while a scenario was driving. Say so once:
 * the run is still useful, but it is no longer the scenario's run. */
void psp_ctrl_replay_taint(uint32_t polls) {
    if (!g_loaded || g_tainted) return;
    g_tainted = 1;
    fprintf(stderr, "psprecomp: replay: live input at poll %u -- this run is no "
                    "longer reproducible from the scenario alone\n", polls);
}

/* ---- the recorder -------------------------------------------------------- */

static void write_buttons(FILE *f, uint32_t m) {
    static const struct { const char *name; uint32_t bit; } B[] = {
        { "select", 0x000001 }, { "start",    0x000008 },
        { "up",     0x000010 }, { "right",    0x000020 },
        { "down",   0x000040 }, { "left",     0x000080 },
        { "l",      0x000100 }, { "r",        0x000200 },
        { "triangle", 0x001000 }, { "circle", 0x002000 },
        { "cross",  0x004000 }, { "square",   0x008000 },
    };
    if (!m) { fputs("none", f); return; }
    int first = 1;
    for (size_t i = 0; i < sizeof B / sizeof B[0]; i++) {
        if (!(m & B[i].bit)) continue;
        fprintf(f, "%s%s", first ? "" : ",", B[i].name);
        first = 0;
        m &= ~B[i].bit;
    }
    /* Anything the table does not name survives as a bit, so a round trip
     * cannot quietly drop it. */
    if (m) fprintf(f, "%s0x%06X", first ? "" : ",", m);
}

void psp_ctrl_replay_record(uint32_t polls, uint64_t us,
                            uint32_t buttons, uint8_t ax, uint8_t ay,
                            uint8_t rx, uint8_t ry, int mdx, int mdy) {
    if (!g_rec) return;
    const pad_state now = { buttons, ax, ay, rx, ry, mdx, mdy };
    if (!g_rec_first && pad_same(&now, &g_rec_pad)) return;

    const int look_still = look_neutral(&now);
    const int was_look = !g_rec_first && !look_neutral(&g_rec_pad);

    /* The first poll establishes the baseline; only write it if it is not
     * already the neutral the player starts from. */
    const int first = g_rec_first;
    g_rec_first = 0;
    g_rec_pad = now;
    if (first && !buttons && ax == STICK_CENTRE && ay == STICK_CENTRE && look_still) return;

    fprintf(g_rec, "@%-6u state ", polls);
    write_buttons(g_rec, buttons);
    fprintf(g_rec, " %u %u", ax, ay);
    /* The look channel is written while it is non-neutral and once more when
     * it returns to neutral, so a replay releases it where the player did. A
     * line without it leaves the channel as it was. The mouse delta, when
     * there is one, follows the stick pair. */
    if (!look_still || was_look) {
        fprintf(g_rec, " %u %u", rx, ry);
        if (mdx || mdy) fprintf(g_rec, " %d %d", mdx, mdy);
    }
    fprintf(g_rec, "   # t=%.3fs\n", (double)us / 1e6);
    /* Flushed per line, not per run: these are a handful of lines even in a
     * long session, and a run that crashes is exactly the run whose input you
     * wanted written down. */
    fflush(g_rec);
    g_rec_events++;
}

void psp_ctrl_replay_finish(FILE *summary) {
    if (g_rec) {
        fprintf(g_rec, "# %u poll(s), %u event(s)\n", psp_ctrl_polls(), g_rec_events);
        fclose(g_rec);
        g_rec = NULL;
    }
    if (!summary) return;
    if (g_loaded)
        fprintf(summary, "replay:   %d/%d event(s) delivered over %u poll(s)%s\n",
                g_cursor, g_ev_n, psp_ctrl_polls(),
                g_tainted ? "  [TAINTED by live input]" : "");
    else if (g_rec_events)
        fprintf(summary, "replay:   recorded %u event(s) over %u poll(s)\n",
                g_rec_events, psp_ctrl_polls());
}
