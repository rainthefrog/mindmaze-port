/* MindMaze rebuild - shared declarations.
 *
 * The game core (mm_*.c) is portable C: it draws into a 32-bit canvas and talks to the outside world only
 * through the plat_* functions below. win32_main.c implements them for Windows; a web build can implement
 * the same set later.
 *
 * Logical coordinates are the original 636x437 MindMaze area. The canvas is that size times `scale`.
 * Function names in comments (FUN_xxxxxxxx) are the ENCARTA.EXE routines each part was ported from.
 */
#pragma once
#include <stdint.h>

#define MM_W 636
#define MM_H 437

/* ---- original data tables (loaded from data/tables.json, see mm_tables.h) ---- */
typedef struct {
    const char *name;
    int x, y;
    int rect[4];            /* click rect l,t,r,b relative to x,y (0 = none) */
    int half;               /* +0x24 */
    unsigned char slots[7]; /* +0x28 placement slots */
    unsigned char flags;    /* +0x30: 1 keeps door occupancy, 2 no floor covering */
    unsigned short id;      /* +0x32 hotspot id */
} MMObject;

typedef struct { const char *name, *image; int min, max; } MMRank;

/* ---- drawing ---- */
/* px = 0x00RRGGBB. ox: left margin in logical pixels (widescreen: the 636 px view keeps its coordinates and
 * the extensions are drawn at x -ox..-1 and 636..636+ox-1); every drawing function adds it. */
typedef struct { uint32_t *px; int w, h, stride, scale, ox; } MMCanvas;
typedef struct { int w, h; uint32_t *px; } MMImage;                     /* px = 0xAARRGGBB, A 0 = clear */

/* FONT_NAME: player name and answer letters (RCDATA 4931, Times New Roman 18), FONT_SCORE: Progress line (RCDATA 4916,
 * Arial 12), FONT_ANSWER(_I): answer buttons (Tahoma 11, centred, from a screenshot of the original; RCDATA 4929 says Times New Roman 14) */
enum { FONT_TEXT, FONT_BOLD, FONT_LIST, FONT_SMALL, FONT_NAME, FONT_LEVEL, FONT_ITALIC, FONT_POINTS, FONT_SCORE, FONT_ANSWER,
       FONT_ANSWER_I, FONT_COUNT };
enum { TX_LEFT = 0, TX_CENTER = 1, TX_RIGHT = 2, TX_VCENTER = 4, TX_WRAP = 8, TX_ELLIPSIS = 16 };

/* ---- platform interface (implemented per platform) ---- */
uint32_t plat_ticks(void);                                        /* GetTickCount */
void *plat_load(const char *relpath, int *size);                  /* file under the data folder, malloc'd */
void plat_text(MMCanvas *c, int x, int y, int w, int h, const char *utf8, int font, uint32_t rgb, int flags);
int plat_text_width(const char *utf8, int font);                  /* logical pixels */
void plat_sound(const char *name);                                /* play a sound effect (WAV name) */
void plat_music(int channel, const char *name);                   /* 0 = background, 1 = question; NULL stops */
void plat_music_pause(int channel, int pause);
void plat_music_fade(int ms);                                     /* the next plat_music(0, ...) fades over ms */
void plat_volume(int music, int sound);
/* music instruments (SoundFonts) the platform can choose from; 0 = the system's synthesizer, no choice (Windows) */
int plat_instrument_count(void);
const char *plat_instrument_name(int i);
int plat_instrument_get(void);
void plat_instrument_set(int i);                           /* 0..100 each (Settings) */
int plat_get_setting(const char *key, char *buf, int n);          /* 1 if found */
void plat_set_setting(const char *key, const char *value);        /* value NULL deletes */
void plat_open_url(const char *url);
void plat_open_data_folder(const char *rel);
/* the HD / widescreen pictures as a zip, from a file or a link (web / Android; CHANGE 2026-11-18): returns 1 where
 * the platform offers that; go = 1 opens it (later mm_pictures_reload) */
