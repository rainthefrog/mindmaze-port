/* MindMaze - SDL3 platform layer (Linux, macOS): what its files share.
 *   sdl_main.c   window, drawing, input, main loop, status screen, background jobs, dialogs
 *   sdl_audio.c  one audio stream: WAV effects mixer + SoundFont music (../common/music.c)
 *   sdl_files.c  data / user files, settings (mindmaze.ini), PNG, online GET, links, data export
 *   sdl_setup.c  the data folder, first start (find or choose the Encarta disc, extract), add a disc's questions
 * Text: ../common/text.c (stb_truetype + fonts/). */
#pragma once
#include <SDL3/SDL.h>
#include <stddef.h>

/* folders, each ending in '/': home = data/ + mindmaze.ini + the player's files; res = fonts/, soundfonts/ */
extern char sdl_home[1024], sdl_data[1024], sdl_res[1024];

/* sdl_main.c */
extern SDL_Window *sdl_window;
void sdl_status(const char *title, const char *step, int pct);   /* a full-window note (pct < 0: no bar) */
/* run fn(arg) on a thread while the window shows title and the progress fn reports (sdl_progress); returns fn's
 * result. The game is not running meanwhile (start-up, or a blocking step like the Windows build's dialogs). */
int sdl_run_job(int (*fn)(void *arg), void *arg, const char *title);
void sdl_progress(int pct, const char *step);                   /* from the job's thread */
/* file dialogs, waited for (NULL = cancelled, or no dialog available: sdl_pick_failed); returns a malloc'd path */
char *sdl_pick_file(int save, const SDL_DialogFileFilter *filters, int nfilters, const char *default_location);
extern int sdl_pick_failed;
/* message box: buttons left to right (NULL = fewer); the index of the one pressed, -1 = closed / failed */
int sdl_ask(int warn, const char *title, const char *msg, const char *b0, const char *b1, const char *b2);
enum { SDL_EV_HTTP, SDL_EV_COUNT };                             /* our own events: sdl_event_base + n */
extern Uint32 sdl_event_base;

/* sdl_audio.c */
void sdl_audio_init(void);
void sdl_audio_poll(void);                                      /* finished music tracks -> mm_music_ended */

/* sdl_files.c */
void sdl_files_init(void);                                      /* after sdl_home is set: settings, fonts */
void sdl_http_done_event(void *job);                            /* SDL_EV_HTTP on the main thread */
int sdl_exists(const char *path);                               /* a file or folder */
void sdl_join(char *out, size_t n, const char *dir, const char *name);   /* dir + '/' (if missing) + name */
void *sdl_slurp(const char *path, int *size);                   /* malloc'd, zero-terminated; NULL if missing */
int sdl_write_file(const char *path, const void *data, size_t len);   /* creates the folders; 1 = ok */

/* sdl_setup.c: find the data folder, or build it on the first start (source: a disc image / folder given on the
 * command line, or NULL; home: --home or NULL; headless: an action flag runs - no window, no questions, messages on
 * stderr). 1 = sdl_home / sdl_data set, 0 = the player cancelled or it failed */
int sdl_setup(const char *source, const char *home, int headless);
int sdl_add_disc_headless(const char *path);                    /* --add-disc; 1 = added */
int sdl_export_headless(const char *zip);                       /* --export (sdl_files.c); 1 = written */

/* sdl_files.c: one GET into memory on the calling thread (the updater); progress fn(ud, bytes, total or 0) */
typedef void (*SdlProgress)(void *ud, long long now, long long total);
char *sdl_download(const char *url, size_t *len, SdlProgress fn, void *ud);   /* malloc'd; NULL on failure */

/* sdl_update.c: the built-in updater (../common/update.h). Tags of its plat_http_get (the core uses tags >= 0) */
enum { SDL_UPD_TAG_START = -1, SDL_UPD_TAG_MANUAL = -2 };
void sdl_update_start(void);                                    /* at start: look for a newer release (unless off) */
void sdl_update_offer(const char *json, int manual);            /* its answer: ask the player, update, restart */
int sdl_update_cli(void);                                       /* --update; 0 = updated or newest, 1 = failed */
extern char **sdl_argv;                                         /* main's argv: the restart after an update */
