#ifndef PSPRECOMP_HOST_SETTINGS_H
#define PSPRECOMP_HOST_SETTINGS_H
/* Player settings: typed options, saved presets and their precedence.
 *
 * The mechanism is shared and the options are not. A title supplies a schema
 * -- its option table, its starter presets and a few hooks -- and every
 * launcher, game and tool built for it reads settings through this API. The
 * title's own enum indexes the table; the shared host finds the options it
 * consumes by key. No SDL: the settings tool and the tests build without it.
 *
 * Precedence is default < preset < environment < command line. Environment
 * values are never written back into a preset. */
#include <stddef.h>
#include <stdio.h>

enum psp_option_type { PSP_OPTION_CHOICE, PSP_OPTION_NUMBER, PSP_OPTION_SIZE, PSP_OPTION_INTEGER };
enum psp_settings_source {
    PSP_SOURCE_DEFAULT, PSP_SOURCE_PRESET, PSP_SOURCE_ENV, PSP_SOURCE_COMMAND_LINE
};
enum {
    PSP_SETTINGS_MAX = 40,          /* options in one schema */
    PSP_SETTINGS_TITLE_DERIVED = 8, /* derived values a title resolves itself */
    PSP_PRESETS_MAX = 32,
    PSP_SETTINGS_NAME = 64, PSP_SETTINGS_VALUE = 96, PSP_SETTINGS_ERROR = 512
};
/* A non-zero choice of this option needs the OpenGL renderer. */
#define PSP_OPTION_NEEDS_GL 0x1u

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
    /* Older spellings accepted on input and stored as the new one:
     * "old=new|old=new". */
    const char *aliases;
    unsigned flags;                 /* PSP_OPTION_NEEDS_GL */
} psp_option_def;

typedef struct {
    char value[PSP_SETTINGS_MAX][PSP_SETTINGS_VALUE];
    double number[PSP_SETTINGS_MAX];          /* Numeric value or choice index. */
    enum psp_settings_source source[PSP_SETTINGS_MAX];
    int width, height;                        /* The SIZE option's dimensions. */
    /* Derived once by psp_settings_resolve. The schema's own options decide
     * render (1 software, 2 gl, 3 null), window and realtime; the title's
     * resolve hook decides gamepad (modern buttons), input and mouse. */
    int render, gamepad, window, realtime;
    int input, mouse;
    int title[PSP_SETTINGS_TITLE_DERIVED];    /* Indexed by the title's own enum. */
} psp_settings;

typedef struct { char name[PSP_SETTINGS_NAME]; psp_settings settings; } psp_preset;
/* game: the launcher's remembered title slug. Empty in a file that predates
 * titles or that a tool wrote; never required. */
typedef struct {
    int count, selected;
    char game[PSP_SETTINGS_NAME];
    psp_preset presets[PSP_PRESETS_MAX];
} psp_presets;

/* A starter preset: its name, and space-separated KEY=value assignments over
 * the defaults. */
typedef struct { const char *name, *values; } psp_preset_def;

typedef struct {
    const char *title;              /* Names the preferences file's header. */
    const psp_option_def *options;
    int count;
    const psp_preset_def *presets;  /* What a new preferences file holds. */
    int preset_count, preset_selected;
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

/* The title's schema. Every title defines it, once, next to its option table:
 * the settings calls fall back to it until psp_settings_schema_use installs
 * another, so a program that reads settings without a title fails to link
 * rather than running with nobody's options -- and the reference pulls the
 * title's table out of a static archive. Error buffers passed to this API
 * must hold PSP_SETTINGS_ERROR bytes.
 *
 * The pull only works when the title's object comes after the mechanism on
 * the link line, or in the same archive. A title building with CMake makes its
 * schema part of the mechanism's interface, so it always follows:
 *     target_link_libraries(psprecomp_settings INTERFACE <its schema library>)
 * The 3rd Birthday's dev/CMakeLists.txt does this. */
extern const psp_settings_schema psp_title_settings;
void psp_settings_schema_use(const psp_settings_schema *schema);
const psp_settings_schema *psp_settings_active_schema(void);
/* The option's index in the schema, or -1. */
int psp_settings_find(const char *key);

void psp_settings_defaults(psp_settings *s);
int psp_settings_set(psp_settings *s, int id, const char *value,
                     enum psp_settings_source source, char *error);
/* Space-separated KEY=value pairs, all or nothing. */
int psp_settings_assign(psp_settings *s, const char *assignments,
                        enum psp_settings_source source, char *error);
int psp_settings_env(psp_settings *s, char *error);
int psp_settings_resolve(psp_settings *s, char *error);
void psp_settings_print(const psp_settings *s, FILE *out);
void psp_option_label(const psp_settings *s, int id, char *out, size_t size);
/* Installation is startup-only, before any guest/SDL threads are started.
 * Standalone fixtures that do not install a snapshot get defaults + env.
 * No mutation is supported during play. */
void psp_settings_use(const psp_settings *s);
const psp_settings *psp_settings_current(void);

void psp_presets_defaults(psp_presets *p);
int psp_presets_find(const psp_presets *p, const char *name);
int psp_presets_name_valid(const char *name);
int psp_presets_add(psp_presets *p, const char *name, const psp_settings *s,
                    char *error);
int psp_presets_load(psp_presets *p, const char *path, char *error);
int psp_presets_save(const psp_presets *p, const char *path, char *error);
/* No implicit preferences file: NULL path means defaults + env only. */
int psp_settings_load(psp_settings *s, const char *path, const char *preset,
                      char *error);
#endif