int plat_art_import(int go);
int plat_can_export(void);                                        /* 1: Settings offers "Export data for the phone app" */
void plat_export_data(void);                                      /* asks where, writes the data folder as one .zip */                     /* show a folder of the data folder (file manager) */
/* data folder files for the built-in art tools (src/art): write (creates sub-folders; 1 = ok), size (-1 = missing) */
int plat_save_data(const char *rel, const void *data, int len);
long plat_data_size(const char *rel);
void mm_pictures_reload(void);                                     /* HD / wide pictures were (re)installed */
void plat_set_cursor(int hand);
void plat_invalidate(void);
void plat_quit(void);
void plat_view_changed(void);                                     /* widescreen toggled: resize the canvas / window */
/* user files next to the exe (custom questions, online cache, seen list); data NULL from load = missing */
void *plat_load_user(const char *name, int *size);
int plat_save_user(const char *name, const void *data, int len);
void plat_append_user(const char *name, const char *text);
void plat_http_get(const char *url, int tag);                     /* async GET; result via mm_http_done */
void plat_beep(void);                                             /* MessageBeep(0) */
void mm_activate(int active);         /* platform: window activated (1) / deactivated (0) */
MMImage *plat_load_png(const char *relpath);                      /* PNG under the data folder, NULL if none */
/* precise text in canvas pixels (rich text layout): width, line height, draw one run clipped to a rect */
int plat_text_px_width(MMCanvas *c, const char *utf8, int font);
int plat_text_px_height(MMCanvas *c, int font);
int plat_text_px_ascent(MMCanvas *c, int font);                   /* tmAscent in canvas pixels (baseline placement) */
void plat_text_px(MMCanvas *c, int X, int Y, const char *utf8, int font, uint32_t rgb, int cx0, int cy0, int cx1, int cy1);

/* ---- question bank (mm_questions.c) ---- */
typedef struct {
    int id;                 /* 1-based question number */
    int area, level;
    char *text;             /* UTF-8 */
    char *answer[4];        /* answer[0] is correct */
    unsigned article[4];
    unsigned key;           /* hash of the question text (mm_question_key): seen tracking, duplicates */
    int src;                /* index of the source entry in the load spec */
} MMQuestion;

/* spec: source entries separated by '|': "original" (MINDMAZE.IDX/DB), "json:<file>" (data folder),
 * "user:<file>" (next to the exe; a missing file is just empty). Questions with the same text load once. */
int mm_questions_load(const char *spec, char *err, int errlen);
int mm_question_count(int area, int level);
const MMQuestion *mm_question_at(int area, int level, int index);
const MMQuestion *mm_question_random_in_area(int area);
int mm_questions_count_src(int src);                              /* questions loaded from spec entry src */
unsigned mm_question_key(const char *text);
/* add a question while running (custom / online); entities decoded; 0 if a question with this text exists */
int mm_questions_add(const char *text, const char *const answers[4], int area, int level, int src);
unsigned mm_questions_generation(void);                           /* changes when questions are added/loaded */

/* ---- question sources, seen questions, online top-up, custom questions (mm_sources.c) ---- */
enum { SRC_ORIGINAL, SRC_PACK, SRC_CUSTOM, SRC_ONLINE, SRC_FILE };
int mm_sources_init(char *err, int errlen);                       /* registry, settings, questions, seen list */
int mm_source_count(void);
const char *mm_source_name(int i);
const char *mm_source_info(int i);                                /* count / licence line for the settings */
int mm_source_kind(int i);
int mm_source_enabled(int i);
void mm_source_set(int i, int on);                                /* staged until mm_sources_apply */
int mm_sources_apply(char *err, int errlen);                      /* save and reload the questions */
/* questions from another Encarta disc (CHANGE 2026-11-18). Settings > "Add questions from another disc..." calls
 * plat_add_disc; the platform asks for the disc, runs mx_add_edition (data/editions/<id>/) and then, at once or
 * later (web: in a worker), calls mm_disc_added. ok 0: msg says why (msg NULL: the player cancelled). */
void plat_add_disc(void);
void mm_disc_added(int ok, const char *id, const char *name, int count, const char *msg);
void mm_disc_reading(void);                                       /* a disc was chosen and is being read */
int mm_sources_add_edition(const char *id, const char *name, int count, char *err, int errlen);   /* source index */
extern int mm_skip_seen;                                          /* skip questions a player has seen */
void mm_set_skip_seen(int on);
int mm_seen(const char *player, unsigned key);
void mm_mark_seen(const char *player, unsigned key);
void mm_seen_forget_all(void);
void mm_seen_forget(const char *player, const unsigned *keys, int n);   /* a list ran out: fresh round */
int mm_seen_total(void);
void mm_online_low(int area, int level);                          /* a pool runs low: fetch more if allowed */
int mm_online_pending(void);
int mm_custom_count(void);
int mm_custom_add(const char *text, const char *const answers[4], int area, int level, char *err, int errlen);
void mm_http_done(int tag, const char *data, int len);            /* platform: plat_http_get finished (data NULL = failed) */

