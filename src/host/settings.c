/* Player settings: the mechanism behind every pack's options, and the
 * player's own. See include/psprecomp/host/settings.h. Generalised from Last
 * Raven's host/settings.c, which The 3rd Birthday had copied and extended;
 * what the two did differently is now the schema's to say, and what they did
 * the same is the player's table below. */
#include <psprecomp/host/settings.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C(k,lbl,pg,hlp,d_,ch_,lb_,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=PSP_OPTION_CHOICE,.dflt=d_,.choices=ch_,.labels=lb_, __VA_ARGS__}
#define N(k,lbl,pg,hlp,type_,d_,lo,hi,st,sp,...) {.key=#k,.env="PSPRECOMP_" #k,.label=lbl,.page=pg,.help=hlp, \
    .type=type_,.dflt=d_,.min=lo,.max=hi,.step=st,.special=sp, __VA_ARGS__}
const psp_option_def psp_player_options[PSP_PLAYER_OPTIONS] = {
    [PSP_OPT_RESOLUTION] = C(RESOLUTION,"Rendering resolution","Display",
        "Original keeps the PSP's 480x272 picture. Match window renders at the window's physical pixel size and needs OpenGL.",
        "psp","psp|window","Original (480x272)|Match window",.flags=PSP_OPTION_NEEDS_GL),
    [PSP_OPT_WINDOW_MODE] = C(WINDOW_MODE,"Window mode","Display",
        "Windowed fullscreen fills the display without borders at the desktop resolution. Rendering resolution and aspect ratio stay separate settings.",
        "windowed","windowed|borderless","Windowed|Windowed fullscreen",.flags=PSP_OPTION_LIVE),
    [PSP_OPT_WINDOW_SIZE] = N(WINDOW_SIZE,"Window size","Display",
        "Starting size in Windowed mode, as WIDTHxHEIGHT in logical pixels. Windowed fullscreen uses the desktop size instead.",
        PSP_OPTION_SIZE,"960x544",1,16384,0,NULL,
        .stops="960x544|1280x720|1600x900|1920x1080|2560x1440|3840x2160"),
    [PSP_OPT_DISPLAY] = N(DISPLAY,"Start on display","Display",
        "The screen the game opens on, in either window mode. If it is unavailable, the primary display is used. Screen numbers follow the current display order.",
        PSP_OPTION_INTEGER,"primary",1,65535,1,"primary",.special_label="Primary display",.format="Display %.0f"),
    [PSP_OPT_VOLUME] = N(VOLUME,"Volume","Audio","The game's overall loudness. The in-game menu also mutes it.",
        PSP_OPTION_NUMBER,"100",0,100,5,NULL,.format="%.0f%%",.flags=PSP_OPTION_LIVE),
    [PSP_OPT_ACTIVE_PAD] = C(ACTIVE_PAD,"Active controller","Controller",
        "Which controller plays when several are connected. First connected keeps the first one until it is unplugged; Last used hands control to whichever was pressed last.",
        "first","first|last","First connected|Last used",.flags=PSP_OPTION_LIVE),
    [PSP_OPT_STATE_LOAD] = C(STATE_LOAD,"Loading a state","Save states",
        "What loading a save state does to the game in progress. Ask first confirms every load, from the menu or the key. Load at once does not ask. Keep an undo loads at once, saving the game as it was first: \"Before the last load\" in the Load state list brings it back.",
        "ask","ask|now|undo","Ask first|Load at once|Load at once, keep an undo",.flags=PSP_OPTION_LIVE),
    [PSP_OPT_STATE_START] = C(STATE_START,"When the game starts","Save states",
        "Start fresh begins at the game's own start. Continue where I quit saves the game as it is when you quit, and loads it the next time the game starts.",
        "fresh","fresh|continue","Start fresh|Continue where I quit",.flags=PSP_OPTION_LIVE),
    [PSP_OPT_LANGUAGE] = C(LANGUAGE,"Language","System",
        "The language the game is told the PSP is set to. Automatic follows this computer's language, or English when the PSP has none like it. A game can only show the languages it was made with.",
        "auto","auto|ja|en|fr|es|de|it|nl|pt|ru|ko|zh-hant|zh-hans",
        "Automatic|Japanese|English|French|Spanish|German|Italian|Dutch|Portuguese|Russian|Korean|Chinese (Traditional)|Chinese (Simplified)"),
    [PSP_OPT_CONFIRM] = C(CONFIRM,"Confirm button","System",
        "Which button the game is told confirms, as a PSP's own setting does. Automatic is Circle when the language is Japanese and Cross otherwise. Some games keep their own region's button whatever this says.",
        "auto","auto|cross|circle","Automatic|Cross|Circle"),
    [PSP_OPT_NICKNAME] = {.key="NICKNAME",.env="PSPRECOMP_NICKNAME",.label="Nickname",.page="System",
        .help="The name games read as the PSP's owner, shown in multiplayer and some saves.",
        .type=PSP_OPTION_TEXT,.dflt="PSP"},
    [PSP_OPT_RENDER] = C(RENDER,"Renderer","Advanced",
        "Automatic picks OpenGL when a setting needs it, otherwise software. Software is the reference renderer. Null is for diagnostics and is not offered in the launcher.",
        "auto","auto|software|gl|null","Automatic|Software|OpenGL|Null (diagnostic)"),
    [PSP_OPT_AUDIO_LEAD_MS] = N(AUDIO_LEAD_MS,"Audio buffer lead","Advanced",
        "Audio queued ahead of playback, in milliseconds. Auto uses two of the channel's buffers; smaller can reduce latency but may crackle.",
        PSP_OPTION_NUMBER,"auto",0,4000,5,"auto",.special_label="Auto",.format="%.0f ms"),
    [PSP_OPT_AUDIO_PREROLL_MS] = N(AUDIO_PREROLL_MS,"Audio preroll","Advanced",
        "Audio buffered before playback starts, in milliseconds. Auto keeps the 4096-frame default, about 93 ms.",
        PSP_OPTION_NUMBER,"auto",0,4000,5,"auto",.special_label="Auto",.format="%.0f ms"),
    [PSP_OPT_MPEG_DECODE] = C(MPEG_DECODE,"Intro movie","Advanced",
        "Play the game's intro movie. Needs a build with the movie decoder.","0","0|1","Off|On"),
    [PSP_OPT_WINDOW] = C(WINDOW,"Window","Launch","A window also enables real-time pacing. OpenGL always requires a window.",
        "0","0|1","Off|On",.flags=PSP_OPTION_HIDDEN),
    [PSP_OPT_REALTIME] = C(REALTIME,"Real-time pacing","Launch","Headless real-time pacing. Windowed play always uses real time.",
        "0","0|1","Off|On",.flags=PSP_OPTION_HIDDEN),
};
#undef C
#undef N

