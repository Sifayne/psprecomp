/* Player settings: the mechanism behind every title's options. See
 * include/psprecomp/host/settings.h. Generalised from Last Raven's
 * host/settings.c, which The 3rd Birthday had copied and extended; what the
 * two did differently is now the schema's to say. */
#include <psprecomp/host/settings.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const psp_settings_schema *schema;
/* The options the shared resolve reads, found by key once per schema. */
static int opt_render = -1, opt_window = -1, opt_window_mode = -1, opt_realtime = -1;

static int fail(char *error, const char *key, const char *message) {
    snprintf(error, PSP_SETTINGS_ERROR, "%s: %s", key, message);
    return -1;
}

static const psp_settings_schema *need_schema(void) {
    if (!schema) psp_settings_schema_use(&psp_title_settings);
    return schema;
}

void psp_settings_schema_use(const psp_settings_schema *s) {
    if (!s || !s->options || s->count < 1 || s->count > PSP_SETTINGS_MAX) {
        fprintf(stderr, "settings: a schema needs 1 to %d options\n", PSP_SETTINGS_MAX);
        abort();
    }
    schema = s;
    opt_render = psp_settings_find("RENDER");
    opt_window = psp_settings_find("WINDOW");
    opt_window_mode = psp_settings_find("WINDOW_MODE");
    opt_realtime = psp_settings_find("REALTIME");
}

const psp_settings_schema *psp_settings_active_schema(void) { return need_schema(); }

