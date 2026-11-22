/* MindMaze - command-line options of the desktop builds (see cli.h). Portable C, no platform calls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli.h"
#include "mm.h"
#include "mm_tables.h"

CliOptions cli = {-1, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0};

#define MAX_OVERRIDES 32
static struct { char key[64]; char value[512]; } g_over[MAX_OVERRIDES];
static int g_nover;

static void override(const char *key, const char *value)
{
    int i = 0;
    while (i < g_nover && strcmp(g_over[i].key, key)) i++;
    if (i == g_nover) {
        if (g_nover == MAX_OVERRIDES) return;
        g_nover++;
    }
    snprintf(g_over[i].key, sizeof g_over[i].key, "%s", key);
    snprintf(g_over[i].value, sizeof g_over[i].value, "%s", value);
}

static int same_key(const char *a, const char *b)               /* ini keys: case does not matter */
{
    for (; *a && *b; a++, b++) {
        int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

int cli_setting(const char *key, char *buf, int n)
{
    for (int i = 0; i < g_nover; i++)
        if (same_key(g_over[i].key, key)) { snprintf(buf, (size_t)n, "%s", g_over[i].value); return 1; }
    return 0;
}

void cli_setting_forget(const char *key)
{
    for (int i = 0; i < g_nover; i++)
        if (same_key(g_over[i].key, key)) { g_over[i] = g_over[--g_nover]; return; }
}

const char *cli_usage(void)
{
    return "MindMaze " MM_VERSION " - open-source port of Encarta MindMaze\n"
           "\n"
           "Usage: MindMaze [options] [disc]\n"
           "\n"
           "Game (this run only; the saved settings stay as they are):\n"
           "  -f, --fullscreen         start in full screen\n"
           "      --windowed           start in a window\n"
           "      --hd, --no-hd        HD pictures on / off (once made, see Settings > Pictures)\n"
           "      --wide, --no-wide    widescreen view on / off (once made)\n"
           "      --music <0-100>      music volume (0 = off)\n"
           "      --sound <0-100>      sound effects volume (0 = off)\n"
           "      --no-title           skip the title screen\n"
           "      --classic            Classic MindMaze: all modern options off (this run)\n"
           "      --modern             the modern options on (this run)\n"
           "      --set <key>=<value>  any mindmaze.ini setting\n"
           "      --no-update-check    do not look for a new version at start (for launchers that update)\n"
           "\n"
           "Folders:\n"
           "      --home <dir>         keep everything (data/, mindmaze.ini, players) in <dir>; default: next to\n"
           "                           the program, or the user's application data folder if that is read-only\n"
           "      --disc <path>, disc  first start: the Encarta disc image (.iso), the disc's folder or a file on\n"
           "                           it (ENCARTA.EXE, MINDMAZE.DB); no questions asked\n"
           "\n"
           "Actions (no game window; exit code 0 = done, 1 = failed, 2 = wrong usage):\n"
           "      --add-disc <path>    add the questions of another Encarta disc\n"
           "      --export <file.zip>  write the data file for the phone app\n"
           "      --prepare-art        write the kit for HD / widescreen pictures again (data/art/work)\n"
           "      --install-art        install finished HD / widescreen pictures from data/art/work\n"
           "      --update             install the newest release now (from GitHub, checksum checked)\n"
           "\n"
           "  -h, --help               this text\n"
           "      --version            the version\n";
}

static int volume(const char *opt, const char *v, int music, char *err, int errlen)
{
    char *end, s[16];
    long n = strtol(v, &end, 10);
    if (*end || end == v || n < 0 || n > 100) { snprintf(err, (size_t)errlen, "%s needs a number from 0 to 100", opt); return 0; }
    snprintf(s, sizeof s, "%ld", n);
    override(music ? "MusicVolume" : "SoundVolume", s);
    override(music ? "Music" : "Sound", n ? "1" : "0");
    return 1;
}

int cli_parse(int argc, char **argv, char *err, int errlen)
{
    *err = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        char opt[64];
        const char *eq = a[0] == '-' && a[1] == '-' ? strchr(a, '=') : NULL;   /* "--opt=value" = "--opt value" */
        if (eq) {
            if ((size_t)(eq - a) >= sizeof opt) { snprintf(err, (size_t)errlen, "unknown option %.40s...", a); return 0; }
            memcpy(opt, a, (size_t)(eq - a));
            opt[eq - a] = 0;
            a = opt;
            v = eq + 1;
        }
        int used = 0;                                                /* the option took a value */
#define VALUE()                                                                              \
        if (!v) { snprintf(err, (size_t)errlen, "%s needs a value", a); return 0; }         \
        used = 1
        if (!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "/?")) cli.help = 1;
        else if (!strcmp(a, "--version")) cli.version = 1;
        else if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) cli.fullscreen = 1;
        else if (!strcmp(a, "--windowed")) cli.fullscreen = 0;
        else if (!strcmp(a, "--hd")) override("hd_art", "1");
        else if (!strcmp(a, "--no-hd")) override("hd_art", "0");
        else if (!strcmp(a, "--wide")) override("widescreen", "1");
        else if (!strcmp(a, "--no-wide")) override("widescreen", "0");
        else if (!strcmp(a, "--no-title")) override("title_seconds", "0");
        else if (!strcmp(a, "--classic") || !strcmp(a, "--modern")) {   /* the "Classic MindMaze" switch, this run only */
            int m = !strcmp(a, "--modern");
            static const char *const K[5] = {"opt_resume", "opt_hints", "opt_prompts", "opt_bigtext", "opt_anim"};
            for (int k = 0; k < 5; k++) if (m ? k != 3 : 1) override(K[k], m ? "1" : "0");
            override("opt_layout", m ? "0" : "1");
        }
        else if (!strcmp(a, "--music")) { VALUE(); if (!volume(a, v, 1, err, errlen)) return 0; }
        else if (!strcmp(a, "--sound")) { VALUE(); if (!volume(a, v, 0, err, errlen)) return 0; }
        else if (!strcmp(a, "--set")) {
            VALUE();
            const char *e = strchr(v, '=');
            if (!e || e == v || (size_t)(e - v) >= sizeof g_over[0].key) {
                snprintf(err, (size_t)errlen, "--set needs key=value");
                return 0;
            }
            char key[64];
            memcpy(key, v, (size_t)(e - v));
            key[e - v] = 0;
            override(key, e + 1);
        }
        else if (!strcmp(a, "--home") || !strcmp(a, "--data-dir")) { VALUE(); cli.home = v; }
        else if (!strcmp(a, "--disc")) { VALUE(); cli.disc = v; }
        else if (!strcmp(a, "--add-disc")) { VALUE(); cli.add_disc = v; cli.action = 1; }
        else if (!strcmp(a, "--export")) { VALUE(); cli.export_zip = v; cli.action = 1; }
        else if (!strcmp(a, "--prepare-art")) { cli.prepare_art = 1; cli.action = 1; }
        else if (!strcmp(a, "--install-art")) { cli.install_art = 1; cli.action = 1; }
        else if (!strcmp(a, "--update")) { cli.update = 1; cli.action = 1; }
        else if (!strcmp(a, "--no-update-check")) override("update_check", "0");
        else if (!strncmp(a, "-psn_", 5)) {}                         /* old macOS Finder adds one */
        else if (a[0] == '-' && a[1]) { snprintf(err, (size_t)errlen, "unknown option %s (see --help)", a); return 0; }
        else if (!cli.disc) cli.disc = a;                            /* the bare argument: the disc */
        else { snprintf(err, (size_t)errlen, "more than one disc given: %s", a); return 0; }
#undef VALUE
        if (eq && !used) { snprintf(err, (size_t)errlen, "%s takes no value", a); return 0; }
        if (used && !eq) i++;
    }
    return 1;
}

int cli_add_edition(const char *id, const char *name, int count, char *msg, int msglen)
{
    if (!mm_tables_load(msg, msglen)) return 0;
    if (!mm_sources_init(msg, msglen)) return 0;
    int i = mm_sources_add_edition(id, name, count, msg, msglen);
    if (i < 0) return 0;
    snprintf(msg, (size_t)msglen, "Added %s, %d questions: %s. Switched on.", name, count, mm_source_info(i));
    return 1;
}
