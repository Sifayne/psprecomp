#ifndef PSPRECOMP_HOST_SETTINGS_H
#define PSPRECOMP_HOST_SETTINGS_H
/* Player settings: typed options, the preferences file and their precedence.
 *
 * Two tables make up the options a game reads. The player's -- what the
 * shared host does the same way for every game: the window, the renderer,
 * the sound, the save states -- is psprecomp's (psp_player_options). The
 * pack's -- what its games' code interprets: their controls, their graphics
 * features -- comes with the title as a schema. A psp_settings holds both,
 * the player's first, so the PSP_OPT_* indices are fixed and a pack's own
 * enum starts at PSP_PLAYER_OPTIONS. Every launcher, game and tool built for
 * a pack reads settings through this API. No SDL: the settings tool and the
 * tests build without it.
 *
 * The preferences file has a section for each table: [player], read by every
 * game, and [pack <id>], read by that pack's games. Precedence is default <
 * file < environment < command line; values the environment or the command
 * line set are never written back. */
#include <stddef.h>
#include <stdio.h>

enum psp_option_type { PSP_OPTION_CHOICE, PSP_OPTION_NUMBER, PSP_OPTION_SIZE, PSP_OPTION_INTEGER };
enum psp_settings_source {
    PSP_SOURCE_DEFAULT, PSP_SOURCE_FILE, PSP_SOURCE_ENV, PSP_SOURCE_COMMAND_LINE,
    PSP_SOURCE_PRESET = PSP_SOURCE_FILE  /* its name while the file held presets */
};
enum {
    PSP_BINDS_MAX = 48,             /* bind.* keys in one pack's section */
    PSP_BIND_KEY = 32, PSP_BIND_VALUE = 80,
    PSP_SETTINGS_MAX = 48,          /* the player's options and a pack's */
    PSP_SETTINGS_TITLE_DERIVED = 8, /* derived values a title resolves itself */
    PSP_SETTINGS_NAME = 64, PSP_SETTINGS_VALUE = 96, PSP_SETTINGS_ERROR = 512
};
/* A non-zero choice of this option needs the OpenGL renderer. */
#define PSP_OPTION_NEEDS_GL 0x1u
/* The launcher does not show this option; files, the environment and the
 * command line still set it. */
#define PSP_OPTION_HIDDEN   0x2u
/* The in-game menu applies a change to this option at once; without it, a
 * change applies the next time the game starts. The host applies its own
 * (the keyboard layout, the active controller, the window mode, the volume);
 * a title that marks one of its own reads it again when it changes. */
#define PSP_OPTION_LIVE     0x4u

typedef struct {
    const char *key, *env, *label, *page, *help;
    enum psp_option_type type;
    const char *dflt;
    /* Choice values and corresponding display labels, separated by '|'. */
    const char *choices, *labels;
    double min, max, step;
    const char *special;            /* Optional sentinel word, stored as -1. */
    /* How a number is shown: special_label for the sentinel, zero_label for
     * 0, otherwise format (one double, multiplied by scale; 0 means 1). With
     * none of them the canonical value is shown as stored. */
    const char *special_label, *zero_label, *format;
    double scale;
    /* Optional: the values offered in place of a free number, separated by
     * '|' -- ascending numbers, then the special word if it is one. */
    const char *stops;
    /* Older spellings accepted on input and stored as the new one:
     * "old=new|old=new". */
    const char *aliases;
    unsigned flags;                 /* PSP_OPTION_* */
} psp_option_def;

/* The player's options, the first PSP_PLAYER_OPTIONS of every psp_settings,
 * in the order of their pages: Display, Audio, Controller, Save states,
 * Advanced. WINDOW and REALTIME are hidden: the launcher starts games with a
 * window, and a headless run asks for real time itself. */
enum psp_player_option {
    PSP_OPT_RESOLUTION, PSP_OPT_WINDOW_MODE, PSP_OPT_WINDOW_SIZE, PSP_OPT_DISPLAY,
    PSP_OPT_VOLUME, PSP_OPT_ACTIVE_PAD, PSP_OPT_STATE_LOAD, PSP_OPT_STATE_START,
    PSP_OPT_RENDER, PSP_OPT_AUDIO_LEAD_MS, PSP_OPT_AUDIO_PREROLL_MS, PSP_OPT_MPEG_DECODE,
    PSP_OPT_WINDOW, PSP_OPT_REALTIME,
    PSP_PLAYER_OPTIONS
};
extern const psp_option_def psp_player_options[PSP_PLAYER_OPTIONS];
/* What a new preferences file, and Reset to defaults, gives a player over
 * the defaults: a window, the intro movie, and window-resolution rendering,
 * which picks OpenGL. The defaults themselves stay headless and original for
 * tools and tests. */