static const psp_settings_schema *schema;

static int fail(char *error, const char *key, const char *message) {
    snprintf(error, PSP_SETTINGS_ERROR, "%.255s: %s", key, message);
    return -1;
}

static const psp_settings_schema *need_schema(void) {
    if (!schema) psp_settings_schema_use(&psp_title_settings);
    return schema;
}

int psp_settings_name_valid(const char *name) {
    size_t n = strlen(name);
    if (!n || n >= PSP_SETTINGS_NAME || isspace((unsigned char)name[0]) || isspace((unsigned char)name[n-1]))
        return 0;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
        if (*c < 32 || *c == 127 || strchr("[]=;#", *c)) return 0;
    return 1;
}

void psp_settings_schema_use(const psp_settings_schema *s) {
    const char *why = NULL;
    if (!s || s->count < 0 || (s->count && !s->options) || PSP_PLAYER_OPTIONS + s->count > PSP_SETTINGS_MAX)
        why = "a pack has 0 to PSP_SETTINGS_MAX - PSP_PLAYER_OPTIONS options";
    else if (s->count && (!s->id || !psp_settings_name_valid(s->id) || strchr(s->id, ' ') || strchr(s->id, '/')))
        why = "a pack with options needs an id without spaces or /";
    /* A table shorter than its declared size leaves empty options at the
     * end, which would shift every index after them. */
    for (int i = 0; !why && i < s->count; i++)
        if (!s->options[i].key || !s->options[i].env || !s->options[i].dflt) why = "an option without a key, env or default";
    for (int i = 0; !why && i < s->count; i++)
        for (int k = 0; k < PSP_PLAYER_OPTIONS; k++)
            if (!strcmp(s->options[i].key, psp_player_options[k].key)) why = "a pack option repeats one of the player's";
    if (why) {
        fprintf(stderr, "settings: %s: %s\n", s && s->title ? s->title : "schema", why);
        abort();
    }
    schema = s;
}

const psp_settings_schema *psp_settings_active_schema(void) { return need_schema(); }

int psp_settings_count(void) { return PSP_PLAYER_OPTIONS + need_schema()->count; }

const psp_option_def *psp_settings_option(int id) {
    const psp_settings_schema *sc = need_schema();
    if (id < 0 || id >= PSP_PLAYER_OPTIONS + sc->count) return NULL;
    return id < PSP_PLAYER_OPTIONS ? &psp_player_options[id] : &sc->options[id - PSP_PLAYER_OPTIONS];
}

int psp_settings_find(const char *key) {
    for (int i = 0; i < psp_settings_count(); i++) if (!strcmp(psp_settings_option(i)->key, key)) return i;
    return -1;
}

/* The index-th '|'-separated field of list. */
static int token(const char *list, int index, char *out, size_t size) {
    if (!list || index < 0) return 0;
    while (index-- > 0) { list = strchr(list, '|'); if (!list) return 0; list++; }
    size_t n = strcspn(list, "|");
    if (n >= size) return 0;
    memcpy(out, list, n); out[n] = 0;
    return 1;
}

/* Whether key appears as a field of the '|'-separated list. */
static int listed(const char *list, const char *key) {
    char part[PSP_SETTINGS_VALUE];
    for (int i = 0; token(list, i, part, sizeof part); i++) if (!strcmp(part, key)) return 1;
    return 0;
}

/* A value the preferences file keeps as written: well-formed UTF-8, no
 * control characters (a line ends at a newline), and nothing for the
 * reader's trim to take from either end. */