/* ---- settings screen on the logon page (mm_settings.c) ---- */
int mm_settings_on(void);
void mm_settings_open(void);
void mm_settings_draw(MMCanvas *c, int mx, int my);
int mm_settings_hit(int x, int y);                                /* clickable id under (x, y), 0 = none */
void mm_settings_mouse(int x, int y, int button, int action);
void mm_settings_key(int key, int mods);
void mm_settings_char(unsigned ch);

/* ---- utility (mm_util.c) ---- */
void mm_srand(unsigned seed);
int mm_rand(void);                                                /* MSVC rand(): same sequence as the original */
MMImage *mm_image(const char *name);                              /* cached; "X.DIB" or "res/29150.bmp" */
void mm_fill(MMCanvas *c, int x, int y, int w, int h, uint32_t rgb);
void mm_frame(MMCanvas *c, int x, int y, int w, int h, int t, uint32_t rgb);   /* t px border inside */
void mm_disc(MMCanvas *c, double cx, double cy, double r, uint32_t rgb);       /* anti-aliased circle */
void mm_ellipse(MMCanvas *c, double cx, double cy, double rx, double ry, uint32_t rgb);   /* anti-aliased filled ellipse */
void mm_blit(MMCanvas *c, const MMImage *im, int x, int y);
void mm_blit_part(MMCanvas *c, const MMImage *im, int sx, int sy, int w, int h, int x, int y);
/* game pictures: the original, or its 2x HD version (data/hd/<name>.png, Codex upscale) when mm_hd_on.
 * Positions and sizes are always in original pixels; HD pictures that grew past their box are shifted by
 * their offset from data/hd/bounds.txt. */
extern int mm_hd_on;
int mm_hd_available(void);
int mm_hd_preload_step(void);
int mm_wide_preload_step(void);
void mm_draw_img(MMCanvas *c, const char *name, int x, int y);
void mm_draw_img_part(MMCanvas *c, const char *name, int sx, int sy, int w, int h, int x, int y);
/* widescreen (CHANGE, not in the original): pictures that reach the side edges of the 636 px view have wide
 * versions (data/wide/<key>.png 2x, <key>_1x.png 1x; wide.txt "key left right"; tools/mm_wide_cut.py) that
 * mm_draw_img uses while mm_wide_on. The view is then MM_W + 2 * MM_EXT wide. */
#define MM_EXT 64
extern int mm_wide_on;
int mm_wide_available(void);
int mm_view_ext(void);                                            /* MM_EXT while widescreen is on, else 0 */
char *mm_strdup(const char *s);
char *mm_cp1252_to_utf8(const unsigned char *s, int len);
void mm_plain(const char *src, char *out, int n);                 /* question text without tags */
void mm_rich(MMCanvas *c, int x, int y, int w, int h, const char *text, int font, int italic_font, uint32_t rgb, int flags);
const char *mm_str(int id);                                       /* ENCARTAR.DLL string */

/* ---- game (mm_game.c) ---- */
enum { MMB_LEFT = 1, MMB_RIGHT = 2 };
enum { MA_DOWN, MA_UP, MA_MOVE, MA_DOUBLE, MA_WHEEL };   /* MA_WHEEL: button = +1 up / -1 down */
enum { MMOD_SHIFT = 1, MMOD_CTRL = 2 };
enum { KEY_ENTER = 13, KEY_ESC = 27, KEY_BACK = 8, KEY_TAB = 9, KEY_DELETE = 127 };
enum { CMD_NEW_GAME = 1, CMD_QUIT, CMD_MUSIC, CMD_SOUND, CMD_HD, CMD_WIDE };
enum { KEY_F2 = 0x71, KEY_F3 = 0x72 };

extern int mm_music_on, mm_sound_on;
extern int mm_music_volume, mm_sound_volume;   /* 0..100, Settings (CHANGE: the original had on/off only) */
void mm_set_volume(int music, int sound);       /* clamps, applies (plat_volume) and saves */
int mm_init(char *err, int errlen);
void mm_draw(MMCanvas *c);
void mm_mouse(int x, int y, int button, int action, int mods);
void mm_key(int key, int mods);       /* VK-like: letters 'A'..'Z', digits '0'..'9', KEY_* */
void mm_char(unsigned ch);            /* typed character (player name entry) */
void mm_tick(void);                   /* call about every 50 ms */
void mm_cmd(int cmd);
void mm_music_ended(int channel);     /* platform: a music track finished */
void mm_shutdown(void);               /* save players */
