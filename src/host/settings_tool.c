/* The settings tool: the preferences file inspected, or a new one made,
 * headless -- no SDL, no game. Linked with one pack's settings (its
 * psp_title_settings), whose section it reads beside the player's
 * (psprecomp/host/settings.h); a pack's development scripts build it. */
#include <psprecomp/host/settings.h>

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    const char *path = NULL, *create = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") || !strcmp(argv[i], "--create-defaults")) {
            const char *key = argv[i];
            if (++i == argc) { fprintf(stderr, "%s needs a value\n", key); return 2; }
            if (!strcmp(key, "--config")) path = argv[i];
            else create = argv[i];
        } else if (!strcmp(argv[i], "--print-settings")) {
        } else if (!strcmp(argv[i], "--help")) {
            puts("settings-tool [--config FILE] [--print-settings]\n"
                 "settings-tool --create-defaults FILE\n"
                 "No preferences file is read unless --config names one.");
            return 0;
        } else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }
    char error[PSP_SETTINGS_ERROR];
    if (create) {
        /* An explicit create must not overwrite an existing preferences file. */
        struct stat st;
        if (!lstat(create, &st)) { fprintf(stderr, "already exists: %s\n", create); return 2; }
        if (errno != ENOENT) { fprintf(stderr, "%s: %s\n", create, strerror(errno)); return 2; }
        psp_settings s;
        psp_settings_play_defaults(&s);
        psp_settings_file *f = psp_settings_file_new();
        if (!f) { fprintf(stderr, "settings: out of memory\n"); return 2; }
        const int rc = psp_settings_file_put(f, &s, error) || psp_settings_file_write(f, create, error);
        psp_settings_file_free(f);
        if (rc) goto failed;
        printf("Created %s\n", create);
        return 0;
    }
    psp_settings s;
    if (psp_settings_load(&s, path, error)) goto failed;
    psp_settings_print(&s, stdout);
    return 0;
failed:
    fprintf(stderr, "settings: %s\n", error);
    return 2;
}