static int text_ok(const char *v) {
    const unsigned char *p = (const unsigned char *)v;
    if (isspace(p[0]) || isspace(p[strlen(v) - 1])) return 0;
    while (*p) {
        if (*p < 0x20 || *p == 0x7F) return 0;
        int more = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 : (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (more < 0 || (*p & 0xFE) == 0xC0) return 0;
        for (p++; more--; p++) if ((*p & 0xC0) != 0x80) return 0;
    }
    return 1;
}

int psp_settings_set(psp_settings *s, int id, const char *value,
                     enum psp_settings_source source, char *error) {
    const psp_option_def *d = psp_settings_option(id);
    if (!d) return fail(error, "settings", "unknown option");
    if (!value || !*value || strlen(value) >= PSP_SETTINGS_VALUE)
        return fail(error, d->key, "missing or excessively long value");
    /* An older spelling, stored as its replacement. */
    char part[PSP_SETTINGS_VALUE], renamed[PSP_SETTINGS_VALUE];
    for (int i = 0; token(d->aliases, i, part, sizeof part); i++) {
        char *eq = strchr(part, '=');
        if (eq && (size_t)(eq - part) == strlen(value) && !strncmp(part, value, eq - part)) {
            snprintf(renamed, sizeof renamed, "%s", eq + 1);
            value = renamed;
            break;
        }
    }
    double n = 0;
    int w = 0, h = 0;
    char canonical[PSP_SETTINGS_VALUE];
    if (d->type == PSP_OPTION_CHOICE) {
        int found = 0;
        /* Explicit conventional booleans; empty environment values are unset. */
        if (!strcmp(d->choices, "0|1")) {
            if (!strcmp(value, "true") || !strcmp(value, "on")) value = "1";
            if (!strcmp(value, "false") || !strcmp(value, "off")) value = "0";
        }
        for (int i = 0; token(d->choices, i, part, sizeof part); i++) {
            if (!strcmp(part, value)) { n = i; found = 1; break; }
        }
        if (!found) {
            snprintf(error, PSP_SETTINGS_ERROR, "%s: expected %s, got '%s'", d->key, d->choices, value);
            return -1;
        }
        snprintf(canonical, sizeof canonical, "%s", value);
    } else if (d->type == PSP_OPTION_TEXT) {
        if (!text_ok(value))
            return fail(error, d->key, "expected UTF-8 text with no control characters, "
                                       "and no spaces at either end");
        snprintf(canonical, sizeof canonical, "%s", value);
    } else if (d->type == PSP_OPTION_SIZE) {
        char *end; errno = 0;
        long ww = strtol(value, &end, 10);
        if (errno || end == value || *end != 'x') return fail(error, d->key, "expected WIDTHxHEIGHT");
        const char *tail = end + 1; long hh = strtol(tail, &end, 10);
        if (errno || end == tail || *end || ww < 1 || hh < 1 || ww > 16384 || hh > 16384)
            return fail(error, d->key, "dimensions must be whole numbers from 1 to 16384");
        w = (int)ww; h = (int)hh;
        snprintf(canonical, sizeof canonical, "%dx%d", w, h);
    } else if (d->special && !strcmp(value, d->special)) {
        n = -1; snprintf(canonical, sizeof canonical, "%s", value);
    } else {
        char *end; errno = 0; n = strtod(value, &end);
        if (errno || end == value || *end || !isfinite(n) || n < d->min || n > d->max) {
            snprintf(error, PSP_SETTINGS_ERROR, "%s: expected a finite number from %g to %g%s%s",
                     d->key, d->min, d->max, d->special ? ", or " : "", d->special ? d->special : "");
            return -1;
        }
        if (d->type == PSP_OPTION_INTEGER && floor(n) != n)
            return fail(error, d->key, "expected a whole number");
        snprintf(canonical, sizeof canonical, "%.9g", n);
    }
    strcpy(s->value[id], canonical); s->number[id] = n; s->source[id] = source;
    if (d->type == PSP_OPTION_SIZE) { s->width = w; s->height = h; }
    return 0;
}

int psp_settings_assign(psp_settings *s, const char *assignments,
                        enum psp_settings_source source, char *error) {
    psp_settings next = *s;
    const char *at = assignments ? assignments : "";
    while (*at) {
        while (*at == ' ') at++;
        size_t n = strcspn(at, " ");
        if (!n) break;
        char pair[PSP_SETTINGS_NAME + PSP_SETTINGS_VALUE];
        if (n >= sizeof pair) return fail(error, "settings", "assignment too long");
        memcpy(pair, at, n); pair[n] = 0; at += n;
        char *eq = strchr(pair, '=');
        if (!eq) return fail(error, pair, "expected KEY=value");
        *eq = 0;
        int id = psp_settings_find(pair);
        if (id < 0) return fail(error, pair, "unknown option");
        if (psp_settings_set(&next, id, eq + 1, source, error)) return -1;
    }
    *s = next; return 0;
}

int psp_settings_bind(psp_settings *s, const char *key, const char *value, char *error) {
    char where[PSP_BIND_KEY + 8];
    snprintf(where, sizeof where, "bind.%.*s", PSP_BIND_KEY, key ? key : "");
    const char *dot = key ? strchr(key, '.') : NULL;
    const size_t device = dot ? (size_t)(dot - key) : 0;
    if (!dot || strlen(key) >= PSP_BIND_KEY || !dot[1] ||
        !((device == 3 && !strncmp(key, "key", 3)) || (device == 3 && !strncmp(key, "pad", 3)) ||
          (device == 5 && !strncmp(key, "mouse", 5))))
        return fail(error, where, "expected bind.key.<target>, bind.pad.<target> or bind.mouse.<target>");
    for (const char *c = dot + 1; *c; c++)
        if (!islower((unsigned char)*c) && !isdigit((unsigned char)*c) && *c != '_')
            return fail(error, where, "a target is lower-case letters, digits and _");
    int at = 0;
    while (at < s->bind_count && strcmp(s->bind[at].key, key)) at++;
    if (!value) {
        if (at < s->bind_count) {
            memmove(&s->bind[at], &s->bind[at + 1], (size_t)(s->bind_count - at - 1) * sizeof *s->bind);
            s->bind_count--;
        }
        return 0;
    }
    const size_t n = strlen(value);
    if (n >= PSP_BIND_VALUE) return fail(error, where, "value too long");
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)value[i] < 0x20 || (unsigned char)value[i] > 0x7e)
            return fail(error, where, "printable ASCII only");
    if (n && (value[0] == ' ' || value[n - 1] == ' ')) return fail(error, where, "no leading or trailing spaces");
    if (at == s->bind_count) {
        if (s->bind_count >= PSP_BINDS_MAX) return fail(error, where, "at most 48 bindings per pack");
        s->bind_count++;
    }
    snprintf(s->bind[at].key, sizeof s->bind[at].key, "%s", key);
    snprintf(s->bind[at].value, sizeof s->bind[at].value, "%s", value);
    return 0;
}