int psp_settings_find(const char *key) {
    const psp_settings_schema *sc = need_schema();
    for (int i = 0; i < sc->count; i++) if (!strcmp(sc->options[i].key, key)) return i;
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

static double num(const psp_settings *s, int id) { return id >= 0 ? s->number[id] : 0; }

int psp_settings_set(psp_settings *s, int id, const char *value,
                     enum psp_settings_source source, char *error) {
    const psp_settings_schema *sc = need_schema();
    if (id < 0 || id >= sc->count) return fail(error, "settings", "unknown option");
    const psp_option_def *d = &sc->options[id];
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
        if (s->bind_count >= PSP_BINDS_MAX) return fail(error, where, "at most 48 bindings per preset");
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
    const psp_settings_schema *sc = need_schema();
    memset(s, 0, sizeof *s); char error[PSP_SETTINGS_ERROR];
    for (int i = 0; i < sc->count; i++)
        if (psp_settings_set(s, i, sc->options[i].dflt, PSP_SOURCE_DEFAULT, error)) {
            fprintf(stderr, "settings: default %s\n", error);
            abort();
        }
    psp_settings_resolve(s, error);
}

int psp_settings_env(psp_settings *s, char *error) {
    const psp_settings_schema *sc = need_schema();
    psp_settings next = *s;
    for (int i = 0; i < sc->count; i++) {
        const char *v = getenv(sc->options[i].env);
        if (v && *v && psp_settings_set(&next, i, v, PSP_SOURCE_ENV, error)) return -1;
    }
    *s = next; return 0;
}

int psp_settings_resolve(psp_settings *s, char *error) {
    const psp_settings_schema *sc = need_schema();
    int enhanced = 0;
    for (int i = 0; i < sc->count; i++)
        if ((sc->options[i].flags & PSP_OPTION_NEEDS_GL) && s->number[i]) enhanced = 1;
    s->render = (int)num(s, opt_render);
    if (!s->render) s->render = enhanced ? 2 : 1; /* software=1, gl=2, null=3 */
    if (enhanced && s->render != 2)
        return fail(error, "RENDER", sc->gl_error ? sc->gl_error
                                     : "this choice requires OpenGL; choose Automatic or OpenGL");
    if (sc->resolve && sc->resolve(s, error)) return -1;
    s->window = num(s, opt_window) != 0 || num(s, opt_window_mode) != 0 || s->render == 2;
    s->realtime = s->window || num(s, opt_realtime) != 0;
    return 0;
}

void psp_option_label(const psp_settings *s, int id, char *out, size_t size) {
    const psp_option_def *d = &need_schema()->options[id];
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
    for (int i = 0; i < sc->count; i++) {
        fprintf(out, "%-24s = %-12s [%s%s]\n", sc->options[i].key, s->value[i],
                s->source[i] == PSP_SOURCE_ENV ? "environment: " :
                s->source[i] == PSP_SOURCE_PRESET ? "preset" :
                s->source[i] == PSP_SOURCE_COMMAND_LINE ? "command line" : "default",
                s->source[i] == PSP_SOURCE_ENV ? sc->options[i].env : "");
    }
    for (int i = 0; i < s->bind_count; i++)
        fprintf(out, "bind.%-19s = %-12s [preset]\n", s->bind[i].key, s->bind[i].value);
    fprintf(out, "effective: renderer=%s gamepad=%s window=%d realtime=%d",
            s->render == 2 ? "gl" : s->render == 3 ? "null" : "software",
            s->gamepad ? "modern" : "classic", s->window, s->realtime);
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

int psp_presets_find(const psp_presets *p, const char *name) {
    for (int i = 0; i < p->count; i++) if (!strcmp(p->presets[i].name, name)) return i;
    return -1;
}
int psp_presets_name_valid(const char *name) {
    size_t n = strlen(name);
    if (!n || n >= PSP_SETTINGS_NAME || isspace((unsigned char)name[0]) || isspace((unsigned char)name[n-1]))
        return 0;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++)
        if (*c < 32 || *c == 127 || strchr("[]=;#", *c)) return 0;
    return 1;
}
int psp_presets_add(psp_presets *p, const char *name, const psp_settings *s, char *error) {
    if (!psp_presets_name_valid(name))
        return fail(error, "preset name", "use 1-63 characters without brackets, =, ; or #, or leading/trailing spaces");
    if (p->count >= PSP_PRESETS_MAX) return fail(error, "presets", "at most 32 presets are supported");
    if (psp_presets_find(p, name) >= 0) return fail(error, "preset name", "already exists");
    psp_preset *v = &p->presets[p->count++]; strcpy(v->name, name); v->settings = *s;
    return 0;
}
void psp_presets_defaults(psp_presets *p) {
    const psp_settings_schema *sc = need_schema();
    memset(p, 0, sizeof *p); char error[PSP_SETTINGS_ERROR];
    for (int i = 0; i < sc->preset_count; i++) {
        psp_settings s; psp_settings_defaults(&s);
        if (psp_settings_assign(&s, sc->presets[i].values, PSP_SOURCE_PRESET, error) ||
            psp_presets_add(p, sc->presets[i].name, &s, error)) {
            fprintf(stderr, "settings: starter preset %s: %s\n", sc->presets[i].name, error);
            abort();
        }
        psp_settings_resolve(&p->presets[p->count - 1].settings, error);
    }
    p->selected = sc->preset_selected;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    return s;
}
int psp_presets_load(psp_presets *p, const char *path, char *error) {
    const psp_settings_schema *sc = need_schema();
    FILE *f = fopen(path, "r");
    if (!f) return fail(error, path, strerror(errno));
    /* On the heap: a book of presets is a third of a megabyte. */
    psp_presets *next = calloc(1, sizeof *next);
    if (!next) { fclose(f); return fail(error, path, "out of memory"); }
    char line[512], selected[PSP_SETTINGS_NAME] = "", why[PSP_SETTINGS_ERROR] = "";
    int at = -1, version = 0, lineno = 0, bad = 0;
    unsigned char seen[PSP_PRESETS_MAX][PSP_SETTINGS_MAX] = {{0}};
    while (fgets(line, sizeof line, f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) { strcpy(why, "line too long"); bad = 1; break; }
        char *v = trim(line);
        if (!*v || *v == '#' || *v == ';') continue;
        if (*v == '[') {
            size_t n = strlen(v);
            if (strncmp(v, "[preset ", 8) || n < 10 || v[n-1] != ']') { strcpy(why, "expected [preset Name]"); bad = 1; break; }
            v[n-1] = 0; psp_settings s; psp_settings_defaults(&s);
            if (psp_presets_add(next, v + 8, &s, why)) { bad = 1; break; }
            at = next->count - 1; continue;
        }
        char *eq = strchr(v, '=');
        if (!eq) { strcpy(why, "expected key=value"); bad = 1; break; }
        *eq = 0; char *key = trim(v); v = trim(eq + 1);
        if (at < 0) {
            if (!strcmp(key, "version") && !version && !strcmp(v, "1")) version = 1;
            else if (!strcmp(key, "selected") && !*selected && psp_presets_name_valid(v)) strcpy(selected, v);
            else if (!strcmp(key, "game") && !*next->game && psp_presets_name_valid(v)) strcpy(next->game, v);
            else { strcpy(why, "expected version=1, selected=Name and at most one game=Slug, before preset sections"); bad = 1; break; }
        } else {
            if (listed(sc->retired_keys, key)) continue;
            if (!strncmp(key, "bind.", 5)) {
                if (psp_settings_binding(&next->presets[at].settings, key + 5)) {
                    snprintf(why, sizeof why, "duplicate binding '%s'", key); bad = 1; break;
                }
                if (psp_settings_bind(&next->presets[at].settings, key + 5, v, why)) { bad = 1; break; }
                continue;
            }
            int id = psp_settings_find(key);
            if (id < 0) { snprintf(why, sizeof why, "unknown option '%s'", key); bad = 1; break; }
            if (seen[at][id]++) { snprintf(why, sizeof why, "duplicate option '%s'", key); bad = 1; break; }
            if (psp_settings_set(&next->presets[at].settings, id, v, PSP_SOURCE_PRESET, why)) { bad = 1; break; }
        }
    }
    if (ferror(f)) { snprintf(why, sizeof why, "read failed: %s", strerror(errno)); bad = 1; }
    fclose(f);
    if (bad) {
        snprintf(error, PSP_SETTINGS_ERROR, "%s:%d: %.300s", path, lineno, why);
        free(next); return -1;
    }
    if (!version || !next->count || (next->selected = psp_presets_find(next, selected)) < 0) {
        free(next);
        return fail(error, path, "requires version=1, at least one preset and an existing selected preset");
    }
    *p = *next; free(next); return 0;
}

int psp_presets_save(const psp_presets *p, const char *path, char *error) {
    const psp_settings_schema *sc = need_schema();
    if (p->count < 1 || p->count > PSP_PRESETS_MAX || p->selected < 0 || p->selected >= p->count)
        return fail(error, "presets", "invalid selection");
    if (*p->game && !psp_presets_name_valid(p->game)) return fail(error, "presets", "invalid game slug");
    for (int i = 0; i < p->count; i++) {
        if (!psp_presets_name_valid(p->presets[i].name) || psp_presets_find(p, p->presets[i].name) != i)
            return fail(error, "presets", "invalid or duplicate name");
        psp_settings check; psp_settings_defaults(&check);
        for (int k = 0; k < sc->count; k++)
            if (psp_settings_set(&check, k, p->presets[i].settings.value[k], PSP_SOURCE_PRESET, error)) return -1;
        if (psp_settings_resolve(&check, error)) return -1;
    }
    size_t n = strlen(path) + 16; char *tmp = malloc(n);
    if (!tmp) return fail(error, path, "out of memory");
    snprintf(tmp, n, "%s.tmp.XXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) { free(tmp); return fail(error, path, strerror(errno)); }
    FILE *f = fdopen(fd, "w");
    if (!f) { int e = errno; close(fd); unlink(tmp); free(tmp); return fail(error, path, strerror(e)); }
    fprintf(f, "# %s player settings. Environment overrides are never saved.\nversion=1\nselected=%s\n",
            sc->title ? sc->title : "Player", p->presets[p->selected].name);
    if (*p->game) fprintf(f, "game=%s\n", p->game);
    for (int i = 0; i < p->count; i++) {
        fprintf(f, "\n[preset %s]\n", p->presets[i].name);
        for (int k = 0; k < sc->count; k++)
            fprintf(f, "%s=%s\n", sc->options[k].key, p->presets[i].settings.value[k]);
        for (int k = 0; k < p->presets[i].settings.bind_count; k++)
            fprintf(f, "bind.%s=%s\n", p->presets[i].settings.bind[k].key, p->presets[i].settings.bind[k].value);
    }
    int bad = ferror(f), saved_errno = errno;
    if (fflush(f) || fsync(fd)) { bad = 1; saved_errno = errno; }
    if (fclose(f)) { bad = 1; saved_errno = errno; }
    if (!bad && rename(tmp, path)) { bad = 1; saved_errno = errno; }
    if (bad) { unlink(tmp); fail(error, path, strerror(saved_errno)); }
    free(tmp); return bad ? -1 : 0;
}

int psp_settings_load(psp_settings *s, const char *path, const char *preset, char *error) {
    psp_settings next; psp_settings_defaults(&next);
    if (preset && !path) return fail(error, "--preset", "requires --config");
    if (path) {
        psp_presets *p = malloc(sizeof *p);
        if (!p) return fail(error, path, "out of memory");
        if (psp_presets_load(p, path, error)) { free(p); return -1; }
        int at = preset ? psp_presets_find(p, preset) : p->selected;
        if (at < 0) { free(p); return fail(error, preset, "preset does not exist"); }
        next = p->presets[at].settings;
        free(p);
    }
    if (psp_settings_env(&next, error) || psp_settings_resolve(&next, error)) return -1;
    *s = next; return 0;
}
