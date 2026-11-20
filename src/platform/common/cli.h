/* MindMaze - command-line options of the desktop builds (Windows, Linux, macOS), shared by win32_main.c and
 * sdl_main.c. Every option is also reachable in the game (Settings, the menu, keys); the flags are for launchers and
 * scripts. Settings given as flags apply to this run only (they are not written to mindmaze.ini); a change made in the
 * game during the run (F2, Settings) is saved as usual and wins from then on.
 *
 * Actions (--add-disc, --export, --prepare-art, --install-art) run without the game window and end the program:
 * exit code 0 = done, 1 = failed (the reason on stderr), 2 = wrong usage. */
#pragma once

#ifndef MM_VERSION
#define MM_VERSION "dev"   /* the Makefile passes APP_VERSION_NAME; the release workflow sets it from the tag */
#endif

typedef struct {
    int fullscreen;              /* -1 not given, 0 --windowed, 1 --fullscreen */
    const char *home;            /* --home <dir>: where data/, mindmaze.ini and the player's files live */
    const char *disc;            /* --disc <path> (or the bare argument): the first start's Encarta disc */
    const char *add_disc;        /* --add-disc <path> */
    const char *export_zip;      /* --export <file.zip> */
    int prepare_art, install_art;
    int help, version;
    int action;                  /* any action given: no game window */
} CliOptions;

extern CliOptions cli;

/* argv[0] is the program. 1 = ok, 0 = wrong usage (err says why). Sets cli and the setting overrides. */
int cli_parse(int argc, char **argv, char *err, int errlen);
const char *cli_usage(void);

/* setting overrides from the flags: plat_get_setting asks these first; plat_set_setting drops the key's override */
int cli_setting(const char *key, char *buf, int n);
void cli_setting_forget(const char *key);

/* --add-disc, after the platform's mx_add_edition: register the edition as a question source (core, no window).
 * 1 = ok (msg = what was added), 0 = failed (msg = why). */
int cli_add_edition(const char *id, const char *name, int count, char *msg, int msglen);