const char *psp_settings_binding(const psp_settings *s, const char *key) {
    for (int i = 0; i < s->bind_count; i++) if (!strcmp(s->bind[i].key, key)) return s->bind[i].value;
    return NULL;
}

void psp_settings_defaults(psp_settings *s) {
    memset(s, 0, sizeof *s); char error[PSP_SETTINGS_ERROR];
    for (int i = 0; i < psp_settings_count(); i++)
        if (psp_settings_set(s, i, psp_settings_option(i)->dflt, PSP_SOURCE_DEFAULT, error)) {
            fprintf(stderr, "settings: default %s\n", error);
            abort();
        }
    psp_settings_resolve(s, error);
}

void psp_settings_play_defaults(psp_settings *s) {
    char error[PSP_SETTINGS_ERROR];
    psp_settings_defaults(s);
    if (psp_settings_assign(s, PSP_PLAYER_PLAY_DEFAULTS, PSP_SOURCE_FILE, error) ||
        psp_settings_assign(s, need_schema()->play_defaults, PSP_SOURCE_FILE, error)) {
        fprintf(stderr, "settings: play defaults: %s\n", error);
        abort();
    }
    psp_settings_resolve(s, error);
}

void psp_settings_reset(psp_settings *s, int id) {
    psp_settings fresh; char error[PSP_SETTINGS_ERROR];
    psp_settings_play_defaults(&fresh);
    const int pack = id >= PSP_PLAYER_OPTIONS;
    for (int k = pack ? PSP_PLAYER_OPTIONS : 0; k < (pack ? psp_settings_count() : PSP_PLAYER_OPTIONS); k++) {
        memcpy(s->value[k], fresh.value[k], sizeof s->value[k]);
        s->number[k] = fresh.number[k];
        s->source[k] = fresh.source[k];
    }
    if (!pack) { s->width = fresh.width; s->height = fresh.height; }
    psp_settings_resolve(s, error);
}

int psp_settings_env(psp_settings *s, char *error) {
    psp_settings next = *s;
    for (int i = 0; i < psp_settings_count(); i++) {
        const char *v = getenv(psp_settings_option(i)->env);
        if (v && *v && psp_settings_set(&next, i, v, PSP_SOURCE_ENV, error)) return -1;
    }
    *s = next; return 0;
}

/* The PSP's language nearest this computer's: the locale's language from
 * LC_ALL, LC_MESSAGES or LANG, in that order, by its first letters, and
 * English for any the PSP does not have ("C" and "POSIX" among them).
 * Chinese is Traditional for Taiwan, Hong Kong and Macau, and for a locale
 * that names the script, Simplified otherwise. */
int psp_settings_host_language(void) {
    const char *v = NULL;
    static const char *const vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (int i = 0; i < 3 && !(v && *v); i++) v = getenv(vars[i]);
    if (!v || !*v) return 1;
    static const char *const codes[] = { "ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko" };
    for (int i = 0; i < 10; i++)
        if (!strncmp(v, codes[i], 2) && !isalpha((unsigned char)v[2])) return i;
    if (!strncmp(v, "zh", 2) && !isalpha((unsigned char)v[2]))
        return strstr(v, "_TW") || strstr(v, "_HK") || strstr(v, "_MO") || strstr(v, "Hant") ? 10 : 11;
    return 1;
}

int psp_settings_resolve(psp_settings *s, char *error) {
    const psp_settings_schema *sc = need_schema();
    int enhanced = 0;
    for (int i = 0; i < psp_settings_count(); i++)
        if ((psp_settings_option(i)->flags & PSP_OPTION_NEEDS_GL) && s->number[i]) enhanced = 1;
    s->render = (int)s->number[PSP_OPT_RENDER];
    if (!s->render) s->render = enhanced ? 2 : 1; /* software=1, gl=2, null=3 */
    if (enhanced && s->render != 2)
        return fail(error, "RENDER", sc->gl_error ? sc->gl_error
                                     : "this choice requires OpenGL; choose Automatic or OpenGL");
    if (sc->resolve && sc->resolve(s, error)) return -1;
    s->window = s->number[PSP_OPT_WINDOW] != 0 || s->number[PSP_OPT_WINDOW_MODE] != 0 || s->render == 2;
    s->realtime = s->window || s->number[PSP_OPT_REALTIME] != 0;
    s->language = s->number[PSP_OPT_LANGUAGE] ? (int)s->number[PSP_OPT_LANGUAGE] - 1 : psp_settings_host_language();
    s->confirm_cross = s->number[PSP_OPT_CONFIRM] ? s->number[PSP_OPT_CONFIRM] == 1 : s->language != 0;
    return 0;
}