#define PSP_PLAYER_PLAY_DEFAULTS "WINDOW=1 MPEG_DECODE=1 RESOLUTION=window"

/* One bind.* key of a pack's section: which of one device's controls press
 * one target -- bind.key.cross=Z, Space; bind.pad.fire=righttrigger. The key
 * is stored without its "bind." prefix, and the value as written; an empty
 * one unbinds the target on that device. The host reads them when it starts
 * (src/host/input.c, docs/PLAYER-LAYER.md §3); a key it does not know is
 * reported and skipped there, so one title's actions ride along harmlessly
 * with another's of the same pack. */
typedef struct { char key[PSP_BIND_KEY], value[PSP_BIND_VALUE]; } psp_binding;

typedef struct {
    char value[PSP_SETTINGS_MAX][PSP_SETTINGS_VALUE];
    double number[PSP_SETTINGS_MAX];          /* Numeric value or choice index. */
    enum psp_settings_source source[PSP_SETTINGS_MAX];
    int width, height;                        /* WINDOW_SIZE's dimensions. */
    /* Derived once by psp_settings_resolve. The player's options decide
     * render (1 software, 2 gl, 3 null), window and realtime; the pack's
     * resolve hook decides gamepad (modern buttons), input and mouse. */
    int render, gamepad, window, realtime;
    int input, mouse;
    int title[PSP_SETTINGS_TITLE_DERIVED];    /* Indexed by the pack's own enum. */
    /* Bindings that differ from the title's defaults, from the file. */
    int bind_count;
    psp_binding bind[PSP_BINDS_MAX];
} psp_settings;

typedef struct {
    /* The pack's name, heading its pages in the launcher; and its id, which
     * names its section of the preferences file, [pack <id>] -- the folder
     * name of its own app before there was one for every pack. */
    const char *title, *id;
    /* The pack's options, PSP_PLAYER_OPTIONS onwards in a psp_settings. */
    const psp_option_def *options;
    int count;
    /* What a new preferences file, and Reset to defaults, gives a player of
     * the pack's options over their defaults, as space-separated KEY=value
     * assignments. Optional. */
    const char *play_defaults;
    /* Keys earlier files may carry and loading skips, separated by '|'. */
    const char *retired_keys;
    /* Why a PSP_OPTION_NEEDS_GL choice refuses a renderer other than GL. */
    const char *gl_error;
    /* Optional. resolve fills the title's derived values after render and
     * before window; it may refuse a combination. print_effective appends to
     * the "effective:" line before its newline; print_notes follows it. */
    int (*resolve)(psp_settings *s, char *error);
    void (*print_effective)(const psp_settings *s, FILE *out);
    void (*print_notes)(const psp_settings *s, FILE *out);
} psp_settings_schema;

/* The title's schema. Every pack defines it, once, next to its option
 * table: the settings calls fall back to it until psp_settings_schema_use
 * installs another, so a program that reads settings without a pack fails to
 * link rather than running with nobody's options -- and the reference pulls
 * the pack's table out of a static archive. One with no options (count 0,
 * no id) is the player's alone: what a launcher defines that holds several
 * packs and installs each in turn. Error buffers passed to this API must hold
 * PSP_SETTINGS_ERROR bytes.
 *
 * The pull only works when the pack's object comes after the mechanism on
 * the link line, or in the same archive. A pack building with CMake makes its
 * schema part of the mechanism's interface, so it always follows:
 *     target_link_libraries(psprecomp_settings INTERFACE <its schema library>)
 * The 3rd Birthday's dev/CMakeLists.txt does this. */
extern const psp_settings_schema psp_title_settings;
void psp_settings_schema_use(const psp_settings_schema *schema);
const psp_settings_schema *psp_settings_active_schema(void);
/* Every option a psp_settings holds under the active schema: the player's,
 * then the pack's. */
int psp_settings_count(void);
const psp_option_def *psp_settings_option(int id);
/* The option's index, or -1. */
int psp_settings_find(const char *key);