void psp_option_label(const psp_settings *s, int id, char *out, size_t size) {
    const psp_option_def *d = psp_settings_option(id);
    const double n = s->number[id];
    if (d->type == PSP_OPTION_CHOICE && token(d->labels, (int)n, out, size)) return;
    if (d->type == PSP_OPTION_SIZE) snprintf(out, size, "%s", s->value[id]);
    else if (d->special && n < 0 && d->special_label) snprintf(out, size, "%s", d->special_label);
    else if (n == 0 && d->zero_label) snprintf(out, size, "%s", d->zero_label);
    else if (d->format && !(d->special && n < 0))
        snprintf(out, size, d->format, n * (d->scale ? d->scale : 1));
    else snprintf(out, size, "%s", s->value[id]);
}

void psp_settings_print(const psp_settings *s, FILE *out) {
    const psp_settings_schema *sc = need_schema();
    for (int i = 0; i < psp_settings_count(); i++) {
        const psp_option_def *d = psp_settings_option(i);
        fprintf(out, "%-24s = %-12s [%s%s]\n", d->key, s->value[i],
                s->source[i] == PSP_SOURCE_ENV ? "environment: " :
                s->source[i] == PSP_SOURCE_FILE ? "file" :
                s->source[i] == PSP_SOURCE_COMMAND_LINE ? "command line" : "default",
                s->source[i] == PSP_SOURCE_ENV ? d->env : "");
    }
    for (int i = 0; i < s->bind_count; i++)
        fprintf(out, "bind.%-19s = %-12s [file]\n", s->bind[i].key, s->bind[i].value);
    fprintf(out, "effective: renderer=%s gamepad=%s window=%d realtime=%d language=%d confirm=%s",
            s->render == 2 ? "gl" : s->render == 3 ? "null" : "software",
            s->gamepad ? "modern" : "classic", s->window, s->realtime,
            s->language, s->confirm_cross ? "cross" : "circle");
    if (sc->print_effective) sc->print_effective(s, out);
    fputc('\n', out);
    if (sc->print_notes) sc->print_notes(s, out);
}

static psp_settings installed, fallback;
static const psp_settings *active;
static pthread_once_t fallback_once = PTHREAD_ONCE_INIT;
static void default_environment(void) {
    char error[PSP_SETTINGS_ERROR]; psp_settings_defaults(&fallback);
    if (psp_settings_env(&fallback, error) || psp_settings_resolve(&fallback, error)) {
        fprintf(stderr, "settings: %s\n", error); exit(2);
    }
}
void psp_settings_use(const psp_settings *s) { installed = *s; active = &installed; }
const psp_settings *psp_settings_current(void) {
    if (active) return active;
    pthread_once(&fallback_once, default_environment); return &fallback;
}

/* ---- the preferences file --------------------------------------------------------- */

/* A section: its header without brackets -- "player", "pack", "pack <id>",
 * "preset <name>" -- and its lines in order. */
typedef struct { char *key, *value; } entry;
typedef struct { char *name; int count, cap; entry *lines; } section;
struct psp_settings_file {
    int count, cap;
    section *sections;
    char game[PSP_SETTINGS_NAME];
};
enum { SECTION_NAME = 160 };

static char *copy(const char *s) {
    const size_t n = strlen(s) + 1;
    char *c = malloc(n);
    if (c) memcpy(c, s, n);
    return c;
}

static void section_clear(section *sec) {
    for (int i = 0; i < sec->count; i++) { free(sec->lines[i].key); free(sec->lines[i].value); }
    free(sec->lines);
    sec->lines = NULL; sec->count = sec->cap = 0;
}

static section *section_find(const psp_settings_file *f, const char *name) {
    for (int i = 0; i < f->count; i++) if (!strcmp(f->sections[i].name, name)) return &f->sections[i];
    return NULL;
}

static section *section_add(psp_settings_file *f, const char *name) {
    if (f->count == f->cap) {
        const int cap = f->cap ? f->cap * 2 : 8;
        section *grown = realloc(f->sections, (size_t)cap * sizeof *grown);
        if (!grown) return NULL;
        f->sections = grown; f->cap = cap;
    }
    section *sec = &f->sections[f->count];
    memset(sec, 0, sizeof *sec);
    if (!(sec->name = copy(name))) return NULL;
    f->count++;
    return sec;
}

static const char *section_get(const section *sec, const char *key) {
    for (int i = 0; i < sec->count; i++) if (!strcmp(sec->lines[i].key, key)) return sec->lines[i].value;
    return NULL;
}

/* Replace key's value where it is, or add it at the end. */
static int section_set(section *sec, const char *key, const char *value) {
    char *v = copy(value);
    if (!v) return -1;
    for (int i = 0; i < sec->count; i++)
        if (!strcmp(sec->lines[i].key, key)) { free(sec->lines[i].value); sec->lines[i].value = v; return 0; }
    if (sec->count == sec->cap) {
        const int cap = sec->cap ? sec->cap * 2 : 16;
        entry *grown = realloc(sec->lines, (size_t)cap * sizeof *grown);
        if (!grown) { free(v); return -1; }
        sec->lines = grown; sec->cap = cap;
    }
    char *k = copy(key);
    if (!k) { free(v); return -1; }
    sec->lines[sec->count++] = (entry){k, v};
    return 0;
}

static void section_remove(section *sec, int at) {
    free(sec->lines[at].key); free(sec->lines[at].value);
    memmove(&sec->lines[at], &sec->lines[at + 1], (size_t)(sec->count - at - 1) * sizeof *sec->lines);
    sec->count--;
}

static int section_copy(psp_settings_file *f, const section *from, const char *name) {
    section *to = section_add(f, name);
    if (!to) return -1;
    for (int i = 0; i < from->count; i++)
        if (section_set(to, from->lines[i].key, from->lines[i].value)) return -1;
    return 0;
}

psp_settings_file *psp_settings_file_new(void) { return calloc(1, sizeof(psp_settings_file)); }

void psp_settings_file_free(psp_settings_file *f) {
    if (!f) return;
    for (int i = 0; i < f->count; i++) { section_clear(&f->sections[i]); free(f->sections[i].name); }
    free(f->sections);
    free(f);
}

static int player_key(const char *key) {
    for (int k = 0; k < PSP_PLAYER_OPTIONS; k++) if (!strcmp(psp_player_options[k].key, key)) return 1;
    return 0;
}

/* A section header's name: player, pack, pack <id>, or preset <name>, where
 * an id is a name without spaces or / and a preset's name may hold one / --
 * <pack id>/<name> for a preset an earlier app's file brought. */
static int section_name_valid(const char *name) {
    if (!strcmp(name, "player") || !strcmp(name, "pack")) return 1;
    if (!strncmp(name, "pack ", 5))
        return psp_settings_name_valid(name + 5) && !strchr(name + 5, ' ') && !strchr(name + 5, '/');
    if (strncmp(name, "preset ", 7)) return 0;
    const char *slash = strchr(name + 7, '/');
    if (!slash) return psp_settings_name_valid(name + 7);
    char id[SECTION_NAME];
    snprintf(id, sizeof id, "%.*s", (int)(slash - name - 7), name + 7);
    return psp_settings_name_valid(id) && !strchr(id, ' ') && psp_settings_name_valid(slash + 1);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    return s;
}

/* A version 1 file's selected preset as the settings: the player's keys
 * into [player], the rest into an unnamed [pack]; the other presets stay. */
static int convert_presets(psp_settings_file *f, const char *selected, char *why) {
    char name[SECTION_NAME];
    snprintf(name, sizeof name, "preset %s", selected);
    section *chosen = section_find(f, name);
    if (!chosen) { snprintf(why, PSP_SETTINGS_ERROR, "the selected preset '%s' does not exist", selected); return -1; }
    const int at = (int)(chosen - f->sections);
    psp_settings_file *next = psp_settings_file_new();
    section *player = next ? section_add(next, "player") : NULL;
    section *pack = player ? section_add(next, "pack") : NULL;
    int bad = !pack;
    for (int i = 0; !bad && i < chosen->count; i++)
        bad = section_set(player_key(chosen->lines[i].key) ? &next->sections[0] : &next->sections[1],
                          chosen->lines[i].key, chosen->lines[i].value);
    for (int i = 0; !bad && i < f->count; i++)
        if (i != at) bad = section_copy(next, &f->sections[i], f->sections[i].name);
    if (bad) { psp_settings_file_free(next); strcpy(why, "out of memory"); return -1; }
    for (int i = 0; i < f->count; i++) { section_clear(&f->sections[i]); free(f->sections[i].name); }
    free(f->sections);
    f->sections = next->sections; f->count = next->count; f->cap = next->cap;
    free(next);
    return 0;
}