void psp_settings_defaults(psp_settings *s);
/* The defaults with both tables' play defaults over them. */
void psp_settings_play_defaults(psp_settings *s);
/* The play defaults into the table holding option id -- the player's or the
 * pack's -- leaving the other table and the bindings as they are. */
void psp_settings_reset(psp_settings *s, int id);
int psp_settings_set(psp_settings *s, int id, const char *value,
                     enum psp_settings_source source, char *error);
/* Space-separated KEY=value pairs, all or nothing. */
int psp_settings_assign(psp_settings *s, const char *assignments,
                        enum psp_settings_source source, char *error);
int psp_settings_env(psp_settings *s, char *error);
/* Set bind.<key>, where key is "<key|pad|mouse>.<target>"; a NULL value
 * removes it, restoring the default. Only the shape is checked here. */
int psp_settings_bind(psp_settings *s, const char *key, const char *value, char *error);
/* The value of bind.<key>, or NULL when the title's default applies. */
const char *psp_settings_binding(const psp_settings *s, const char *key);
int psp_settings_resolve(psp_settings *s, char *error);
void psp_settings_print(const psp_settings *s, FILE *out);
void psp_option_label(const psp_settings *s, int id, char *out, size_t size);
/* Installation is startup-only, before any guest/SDL threads are started.
 * Standalone fixtures that do not install a snapshot get defaults + env.
 * No mutation is supported during play. */
void psp_settings_use(const psp_settings *s);
const psp_settings *psp_settings_current(void);

/* ---- the preferences file ------------------------------------------------------- *
 *
 *     version=2
 *     game=aclr                   the launcher's remembered title; optional
 *     [player]                    the player's options, for every game
 *     [pack last-raven]           one pack's options and bindings
 *     [preset last-raven/Classic] an earlier preset, kept but never read
 *
 * Sections hold KEY=value lines, a section's options all written; a missing
 * key keeps its default. A file of version 1 -- presets, one selected --
 * reads as its selected preset split into [player] and an unnamed [pack],
 * which the first pack to read it claims, with the other presets kept. Every
 * section the reader does not take is written back as it was. */
typedef struct psp_settings_file psp_settings_file;

psp_settings_file *psp_settings_file_new(void);
/* NULL, with error, when the file cannot be read or is malformed. */
psp_settings_file *psp_settings_file_read(const char *path, char *error);
/* Atomically, by a rename over the old file. */
int psp_settings_file_write(const psp_settings_file *f, const char *path, char *error);
void psp_settings_file_free(psp_settings_file *f);
/* The defaults, then [player], then the active pack's section (claiming an
 * unnamed one if it has none). All or nothing: on an unknown key or a bad
 * value s is left as it was and the call fails. Not resolved against the
 * environment: psp_settings_env and psp_settings_resolve follow. */
int psp_settings_file_get(psp_settings_file *f, psp_settings *s, char *error);
/* Whether the file has a [player] section (pack 0), or one the active pack
 * reads (pack 1). */
int psp_settings_file_has(const psp_settings_file *f, int pack);
/* Into [player] and the active pack's section: every option, and the
 * bindings. A value the environment or the command line set keeps the
 * file's own. */
int psp_settings_file_put(psp_settings_file *f, const psp_settings *s, char *error);
const char *psp_settings_file_game(const psp_settings_file *f);
int psp_settings_file_set_game(psp_settings_file *f, const char *slug, char *error);
/* An earlier app's file, read, into f for the pack pack_id: its [player]
 * where f has none, its pack's section (named or unnamed) where f has none
 * for pack_id, and its presets as [preset <pack_id>/<name>]. */
int psp_settings_file_adopt(psp_settings_file *f, const psp_settings_file *old,
                            const char *pack_id, char *error);
/* A name a section or a remembered game may use: 1-63 bytes without control
 * characters, brackets, =, ; or #, or leading or trailing spaces. */
int psp_settings_name_valid(const char *name);

/* A game's settings: the defaults, then the file's sections for the active
 * schema when path is not NULL, then the environment, resolved. There is no
 * implicit preferences file. */
int psp_settings_load(psp_settings *s, const char *path, char *error);
/* The preferences file the last psp_settings_load read, or NULL. */
const char *psp_settings_origin(void);
/* Write s back into that file (psp_settings_file_put): what the in-game
 * menu changed. The file is read again first, so what another program wrote
 * there since stays. */
int psp_settings_save_origin(const psp_settings *s, char *error);
#endif