psp_settings_file *psp_settings_file_read(const char *path, char *error) {
    FILE *in = fopen(path, "r");
    if (!in) { fail(error, path, strerror(errno)); return NULL; }
    psp_settings_file *f = psp_settings_file_new();
    if (!f) { fclose(in); fail(error, path, "out of memory"); return NULL; }
    char line[512], selected[PSP_SETTINGS_NAME] = "", why[PSP_SETTINGS_ERROR] = "";
    int version = 0, lineno = 0, bad = 0;
    section *at = NULL;
    while (fgets(line, sizeof line, in)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(in)) { strcpy(why, "line too long"); bad = 1; break; }
        char *v = trim(line);
        if (!*v || *v == '#' || *v == ';') continue;
        if (*v == '[') {
            size_t n = strlen(v);
            if (n < 3 || v[n-1] != ']') { strcpy(why, "expected [section]"); bad = 1; break; }
            v[n-1] = 0; v++;
            if (!version) { strcpy(why, "expected version= before the first section"); bad = 1; break; }
            if (version == 1 ? strncmp(v, "preset ", 7) || !psp_settings_name_valid(v + 7) : !section_name_valid(v)) {
                snprintf(why, sizeof why, "unexpected section [%.200s]", v); bad = 1; break;
            }
            if (section_find(f, v)) { snprintf(why, sizeof why, "duplicate section [%.200s]", v); bad = 1; break; }
            if (!(at = section_add(f, v))) { strcpy(why, "out of memory"); bad = 1; break; }
            continue;
        }
        char *eq = strchr(v, '=');
        if (!eq) { strcpy(why, "expected key=value"); bad = 1; break; }
        *eq = 0; char *key = trim(v); v = trim(eq + 1);
        if (!at) {
            if (!strcmp(key, "version") && !version && (!strcmp(v, "1") || !strcmp(v, "2"))) version = *v - '0';
            else if (!strcmp(key, "selected") && version == 1 && !*selected && psp_settings_name_valid(v)) strcpy(selected, v);
            else if (!strcmp(key, "game") && !*f->game && psp_settings_name_valid(v)) strcpy(f->game, v);
            else {
                strcpy(why, "expected version=1 or 2, at most one game=Slug and, in version 1, selected=Name, before the sections");
                bad = 1; break;
            }
            continue;
        }
        if (!*key) { strcpy(why, "expected key=value"); bad = 1; break; }
        if (section_get(at, key)) { snprintf(why, sizeof why, "duplicate key '%.200s'", key); bad = 1; break; }
        if (section_set(at, key, v)) { strcpy(why, "out of memory"); bad = 1; break; }
    }
    if (!bad && ferror(in)) { snprintf(why, sizeof why, "read failed: %s", strerror(errno)); bad = 1; }
    fclose(in);
    if (bad) {
        snprintf(error, PSP_SETTINGS_ERROR, "%s:%d: %.300s", path, lineno, why);
        psp_settings_file_free(f);
        return NULL;
    }
    if (!version) strcpy(why, "requires version=1 or 2");
    else if (version == 1 && (!f->count || !*selected)) strcpy(why, "a version 1 file needs at least one preset and selected=Name");
    else if (version == 1) convert_presets(f, selected, why);
    if (*why) {
        snprintf(error, PSP_SETTINGS_ERROR, "%s: %.300s", path, why);
        psp_settings_file_free(f);
        return NULL;
    }
    return f;
}

/* The active pack's section: its own, else an unnamed one it claims when
 * claim is set; NULL for none, or for the player's schema alone. */
static section *pack_section(psp_settings_file *f, int claim) {
    const psp_settings_schema *sc = need_schema();
    if (!sc->count) return NULL;
    char name[SECTION_NAME];
    snprintf(name, sizeof name, "pack %s", sc->id);
    section *sec = section_find(f, name);
    if (sec || !(sec = section_find(f, "pack")) || !claim) return sec;
    char *renamed = copy(name);
    if (!renamed) return NULL;
    free(sec->name);
    sec->name = renamed;
    return sec;
}

int psp_settings_file_has(const psp_settings_file *f, int pack) {
    if (!pack) return section_find(f, "player") != NULL;
    return pack_section((psp_settings_file *)f, 0) != NULL;
}

int psp_settings_file_get(psp_settings_file *f, psp_settings *s, char *error) {
    const psp_settings_schema *sc = need_schema();
    psp_settings next;
    psp_settings_defaults(&next);
    const section *player = section_find(f, "player"), *pack = pack_section(f, 1);
    for (int i = 0; player && i < player->count; i++) {
        const char *key = player->lines[i].key;
        const int id = psp_settings_find(key);
        if (id < 0 || id >= PSP_PLAYER_OPTIONS) {
            snprintf(error, PSP_SETTINGS_ERROR, "[player]: unknown option '%.200s'", key);
            return -1;
        }
        if (psp_settings_set(&next, id, player->lines[i].value, PSP_SOURCE_FILE, error)) return -1;
    }
    for (int i = 0; pack && i < pack->count; i++) {
        const char *key = pack->lines[i].key, *value = pack->lines[i].value;
        if (listed(sc->retired_keys, key)) continue;
        if (!strncmp(key, "bind.", 5)) {
            if (psp_settings_bind(&next, key + 5, value, error)) return -1;
            continue;
        }
        const int id = psp_settings_find(key);
        if (id < 0 || id < PSP_PLAYER_OPTIONS) {
            snprintf(error, PSP_SETTINGS_ERROR, "[%s]: %s '%.200s'", pack->name,
                     id < 0 ? "unknown option" : "an option of every game's, which belongs in [player]:", key);
            return -1;
        }
        if (psp_settings_set(&next, id, value, PSP_SOURCE_FILE, error)) return -1;
    }
    char ignored[PSP_SETTINGS_ERROR];
    psp_settings_resolve(&next, ignored);
    *s = next;
    return 0;
}

int psp_settings_file_put(psp_settings_file *f, const psp_settings *s, char *error) {
    const psp_settings_schema *sc = need_schema();
    section *player = section_find(f, "player");
    if (!player && !(player = section_add(f, "player"))) return fail(error, "settings", "out of memory");
    section *pack = NULL;
    if (sc->count && !(pack = pack_section(f, 1))) {
        char name[SECTION_NAME];
        snprintf(name, sizeof name, "pack %s", sc->id);
        if (!(pack = section_add(f, name))) return fail(error, "settings", "out of memory");
        player = section_find(f, "player");     /* the array may have moved */
    }
    for (int id = 0; id < psp_settings_count(); id++) {
        if (s->source[id] == PSP_SOURCE_ENV || s->source[id] == PSP_SOURCE_COMMAND_LINE) continue;
        if (section_set(id < PSP_PLAYER_OPTIONS ? player : pack, psp_settings_option(id)->key, s->value[id]))
            return fail(error, "settings", "out of memory");
    }
    if (pack) {
        /* The bindings follow the options; retired keys go. */
        for (int i = pack->count - 1; i >= 0; i--)
            if (!strncmp(pack->lines[i].key, "bind.", 5) || listed(sc->retired_keys, pack->lines[i].key))
                section_remove(pack, i);
        for (int i = 0; i < s->bind_count; i++) {
            char key[PSP_BIND_KEY + 8];
            snprintf(key, sizeof key, "bind.%s", s->bind[i].key);
            if (section_set(pack, key, s->bind[i].value)) return fail(error, "settings", "out of memory");
        }
    }
    return 0;
}

const char *psp_settings_file_game(const psp_settings_file *f) { return f->game; }

int psp_settings_file_set_game(psp_settings_file *f, const char *slug, char *error) {
    if (*slug && !psp_settings_name_valid(slug)) return fail(error, "game", "invalid slug");
    snprintf(f->game, sizeof f->game, "%s", slug);
    return 0;
}

int psp_settings_file_adopt(psp_settings_file *f, const psp_settings_file *old,
                            const char *pack_id, char *error) {
    char name[SECTION_NAME];
    if (!psp_settings_name_valid(pack_id) || strchr(pack_id, ' ') || strchr(pack_id, '/'))
        return fail(error, pack_id, "not a pack id");
    const section *player = section_find(old, "player");
    if (player && !section_find(f, "player") && section_copy(f, player, "player"))
        return fail(error, "settings", "out of memory");
    snprintf(name, sizeof name, "pack %s", pack_id);
    const section *pack = section_find(old, name);
    if (!pack) pack = section_find(old, "pack");
    if (pack && !section_find(f, name) && section_copy(f, pack, name))
        return fail(error, "settings", "out of memory");
    for (int i = 0; i < old->count; i++) {
        const section *sec = &old->sections[i];
        if (strncmp(sec->name, "preset ", 7)) continue;
        if (strchr(sec->name + 7, '/')) snprintf(name, sizeof name, "%s", sec->name);
        else snprintf(name, sizeof name, "preset %s/%s", pack_id, sec->name + 7);
        if (!section_name_valid(name) || section_find(f, name)) continue;
        if (section_copy(f, sec, name)) return fail(error, "settings", "out of memory");
    }
    if (!*f->game && *old->game) snprintf(f->game, sizeof f->game, "%s", old->game);
    return 0;
}

/* Sections written player first, then the packs', then the kept presets. */
static int section_rank(const section *sec) {
    return !strcmp(sec->name, "player") ? 0 : !strncmp(sec->name, "pack", 4) ? 1 : 2;
}

int psp_settings_file_write(const psp_settings_file *f, const char *path, char *error) {
    size_t n = strlen(path) + 16; char *tmp = malloc(n);
    if (!tmp) return fail(error, path, "out of memory");
    snprintf(tmp, n, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) { int e = errno; free(tmp); return fail(error, path, strerror(e)); }
    FILE *out = fdopen(fd, "w");
    if (!out) { int e = errno; close(fd); unlink(tmp); free(tmp); return fail(error, path, strerror(e)); }
    fputs("# psprecomp player settings. Environment overrides are never saved.\n"
          "# [player] is read by every game, [pack ID] by that pack's games. A\n"
          "# [preset ...] section is an earlier preset, kept but not read.\n", out);
    fprintf(out, "version=2\n");
    if (*f->game) fprintf(out, "game=%s\n", f->game);
    for (int rank = 0; rank < 3; rank++)
        for (int i = 0; i < f->count; i++) {
            const section *sec = &f->sections[i];
            if (section_rank(sec) != rank) continue;
            fprintf(out, "\n[%s]\n", sec->name);
            for (int k = 0; k < sec->count; k++) fprintf(out, "%s=%s\n", sec->lines[k].key, sec->lines[k].value);
        }
    int bad = ferror(out), saved_errno = errno;
    if (fflush(out) || fsync(fd)) { bad = 1; saved_errno = errno; }
    if (fclose(out)) { bad = 1; saved_errno = errno; }
    if (!bad && rename(tmp, path)) { bad = 1; saved_errno = errno; }
    if (bad) { unlink(tmp); fail(error, path, strerror(saved_errno)); }
    free(tmp); return bad ? -1 : 0;
}

/* ---- a game's settings ------------------------------------------------------------- */

static char origin_path[4096];

int psp_settings_load(psp_settings *s, const char *path, char *error) {
    psp_settings next; psp_settings_defaults(&next);
    if (path) {
        if (strlen(path) >= sizeof origin_path) return fail(error, path, "path too long");
        psp_settings_file *f = psp_settings_file_read(path, error);
        if (!f) return -1;
        const int rc = psp_settings_file_get(f, &next, error);
        psp_settings_file_free(f);
        if (rc) return -1;
    }
    if (psp_settings_env(&next, error) || psp_settings_resolve(&next, error)) return -1;
    snprintf(origin_path, sizeof origin_path, "%s", path ? path : "");
    *s = next; return 0;
}

const char *psp_settings_origin(void) { return *origin_path ? origin_path : NULL; }

int psp_settings_save_origin(const psp_settings *s, char *error) {
    if (!*origin_path) return fail(error, "settings", "the game was started without a preferences file");
    psp_settings_file *f = psp_settings_file_read(origin_path, error);
    if (!f) return -1;
    const int rc = psp_settings_file_put(f, s, error) || psp_settings_file_write(f, origin_path, error) ? -1 : 0;
    psp_settings_file_free(f);
    return rc;
}
