/* MindMaze game core, ported from ENCARTA.EXE (Encarta 2006). See SPEC.md / NOTES.md for the analysis.
 * Each routine names the original function it follows. Deliberate differences are marked CHANGE.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "mm.h"
#include "mm_tables.h"

#define NONE 0x28                 /* "no floor / ceiling / character" marker used by the original */
#define NPLAYERS 5
#define SEC_START 66              /* question clock, q[0xf] = 0x42 in FUN_00522030 */

/* ------------------------------------------------------------------------------------------------ */
/* data structures (sizes/offsets of the originals are in SPEC.md section 1)                         */

typedef struct {
    int wall, floor, ceil, chr;       /* +0 +4 +8 +0xc */
    unsigned char doors;              /* +0x10: bit0 N, bit1 S, bit2 E, bit3 W, bit4 stairs */
    unsigned char opened;             /* +0x18 */
    unsigned long long furnA, furnB;  /* +0x20 / +0x28 furniture bitsets */
    int visited;                      /* +0x30 */
} Room;

typedef struct {
    int w, h, cx, cy, sx, sy, tx, ty;
    Room *grid[100];
    int reveal;                       /* +0x2c, a match is burning */
    int match[5];                     /* +0x30.. match still available */
} Maze;

typedef struct { int tab[4][20]; int cnt[4]; } AreaPick;   /* per-player "All Areas" tables, list object +0x324/+0x644 */

typedef struct {
    char name[64];
    int slot, saved_n, score, award, level, areabtn;
    Maze *maze;
    AreaPick pick;
    int ans, right, streak;                /* this game (CHANGE 2026-11-21: shown in the touch layout) */
} Player;

typedef struct {                      /* per slot list, globals 0x6c20c0.. in the original */
    int *list, n, pos, area, level;
    int req_level;                    /* CHANGE: level asked for (level = the one used when that one is empty) */
    unsigned gen;                     /* question bank generation the list was built from */
} SlotList;

typedef struct {
    const MMQuestion *q;
    int order[4];                     /* button -> original answer index (q+0x30) */
    int seconds;                      /* q[0xf] */
    int tried;                        /* panel +100 */
    int disabled[4];
    int dir;                          /* game+0xd4 */
    int timer_on;
    uint32_t next_tick;
    int hint;                         /* CHANGE 2026-11-21: the 50/50 hint was used (points halved) */
} Question;

enum { SCR_LOGON, SCR_GRATING, SCR_DOORS, SCR_ROOM };

/* ------------------------------------------------------------------------------------------------ */
static Player *g_pl[NPLAYERS];
static int g_cur = 6;                 /* game+0xb8, 6 = none */
static SlotList g_slot[NPLAYERS];
static Question g_q;
static int g_qopen;                   /* game+0xd0 != 0 */
static int g_screen = SCR_LOGON;
int mm_music_on = 1, mm_sound_on = 1;
int mm_music_volume = 100, mm_sound_volume = 100;
/* mute (CHANGE 2026-11-21, user: "a mute button, left corner"): a speaker button in the picture's top left corner, on every
 * screen; silences music and sounds without touching the volume levels (Settings), saved as "Muted" */
static int g_mute;
static void apply_volume(void) { plat_volume(g_mute ? 0 : mm_music_volume, g_mute ? 0 : mm_sound_volume); }
static void beep(void) { if (!g_mute) plat_beep(); }                  /* (silent while muted) */

void mm_set_volume(int music, int sound)
{
    mm_music_volume = music < 0 ? 0 : music > 100 ? 100 : music;
    mm_sound_volume = sound < 0 ? 0 : sound > 100 ? 100 : sound;
    char v[8];
    snprintf(v, sizeof v, "%d", mm_music_volume);
    plat_set_setting("MusicVolume", v);
    snprintf(v, sizeof v, "%d", mm_sound_volume);
    plat_set_setting("SoundVolume", v);
    apply_volume();
}

static int g_speech;                  /* speech overlay shown (mode 0) */
static char g_speech_text[2048];
static int g_atrium_text;             /* room +0x8c */
/* pop-up text (CHANGE 2026-11-21, Animations; user: "instead of type write lets do line fade ins - starting with the top
 * and the following lines"): the lines of a speech's page fade in one after another, top first; a tap (or Next /
 * Close) while they appear shows them all - it does not turn the page. Pages (draw_overlay): the text keeps its font
 * and is shown page by page with Previous / Next; a speech always has its buttons, Next becomes Close on the last
 * page (also a one-page speech). g_ov_lines = the lines of the page shown, from the last drawing. */
static uint32_t g_type_t0;
static int g_type_done = 1;
static int g_ov_page, g_ov_pages = 1, g_ov_lines = 64;    /* the pop-up's page, its pages, the lines on the page */
static int g_ov_btn[2][4], g_ov_btn_on;                   /* Previous / Next (room units), drawn now */
enum { LINE_DELAY = 250, LINE_STEP = 240, LINE_FADE = 380 };   /* ms: after the pop-up, between lines, one line's fade */
static double line_alpha(int l)                           /* 0..1: how far line l of the page has faded in */
{
    if (g_type_done || !mm_opt.anim) return 1;
    int ms = (int)(uint32_t)(plat_ticks() - g_type_t0) - LINE_DELAY - l * LINE_STEP;
    return ms <= 0 ? 0 : ms >= LINE_FADE ? 1 : (double)ms / LINE_FADE;
}
static int typing(void)                                   /* lines still fading in */
{
    if (g_type_done || !mm_opt.anim) return 0;
    int ms = (int)(uint32_t)(plat_ticks() - g_type_t0);
    if (ms < LINE_DELAY + (g_ov_lines - 1) * LINE_STEP + LINE_FADE) return 1;
    g_type_done = 1;
    plat_invalidate();                                     /* (one more frame with the whole page) */
    return 0;
}
static void type_start(int delay)                         /* a new speech (delay) or a new page (at once) */
{
    g_type_t0 = plat_ticks() - (delay ? 0 : LINE_DELAY);
    g_type_done = !mm_opt.anim;
    g_ov_lines = 64;
}
static void ov_reset(void) { g_ov_page = 0; g_ov_pages = 1; g_ov_btn_on = 0; }
static uint32_t g_match_end, g_next_amb;
static int g_music_state;
static int g_intro_on;              /* INTRO.MID is the background track right now (CHANGE below) */
static int g_inactive;                /* window deactivated (mm_activate) */
static int g_mx = -1, g_my = -1;
static int g_fixed_seed;              /* setting "seed": repeatable games for testing */
/* title screen (CHANGE, user 2026-11-18: the original showed TITLESCR.DIB before the logon screen; the decomp does
 * not show where): drawn over the logon screen at start until the time is up, a click or a key. INTRO.MID already
 * plays. Seconds from the setting title_seconds (0 = no title screen). */
static int g_title_on;
static uint32_t g_title_until;
#define TITLE_SECONDS_DEFAULT 5             /* user 2026-11-18: "a few seconds" */
static int g_hover = -1;            /* clickable target under the pointer (clickable_at) */
static int clickable_at(int x, int y);
static uint32_t g_panel_bg = 0xD9D6C5;

/* hotspots of the room view */
typedef struct { int x, y, w, h, id; const MMObject *o; } Hot;   /* o: the object picture (CHANGE: its outline glows on touch) */
static const MMObject *g_drawn[160];   /* the room's objects in drawing order (room_hots), for the glint */
static int g_ndrawn;
static const int DOOR_HOT[5][4] = {{0, 0, 125, 300}, {349, 51, 76, 191}, {197, 76, 59, 108}, {501, 32, 118, 245}, {200, 232, 237, 69}};
static Hot g_hot[64];
static int g_nhot;

/* message boxes (the original uses MessageBox and continues after it returns) */
typedef void (*MsgCb)(int yes, int arg);
typedef struct { char text[1024]; int yesno; MsgCb cb; int arg; } Msg;
static Msg g_msg[8];
static int g_nmsg;

/* stories (FUN_0052fd80 / FUN_0052fec0, table 0x6b55e0) */
static struct { int on, kind, page, npages; const char *pages[2]; } g_story;

/* logon */
typedef struct { char name[64]; int n, score, award, sel, tourist; } LogonEntry;
static LogonEntry g_lg[64];
static int g_nlg, g_lg_scroll, g_lg_edit;
static char g_resume_names[400];        /* a saved game's players ("" = none): the gate offers Continue */
static int g_lg_mp;                     /* CHANGE 2026-11-21: touch layout "Several players" (else a tap picks one) */
/* a colour per rank (Tourist and ranks 1-5), for the touch layout's small rank marks */
static const uint32_t RANK_COL[6] = {0xA8A498, 0xB0703A, 0xA9ABAE, 0xD4A017, 0x6C3FA0, 0x972938};
static int touch_view(void);
static int lg_modes(void);
static int g_col_areas;                 /* touch column: the area picker is open (instead of the map) */
static uint32_t g_taphint_until;        /* CHANGE 2026-11-21: touch layout, the room's tappable spots are outlined until */
static uint32_t g_shimmer_next;
static void taphint(void) { g_taphint_until = plat_ticks() + 1600; g_shimmer_next = plat_ticks() + 400; }   /* (glints soon) */
static char g_lg_text[64];

static void next_turn(void);
static void dialog_anim(void);
static void stats_now(void);
static void screen_anim(int fx, int fy);
static void show_logon(void);
static void draw_text(MMCanvas *c, int x, int y, int w, int h, const char *s, int font, uint32_t rgb, int fl)
{
    plat_text(c, x, y, w, h, s, font, rgb, fl);
}

/* ------------------------------------------------------------------------------------------------ */
/* message boxes                                                                                     */

static void msgbox(const char *text, int yesno, MsgCb cb, int arg)
{
    if (g_nmsg >= 8) return;
    if (!g_nmsg) dialog_anim();                                            /* CHANGE: animations */
    Msg *m = &g_msg[g_nmsg++];
    snprintf(m->text, sizeof m->text, "%s", text);
    m->yesno = yesno;
    m->cb = cb;
    m->arg = arg;
    plat_invalidate();
}

static void msg_close(int yes)
{
    if (!g_nmsg) return;
    Msg m = g_msg[0];
    memmove(g_msg, g_msg + 1, sizeof(Msg) * (--g_nmsg));
    if (g_nmsg) dialog_anim();                                           /* the next message pops up too */
    if (m.cb) m.cb(yes, m.arg);
    plat_invalidate();
}

/* ------------------------------------------------------------------------------------------------ */
/* sound and music (FUN_0051adb0 / FUN_0051b660 / 0x51b770)                                          */

static void sound(int id)                              /* FUN_0051b660; id = index into the WAV table 0x6b41d0 */
{
    if (mm_sound_on && id >= 0 && id < 35) plat_sound(mm_sound_files[id]);
}

/* The sound object (FUN_0051ad60, game+0x90): +8 sound on, +0xc music on, +0x14 state, +0x18 background
 * mode, +4 current loop, +6 next loop, +0x20 ambient timer running, +0x24 ambient allowed. The original drives
 * three Windows Media Player controls: effects, music (channel 0 here, game+0xa8) and question music
 * (channel 1, game+0xac). The core keeps each channel's play state so the state machine below can test it
 * like the original tests the players' playState. */
enum { CH_STOPPED, CH_PLAYING, CH_PAUSED };
static struct { int st; const char *name; } g_ch[2];
static struct { int mode, loop, next, amb_running, amb_allowed; } g_snd;

static void ch_play(int ch, const char *name) { g_ch[ch].name = name; g_ch[ch].st = CH_PLAYING; plat_music(ch, name); }
static void ch_stop(int ch) { if (g_ch[ch].st != CH_STOPPED) plat_music(ch, NULL); g_ch[ch].st = CH_STOPPED; }
static void ch_pause(int ch) { if (g_ch[ch].st == CH_PLAYING) { plat_music_pause(ch, 1); g_ch[ch].st = CH_PAUSED; } }
static void ch_resume(int ch)                          /* controls.play (FUN_004d23d0): resume, or restart a stopped track */
{
    if (g_ch[ch].st == CH_PAUSED) { plat_music_pause(ch, 0); g_ch[ch].st = CH_PLAYING; }
    else if (g_ch[ch].st == CH_STOPPED && g_ch[ch].name) ch_play(ch, g_ch[ch].name);
}

/* ambient sounds: panel timer 2 */
static void amb_start(void)                                                /* FUN_0051b7d0 */
{
    if (g_screen != SCR_LOGON && !g_snd.amb_running && mm_sound_on && g_snd.amb_allowed) {
        g_next_amb = plat_ticks() + (mm_rand() % 30 + 8) * 1000u;
        g_snd.amb_running = 1;
    }
}
static void amb_kill(void) { g_snd.amb_running = 0; }                      /* FUN_0051b830 */
static void amb_fire(void)                                                 /* timer 2 -> 0x51b770 */
{
    amb_kill();
    if (mm_sound_on) { sound(mm_rand() % 23); amb_start(); }              /* sound off: not re-armed */
}

static void play_loop(int n)                                               /* FUN_0051b340 */
{
    ch_stop(1);
    ch_stop(0);
    ch_play(0, mm_music_files[n]);                                         /* BGLOOP1..3 */
    g_intro_on = 0;
    g_snd.mode = n;
    g_snd.loop = n;
}

static void bg_next(void)                                                  /* FUN_0051b0d0 */
{
    switch (g_snd.mode) {
    case 0: play_loop(mm_rand() % 3 + 1); break;
    case 1: case 2: case 3: {                                              /* FUN_0051b4f0 */
        int r = g_snd.loop;
        while (r == g_snd.loop) r = mm_rand() % 3 + 1;
        /* The original now plays the transition "<from>-<to>.mid" and starts loop r when that ends (mode 4).
         * The 2006 disc ships no transition files; the Encarta 95-98 discs do (1-2.mid ... 3-2.mid, copied into
         * the data folder when the player adds such a disc). Without the file loop r follows at once.
         * (The original played it on the question player; here on the background channel, whose end event
         * then starts loop r - the same order.) */
        g_snd.mode = 4;
        g_snd.next = r;
        static char tr[16];                                         /* the name outlives the call */
        snprintf(tr, sizeof tr, "%d-%d.mid", g_snd.loop, r);
        if (plat_data_size(tr) > 0) { ch_stop(1); ch_stop(0); ch_play(0, tr); g_intro_on = 0; }
        else play_loop(r);
        break;
    }
    case 4: play_loop(g_snd.next); break;
    }
}

/* FUN_0051adb0: the music state machine, run after the state (+0x14) is set */
static void music_update(int s)
{
    g_music_state = s;
    switch (s) {
    case 0: ch_stop(0); ch_stop(1); break;
    case 1:                                                                /* logon */
        ch_stop(0);
        ch_stop(1);
        g_intro_on = mm_music_on;
        if (mm_music_on) ch_play(0, mm_music_files[0]);                    /* INTRO.MID, once */
        g_music_state = 2;                                                 /* its end starts a background loop */
        break;
    case 2:
        if (mm_music_on) bg_next();
        if (g_snd.mode) amb_start();
        break;
    case 3:                                                                /* question */
        if (mm_music_on) { ch_pause(0); ch_play(1, mm_music_files[4]); }  /* QUESTION.MID */
        amb_kill();
        break;
    case 4: ch_stop(1);                                                    /* question closed; falls through */
        /* fall through */
    case 6:                                                                /* resume */
        if (mm_music_on) {
            if (g_ch[1].st == CH_PAUSED) { ch_resume(1); g_music_state = 3; }
            else { ch_resume(0); g_music_state = 2; if (g_snd.mode) amb_start(); }
        }
        break;
    case 5:                                                                /* pause */
        if (mm_music_on) { ch_pause(0); ch_pause(1); }
        amb_kill();
        break;
    }
    if (!mm_music_on) { ch_stop(0); ch_stop(1); g_snd.mode = 0; }
    if (!mm_sound_on) { g_snd.mode = 0; amb_kill(); }
}

/* FUN_0051c190 sets state 5 before drawing a room and 6 after it. Pausing and resuming the MIDI around a
 * redraw would only make the sequencer stutter here, so the net effect of the pair is applied: the ambient
 * timer is stopped and restarted only when music is on and a background loop has been started (+0x18). */
static void room_redraw_music(void)
{
    amb_kill();
    if (mm_music_on) {
        if (g_ch[1].st == CH_PLAYING) g_music_state = 3;
        else { ch_resume(0); g_music_state = 2; if (g_snd.mode) amb_start(); }
    }
    if (!mm_music_on || !mm_sound_on) g_snd.mode = 0;
}

/* Media player end events 0x4f52b0 (music) / 0x4f52f0 (question music), event 0xbc4 */
void mm_music_ended(int channel)
{
    if (channel < 0 || channel > 1) return;
    g_ch[channel].st = CH_STOPPED;
    if (!mm_music_on) return;
    if (g_music_state == 2) bg_next();                                     /* INTRO or a loop ended: next loop */
    else if (g_music_state == 3) music_update(3);                         /* QUESTION.MID loops */
}

/* ------------------------------------------------------------------------------------------------ */
/* rooms: FUN_0052f890 (decoration), FUN_0052fbc0 (conflict), FUN_0052fc60 (mark)                   */

static const MMObject *obj(int group, int i) { return &mm_objects[group + i]; }

static int conflict(const MMObject *o, const unsigned char occ[7])
{
    for (int i = 0; i < 7; i++)
        if (occ[i] == 1 && o->slots[i] == 1) return 1;
    return 0;
}

static void mark(const MMObject *o, unsigned char occ[7])
{
    for (int i = 0; i < 7; i++)
        if (o->slots[i] == 1) occ[i] = 1;
}

/* CHANGES (user, 2026-11-17), not in the original FUN_0052f890:
 *  - avoid: characters of already decorated rooms connected to this one through a door; the character is picked
 *    again when it matches one (the original repeats the neighbour's character in 5.3% of moves);
 *  - door occupancy is kept when furniture is placed (the original clears it at 0x52fa2b when the character's
 *    flag bit 0 is clear, so in 55% of rooms with a side door furniture was drawn over that door). */
static int avoided(int chr, const int *avoid, int navoid)
{
    for (int i = 0; i < navoid; i++) if (avoid[i] == chr) return 1;
    return 0;
}

/* paintings never overlap (CHANGE 2026-11-21, user): the slot table lets a wall painting (PIC1-x, PIC2-x) share the
 * wall with a character or a cupboard that reaches over it (in 24% of rooms: QUEEN, KING1, DUKE2, SUITARM1/2,
 * ASIANTRA, FURNITUR; test_room_overlap). A painting is not placed where its visible pixels meet those of anything
 * already in the room, and nothing is placed over a painting. Pixel test from the pictures' transparency, cached per
 * pair; without pictures (no data yet) nothing overlaps, as before. */
static int is_painting(const MMObject *o) { return !strncmp(o->name, "PIC1-", 5) || !strncmp(o->name, "PIC2-", 5); }
static int pix_overlap(const MMObject *a, const MMObject *b)
{
    static signed char memo[256][256];
    int ia = (int)(a - mm_objects), ib = (int)(b - mm_objects);
    if (ia < 0 || ib < 0 || ia >= 256 || ib >= 256) return 0;
    if (memo[ia][ib]) return memo[ia][ib] > 0;
    MMImage *A = mm_image(a->name), *B = mm_image(b->name);
    int hit = 0;
    if (A && B) {
        int x0 = a->x > b->x ? a->x : b->x, y0 = a->y > b->y ? a->y : b->y;
        int x1 = a->x + A->w < b->x + B->w ? a->x + A->w : b->x + B->w, y1 = a->y + A->h < b->y + B->h ? a->y + A->h : b->y + B->h;
        for (int y = y0; y < y1 && !hit; y++)
            for (int x = x0; x < x1 && !hit; x++)
                hit = (A->px[(y - a->y) * A->w + x - a->x] >> 24) > 128 && (B->px[(y - b->y) * B->w + x - b->x] >> 24) > 128;
    }
    memo[ia][ib] = memo[ib][ia] = hit ? 1 : -1;
    return hit;
}
static int painting_clash(const Room *r, const MMObject *o)  /* o would overlap a painting, or is one over something */
{
    const MMObject *L[80];
    int n = 0;
    for (int d = 0; d < 5; d++) if (r->doors & (1 << d)) L[n++] = obj(MM_G_DOORS, d);
    for (int f = 0; f < MM_N_FURNITURE; f++) if ((r->furnA | r->furnB) >> f & 1) L[n++] = obj(MM_G_FURNITURE, f);
    if (r->chr != NONE && MM_G_CHARACTERS + r->chr < MM_N_OBJECTS) L[n++] = obj(MM_G_CHARACTERS, r->chr);
    for (int i = 0; i < n; i++)
        if (L[i] != o && (is_painting(o) || is_painting(L[i])) && pix_overlap(o, L[i])) return 1;
    return 0;
}

/* the same painting on the other wall (PIC1-n <-> PIC2-n), -1 = none (user 2026-11-21: "the painting could have moved
 * sides" instead of overlapping) */
static int other_side(int f)
{
    const MMObject *o = obj(MM_G_FURNITURE, f);
    if (!is_painting(o)) return -1;
    char want[16];
    snprintf(want, sizeof want, "%s", o->name);
    want[3] = want[3] == '1' ? '2' : '1';
    for (int g = 0; g < MM_N_FURNITURE; g++) if (!strcmp(obj(MM_G_FURNITURE, g)->name, want)) return g;
    return -1;
}
static int place_pick(Room *r, int f, const unsigned char occ[7])   /* f, or its other side when f would overlap */
{
    const MMObject *o = obj(MM_G_FURNITURE, f);
    if (!is_painting(o) || !painting_clash(r, o)) return f;
    int g = other_side(f);
    return g >= 0 && !conflict(obj(MM_G_FURNITURE, g), occ) && !painting_clash(r, obj(MM_G_FURNITURE, g)) ? g : f;
}
/* a room from a saved game (laid out before this rule): an overlapping painting moves to the other wall, or goes */
static void room_fix_paintings(Room *r)
{
    for (int f = 0; f < MM_N_FURNITURE; f++) {
        unsigned long long bit = 1ull << f, *set = (r->furnA & bit) ? &r->furnA : (r->furnB & bit) ? &r->furnB : NULL;
        if (!set || !is_painting(obj(MM_G_FURNITURE, f))) continue;
        *set &= ~bit;                                       /* (tested without itself) */
        if (!painting_clash(r, obj(MM_G_FURNITURE, f))) { *set |= bit; continue; }
        int g = other_side(f);
        if (g >= 0 && !((r->furnA | r->furnB) >> g & 1) && !painting_clash(r, obj(MM_G_FURNITURE, g))) *set |= 1ull << g;
    }
}

static void decorate(Room *r, int force, const int *avoid, int navoid)
{
    if (r->doors & 0x10) { r->floor = r->ceil = r->chr = NONE; r->wall = 0; return; }
    r->wall = mm_rand() % MM_N_WALLS;
    r->floor = mm_rand() % MM_N_FLOORS;
    r->ceil = mm_rand() % MM_N_CEILINGS;

    unsigned char occ[7] = {0};
    if (r->doors & 4) occ[3] = 1;
    if (r->doors & 8) occ[2] = 1;
    occ[4] = (r->doors & 0x10) != 0;
    occ[6] = occ[4] || (r->doors & 1);
    int ndoors = 0;
    for (int b = 0; b < 5; b++) ndoors += (r->doors >> b) & 1;

    if (ndoors < 2 && !force) r->chr = NONE;
    else {
        r->chr = NONE;
        for (int t = 25; t && r->chr == NONE; t--) {
            r->chr = mm_rand() % MM_N_CHARACTERS;
            if (conflict(obj(MM_G_CHARACTERS, r->chr), occ) || avoided(r->chr, avoid, navoid)) r->chr = NONE;
        }
        if (r->chr == NONE) {
            unsigned char occ2[7] = {1, 0, 0, 0, 0, 0, 0};
            for (int t = 25; t && r->chr == NONE; t--) {
                r->chr = mm_rand() % MM_N_CHARACTERS;
                if (conflict(obj(MM_G_CHARACTERS, r->chr), occ2) || avoided(r->chr, avoid, navoid)) r->chr = NONE;
            }
            for (int c = 3; r->chr == NONE && c < 3 + MM_N_CHARACTERS; c++)   /* original: 3; skip a neighbour's */
                if (!avoided(c % MM_N_CHARACTERS, avoid, navoid)) r->chr = c % MM_N_CHARACTERS;
        }
    }
    /* The original reads record chr even for NONE (0x28): character table + 40 = furniture 18 (PIC1-3).
     * Kept, it decides the furniture layout of rooms without a character. */
    static const MMObject no_object;                         /* tables too short for that read: nothing marked */
    const MMObject *C = MM_G_CHARACTERS + r->chr < MM_N_OBJECTS ? obj(MM_G_CHARACTERS, r->chr) : &no_object;
    if (C->flags & 2) r->floor = NONE;
    /* (original: if (!(C->flags & 1)) memset(occ, 0, 7); - door occupancy kept, see above) */
    mark(C, occ);

    if (occ[0] == 0) {                                       /* one item in front of the character */
        occ[1] = 1;
        for (int t = 25; t; t--) {
            int f = mm_rand() % MM_N_FURNITURE;
            f = place_pick(r, f, occ);
            if (!conflict(obj(MM_G_FURNITURE, f), occ) && !painting_clash(r, obj(MM_G_FURNITURE, f))) {
                mark(obj(MM_G_FURNITURE, f), occ);
                r->furnB |= 1ull << f;
                break;
            }
        }
    }
    for (int pass = 0; pass < 5; pass++) {                   /* up to five items behind */
        occ[0] = 1;
        occ[1] = 0;
        for (int t = 25; t; t--) {
            int f = mm_rand() % MM_N_FURNITURE;
            f = place_pick(r, f, occ);
            if (!conflict(obj(MM_G_FURNITURE, f), occ) && !painting_clash(r, obj(MM_G_FURNITURE, f))) {
                mark(obj(MM_G_FURNITURE, f), occ);
                if (!(r->furnA & (1ull << f))) { r->furnA |= 1ull << f; break; }
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* maze: FUN_0051d390(10, 10, 40)                                                                    */

static Room *room_at(Maze *m, int x, int y)
{
    if (x < 0 || y < 0 || x >= m->w || y >= m->h) return NULL;
    return m->grid[y * m->w + x];
}

static const int DX[4] = {0, 0, 1, -1}, DY[4] = {-1, 1, 0, 0};   /* 0 N, 1 S, 2 E, 3 W */
static const int OPP[4] = {1, 0, 3, 2};

static void maze_free(Maze *m)
{
    if (!m) return;
    for (int i = 0; i < 100; i++) free(m->grid[i]);
    free(m);
}

static Maze *maze_new(void)
{
    Maze *m = calloc(1, sizeof *m);
    int R = 40;
    m->w = m->h = 10;
    int pct = R * 100 / (m->w * m->h);
    m->sx = mm_rand() % m->w;
    m->sy = m->h - 1;
    m->grid[m->sy * m->w + m->sx] = calloc(1, sizeof(Room));
    int cx = m->sx, cy = m->sy, steps = 0;
    while (R > 0) {
        int freed[4], n = 0;
        for (int d = 0; d < 4; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            freed[d] = nx >= 0 && ny >= 0 && nx < m->w && ny < m->h && !m->grid[ny * m->w + nx];
            n += freed[d];
        }
        if (n > 0 && (n > 2 || steps < 5 || mm_rand() % 100 >= pct)) {
            int d;
            do d = mm_rand() % 4; while (!freed[d]);
            Room *a = m->grid[cy * m->w + cx], *b = calloc(1, sizeof(Room));
            cx += DX[d];
            cy += DY[d];
            m->grid[cy * m->w + cx] = b;
            a->doors |= 1 << d;
            b->doors |= 1 << OPP[d];
            R--;
            steps++;
        } else {
            do { cx = mm_rand() % m->w; cy = mm_rand() % m->h; } while (!m->grid[cy * m->w + cx]);
            steps = 0;
        }
    }
    if (cx == m->sx || cy == m->sy) {                           /* too close to the start */
        for (int i = 0; i < 100; i++)
            if (m->grid[i]) { cx = i % m->w; cy = i / m->w; break; }
    }
    while (m->grid[cy * m->w + cx]->doors & 1) cy--;            /* stairs room never has a north door */
    m->grid[cy * m->w + cx]->doors |= 0x10;
    m->tx = cx;
    m->ty = cy;
    m->cx = m->sx;
    m->cy = m->sy;
    m->grid[m->sy * m->w + m->sx]->visited = 1;
    int done[100] = {0};
    for (int i = 0; i < 100; i++) {
        if (!m->grid[i]) continue;
        int avoid[4], navoid = 0, x = i % m->w, y = i / m->w;     /* decorated neighbours through a door */
        for (int d = 0; d < 4; d++) {
            Room *o = (m->grid[i]->doors & (1 << d)) ? room_at(m, x + DX[d], y + DY[d]) : NULL;
            if (o && done[(y + DY[d]) * m->w + x + DX[d]] && o->chr != NONE) avoid[navoid++] = o->chr;
        }
        decorate(m->grid[i], i == m->sy * m->w + m->sx, avoid, navoid);
        done[i] = 1;
    }
    m->reveal = 0;
    for (int i = 0; i < 5; i++) m->match[i] = 1;
    return m;
}

/* ------------------------------------------------------------------------------------------------ */
/* question picking: FUN_00522a30 (build list), FUN_00522c10 (take), FUN_005226a0 (All Areas),      */
/* FUN_0052f080 / FUN_0052f120 (area tables). The original's "used" bitmap is never set, so it is    */
/* left out: questions repeat once a list has been cycled through, as in the original.               */

static Player *player_of_slot(int slot)
{
    for (int i = 0; i < NPLAYERS; i++) if (g_pl[i] && g_pl[i]->slot == slot) return g_pl[i];
    return NULL;
}

/* CHANGE (question sources, mm_sources.c): an (area, level) without questions uses the nearest level that has
 * some (packs without a level 4); with "skip seen" on, questions the player has seen are left out until all of
 * them are seen (then the whole list again, like the original's cycling); when fewer than 10 unseen are left
 * the online top-up is asked for more. Only the original questions and skip seen off: as the original. */
static void list_build(int level, int slot, int area)                     /* FUN_00522a30 */
{
    if (slot > 4 || area > 8 || level > 4) return;
    SlotList *s = &g_slot[slot];
    s->area = area;
    s->req_level = level;
    if (mm_question_count(area, level) == 0)
        for (int d = 1; d < 4; d++) {
            if (level - d >= 0 && mm_question_count(area, level - d)) { level -= d; break; }
            if (level + d < 4 && mm_question_count(area, level + d)) { level += d; break; }
        }
    s->level = level;
    s->gen = mm_questions_generation();
    if (!g_fixed_seed) mm_srand(plat_ticks() % 1000);
    free(s->list);
    int total = mm_question_count(area, level);
    s->list = malloc(sizeof(int) * (total + 1));
    s->n = 0;
    Player *pl = mm_skip_seen ? player_of_slot(slot) : NULL;
    for (int i = 0; i < total; i++)
        if (!pl || !mm_seen(pl->name, mm_question_at(area, level, i)->key)) s->list[s->n++] = i;
    if (s->n < 10) mm_online_low(area, s->req_level);
    if (pl && s->n == 0 && total > 0) {
        /* all seen: start over. CHANGE (user, 2026-11-18): the player's seen marks for this area and level are
         * forgotten too, except the newest quarter, so the new round shows every question once before any repeats
         * and the ones seen last come back last */
        unsigned *keys = malloc(sizeof *keys * (size_t)total);
        if (keys) {
            for (int i = 0; i < total; i++) keys[i] = mm_question_at(area, level, i)->key;
            mm_seen_forget(pl->name, keys, total);
            free(keys);
        }
        for (int i = 0; i < total; i++)
            if (!mm_seen(pl->name, mm_question_at(area, level, i)->key)) s->list[s->n++] = i;
        if (s->n == 0) for (int i = 0; i < total; i++) s->list[s->n++] = i;
    }
    for (int i = 0; i < s->n; i++) {
        int j = mm_rand() % s->n, t = s->list[j];
        s->list[j] = s->list[i];
        s->list[i] = t;
    }
    s->pos = 0;
}

static const MMQuestion *list_take(int slot)                               /* FUN_00522c10 */
{
    SlotList *s = &g_slot[slot];
    /* CHANGE: rebuilt when questions were added (online top-up, custom) or, with skip seen, when it wraps */
    if (s->list && (s->gen != mm_questions_generation() || (mm_skip_seen && s->pos + 1 >= s->n)))
        list_build(s->req_level, slot, s->area);
    if (!s->list || s->n == 0) return NULL;
    if (++s->pos >= s->n) s->pos = 0;
    return mm_question_at(s->area, s->level, s->list[s->pos]);
}

static void areapick_init(AreaPick *p)                                     /* FUN_0052f080(-1) */
{
    for (int l = 0; l < 4; l++) {
        for (int a = 0; a < 9; a++) p->tab[l][a] = a;
        p->cnt[l] = 9;
    }
}

static const MMQuestion *pick_all_areas(Player *pl)                        /* FUN_005226a0 */
{
    for (;;) {
        AreaPick *p = &pl->pick;
        int l = pl->level;
        if (p->cnt[l] == 0) return NULL;
        int r = mm_rand() % p->cnt[l];
        int area = p->tab[l][r];
        if (area == -1) return NULL;
        list_build(l, pl->slot, area);
        const MMQuestion *q = list_take(pl->slot);
        if (!q) {                                                          /* FUN_0052f120: drop the area */
            int i;
            for (i = 0; i < p->cnt[l] && p->tab[l][i] != area; i++) {}
            if (i != p->cnt[l]) { p->tab[l][i] = p->tab[l][p->cnt[l] - 1]; p->cnt[l]--; }
            continue;
        }
        return q;
    }
}

static const int AREA_OF_BUTTON[10] = {-1, 1, 2, 3, 0, 4, 5, 6, 7, 8};    /* FUN_00520270 */

static void area_level_changed(Player *pl)                                 /* FUN_00520270 */
{
    if (pl->areabtn != 0) list_build(pl->level, pl->slot, AREA_OF_BUTTON[pl->areabtn]);
}

/* ------------------------------------------------------------------------------------------------ */
/* players and persistence (FUN_0051b880, FUN_0051bcb0, FUN_0051bb80, FUN_0051bf40, FUN_0051be60)  */

static Player *player_new(const char *name, int slot)
{
    Player *p = calloc(1, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    p->slot = slot;
    p->maze = maze_new();
    areapick_init(&p->pick);
    return p;
}

static int is_tourist(const char *name) { return !strcmp(name, mm_str(29043)); }

static void save_player(Player *p)
{
    if (is_tourist(p->name)) return;
    char key[32], val[128];
    if (!p->saved_n) {
        for (int n = 1;; n++) {
            snprintf(key, sizeof key, "MindmazePlayer%u", n);
            if (!plat_get_setting(key, val, sizeof val) || !strcmp(val, "deleted")) { p->saved_n = n; break; }
        }
    }
    snprintf(key, sizeof key, "MindmazePlayer%u", p->saved_n);
    snprintf(val, sizeof val, "%u,%u,%s", p->score, p->award, p->name);
    plat_set_setting(key, val);
}

static void free_players(void)                                             /* FUN_004f3e30 */
{
    for (int i = 0; i < NPLAYERS; i++)
        if (g_pl[i]) { save_player(g_pl[i]); maze_free(g_pl[i]->maze); free(g_pl[i]); g_pl[i] = NULL; }
    g_cur = 6;
}

static Player *cur(void) { return g_cur < NPLAYERS ? g_pl[g_cur] : NULL; }
static Room *cur_room(void)
{
    Player *p = cur();
    return p ? room_at(p->maze, p->maze->cx, p->maze->cy) : NULL;
}

/* ------------------------------------------------------------------------------------------------ */
/* Continue unfinished games (CHANGE 2026-11-21, Modern options; the original kept only score and rank). The game in
 * progress is written to the user file savegame.txt after every turn and when the app goes to the background, and
 * removed when it ends (New Game, the ending). At the castle gate the game offers to continue it. An open question
 * is not saved: the game goes on in the same room, the question not asked yet.
 *   MindMaze savegame 1
 *   cur <slot>
 *   player <slot> <saved_n> <score> <award> <level> <areabtn> <reveal> <m0> <m1> <m2> <m3> <m4> <cx> <cy> <sx> <sy> <tx> <ty>\t<name>
 *   room <slot> <index 0..99> <wall> <floor> <ceil> <chr> <doors> <opened> <furnA hex> <furnB hex> <visited>       */
#define SAVEGAME "savegame.txt"

static void game_save(void)
{
    if (!mm_opt.resume || g_screen != SCR_ROOM) return;
    size_t cap = 64 * 1024, n = 0;
    char *b = malloc(cap);
    if (!b) return;
    n += (size_t)snprintf(b + n, cap - n, "MindMaze savegame 1\ncur %d\n", g_cur);
    for (int s = 0; s < NPLAYERS; s++) {
        Player *p = g_pl[s];
        if (!p) continue;
        Maze *m = p->maze;
        n += (size_t)snprintf(b + n, cap - n, "player %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\t%s\n", s, p->saved_n,
                              p->score, p->award, p->level, p->areabtn, m->reveal, m->match[0], m->match[1], m->match[2],
                              m->match[3], m->match[4], m->cx, m->cy, m->sx, m->sy, m->tx, m->ty, p->name);
        for (int i = 0; i < 100 && n + 200 < cap; i++) {
            Room *r = m->grid[i];
            if (!r) continue;
            n += (size_t)snprintf(b + n, cap - n, "room %d %d %d %d %d %d %d %d %llx %llx %d\n", s, i, r->wall, r->floor, r->ceil,
                                  r->chr, r->doors, r->opened, r->furnA, r->furnB, r->visited);
        }
    }
    plat_save_user(SAVEGAME, b, (int)n);
    free(b);
}

static void game_forget(void) { plat_save_user(SAVEGAME, "", 0); g_resume_names[0] = 0; }

/* the saved game's players into g_pl (1 = ok); the logon list is not touched */
static int game_load(int apply, char *names, size_t nn)
{
    int size = 0;
    char *d = plat_load_user(SAVEGAME, &size);
    if (!d || size < 20 || strncmp(d, "MindMaze savegame 1", 19)) { free(d); return 0; }
    Player *pl[NPLAYERS] = {0};
    int cur = -1, ok = 1;
    if (names) names[0] = 0;
    for (char *line = d, *next; ok && line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        int s, i, v[18];
        unsigned long long fa, fb;
        if (sscanf(line, "cur %d", &cur) == 1) continue;
        if (!strncmp(line, "player ", 7)) {
            char *tab = strchr(line, '\t');
            if (!tab || sscanf(line + 7, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &s, &v[0], &v[1], &v[2], &v[3],
                               &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15],
                               &v[16]) != 18 || s < 0 || s >= NPLAYERS || pl[s] || v[3] < 0 || v[3] > 3 || v[4] < 0 ||
                v[4] > 9 || v[2] < 0 || v[2] > 5) { ok = 0; break; }
            for (int k = 11; k <= 16; k++) if (v[k] < 0 || v[k] > 9) ok = 0;
            if (!ok) break;
            Player *p = calloc(1, sizeof *p);
            Maze *m = calloc(1, sizeof *m);
            if (!p || !m) { free(p); free(m); ok = 0; break; }
            snprintf(p->name, sizeof p->name, "%s", tab + 1);
            p->slot = s;
            p->saved_n = v[0]; p->score = v[1]; p->award = v[2]; p->level = v[3]; p->areabtn = v[4];
            m->w = m->h = 10;
            m->reveal = 0;                                        /* (a burning match does not survive a restart) */
            for (int k = 0; k < 5; k++) m->match[k] = v[6 + k] != 0;
            m->cx = v[11]; m->cy = v[12]; m->sx = v[13]; m->sy = v[14]; m->tx = v[15]; m->ty = v[16];
            p->maze = m;
            areapick_init(&p->pick);
            pl[s] = p;
            if (names) snprintf(names + strlen(names), nn - strlen(names), "%s%s", names[0] ? ", " : "", p->name);
        } else if (!strncmp(line, "room ", 5)) {
            int w, f, c2, ch, dr, op, vis;
            if (sscanf(line + 5, "%d %d %d %d %d %d %d %d %llx %llx %d", &s, &i, &w, &f, &c2, &ch, &dr, &op, &fa, &fb, &vis) != 11 ||
                s < 0 || s >= NPLAYERS || !pl[s] || i < 0 || i >= 100 || pl[s]->maze->grid[i] || w < 0 || w >= MM_N_WALLS ||
                (f != NONE && (f < 0 || f >= MM_N_FLOORS)) || (c2 != NONE && (c2 < 0 || c2 >= MM_N_CEILINGS)) ||
                (ch != NONE && (ch < 0 || ch >= MM_N_CHARACTERS)) || dr < 0 || dr > 31) { ok = 0; break; }
            Room *r = calloc(1, sizeof *r);
            if (!r) { ok = 0; break; }
            r->wall = w; r->floor = f; r->ceil = c2; r->chr = ch; r->doors = (unsigned char)dr; r->opened = (unsigned char)op;
            r->furnA = fa; r->furnB = fb; r->visited = vis;
            room_fix_paintings(r);                         /* (rooms saved before paintings were kept clear) */
            pl[s]->maze->grid[i] = r;
        }
    }
    free(d);
    int any = 0;
    for (int s = 0; s < NPLAYERS && ok; s++) {
        if (!pl[s]) continue;
        any = 1;
        Maze *m = pl[s]->maze;
        if (!room_at(m, m->cx, m->cy) || !room_at(m, m->tx, m->ty) || !room_at(m, m->sx, m->sy)) ok = 0;
    }
    if (!any || cur < 0 || cur >= NPLAYERS || !pl[cur]) ok = 0;
    if (!ok || !apply) {
        for (int s = 0; s < NPLAYERS; s++) if (pl[s]) { maze_free(pl[s]->maze); free(pl[s]); }
        return ok;
    }
    free_players();                                            /* (none at the castle gate; to be safe) */
    for (int s = 0; s < NPLAYERS; s++) g_pl[s] = pl[s];
    g_cur = cur;
    return 1;
}

static void enter_castle(void);
static void on_resume_box(int yes, int arg)
{
    (void)arg;
    g_resume_names[0] = 0;
    if (!yes) { game_forget(); return; }
    if (!game_load(1, NULL, 0)) { game_forget(); return; }
    g_screen = SCR_ROOM;
    if (mm_music_on && g_music_state == 2 && g_intro_on) { plat_music_fade(2000); bg_next(); }
    enter_castle();
}

/* at the castle gate: a saved game -> "continue?" */
static int g_no_offer;                                     /* (Quit Game: no offer right after) */
static void resume_refresh(void)
{
    g_resume_names[0] = 0;
    if (mm_opt.resume && !game_load(0, g_resume_names, sizeof g_resume_names)) g_resume_names[0] = 0;
}
static void offer_resume(void)
{
    char names[400];
    resume_refresh();
    if (g_no_offer || touch_view()) return;                /* (touch layout: the gate's Continue button) */
    if (!mm_opt.resume || !game_load(0, names, sizeof names)) return;
    char t[600];
    snprintf(t, sizeof t, "Continue the unfinished game of %s?\n\n(No starts fresh: the saved game is dropped.)", names);
    msgbox(t, 1, on_resume_box, 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* game flow                                                                                         */

/* ------------------------------------------------------------------------------------------------ */
/* animations (CHANGE 2026-11-21, Modern options > Animations; off = nothing moves, as the original). Each kind has one
 * slot: start time, length, two arguments. mm_animating tells the platform to draw at the display's rate.           */
enum { AN_ROOM, AN_FADE, AN_FEEDBACK, AN_SCORE, AN_DIALOG, AN_ANSWERS, AN_RANK, AN_CONFETTI, AN_POPUP, AN_SCREEN, AN_SHIMMER, AN_N };
static struct { uint32_t t0; int dur, on, a, b; } g_an[AN_N];

static void anim_start(int k, int dur, int a, int b)
{
    if (!mm_opt.anim) return;
    g_an[k].t0 = plat_ticks();
    g_an[k].dur = dur;
    g_an[k].on = 1;
    g_an[k].a = a;
    g_an[k].b = b;
    plat_invalidate();
}

static double anim_t(int k)                               /* 0..1 while running; 1 when done or off */
{
    if (!g_an[k].on || !mm_opt.anim) return 1;
    double t = (double)(uint32_t)(plat_ticks() - g_an[k].t0) / g_an[k].dur;
    return t >= 1 ? 1 : t < 0 ? 0 : t;
}
static void dialog_anim(void) { anim_start(AN_DIALOG, 220, 0, 0); }
static int anim_on(int k) { return mm_opt.anim && g_an[k].on && anim_t(k) < 1; }
static double ease(double t) { double u = 1 - t; return 1 - u * u * u; }   /* fast start, soft end */

static int match_burning(void) { Player *p = cur(); return g_screen == SCR_ROOM && p && p->maze->reveal; }

int mm_animating(void)
{
    if (!mm_opt.anim) return 0;
    for (int k = 0; k < AN_N; k++) if (anim_on(k)) return 1;
    if (g_speech && typing()) return 1;                    /* the words are being typed */
    if (g_taphint_until && touch_view() && (int32_t)(g_taphint_until - plat_ticks()) > 0) return 1;   /* the glow fades */
    return match_burning();                                /* the map's glow flickers while a match burns */
}

static void anim_tick(void)                               /* from mm_tick: finished ones off, running ones redrawn */
{
    int any = 0;
    for (int k = 0; k < AN_N; k++) {
        if (!g_an[k].on) continue;
        if (anim_t(k) >= 1) g_an[k].on = 0;
        any = 1;                                           /* (one last frame after the end) */
    }
    if (any || (mm_opt.anim && match_burning())) plat_invalidate();
}

/* rgb over (x, y, w, h) with alpha 0..255 (logical coordinates, like mm_fill) */
static void fill_alpha(MMCanvas *c, int x, int y, int w, int h, uint32_t rgb, int a)
{
    if (a <= 0) return;
    if (a >= 255) { mm_fill(c, x, y, w, h, rgb); return; }
    x += c->ox;
    int s = c->scale, x0 = x * s, y0 = y * s, x1 = (x + w) * s, y1 = (y + h) * s;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    uint32_t R = rgb >> 16 & 255, G = rgb >> 8 & 255, B = rgb & 255;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *p = c->px + (size_t)yy * c->stride;
        for (int xx = x0; xx < x1; xx++) {
            uint32_t v = p[xx];
            uint32_t r = ((v >> 16 & 255) * (255 - a) + R * a) / 255, g = ((v >> 8 & 255) * (255 - a) + G * a) / 255,
                     b = ((v & 255) * (255 - a) + B * a) / 255;
            p[xx] = r << 16 | g << 8 | b;
        }
    }
}

/* fade-in of something drawn over the picture: snap() keeps the pixels of (x, y, w, h) before it is drawn, unsnap()
 * mixes them back with weight 1 - t, so the new drawing appears with opacity t (works with any text renderer,
 * as it only reads the finished pixels) */
typedef struct { uint32_t *px; int X0, Y0, X1, Y1; } Snap;
static Snap snap(MMCanvas *c, int x, int y, int w, int h)
{
    Snap s = {0};
    x += c->ox;
    int sc = c->scale;
    s.X0 = x * sc < 0 ? 0 : x * sc;
    s.Y0 = y * sc < 0 ? 0 : y * sc;
    s.X1 = (x + w) * sc > c->w ? c->w : (x + w) * sc;
    s.Y1 = (y + h) * sc > c->h ? c->h : (y + h) * sc;
    if (s.X1 <= s.X0 || s.Y1 <= s.Y0) return s;
    s.px = malloc(sizeof(uint32_t) * (size_t)(s.X1 - s.X0) * (size_t)(s.Y1 - s.Y0));
    if (!s.px) return s;
    for (int Y = s.Y0; Y < s.Y1; Y++)
        memcpy(s.px + (size_t)(Y - s.Y0) * (s.X1 - s.X0), c->px + (size_t)Y * c->stride + s.X0, sizeof(uint32_t) * (size_t)(s.X1 - s.X0));
    return s;
}
static void unsnap(MMCanvas *c, Snap *s, double t)
{
    if (!s->px) return;
    int a = (int)(t * 256), W = s->X1 - s->X0;
    if (a < 256)
        for (int Y = s->Y0; Y < s->Y1; Y++) {
            uint32_t *p = c->px + (size_t)Y * c->stride + s->X0, *o = s->px + (size_t)(Y - s->Y0) * W;
            for (int X = 0; X < W; X++) {
                uint32_t n = p[X], b = o[X];
                uint32_t r = ((n >> 16 & 255) * a + (b >> 16 & 255) * (256 - a)) >> 8;
                uint32_t g = ((n >> 8 & 255) * a + (b >> 8 & 255) * (256 - a)) >> 8;
                uint32_t bl = ((n & 255) * a + (b & 255) * (256 - a)) >> 8;
                p[X] = r << 16 | g << 8 | bl;
            }
        }
    free(s->px);
    s->px = NULL;
}

static void question_close(void)                                           /* FUN_00522470 */
{
    g_qopen = 0;
    g_q.timer_on = 0;
    music_update(4);                                                       /* FUN_0051fcd0(panel, 0) */
}

static void move(int dir)                                                  /* FUN_0051d900 */
{
    Maze *m = cur()->maze;
    Room *a = room_at(m, m->cx, m->cy), *b = room_at(m, m->cx + DX[dir], m->cy + DY[dir]);
    if (!b) return;
    m->cx += DX[dir];
    m->cy += DY[dir];
    sound(25 + mm_rand() % 5);                                             /* FOOT1..FOOT5 */
    anim_start(AN_ROOM, 340, dir, 0);                                     /* CHANGE: the next room slides in */
    taphint();
    a->visited = b->visited = 1;
}

/* CHANGE 2026-11-21 (Modern options > Turn prompts): with two or more players, who plays next is said in a box, so a
 * phone or tablet can be handed on */
static void turn_prompt(int prev)
{
    int n = 0;
    for (int i = 0; i < NPLAYERS; i++) n += g_pl[i] != NULL;
    if (!mm_opt.prompts || n < 2 || g_cur == prev || !cur()) return;
    char t[160];
    snprintf(t, sizeof t, "%s, it's your turn!", cur()->name);
    msgbox(t, 0, NULL, 0);
}

static void next_turn(void)                                                /* FUN_004f3670 / FUN_004f3da0 */
{
    int prev = g_cur;
    for (int i = 1; i <= NPLAYERS; i++) {
        int s = (g_cur + i) % NPLAYERS;
        if (g_pl[s]) { g_cur = s; break; }
    }
    g_speech = 0;
    room_redraw_music();                                                   /* FUN_0051c190 */
    Player *p = cur();
    if (p && p->maze->reveal) { sound(32); p->maze->reveal = 0; }        /* FUN_004f3750 */
    /* CHANGE: the original means to say 29049 "Click the stairs to continue your quest." here but a bug
     * (0x4f36d0) keeps it from ever showing. Shown as intended. */
    Room *r = cur_room();
    if (r && (r->doors & 0x10) && p->award < 5) msgbox(mm_str(29049), 0, NULL, 0);
    turn_prompt(prev);
    game_save();                                                           /* CHANGE: continue unfinished games */
    plat_invalidate();
}

static void on_correct_box(int yes, int points)
{
    (void)yes;
    Player *p = cur();
    anim_start(AN_SCORE, 900, p->score, 0);                                /* CHANGE: the score counts up */
    p->score += points;
    save_player(p);                                     /* CHANGE: saved at once (the original only when a game ended) */
    Maze *m = p->maze;
    room_at(m, m->cx, m->cy)->opened |= 1 << g_q.dir;                      /* FUN_0052fcf0 */
    move(g_q.dir);
    room_at(m, m->cx, m->cy)->opened |= 1 << OPP[g_q.dir];                 /* FUN_0052fd10 */
    next_turn();
}

static void on_wrong1_box(int yes, int button)
{
    (void)yes;
    g_q.disabled[button] = 1;
    g_q.tried = 1;
    g_q.timer_on = 1;                                                      /* FUN_00522640 */
    g_q.next_tick = plat_ticks() + 1000;
}

static void on_wrong2_box(int yes, int arg)
{
    (void)yes;
    (void)arg;
    question_close();          /* FUN_0051f750: the question is deleted only after this box (banner and music stay) */
    next_turn();
}

/* points for the open question: (level + 1) * seconds left (the original), halved after the 50/50 hint */
static int q_points(void)
{
    Player *p = cur();
    int pts = p ? (p->level + 1) * g_q.seconds : 0;
    return g_q.hint ? pts / 2 : pts;
}

/* CHANGE 2026-11-21 (Modern options > Hints): 50/50 - two wrong answers are taken away (one when fewer than four are
 * still open, after a first wrong try), once per question; the question is then worth half its points and there is
 * only one chance left (user 2026-11-21): a wrong answer after the hint ends the question */
static void hint_5050(void)
{
    if (!g_qopen || !mm_opt.hints || g_q.hint || g_nmsg) return;
    int wrong[4], n = 0, open = 0;
    for (int b = 0; b < 4; b++) {
        open += !g_q.disabled[b];
        if (g_q.order[b] != 0 && !g_q.disabled[b]) wrong[n++] = b;
    }
    for (int k = 0; k < (open >= 4 ? 2 : 1) && n > 1; k++) {   /* (one wrong answer always stays) */
        int i = mm_rand() % n;
        g_q.disabled[wrong[i]] = 1;
        wrong[i] = wrong[--n];
    }
    g_q.hint = 1;
    sound(31);                                                             /* the match sound: a small "pff" */
    plat_invalidate();
}

static void feedback(int button, int right) { anim_start(AN_FEEDBACK, 900, button, right); }

static void answer(int button)                                             /* FUN_0051f750 */
{
    if (!g_qopen || button < 0 || button > 3 || g_q.disabled[button]) return;
    Player *p = cur();
    char buf[1024];
    if (g_q.order[button] == 0) {
        sound(33);                                                         /* RIGHT */
        int points = q_points();
        mm_stats_answer(p->name, g_q.q->area, p->level, g_q.tried ? 2 : 1, g_q.seconds);
        p->ans++; p->right++; p->streak++;
        feedback(button, 1);
        question_close();
        snprintf(buf, sizeof buf, mm_str(points != 1 ? 29041 : 29057), points);
        msgbox(buf, 0, on_correct_box, points);
    } else if (!g_q.tried && !g_q.hint) {                                /* (after the hint: no second try) */
        sound(34);                                                         /* WRONG */
        feedback(button, 0);
        g_q.timer_on = 0;                                                  /* FUN_00522620 */
        msgbox(mm_str(29039), 0, on_wrong1_box, button);
    } else {
        sound(34);
        g_q.timer_on = 0;
        mm_stats_answer(p->name, g_q.q->area, p->level, 0, g_q.seconds);
        p->ans++; p->streak = 0;
        feedback(button, 2);                                               /* and the right one */
        char fmt[256];
        snprintf(fmt, sizeof fmt, "%s", mm_str(29040));
        char *s = strstr(fmt, "%s");
        if (s) s[1] = 's';
        char plain[512];
        mm_plain(g_q.q->answer[0], plain, sizeof plain);
        snprintf(buf, sizeof buf, fmt, plain);
        msgbox(buf, 0, on_wrong2_box, 0);
    }
    plat_invalidate();
}

static void on_no_question_box(int yes, int dir)
{
    (void)yes;
    move(dir);                                                             /* FUN_0051ccb0 falls through to the move */
    next_turn();
}

/* returns 0 when no question could be picked (the message box is then queued) */
static int start_question(int dir)                                         /* FUN_00522030 */
{
    Player *p = cur();
    int order[4];
    unsigned used = 0;
    for (int k = 0; k < 4; k++) {                  /* the answers are shuffled first, before the question is picked */
        int b;
        do b = mm_rand() % 4; while (used & (1u << b));
        used |= 1u << b;
        order[b] = k;
    }
    const MMQuestion *q = p->areabtn == 0 ? pick_all_areas(p) : list_take(p->slot);
    if (!q) {
        /* FUN_00522030 returns 0 after this box and FUN_0051ccb0 then moves the player through the door
         * anyway (no question, door not marked open) and passes the turn */
        msgbox("You have already answered all of the questions for this selection.", 0, on_no_question_box, dir);
        return 0;
    }
    mm_mark_seen(p->name, q->key);                                         /* CHANGE: seen list (mm_sources.c) */
    memset(&g_q, 0, sizeof g_q);
    g_q.q = q;
    memcpy(g_q.order, order, sizeof order);
    g_q.seconds = SEC_START;
    g_q.dir = dir;
    g_q.timer_on = 1;
    g_q.next_tick = plat_ticks() + 1000;
    anim_start(AN_ANSWERS, 320, 0, 0);                                     /* CHANGE: the answers slide in */
    anim_start(AN_POPUP, 320, 0, 0);                                       /* CHANGE: the question banner fades in */
    g_qopen = 1;
    ov_reset();                                            /* (the banner text starts on its first page) */
    g_speech = 0;
    music_update(3);                                                       /* FUN_0051fcd0(panel, q) */
    plat_invalidate();
    return 1;
}

static void door_click(int dir)                                            /* FUN_0051ccb0 */
{
    Room *r = cur_room();
    if (g_qopen || !(r->doors & (1 << dir))) return;
    if (r->opened & (1 << dir)) { move(dir); next_turn(); }
    else start_question(dir);
}

static void rerank(Player *p)                                              /* FUN_0051c9f0 */
{
    for (int i = 0; i < 6; i++)
        if (p->score >= mm_ranks[i].min && p->score <= mm_ranks[i].max) {
            if (i != p->award) { p->award = i; anim_start(AN_RANK, 2200, 0, 0); msgbox("Congratulations!  You now have a new award!", 0, NULL, 0); }
            return;
        }
}

static void stairs(void)                                                   /* FUN_0051c940 */
{
    Player *p = cur();
    rerank(p);
    save_player(p);                                                        /* CHANGE: the new rank is saved at once */
    maze_free(p->maze);
    p->maze = maze_new();
    room_redraw_music();                                                   /* FUN_0051c190 */
    anim_start(AN_FADE, 650, 0, 0);                                        /* CHANGE: fade into the new maze */
    taphint();
    mm_stats_stairs(p->name);
    game_save();
    plat_invalidate();
}

static void ending_closed(void)                                            /* FUN_004f3840 */
{
    Player *p = cur();
    if (p && p->award == 5) { p->score = 0; p->award = 0; }
    game_forget();                                                         /* the game is over */
    show_logon();
}

static void open_story(int kind)                                           /* FUN_0052fec0 */
{
    memset(&g_story, 0, sizeof g_story);
    g_story.on = 1;
    anim_start(AN_DIALOG, 220, 0, 0);
    g_story.kind = kind;
    if (kind == 0) { g_story.pages[0] = "OPEN1.DIB"; g_story.pages[1] = "OPEN2.DIB"; g_story.npages = 2; }
    else {
        anim_start(AN_CONFETTI, 4500, 0, 0);                              /* CHANGE: the castle is won */
        static char name[16];
        snprintf(name, sizeof name, "END%d.DIB", 1 + mm_rand() % 6);
        g_story.pages[0] = name;
        g_story.npages = 1;
    }
    plat_invalidate();
}

static void close_story(void)                                              /* FUN_00530090 */
{
    int kind = g_story.kind;
    g_story.on = 0;
    if (kind == 1) ending_closed();
    plat_invalidate();
}

static void show_speech(int id)                                            /* FUN_0051ce70 / FUN_005308c0 */
{
    int k = id - 7;
    if (k < 0 || k >= 23 || !mm_speech_count[k]) return;
    const char *t = mm_speech[k][mm_rand() % mm_speech_count[k]];
    char *o = g_speech_text;
    for (; *t && o < g_speech_text + sizeof g_speech_text - 1; t++) {
        if (t[0] == '\\' && t[1] == 'n') { *o++ = '\n'; t++; }            /* limericks use a literal \n */
        else *o++ = *t;
    }
    *o = 0;
    g_speech = 1;
    anim_start(AN_POPUP, 320, 0, 0);                                       /* CHANGE: the speech fades in */
    type_start(1);                                         /* CHANGE (Animations, user): the words are typed out */
    ov_reset();
    plat_invalidate();
}

static char g_url_tmpl[256] = "https://en.wikipedia.org/wiki/Special:Search?search=%s";

/* CHANGE: "refer to Encarta" opens a web reference instead (setting reference_url, %s = search text) */
static void open_reference(const char *text)
{
    char q[512], url[1024], *o = q;
    for (const unsigned char *s = (const unsigned char *)text; *s && o < q + sizeof q - 4; s++) {
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '.')
            *o++ = (char)*s;
        else o += sprintf(o, "%%%02X", *s);
    }
    *o = 0;
    snprintf(url, sizeof url, g_url_tmpl, q);
    plat_open_url(url);
}

static void on_refer_box(int yes, int button)
{
    if (yes && g_qopen) {
        char plain[512];
        mm_plain(g_q.q->answer[g_q.order[button]], plain, sizeof plain);
        open_reference(plain);
    }
    if (g_qopen) { g_q.timer_on = 1; g_q.next_tick = plat_ticks() + 1000; }
}

static void refer(int button)                                              /* cmd 29150..29153 */
{
    if (!g_qopen || button < 0 || button > 3) return;
    g_q.timer_on = 0;
    msgbox(mm_str(29042), 1, on_refer_box, button);
}

static void on_picture_box(int yes, int area)
{
    if (!yes) return;
    const MMQuestion *q = mm_question_random_in_area(area);
    char plain[512];
    mm_plain(q ? q->answer[0] : mm_picture_area[0], plain, sizeof plain);
    open_reference(plain);
}

static const int PIC_AREA[9] = {1, 2, 0, 4, 5, 6, 7, 8, 3};                 /* picture id 31.. -> DB area */

static void room_click(int id)                                             /* 0x51c680 */
{
    if (!cur()) return;
    if (g_qopen) { beep(); return; }                                  /* 0x51c6fc: MessageBeep(0) */
    if (id >= 0 && id <= 3) door_click(id);
    else if (id == 4) stairs();
    else if (id >= 7 && id <= 28) show_speech(id);
    else if (id == 0x1E) { show_speech(29); g_atrium_text = 1; }
    else if (id >= 31 && id <= 39) {
        char buf[256];
        snprintf(buf, sizeof buf, "Would you like to see an article about %s?", mm_picture_area[id - 31]);
        msgbox(buf, 1, on_picture_box, PIC_AREA[id - 31]);
    }
}

static void match(int i)                                                   /* 0x51e250 */
{
    Player *p = cur();
    /* the test is this player's own map flag (maze+0x2c), not a global "a match burns": the next player can
     * light one while the previous player's 8 s timer runs; SetTimer(1) then restarts it */
    if (!p || !p->maze->match[i] || p->maze->reveal) return;
    sound(31);
    p->maze->reveal = 1;
    p->maze->match[i] = 0;
    g_match_end = plat_ticks() + 8000;
    plat_invalidate();
}

static void enter_castle(void)                                             /* FUN_00521e60 */
{
    g_screen = SCR_ROOM;
    taphint();
    anim_start(AN_FADE, 700, 0, 0);                          /* into the castle: from black */
    room_redraw_music();                       /* FUN_0051c190 */
    g_snd.amb_allowed = 1;                     /* sound object +0x24 */
    amb_start();                               /* FUN_0051b7d0 */
    plat_invalidate();
}

/* ------------------------------------------------------------------------------------------------ */
/* logon (FUN_00520860, 0x520cc0, FUN_00520ad0)                                                       */

static void show_logon(void)
{
    free_players();
    g_screen = SCR_LOGON;
    g_qopen = 0;
    g_speech = 0;
    g_atrium_text = 0;
    g_match_end = 0;
    g_nlg = 0;
    g_lg_edit = 0;
    g_lg_scroll = 0;
    char key[32], val[160];
    for (int n = 1; g_nlg < 63; n++) {
        snprintf(key, sizeof key, "MindmazePlayer%u", n);
        if (!plat_get_setting(key, val, sizeof val)) break;
        if (!strcmp(val, "deleted")) continue;
        unsigned sc = 0, aw = 0;
        int used = 0;
        if (sscanf(val, "%u,%u,%n", &sc, &aw, &used) < 2 || !used) continue;
        LogonEntry *e = &g_lg[g_nlg++];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", val + used);
        e->n = n;
        e->score = (int)sc;
        e->award = aw > 5 ? 5 : (int)aw;
    }
    LogonEntry *t = &g_lg[g_nlg++];
    memset(t, 0, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", mm_str(29043));
    t->tourist = 1;
    music_update(1);                                                       /* logon OnCreate 0x521070: INTRO.MID */
    offer_resume();                                                        /* CHANGE: an unfinished game */
    plat_invalidate();
}

static int logon_commit_name(void);

static void logon_start(void)                                              /* FUN_00520ad0 / FUN_004f3970 */
{
    if (g_lg_edit && !logon_commit_name()) return;   /* FUN_00520ad0 commits a name being typed first (FUN_005215d0) */
    int nsel = 0;
    for (int i = 0; i < g_nlg; i++) nsel += g_lg[i].sel;
    if (nsel == 0) { msgbox(mm_str(29044), 0, NULL, 0); return; }
    if (nsel > NPLAYERS) { msgbox(mm_str(29045), 0, NULL, 0); return; }
    game_forget();                                         /* a new game replaces an unfinished one */
    free_players();                                        /* (a restored game still in memory) */
    int slot = 0;
    for (int i = 0; i < g_nlg; i++) {
        if (!g_lg[i].sel) continue;
        Player *p = player_new(g_lg[i].name, slot);
        p->saved_n = g_lg[i].n;
        p->score = g_lg[i].score;
        p->award = g_lg[i].award;
        g_pl[slot++] = p;
    }
    g_cur = 0;
    g_screen = SCR_GRATING; g_shimmer_next = plat_ticks();
    screen_anim(314, 129);                                 /* CHANGE (Animations): through the castle door */
    /* CHANGE (user, 2026-11-18): the maze has its own music from the castle door on. The original does not switch
     * (INTRO.MID played on into the maze until its end); now, if the intro is still playing, it fades into a
     * background loop when the door is clicked (a loop already running after the intro continues). */
    if (mm_music_on && g_music_state == 2 && g_intro_on) { plat_music_fade(2000); bg_next(); }
    plat_invalidate();
}

static int same_name(const char *a, const char *b)                         /* LB_FINDSTRINGEXACT ignores case */
{
    for (; *a && *b; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

static void logon_new_name(void)                                           /* FUN_005212b0 */
{
    g_lg_edit = 1;                           /* "--" row + edit box (471,297)-(595,312), EM_LIMITTEXT 12 */
    g_lg_text[0] = 0;
    plat_invalidate();
}

/* FUN_005215d0. Returns 0 if the name was refused (the edit box is then opened again). */
static int logon_commit_name(void)
{
    g_lg_edit = 0;
    if (!g_lg_text[0] || !strcmp(g_lg_text, "--")) { plat_invalidate(); return 1; }
    for (int i = 0; i < g_nlg; i++)
        if (same_name(g_lg[i].name, g_lg_text)) {
            msgbox(mm_str(29046), 0, NULL, 0);
            logon_new_name();
            return 0;
        }
    if (g_nlg >= 63) return 1;
    memmove(g_lg + 1, g_lg, sizeof(LogonEntry) * g_nlg++);
    memset(g_lg, 0, sizeof g_lg[0]);
    snprintf(g_lg[0].name, sizeof g_lg[0].name, "%s", g_lg_text);
    g_lg[0].sel = 1;                                                       /* LB_SETSEL(TRUE, 0) */
    if (lg_modes() && !g_lg_mp) for (int k = 1; k < g_nlg; k++) g_lg[k].sel = 0;   /* 1 player: just the new one */
    Player tmp;                                  /* FUN_0051b880 + FUN_0051bb80: the new player is saved at once */
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.name, sizeof tmp.name, "%s", g_lg_text);
    save_player(&tmp);
    g_lg[0].n = tmp.saved_n;
    plat_invalidate();
    return 1;
}

/* CHANGE 2026-11-21 (user): who plays - "1 player" (a click picks that name) or "Several players" (clicks add and remove,
 * up to five), in both layouts while Modern options > Turn prompts and player stats is on (touch layout: always);
 * otherwise the original multi-select list */
static int lg_modes(void) { return mm_opt.prompts || touch_view(); }
static void lg_pick(int i)
{
    int n = 0;
    for (int k = 0; k < g_nlg; k++) n += g_lg[k].sel;
    if (lg_modes() && !g_lg_mp) { for (int k = 0; k < g_nlg; k++) g_lg[k].sel = k == i; }
    else if (g_lg[i].sel || n < NPLAYERS || !lg_modes()) g_lg[i].sel = !g_lg[i].sel;
    else beep();
    plat_invalidate();
}
static void lg_mode(int mp)
{
    g_lg_mp = mp;
    int keep = 1;
    if (!mp) for (int k = 0; k < g_nlg; k++) if (g_lg[k].sel) { g_lg[k].sel = keep; keep = 0; }
    plat_invalidate();
}

static void logon_delete(void)
{
    char key[32];
    for (int i = 0; i < g_nlg;) {
        if (g_lg[i].sel && !g_lg[i].tourist) {
            if (g_lg[i].n) { snprintf(key, sizeof key, "MindmazePlayer%u", g_lg[i].n); plat_set_setting(key, "deleted"); }
            mm_stats_forget(g_lg[i].name);                                /* CHANGE: player stats */
            memmove(g_lg + i, g_lg + i + 1, sizeof(LogonEntry) * (--g_nlg - i));
        } else i++;
    }
    plat_invalidate();
}

/* ------------------------------------------------------------------------------------------------ */
/* drawing                                                                                           */

#define C_TEXT 0x34322D
#define C_RED 0x972938
#define C_LINE 0xA09683
#define C_MAPBG 0xD9D6C5
#define C_PALE 0xEBEAE1

static int inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }
/* CHANGE 2026-11-21: hover() tests the pointer in the coordinates of what is being drawn - the game area, a dialog
 * moved to the middle of a touch view (offset) or the touch column (offset and zoom); set with hover_space */
static int g_tx, g_ty, g_tz = 1;
static void hover_space(int x, int y, int zoom) { g_tx = x; g_ty = y; g_tz = zoom; }
static int hover(int rx, int ry, int rw, int rh)
{
    if (g_mx < 0) return 0;
    return inside((g_mx - g_tx) / g_tz, (g_my - g_ty) / g_tz, rx, ry, rw, rh);
}

static void add_hot(int x, int y, int w, int h, int id)
{
    if (g_nhot < 64) g_hot[g_nhot++] = (Hot){x, y, w, h, id, NULL};
}

static void draw_object(MMCanvas *c, const MMObject *o) { mm_draw_img(c, o->name, o->x, o->y); }

/* CHANGE 2026-11-21 (overlay check, tests/test_core.c test_overlay): a few click rects of the original tables sit off
 * their picture - NDOOR 12 px right / 7 down, ASIANTRA 10 left / 6 up, SUITARM2 5 below. A rect that reaches more
 * than 4 px past the picture's visible pixels is moved back onto it (and cut to it when bigger); the 1-3 px of the
 * others are kept as they were. The visible box of each picture is measured once. */
static struct { const MMObject *o; int x0, y0, x1, y1; } g_box[128];   /* visible box of each picture (screen) */
static int g_nbox;
static int pic_box_k(const MMObject *o)                     /* index into g_box, -1 = no picture */
{
    int k = 0;
    while (k < g_nbox && g_box[k].o != o) k++;
    if (k == g_nbox) {
        MMImage *im = mm_image(o->name);
        if (!im || g_nbox >= 128) return -1;
        int x0 = im->w, y0 = im->h, x1 = -1, y1 = -1;
        for (int y = 0; y < im->h; y++)
            for (int x = 0; x < im->w; x++)
                if ((im->px[y * im->w + x] >> 24) > 128) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        g_box[g_nbox].o = o;
        g_box[g_nbox].x0 = o->x + x0; g_box[g_nbox].y0 = o->y + y0; g_box[g_nbox].x1 = o->x + x1; g_box[g_nbox].y1 = o->y + y1;
        g_nbox++;
    }
    return k;
}
static void hot_onto_picture(Hot *h, const MMObject *o)
{
    int k = pic_box_k(o);
    if (k < 0 || g_box[k].x1 < 0) return;
    int bx0 = g_box[k].x0, by0 = g_box[k].y0, bw = g_box[k].x1 - bx0 + 1, bh = g_box[k].y1 - by0 + 1;
    int out = bx0 - h->x > 4 || by0 - h->y > 4 || h->x + h->w - bx0 - bw > 4 || h->y + h->h - by0 - bh > 4;
    if (!out) return;
    if (h->w > bw) h->w = bw;
    if (h->h > bh) h->h = bh;
    if (h->x < bx0) h->x = bx0;
    if (h->y < by0) h->y = by0;
    if (h->x + h->w > bx0 + bw) h->x = bx0 + bw - h->w;
    if (h->y + h->h > by0 + bh) h->y = by0 + bh - h->h;
}

static void hot_object(const MMObject *o, int mode)                        /* FUN_0051c410 */
{
    int n0 = g_nhot;
    int l = o->rect[0], t = o->rect[1], r = o->rect[2], b = o->rect[3];
    if (mode == 0) {
        if (l && t) add_hot(o->x + l, o->y + t, abs(r - l), abs(b - t), o->id);
    } else if (!l || !t || !r || !b) {
        MMImage *im = mm_image(o->name);
        if (!im) return;
        add_hot(o->x, o->y, o->half == 1 ? im->w / 2 : im->w, im->h, o->id);
    } else add_hot(o->x + l, o->y + t, abs(r - l), abs(b - t), o->id);
    if (g_nhot > n0) {
        g_hot[g_nhot - 1].o = o;
        hot_onto_picture(&g_hot[g_nhot - 1], o);
    }
}

/* tap areas fitted to the picture (CHANGE 2026-11-21, user: "some of the objects are not fully shimmering ... fit the
 * hitbox if there is nothing overlapping"): the original rects cover only part of most characters (11-85% of the
 * picture, test_overlay), so the glint, which runs over the tappable part, lit only that part. Where the box around
 * an object's visible pixels holds no visible pixel of any other object in the room and no other tap area, the tap
 * area becomes that box (and the glint covers the whole picture); otherwise the original rect stays. Worked out once
 * per room (the list is rebuilt every frame). */
static int solid_in(const MMObject *o, int x0, int y0, int x1, int y1)   /* o has a visible pixel in [x0,x1]x[y0,y1] */
{
    MMImage *im = mm_image(o->name);
    if (!im) return 0;
    if (x0 < o->x) x0 = o->x;
    if (y0 < o->y) y0 = o->y;
    if (x1 > o->x + im->w - 1) x1 = o->x + im->w - 1;
    if (y1 > o->y + im->h - 1) y1 = o->y + im->h - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if ((im->px[(y - o->y) * im->w + x - o->x] >> 24) > 128) return 1;
    return 0;
}
static void fit_hots(const Room *r)
{
    static struct { const Room *r; unsigned long long a, b; int chr, doors, n; Hot h[64]; } memo;
    if (memo.r == r && memo.a == r->furnA && memo.b == r->furnB && memo.chr == r->chr && memo.doors == r->doors && memo.n == g_nhot) {
        memcpy(g_hot, memo.h, sizeof(Hot) * (size_t)g_nhot);
        return;
    }
    const MMObject *L[80];
    int n = 0;
    for (int d = 0; d < 5; d++) if (r->doors & (1 << d)) L[n++] = obj(MM_G_DOORS, d);
    for (int f = 0; f < MM_N_FURNITURE; f++) if ((r->furnA | r->furnB) >> f & 1) L[n++] = obj(MM_G_FURNITURE, f);
    if (r->chr != NONE) L[n++] = obj(MM_G_CHARACTERS, r->chr);
    Hot orig[64];
    memcpy(orig, g_hot, sizeof(Hot) * (size_t)g_nhot);
    for (int i = 0; i < g_nhot; i++) {
        const MMObject *o = orig[i].o;
        int k = o ? pic_box_k(o) : -1;
        if (k < 0 || g_box[k].x1 < 0) continue;
        int x0 = g_box[k].x0, y0 = g_box[k].y0, x1 = g_box[k].x1, y1 = g_box[k].y1, ok = 1;
        for (int j = 0; j < n && ok; j++) if (L[j] != o && solid_in(L[j], x0, y0, x1, y1)) ok = 0;
        for (int j = 0; j < g_nhot && ok; j++)
            if (j != i && orig[j].x <= x1 && orig[j].x + orig[j].w > x0 && orig[j].y <= y1 && orig[j].y + orig[j].h > y0) ok = 0;
        if (ok) { g_hot[i].x = x0; g_hot[i].y = y0; g_hot[i].w = x1 - x0 + 1; g_hot[i].h = y1 - y0 + 1; }
    }
    memo.r = r; memo.a = r->furnA; memo.b = r->furnB; memo.chr = r->chr; memo.doors = r->doors; memo.n = g_nhot;
    memcpy(memo.h, g_hot, sizeof(Hot) * (size_t)g_nhot);
}

static void room_hots(const Room *r);
static void draw_room(MMCanvas *c)                                         /* FUN_0051c190 */
{
    g_nhot = 0;
    Player *p = cur();
    if (p->award == 5) {
        mm_draw_img(c, "ATRIUM.DIB", 0, 0);
        add_hot(0, 0, 636, 301, 0x1E);
        return;
    }
    Room *r = cur_room();
    draw_object(c, obj(MM_G_WALLS, r->wall));
    if (r->floor != NONE) draw_object(c, obj(MM_G_FLOORS, r->floor));
    if (r->ceil != NONE) draw_object(c, obj(MM_G_CEILINGS, r->ceil));
    for (int d = 0; d < 5; d++)
        if (r->doors & (1 << d)) draw_object(c, obj(MM_G_DOORS, d));
    for (int f = 0; f < MM_N_FURNITURE; f++)
        if (r->furnA & (1ull << f)) draw_object(c, obj(MM_G_FURNITURE, f));
    if (r->chr != NONE) draw_object(c, obj(MM_G_CHARACTERS, r->chr));
    for (int f = 0; f < MM_N_FURNITURE; f++)
        if (r->furnB & (1ull << f)) draw_object(c, obj(MM_G_FURNITURE, f));
    if (r->doors & 2) draw_object(c, obj(MM_G_DOORS, 1));                 /* SDOOR again, in front */
    room_hots(r);
}

static void room_hots(const Room *r)
{
    g_nhot = 0;
    g_ndrawn = 0;                                          /* the drawing order (draw_room), for what covers what */
    for (int d = 0; d < 5; d++) if (r->doors & (1 << d)) g_drawn[g_ndrawn++] = obj(MM_G_DOORS, d);
    for (int f = 0; f < MM_N_FURNITURE; f++) if (r->furnA & (1ull << f)) g_drawn[g_ndrawn++] = obj(MM_G_FURNITURE, f);
    if (r->chr != NONE) g_drawn[g_ndrawn++] = obj(MM_G_CHARACTERS, r->chr);
    for (int f = 0; f < MM_N_FURNITURE; f++) if (r->furnB & (1ull << f)) g_drawn[g_ndrawn++] = obj(MM_G_FURNITURE, f);
    if (r->doors & 2) g_drawn[g_ndrawn++] = obj(MM_G_DOORS, 1);
    /* hotspots, in the original's order (0x51c33e..0x51c38c): furniture A and B (mode 0: only the PIC1-x /
     * PIC2-x pictures have a rect), the character (mode 1), the doors (mode 1). They go into a list and the
     * click test FUN_0052f840 takes the FIRST rect that holds the point, so earlier ones win where they overlap. */
    for (int f = 0; f < MM_N_FURNITURE; f++)
        if (r->furnA & (1ull << f)) hot_object(obj(MM_G_FURNITURE, f), 0);
    for (int f = 0; f < MM_N_FURNITURE; f++)
        if (r->furnB & (1ull << f)) hot_object(obj(MM_G_FURNITURE, f), 0);
    if (r->chr != NONE) hot_object(obj(MM_G_CHARACTERS, r->chr), 1);
    for (int d = 0; d < 5; d++)
        if (r->doors & (1 << d)) hot_object(obj(MM_G_DOORS, d), 1);
    fit_hots(r);
}

static int hot_at(int x, int y)                                            /* FUN_0052f840: first match, or -1 */
{
    for (int i = 0; i < g_nhot; i++)
        if (inside(x, y, g_hot[i].x, g_hot[i].y, g_hot[i].w, g_hot[i].h)) return g_hot[i].id;
    return -1;
}

static int draw_overlay(MMCanvas *c, int mode, const char *text)   /* FUN_0051ced0 / FUN_0051d0d0 */
{
    /* CHANGE 2026-11-21 (user): ONE font per layout, chosen up front - touch 17 px, Large text 13 px, otherwise the
     * original 11 px in the original box - and the whole text laid out once at that font (mm_rich_page). Text longer
     * than the box goes on pages; a speech always has Previous / Next-or-Close under its text, a question only when it
     * has pages. A speech's lines fade in one after another (line_alpha): the page is drawn whole and each line's rows
     * are mixed with the picture under them. Returns the letters on the page shown. */
    int touch = touch_view(), speech = !mode;
    int font = touch ? FONT_T_L : mm_opt.bigtext ? FONT_Q_L : FONT_TEXT;
    int ifont = touch ? FONT_T_L : mm_opt.bigtext ? FONT_Q_LI : FONT_ITALIC;
    int x = mode ? 194 : 99, y = mode ? 95 : 44, w = mode ? 191 : 230;
    int h = font == FONT_TEXT ? (mode ? 158 : 172) : (mode ? 130 : 160);   /* (clear of the zig-zag / frame below) */
    mm_draw_img(c, mode ? "BANNER.DIB" : "DIALOG.DIB", 67, 0);
    int pages = 1, bh = touch ? 26 : 20, nl = 0;
    mm_rich_page(c, x, y, w, h, text, font, ifont, 0, 0, 0, &pages, NULL);    /* (measure: does it fit?) */
    int buttons = speech || pages > 1, th = buttons ? h - bh - 4 : h;
    if (buttons) mm_rich_page(c, x, y, w, th, text, font, ifont, 0, 0, 0, &pages, NULL);   /* room for the buttons */
    g_ov_pages = pages;
    if (g_ov_page >= pages) g_ov_page = pages - 1;
    int fade = speech && typing();
    Snap sv = fade ? snap(c, x, y, w, th) : (Snap){0};
    int n = mm_rich_page(c, x, y, w, th, text, font, ifont, 0, g_ov_page, -1, NULL, &nl);
    if (speech) g_ov_lines = nl > 0 ? nl : 1;
    if (sv.px) {                                            /* each line's rows: picture -> text by its alpha */
        int lh = plat_text_px_height(c, font), W = sv.X1 - sv.X0;
        for (int Y = sv.Y0; Y < sv.Y1; Y++) {
            int a = (int)(line_alpha(lh > 0 ? (Y - y * c->scale) / lh : 0) * 256);
            if (a >= 256) continue;
            uint32_t *p = c->px + (size_t)Y * c->stride + sv.X0, *o = sv.px + (size_t)(Y - sv.Y0) * W;
            for (int X = 0; X < W; X++) {
                uint32_t nw = p[X], b = o[X];
                p[X] = ((((nw >> 16 & 255) * a + (b >> 16 & 255) * (256 - a)) >> 8) << 16) |
                       ((((nw >> 8 & 255) * a + (b >> 8 & 255) * (256 - a)) >> 8) << 8) |
                       (((nw & 255) * a + (b & 255) * (256 - a)) >> 8);
            }
        }
        free(sv.px);
    }
    g_ov_btn_on = buttons;
    if (buttons) {
        int by = y + h - bh, bw = touch ? 84 : 70, bf = touch ? FONT_T_B : FONT_BOLD;
        int last = g_ov_page + 1 >= pages;                 /* (user: Next turns into Close on a speech's last page) */
        const char *L[2] = {"< Previous", last && speech ? "Close" : "Next >"};
        for (int b = 0; b < 2; b++) {
            int bx = b ? x + w - bw : x, on = b ? !last || speech : g_ov_page > 0;
            int *r = g_ov_btn[b];
            r[0] = bx; r[1] = by; r[2] = bw; r[3] = bh;
            int hv = on && !touch && hover(bx, by, bw, bh);
            mm_fill(c, bx, by, bw, bh, on ? (hv ? 0xF4F3EE : 0xF2F1EA) : 0xE4E2D8);
            mm_frame(c, bx, by, bw, bh, 1, on ? 0x2E2B25 : 0xA8A498);
            draw_text(c, bx, by, bw, bh, L[b], bf, !on ? 0x9A968A : hv ? C_RED : 0, TX_CENTER | TX_VCENTER);
        }
        if (pages > 1) {
            char pg[24];
            snprintf(pg, sizeof pg, "%d / %d", g_ov_page + 1, pages);
            draw_text(c, x + bw, by, w - 2 * bw, bh, pg, bf, 0x3C3A34, TX_CENTER | TX_VCENTER);
        }
    }
    return n;
}

static void speech_close(void)                          /* the speech goes (a tap, Close, Enter) */
{
    g_speech = 0;
    if (g_atrium_text) { g_atrium_text = 0; open_story(1); }
    plat_invalidate();
}
/* Previous / Next under a paged pop-up: 1 = the tap was on one of them (a page turned, or a greyed button) */
static int ov_page_click(int x, int y)
{
    if (!g_ov_btn_on) return 0;
    for (int b = 0; b < 2; b++) {
        int *r = g_ov_btn[b];
        if (!inside(x, y, r[0], r[1], r[2], r[3])) continue;
        int speech = g_speech && !g_qopen;
        if (b && speech && typing()) { g_type_done = 1; plat_invalidate(); return 1; }   /* (lines appearing: all of them) */
        if (b && speech && g_ov_page + 1 >= g_ov_pages) { speech_close(); return 1; }   /* Close */
        int to = g_ov_page + (b ? 1 : -1);
        if (to >= 0 && to < g_ov_pages) { g_ov_page = to; if (speech) type_start(0); }
        plat_invalidate();
        return 1;
    }
    return 0;
}

static void draw_map(MMCanvas *c, Maze *m, int x0, int y0, int x1, int y1) /* FUN_0051da70 */
{
    mm_fill(c, x0, y0, x1 - x0 + 1, y1 - y0 + 1, C_MAPBG);
    int cw = (x1 - x0) / m->w, ch = (y1 - y0) / m->h;
    int y = y0;
    for (int j = 0; j < m->h; j++, y += ch + 1) {
        int x = x0;
        for (int i = 0; i < m->w; i++, x += cw + 1) {
            Room *r = m->grid[j * m->w + i];
            int stairs = (i == m->tx && j == m->ty);
            if (!r || (!r->visited && !m->reveal && !stairs)) { mm_frame(c, x, y, cw, ch, 1, C_PALE); continue; }
            mm_frame(c, x, y, cw, ch, 2, stairs ? C_RED : C_LINE);   /* 2 px pen centred on the cell edge, as GDI drew it */
            if (r->doors & 1) mm_fill(c, x + 4, y, 5, 2, C_MAPBG);
            if (r->doors & 8) mm_fill(c, x, y + 3, 2, 5, C_MAPBG);
            if (r->doors & 2) mm_fill(c, x + 4, y + ch - 2, 5, 2, C_MAPBG);
            if (r->doors & 4) mm_fill(c, x + cw - 2, y + 3, 2, 5, C_MAPBG);
            /* current room. FUN_0051da70 (0x51decf..0x51dfcb) has no Ellipse call: it FillRects (x+3, y+3)-(x+9, y+8)
             * in #972938 and clears the 4 corner pixels, a 6x5 blob that reads as a red ellipse in UPSELL_MINDMAZE.
             * CHANGE (user request 2026-11-16): drawn as an anti-aliased ellipse with the same box (6x5 px, centre
             * (x+6, y+5.5)) and colour, smooth at 2x/4x instead of a blocky blob. */
            /* (CHANGE 2026-11-21, user: the dot centred in its room on the touch map - the middle of the cell; the classic
             * map keeps the original place 6 / 5.5 of its 13 x 11 cells) */
            if (i == m->cx && j == m->cy) mm_ellipse(c, cw == 13 && ch == 11 ? x + 6 : x + cw / 2.0, cw == 13 && ch == 11 ? y + 5.5 : y + ch / 2.0, 3, 2.5, C_RED);
        }
    }
}

/* 4-state button strip: 0 normal, 1 hover, 2 disabled, 3 selected */
static void strip(MMCanvas *c, const char *res, int state, int x, int y, int w, int h)
{
    MMImage *im = mm_image(res);
    if (!im) return;
    int fw = im->w / 4;
    mm_draw_img_part(c, res, fw * state, 0, fw, im->h, x + (w - fw) / 2, y + (h - im->h) / 2);
}

/* New Game / Quit Game (style RCDATA 29353 via FUN_0051e630: face #D9D6C5, black frame, Tahoma 11 black text).
 * UPSELL_MINDMAZE fitted per side: 1 px black frame, face = panel colour, no visible bevel (was a #F2F1EA face and a
 * #2E2B25 frame). Hover text red stays (rebuild feedback). */
static void panel_button(MMCanvas *c, int x, int y, int w, int h, const char *s)
{
    int hv = hover(x, y, w, h);
    mm_fill(c, x, y, w, h, g_panel_bg);
    mm_frame(c, x, y, w, h, 1, 0x000000);
    draw_text(c, x, y, w, h, s, FONT_TEXT, hv ? C_RED : 0, TX_CENTER | TX_VCENTER);
}

static void text_button(MMCanvas *c, int x, int y, int w, int h, const char *s, int enabled)
{
    int hv = enabled && hover(x, y, w, h);                                 /* original: light button, dark frame */
    mm_fill(c, x, y, w, h, 0xF2F1EA);
    mm_frame(c, x, y, w, h, 1, enabled ? 0x2E2B25 : 0xA8A498);
    draw_text(c, x, y, w, h, s, FONT_TEXT, !enabled ? 0x9A968A : hv ? C_RED : 0, TX_CENTER | TX_VCENTER);
}

/* panel geometry (SPEC.md section 10) */
static const int AREA_X[2] = {174, 282}, AREA_W[2] = {100, 165}, AREA_Y[5] = {27, 46, 65, 84, 103};
static const int AREA_TEXT[10] = {29014, 29015, 29016, 29017, 29018, 29019, 29020, 29021, 29022, 29023};
static const int LEVEL_Y[4] = {27, 53, 79, 105}, ANS_Y[4] = {10, 40, 70, 100};

static void area_rect(int b, int *x, int *y, int *w) { *x = AREA_X[b / 5]; *w = AREA_W[b / 5]; *y = AREA_Y[b % 5]; }

/* ---- Settings during the game (CHANGE 2026-11-21, user: "access the settings always"): a small gear button in the
 * room picture's top right corner, and Windows menu > Settings... (CMD_SETTINGS). Not while a question is open: applying
 * other question sources reloads the question bank the open question points into (the slot lists are rebuilt
 * through mm_questions_generation, so later questions come from the new sources). ---- */
enum { GEAR_X = 610, GEAR_Y = 4, GEAR_S = 22 };
/* the 50/50 hint button takes the gear's corner while a question is open (classic layout) */
enum { HINT_X = 556, HINT_Y = 4, HINT_W = 76, HINT_H = 22 };
static int hint_ok(void) { return g_qopen && mm_opt.hints && !g_q.hint && !g_nmsg && !g_inactive && !mm_settings_on() && !g_story.on; }

static void draw_hint(MMCanvas *c, int hot)
{
    mm_fill(c, HINT_X, HINT_Y, HINT_W, HINT_H, hot ? 0xF4F3EE : g_panel_bg);
    mm_frame(c, HINT_X, HINT_Y, HINT_W, HINT_H, 1, hot ? 0x000000 : C_LINE);
    draw_text(c, HINT_X, HINT_Y, HINT_W, HINT_H, "50/50 hint", FONT_BOLD, hot ? C_RED : 0, TX_CENTER | TX_VCENTER);
}

static int gear_ok(void)
{
    return g_screen != SCR_LOGON && !g_qopen && !g_title_on && !g_nmsg && !g_story.on && !mm_settings_on() && !mm_stats_on();
}

static void draw_gear(MMCanvas *c, int hot)
{
    static const double DIR[8][2] = {{1, 0}, {.7071, .7071}, {0, 1}, {-.7071, .7071}, {-1, 0}, {-.7071, -.7071},
                                     {0, -1}, {.7071, -.7071}};
    uint32_t ink = hot ? 0x000000 : 0x34322D;
    double cx = GEAR_X + GEAR_S / 2.0, cy = GEAR_Y + GEAR_S / 2.0;
    mm_fill(c, GEAR_X, GEAR_Y, GEAR_S, GEAR_S, g_panel_bg);
    mm_frame(c, GEAR_X, GEAR_Y, GEAR_S, GEAR_S, 1, hot ? 0x6E655A : C_LINE);
    for (int i = 0; i < 8; i++) mm_disc(c, cx + DIR[i][0] * 6.6, cy + DIR[i][1] * 6.6, 2.1, ink);   /* teeth */
    mm_disc(c, cx, cy, 6.0, ink);
    mm_disc(c, cx, cy, 2.4, g_panel_bg);                                    /* the hole */
}


/* the mute button (see g_mute): top left of the picture, every screen; not under the title or a dialog */
enum { MUTE_X = 4, MUTE_Y = 4, MUTE_S = 22 };
static int mute_ok(void) { return !g_title_on && !g_nmsg && !g_story.on && !mm_settings_on() && !mm_stats_on(); }
static void mute_toggle(void)
{
    g_mute = !g_mute;
    plat_set_setting("Muted", g_mute ? "1" : "0");
    apply_volume();
    plat_invalidate();
}
static void draw_mute(MMCanvas *c, int hot)
{
    uint32_t ink = hot ? 0x000000 : 0x34322D;
    int x = MUTE_X, y = MUTE_Y;
    mm_fill(c, x, y, MUTE_S, MUTE_S, g_panel_bg);
    mm_frame(c, x, y, MUTE_S, MUTE_S, 1, hot ? 0x6E655A : C_LINE);
    mm_fill(c, x + 4, y + 8, 4, 6, ink);                                   /* the speaker: box + cone */
    for (int i = 0; i < 6; i++) mm_fill(c, x + 8 + i, y + 8 - i, 1, 6 + 2 * i, ink);
    if (g_mute) {                                                          /* a red cross: sound off */
        for (int i = 0; i < 6; i++) {
            mm_fill(c, x + 15 + i, y + 8 + i, 2, 1, C_RED);
            mm_fill(c, x + 20 - i, y + 8 + i, 2, 1, C_RED);
        }
    } else {                                                               /* sound waves */
        mm_fill(c, x + 16, y + 9, 1, 4, ink);
        mm_fill(c, x + 18, y + 7, 1, 8, ink);
    }
}
static void settings_now(void)
{
    if (g_screen == SCR_LOGON && g_lg_edit && !logon_commit_name()) return;   /* typed name kept (refused: message) */
    if (g_screen != SCR_LOGON && !gear_ok()) { beep(); return; }     /* a question is open */
    anim_start(AN_DIALOG, 220, 0, 0);
    mm_settings_open();
}

/* ---- animation drawing (CHANGE 2026-11-21) ---- */
static uint32_t mix(uint32_t a, uint32_t b, double t)      /* a..b */
{
    int r = (int)((a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * t);
    int g = (int)((a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * t);
    int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
    return (uint32_t)(r << 16 | g << 8 | bl);
}

/* the chosen answer flashes green (right) or red (wrong); after a second wrong answer the right one turns green too */
static void answer_feedback(MMCanvas *c, int b, int x, int y, int w, int h)
{
    if (!anim_on(AN_FEEDBACK) || !g_qopen) return;
    int sel = g_an[AN_FEEDBACK].a, kind = g_an[AN_FEEDBACK].b;
    int green = (kind == 1 && b == sel) || (kind == 2 && g_q.order[b] == 0), red = kind != 1 && b == sel;
    if (!green && !red) return;
    double t = anim_t(AN_FEEDBACK), pulse = 0.65 + 0.35 * cos(t * 3.14159 * 4);
    uint32_t col = green ? 0x2E8B3A : 0xC0392B;
    fill_alpha(c, x, y, w, h, col, (int)(110 * pulse * (1 - t * 0.5)));
    mm_frame(c, x, y, w, h, 2, col);
}

/* a new rank: gold rays turn around the award picture and a glow grows behind it */
static void rank_glow(MMCanvas *c, double cx, double cy)
{
    if (!anim_on(AN_RANK)) return;
    double t = anim_t(AN_RANK), e = ease(t);
    mm_disc(c, cx, cy, 30 + 8 * e, mix(0xF6E7A8, g_panel_bg, t));
    for (int i = 0; i < 12; i++) {
        double a = i * 3.14159 / 6 + t * 2.5, r = 20 + 26 * e;
        mm_disc(c, cx + cos(a) * r, cy + sin(a) * r, 2.6 * (1 - t) + 0.6, mix(0xD4A017, g_panel_bg, t));
    }
}

/* the map's frame glows and flickers like a flame while a match burns */
static void match_glow(MMCanvas *c, int x, int y, int w, int h)
{
    if (!mm_opt.anim || !match_burning()) return;
    double f = 0.5 + 0.25 * sin(plat_ticks() / 70.0) + 0.25 * sin(plat_ticks() / 23.0);
    mm_frame(c, x - 2, y - 2, w + 4, h + 4, 2, mix(0xC0501A, 0xF7C548, f));
}

/* ---- what can be tapped (CHANGE 2026-11-21, touch layout; user: no boxes, they do not fit the pictures) ----
 * Room objects are separate pictures, so their real outline is traced from the picture's transparency and glows
 * warm for a moment when a room appears. The castle gate's door and "?" banner are part of one painted picture: a
 * slow glint of light sweeps over them now and then instead. */
static int opaque(const MMImage *im, int w, int x, int y)
{
    return x >= 0 && y >= 0 && x < w && y < im->h && (im->px[y * im->w + x] >> 24) > 128;
}
static void obj_glow(MMCanvas *c, const Hot *h, int a)    /* (only the tappable part: the picture inside the tap rect) */
{
    const MMObject *o = h->o;
    MMImage *im = o ? mm_image(o->name) : NULL;
    if (!im || a <= 0) return;
    int w = im->w;
    for (int y = -2; y < im->h + 2; y++)
        for (int x = -2; x < w + 2; x++) {
            if (!inside(o->x + x, o->y + y, h->x - 2, h->y - 2, h->w + 4, h->h + 4)) continue;
            if (opaque(im, w, x, y) && inside(o->x + x, o->y + y, h->x, h->y, h->w, h->h)) continue;
            if (opaque(im, w, x - 1, y) || opaque(im, w, x + 1, y) || opaque(im, w, x, y - 1) || opaque(im, w, x, y + 1))
                fill_alpha(c, o->x + x, o->y + y, 1, 1, 0xFFE08A, a);                      /* the edge */
            else if (opaque(im, w, x - 2, y) || opaque(im, w, x + 2, y) || opaque(im, w, x, y - 2) || opaque(im, w, x, y + 2) ||
                     opaque(im, w, x - 1, y - 1) || opaque(im, w, x + 1, y + 1) || opaque(im, w, x - 1, y + 1) || opaque(im, w, x + 1, y - 1))
                fill_alpha(c, o->x + x, o->y + y, 1, 1, 0xF7C548, a / 3);                  /* a soft halo */
        }
}
static int taphint_alpha(void)                             /* 0..200: fades in, holds, fades out over 1.6 s */
{
    int left = (int)(int32_t)(g_taphint_until - plat_ticks()), gone = 1600 - left;
    if (left <= 0) return 0;
    if (!mm_opt.anim) return 170;
    int a = 200;
    if (gone < 250) a = a * gone / 250;
    if (left < 600) a = a * left / 600;
    return a;
}
static void shimmer(MMCanvas *c, int x, int y, int w, int h, double t)
{
    if (t <= 0 || t >= 1) return;
    for (int yy = 0; yy < h; yy++) {
        int cx = x + (int)(-60 + (w + 140) * t - yy * 0.45);
        for (int k = 0; k < 2; k++) {                       /* a wide soft band and a narrow bright core */
            int half = k ? 5 : 16, a = k ? 60 : 34;
            int x0 = cx - half < x ? x : cx - half, x1 = cx + half > x + w ? x + w : cx + half;
            if (x1 > x0) fill_alpha(c, x0, y + yy, x1 - x0, 1, 0xFFF1C8, a);
        }
    }
}

/* the same glint over a room object, over its whole picture (CHANGE 2026-11-21, user: the glint lit only half of some
 * objects - it followed the tap area, which keeps the original part-rect where something else is inside the picture's
 * box): it sweeps the box of the picture's visible pixels and lights those pixels, except where an object drawn in
 * front of it (g_drawn, the room's drawing order) covers them */
typedef struct { const MMObject *o; const MMImage *im; } Front;
static int fronts_of(const MMObject *o, Front *f)          /* objects drawn in front of o, with their pictures */
{
    int i = g_ndrawn - 1, n = 0;
    while (i >= 0 && g_drawn[i] != o) i--;                  /* (its last drawing: SDOOR is drawn twice) */
    for (int j = i + 1; i >= 0 && j < g_ndrawn; j++) {
        MMImage *im = g_drawn[j] != o ? mm_image(g_drawn[j]->name) : NULL;
        if (im) { f[n].o = g_drawn[j]; f[n].im = im; n++; }
    }
    return n;
}
static int covered_by(const Front *f, int n, int X, int Y)
{
    for (int j = 0; j < n; j++) {
        int x = X - f[j].o->x, y = Y - f[j].o->y;
        if (x >= 0 && y >= 0 && x < f[j].im->w && y < f[j].im->h && (f[j].im->px[y * f[j].im->w + x] >> 24) > 128) return 1;
    }
    return 0;
}
static void obj_glint(MMCanvas *c, const Hot *h, double t)
{
    const MMObject *o = h->o;
    MMImage *im = o ? mm_image(o->name) : NULL;
    if (!im || t <= 0 || t >= 1) return;
    int k = pic_box_k(o);
    if (k < 0 || g_box[k].x1 < 0) return;
    int w = im->w, X0 = g_box[k].x0 - o->x, Y0 = g_box[k].y0 - o->y;
    int bw = g_box[k].x1 - g_box[k].x0 + 1, bh = g_box[k].y1 - g_box[k].y0 + 1;
    Front fr[160];
    int nf = fronts_of(o, fr);
    for (int y = Y0; y < Y0 + bh; y++) {
        int cx = X0 + (int)(-60 + (bw + 140) * t - (y - Y0) * 0.45);
        for (int x = cx - 16; x < cx + 16; x++) {
            if (!opaque(im, w, x, y) || covered_by(fr, nf, o->x + x, o->y + y)) continue;
            int core = x >= cx - 5 && x < cx + 5;
            fill_alpha(c, o->x + x, o->y + y, 1, 1, 0xFFF1C8, core ? 94 : 34);   /* (both bands where they overlap) */
        }
    }
}

/* glints in a row, left to right (user): item i starts GLINT_STEP ms after item i - 1 and takes GLINT_LEN ms */
enum { GLINT_STEP = 350, GLINT_LEN = 900 };
static double glint_t(int i)
{
    if (!anim_on(AN_SHIMMER)) return -1;
    double ms = anim_t(AN_SHIMMER) * g_an[AN_SHIMMER].dur - i * GLINT_STEP;
    return ms / GLINT_LEN;
}
static int hot_order(const void *a, const void *b)
{
    const Hot *x = a, *y = b;
    return x->x - y->x;
}

/* screen changes (CHANGE 2026-11-21, user): the castle door, the gate and the door picture lead on with a short
 * walk-in - the old screen grows towards the place tapped while the next one shows through. The old screen is taken
 * from the canvas at the start of the next draw (its pixels are still there), so nothing is drawn twice. */
static Snap g_scr_old;
static int g_scr_capture, g_scr_fx, g_scr_fy, g_scr_scale;
static void screen_anim(int fx, int fy)
{
    if (!mm_opt.anim) return;
    g_scr_capture = 1;
    g_scr_fx = fx;
    g_scr_fy = fy;
    anim_start(AN_SCREEN, 700, 0, 0);
}
static void screen_old_draw(MMCanvas *c)                   /* after the new screen is drawn */
{
    if (!g_scr_old.px) return;
    if (!anim_on(AN_SCREEN) || g_scr_scale != c->scale) { free(g_scr_old.px); g_scr_old.px = NULL; return; }
    double e = ease(anim_t(AN_SCREEN)), z = 1 + 0.9 * e;
    int a = (int)((1 - e) * 256), W = g_scr_old.X1 - g_scr_old.X0, H = g_scr_old.Y1 - g_scr_old.Y0;
    double fx = (g_scr_fx + c->ox) * c->scale - g_scr_old.X0, fy = g_scr_fy * c->scale - g_scr_old.Y0;
    for (int Y = 0; Y < H; Y++) {
        int sy = (int)(fy + (Y - fy) / z);
        if (sy < 0 || sy >= H) continue;
        uint32_t *p = c->px + (size_t)(Y + g_scr_old.Y0) * c->stride + g_scr_old.X0, *src = g_scr_old.px + (size_t)sy * W;
        for (int X = 0; X < W; X++) {
            int sx = (int)(fx + (X - fx) / z);
            if (sx < 0 || sx >= W) continue;
            uint32_t n = p[X], b = src[sx];
            uint32_t r = ((b >> 16 & 255) * a + (n >> 16 & 255) * (256 - a)) >> 8;
            uint32_t g = ((b >> 8 & 255) * a + (n >> 8 & 255) * (256 - a)) >> 8;
            uint32_t bl = ((b & 255) * a + (n & 255) * (256 - a)) >> 8;
            p[X] = r << 16 | g << 8 | bl;
        }
    }
}

/* the score while it counts up after a right answer */
static int shown_score(const Player *p)
{
    if (!anim_on(AN_SCORE) || p != cur()) return p->score;
    int from = g_an[AN_SCORE].a;
    return from + (int)((p->score - from) * ease(anim_t(AN_SCORE)));
}

/* confetti over the ending story */
static void confetti(MMCanvas *c, int x0, int y0, int w, int h)
{
    if (!anim_on(AN_CONFETTI)) return;
    static const uint32_t COL[6] = {0xC0392B, 0xE8C872, 0x2E8B3A, 0x2C6FBB, 0x8E44AD, 0xE67E22};
    double t = anim_t(AN_CONFETTI);
    for (int i = 0; i < 80; i++) {
        unsigned r = (unsigned)i * 2654435761u;
        double fx = (r % 1000) / 1000.0, speed = 0.6 + (r >> 10) % 400 / 1000.0, start = ((r >> 20) % 1000) / 1000.0;
        double fy = fmod(start + t * speed * 1.6, 1.2) - 0.1;
        int px = x0 + (int)(fx * w + sin(t * 9 + i) * 8), py = y0 + (int)(fy * h);
        if (py < y0 || py > y0 + h - 4) continue;
        mm_fill(c, px, py, 3 + (int)(r % 3), 4 + (int)((r >> 3) % 3), COL[i % 6]);
    }
}

static void draw_panel(MMCanvas *c)                                        /* FUN_0051edb0 / FUN_0051fcd0 */
{
    const int Y = 301;
    Player *p = cur();
    mm_fill(c, -c->ox, Y, 636 + 2 * c->ox, 136, g_panel_bg);                /* widescreen: the panel background spans the
                                                                               whole width, its contents stay centred */
    char res[32];
    for (int i = 0; i < 5; i++) {
        snprintf(res, sizeof res, "res/%d.bmp", 29158 + i);
        int st = !p->maze->match[i] ? 2 : hover(6, Y + 8 + 24 * i, 20, 23) ? 1 : 0;
        strip(c, res, st, 6, Y + 8 + 24 * i, 20, 23);
        /* UPSELL_MINDMAZE: each match button has a 1 px frame in the map-frame colour (#A09683, fitted per side)
         * around the 18x21 bitmap; the rebuild left the 1 px ring in the panel colour */
        mm_frame(c, 6, Y + 8 + 24 * i, 20, 23, 1, C_LINE);
    }
    mm_frame(c, 29, Y + 7, 141, 121, 1, mm_opt.bigtext ? 0x6E655A : C_LINE);
    draw_map(c, p->maze, 30, Y + 8, 162, Y + 126);
    match_glow(c, 29, Y + 7, 141, 121);
    if (!g_qopen) {
        /* labels 0x7164 / 0x7165 (tables 0x6b4498 rects, 0x6b4518 alignment, 0x6b4534 fonts): Tahoma 11 bold, left,
         * top-aligned. Text black: UPSELL_MINDMAZE's bold strokes reach luminance 5, darker than #34322D allows. */
        draw_text(c, 174, Y + 6, 150, 15, mm_str(29028), FONT_BOLD, 0, TX_LEFT);
        /* underlines: FUN_0051eb00 MoveTo(174,21) LineTo(450,21) / MoveTo(454,21) LineTo(489,21) with the DC's default
         * pen (the #A09683 pen is deselected after the map frame) = 1 px BLACK, end point excluded (was #34322D,
         * one pixel too long each); the screenshot line fits #000000 */
        mm_fill(c, 174, Y + 21, 276, 1, 0x000000);
        for (int b = 0; b < 10; b++) {
            int x, y, w;
            area_rect(b, &x, &y, &w);
            /* radio button = bitmap 29164 (style 29357): 12x12 circle, #A09683 1 px ring, white face, 6 px dot.
             * Drawn as smooth circles (crisp at any scale). UPSELL_MINDMAZE fit: the bitmap sits at (x+1, y+1) of the
             * button (was a 11 px circle at x+0.5 with a 4.4 px dot) and the dot is black (fits better than #34322D). */
            double rx = x + 7, ry = Y + y + 7;
            mm_disc(c, rx, ry, 6, hover(x, Y + y, w, 15) ? 0x6E655A : C_LINE);
            mm_disc(c, rx, ry, 5, 0xFFFFFF);
            if (p->areabtn == b) mm_disc(c, rx, ry, 3, 0x000000);
            /* label text starts 2 px further right than the rebuild had (screenshot: "All Areas" at x 192, "Art,..." at
             * 300) and is black (style 29357 text colour 0) */
            draw_text(c, x + 18, Y + y, w - 18, 15, mm_str(AREA_TEXT[b]), FONT_TEXT, 0, TX_LEFT | TX_VCENTER);
        }
        draw_text(c, 455, Y + 6, 33, 15, mm_str(29029), FONT_BOLD, 0, TX_LEFT);
        mm_fill(c, 454, Y + 21, 35, 1, 0x000000);
        for (int l = 0; l < 4; l++) {
            /* level buttons (bitmaps 29154-29157 in the original): the light square and a calligraphic numeral
             * drawn with a font, crisp at any scale; red numeral on hover, red frame on the chosen level
             * (FUN_0051f530 colours 5/6/7 = #972938; the screenshot's frame fits 1 px red). */
            MMImage *lv = mm_image("res/29154.bmp");
            int by = Y + LEVEL_Y[l];
            mm_fill(c, 456, by + 1, 27, 24, lv ? lv->px[0] & 0xFFFFFF : 0xECEBE4);
            /* numeral like the bitmap (27x24 at +1,+1): black digit 17 px tall, ink rows 3..19 -> baseline at by+21,
             * centred on x 469. Placed by the font's ascent so it does not depend on the line height
             * (was DT_VCENTER with a 22 px font: 15 px digits, 1-2 px low) */
            char num[2] = {(char)('1' + l), 0};
            int s = c->scale, tw = plat_text_px_width(c, num, FONT_LEVEL);
            plat_text_px(c, 469 * s - tw / 2, (by + 21) * s - plat_text_px_ascent(c, FONT_LEVEL), num, FONT_LEVEL,
                         hover(455, by, 29, 26) ? C_RED : 0x000000, 455 * s, by * s, 484 * s, (by + 26) * s);
            if (p->level == l) mm_frame(c, 455, by, 29, 26, 1, C_RED);
        }
        /* award (FUN_005201a0 at 529,35). CHANGE (user, 2026-11-17): the pictures are 66x65 squares on the panel
         * colour, so at y 35 their bottom (to 100) ran into the Progress line (90) and the HD versions showed their
         * slightly lighter square. Now the background is keyed out (here for the original pictures, in build.sh for
         * the HD ones) and the picture itself is centred between the name (to 25) and Progress (90), on x 562. */
        MMImage *aw = mm_image(mm_ranks[p->award].image);
        if (aw) {
            int x0 = aw->w, y0 = aw->h, x1 = -1, y1 = -1;
            uint32_t bgc = aw->px[0] & 0xFFFFFF;
            for (int i = 0; i < aw->w * aw->h; i++) {
                uint32_t v = aw->px[i] & 0xFFFFFF;
                int dr = (int)(v >> 16) - (int)(bgc >> 16), dg = (int)(v >> 8 & 255) - (int)(bgc >> 8 & 255), db = (int)(v & 255) - (int)(bgc & 255);
                if (dr * dr + dg * dg + db * db < 300) aw->px[i] &= 0x00FFFFFF;    /* key the flat background (once) */
                else if (aw->px[i] >> 24) {
                    int x = i % aw->w, y = i / aw->w;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
            }
            if (x1 >= 0) {
                int ax = 562 - (x0 + x1 + 1) / 2, ay = Y + 26 + (64 - (y1 - y0 + 1)) / 2 - y0;   /* rows 26-89 */
                rank_glow(c, 562, Y + 58);
                mm_draw_img(c, mm_ranks[p->award].image, ax, ay);
            }
        }
        /* labels 0x7167 / 0x7166 / 0x7168: font RCDATA 4916 = Arial 12 (was Tahoma 11), top-aligned like the static
         * controls (screenshot digits at y 393..402 = Arial 12 from y 90; the rebuild's VCENTER put them 3-4 px low),
         * alignment table 0x6b4518: "Progress:" centred, score right, "/20000" left; black */
        draw_text(c, 492, Y + 90, 55, 20, mm_str(29031), FONT_SCORE, 0, TX_CENTER);
        char sc[16];
        snprintf(sc, sizeof sc, "%d", shown_score(p));
        draw_text(c, 551, Y + 90, 35, 20, sc, FONT_SCORE, 0, TX_RIGHT);
        draw_text(c, 586, Y + 90, 40, 20, mm_str(29032), FONT_SCORE, 0, TX_LEFT);
    } else {
        /* CHANGE 2026-11-21 (Animations): the answers slide in from the right when a question opens */
        MMCanvas acv = *c, *ac = &acv;
        if (anim_on(AN_ANSWERS)) acv.ox += (int)((1 - ease(anim_t(AN_ANSWERS))) * 70);
        for (int b = 0; b < 4 && !g_inactive; b++) {                       /* hidden while inactive (FUN_005204d0) */
            int en = !g_q.disabled[b];
            /* letters (group 0x3178, FUN_0051edb0 WM_SETFONT 0x1343): Times New Roman 18, top-aligned static, black
             * (was Tahoma 11 bold) */
            draw_text(ac, 175, Y + ANS_Y[b], 17, 18, mm_str(29035 + b), FONT_NAME, 0, TX_LEFT);
            int hv = en && !g_nmsg && hover(195, Y + ANS_Y[b], 265, 18);
            int ay = Y + ANS_Y[b];
            if (hv) {
                /* hover as in a screenshot of the original (user, 2026-11-18): the button stands out as a raised
                 * button - lighter face, dark outline, white top/left and grey bottom/right bevel; text stays black */
                mm_fill(ac, 195, ay, 265, 18, 0xF4F3EE);
                mm_frame(ac, 195, ay, 265, 18, 1, 0x3C3A34);
                mm_fill(ac, 196, ay + 1, 263, 1, 0xFFFFFF);
                mm_fill(ac, 196, ay + 1, 1, 16, 0xFFFFFF);
                mm_fill(ac, 196, ay + 16, 263, 1, 0x9A968A);
                mm_fill(ac, 458, ay + 1, 1, 16, 0x9A968A);
            } else {
                mm_fill(ac, 195, ay, 265, 18, mm_opt.bigtext && en ? 0xFFFFFF : g_panel_bg);   /* high contrast: white */
                mm_frame(ac, 195, ay, 265, 18, 1, en ? (mm_opt.bigtext ? 0x3C3A34 : C_LINE) : 0xC0BCAE);
            }
            answer_feedback(ac, b, 195, ay, 265, 18);
            /* answer buttons (group 0x88c): FONT_ANSWER = Tahoma 11, centred, like the user's screenshot of the original
             * (FUN_0051edb0 WM_SETFONT 0x1341 suggested Times New Roman 14; the screenshot wins, 2026-11-18) */
            mm_rich(ac, 199, ay, 259, 18, g_q.q->answer[g_q.order[b]], FONT_ANSWER, FONT_ANSWER_I,
                      !en ? (mm_opt.bigtext ? 0x8A8678 : 0xA8A498) : mm_opt.bigtext ? 0 : C_TEXT, TX_VCENTER | TX_CENTER);   /* centred, user 2026-11-18 */
            snprintf(res, sizeof res, "res/%d.bmp", 29150 + b);
            strip(ac, res, hover(465, Y + ANS_Y[b], 18, 18) ? 1 : 0, 465, Y + ANS_Y[b], 18, 18);
        }
        mm_draw_img(c, "res/29166.bmp", 522, Y + 17);
        char pts[16];
        snprintf(pts, sizeof pts, "%d", q_points());
        /* bigger than the original; centred on the seal's circle (bitmap 29166 x 13-68 -> centre 562.5, not the bitmap's 567) */
        /* maroon as in a screenshot of the original (user, 2026-11-18); the decomp does not show the colour (set
         * in the timer callback LAB_00522660, not decompiled) */
        draw_text(c, 518, Y + 28, 90, 30, pts, FONT_POINTS, C_RED, TX_CENTER | TX_VCENTER);
    }
    if (mm_opt.resume) panel_button(c, 492, Y + 108, 140, 19, "Main Menu");   /* CHANGE: one button, the game kept */
    else {
        panel_button(c, 492, Y + 108, 69, 19, mm_str(29068));
        panel_button(c, 563, Y + 108, 69, 19, mm_str(29069));
    }
    /* name: label 0x7169 at (488,4,148,20), centred, top-aligned, font RCDATA 4931 = Times New Roman 18 (was 16 px,
     * vertically centred in a 22 px box); screenshot "Tourist" spans y 308..321 = TNR 18 from y 4; black.
     * Box 2 px taller than the original so descenders are not clipped. Drawn last: never covered. */
    draw_text(c, 488, Y + 4, 148, 22, p->name, FONT_NAME, 0, TX_CENTER | TX_ELLIPSIS);
}

/* touch layout (CHANGE 2026-11-21, user): the player list and its buttons live in the column, so the picture keeps
 * only its two hotspots - marked, as a finger has no hover - and a card with who will play */
static void draw_logon_touch(MMCanvas *c)
{
    mm_draw_img(c, "LOGONSCR.DIB", 0, 0);
    if (anim_on(AN_SHIMMER)) {                              /* a glint over the door, then over the story banner */
        shimmer(c, 135, 21, 64, 100, glint_t(0));            /* left to right: the story banner, then the door */
        shimmer(c, 253, 29, 123, 200, glint_t(1));
    }
    int x = 462, y = 268, w = 160, h = 156, n = 0;
    mm_fill(c, x, y, w, h, 0xF4F3EE);
    mm_frame(c, x, y, w, h, 1, 0x3C3A34);
    draw_text(c, x + 8, y + 4, w - 16, 22, g_lg_mp ? "This game: players" : "This game: player", FONT_T_B, 0, TX_LEFT | TX_VCENTER);
    mm_fill(c, x + 8, y + 27, w - 16, 1, 0x000000);
    for (int i = 0; i < g_nlg && n < 5; i++) {
        if (!g_lg[i].sel) continue;
        int ry = y + 32 + n * 24;
        mm_disc(c, x + 16, ry + 11, 6, RANK_COL[g_lg[i].award]);
        draw_text(c, x + 28, ry, w - 36, 22, g_lg[i].name, FONT_T, 0, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
        n++;
    }
    if (n) draw_text(c, x + 8, y + h - 26, w - 16, 22, "Tap the castle door", FONT_T_B, C_RED, TX_LEFT | TX_VCENTER);
    int tall = mm_view() == MM_VIEW_TOUCH_TALL;
    if (!n) draw_text(c, x + 8, y + 36, w - 16, h - 44, g_lg_mp ? (tall ? "Tap 2 to 5 names in the list below." : "Tap 2 to 5 names in the list on the right.")
                      : tall ? "Tap your name in the list below." : "Tap your name in the list on the right.", FONT_T, 0x6E655A, TX_WRAP);
}

static void draw_logon(MMCanvas *c)
{
    if (touch_view()) { draw_logon_touch(c); return; }
    mm_draw_img(c, "LOGONSCR.DIB", 0, 0);
    if (anim_on(AN_SHIMMER)) {                              /* (desktop too) the glint, banner then door */
        shimmer(c, 135, 21, 64, 100, glint_t(0));
        shimmer(c, 253, 29, 123, 200, glint_t(1));
    }
    text_button(c, 470, 277, 80, 17, mm_str(29024), !g_lg_edit);
    text_button(c, 556, 277, 57, 17, mm_str(29025), !g_lg_edit);
    if (mm_opt.prompts) {                                  /* CHANGE: who plays (see lg_pick) */
        static const char *const M[2] = {"1 player", "Several"};
        for (int k = 0; k < 2; k++) {
            int x = k ? 543 : 470, on = g_lg_mp == k, hv = !g_lg_edit && hover(x, 258, 70, 17);
            mm_fill(c, x, 258, 70, 17, on ? 0x3C3A34 : 0xF2F1EA);
            mm_frame(c, x, 258, 70, 17, 1, 0x2E2B25);
            draw_text(c, x, 258, 70, 17, M[k], FONT_TEXT, on ? 0xFFFFFF : hv ? C_RED : 0, TX_CENTER | TX_VCENTER);
        }
    }
    text_button(c, 470, 404, mm_opt.prompts ? 80 : 143, 17, "Settings...", 1);   /* CHANGE: settings, always usable */
    if (mm_opt.prompts) text_button(c, 556, 404, 57, 17, "Stats", !g_lg_edit);   /* CHANGE: player stats */
    mm_fill(c, 471, 297, 141, 102, 0xFFFFFF);
    mm_frame(c, 470, 296, 143, 104, 1, C_LINE);
    int y = 297, row = 0, ih = 16;
    if (g_lg_edit) {
        mm_frame(c, 471, 297, 124, 15, 1, C_TEXT);
        char t[80];
        snprintf(t, sizeof t, "%s%s", g_lg_text, (plat_ticks() / 500) & 1 ? "|" : "");
        draw_text(c, 474, 297, 120, 15, t, FONT_LIST, 0, TX_LEFT | TX_VCENTER);
        y += ih;
        row = 1;
    }
    for (int i = g_lg_scroll; i < g_nlg && y + ih <= 399; i++, y += ih, row++) {
        if (g_lg[i].sel) mm_fill(c, 471, y, 141, ih, C_RED);
        draw_text(c, 474, y, 136, ih, g_lg[i].name, FONT_LIST, g_lg[i].sel ? 0xFFFFFF : 0, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
    }
    (void)row;
}

static void draw_msgbox(MMCanvas *c)
{
    Msg *m = &g_msg[0];
    int w = 320, h = 130, x = (MM_W - w) / 2, y = (MM_H - h) / 2 - 20;
    mm_fill(c, x, y, w, h, g_panel_bg);
    mm_frame(c, x, y, w, h, 2, C_TEXT);
    mm_fill(c, x + 2, y + 2, w - 4, 18, C_RED);
    draw_text(c, x + 8, y + 2, w - 16, 18, mm_str(29009), FONT_BOLD, 0xFFFFFF, TX_LEFT | TX_VCENTER);
    int big = mm_opt.bigtext && mm_rich_height(c, w - 28, m->text, FONT_Q_L, FONT_Q_L) <= h - 64;
    draw_text(c, x + 14, y + 28, w - 28, h - 64, m->text, big ? FONT_Q_L : FONT_TEXT, mm_opt.bigtext ? 0 : C_TEXT, TX_WRAP);
    if (m->yesno) {
        text_button(c, x + w / 2 - 80, y + h - 30, 70, 20, "Yes", 1);
        text_button(c, x + w / 2 + 10, y + h - 30, 70, 20, "No", 1);
    } else text_button(c, x + (w - 70) / 2, y + h - 30, 70, 20, "OK", 1);
}

static void story_geom(int *x, int *y, int *w, int *h)
{
    *w = g_story.kind ? 433 : 432;
    *h = g_story.kind ? 285 : 380;
    *x = (MM_W - *w) / 2;
    *y = (MM_H - *h - 18) / 2 + 18;
}

static void draw_story(MMCanvas *c)                                        /* FUN_005301e0 */
{
    int x, y, w, h;
    story_geom(&x, &y, &w, &h);
    mm_fill(c, x - 2, y - 20, w + 4, h + 22, C_TEXT);
    mm_fill(c, x, y - 18, w, 18, C_RED);
    draw_text(c, x + 6, y - 18, w - 30, 18, mm_str(29009), FONT_BOLD, 0xFFFFFF, TX_LEFT | TX_VCENTER);
    draw_text(c, x + w - 20, y - 18, 18, 18, "X", FONT_BOLD, hover(x + w - 20, y - 18, 18, 18) ? 0xFFFF80 : 0xFFFFFF,
              TX_CENTER | TX_VCENTER);
    mm_fill(c, x, y, w, h, 0xFFFFFF);
    mm_draw_img(c, g_story.pages[g_story.page], x, y);
    if (g_story.page + 1 < g_story.npages) {
        int bx = x + (g_story.kind ? 390 : 397), by = y + (g_story.kind ? 243 : 342);
        strip(c, "res/29163.bmp", hover(bx, by, 18, 17) ? 1 : 0, bx, by, 18, 17);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* touch layout (CHANGE 2026-11-21, Modern options > Layout). The 636 x 437 game stays as it is in the top left; its
 * panel (y 301..436) becomes a strip with big matches, the map and the award / points; everything else is in a
 * column of 300 x 437 units: right of the game (landscape, zoom 1) or below it (portrait, zoom 2). Dialogs (messages,
 * settings, stats, stories) keep their 636 x 437 space, moved to the middle. */
enum { COL_W = 300, COL_H = 437 };

static int touch_view(void) { return mm_view() != MM_VIEW_CLASSIC; }
static void col_place(int *x, int *y, int *zoom)           /* the column's place in the view (logical units) */
{
    if (mm_view() == MM_VIEW_TOUCH_TALL) { *x = (MM_W - 2 * COL_W) / 2; *y = 301 + mm_view_strip(); *zoom = 2; }
    else { *x = MM_W + mm_view_ox(); *y = 0; *zoom = 1; }        /* (right of the widescreen room's edge too) */
}
static int in_col(int x, int y, int *cx, int *cy)          /* view -> column units */
{
    int ox, oy, z;
    col_place(&ox, &oy, &z);
    if (!touch_view() || x < ox || y < oy || x >= ox + COL_W * z || y >= oy + COL_H * z) return 0;
    *cx = (x - ox) / z;
    *cy = (y - oy) / z;
    return 1;
}
static void dlg_origin(int *dx, int *dy)                   /* where the 636 x 437 dialog space is drawn */
{
    *dx = mm_view() == MM_VIEW_TOUCH_WIDE ? (mm_view_w() - MM_W) / 2 - mm_view_ox() : 0;
    *dy = mm_view() == MM_VIEW_CLASSIC ? 0 : (mm_view_h() - MM_H) / 2;
}
static int dialog_open(void) { return g_nmsg || g_story.on || mm_settings_on() || mm_stats_on(); }

static int col_font(int big) { return big ? (mm_opt.bigtext ? FONT_T_LB : FONT_T_B) : mm_opt.bigtext ? FONT_T_L : FONT_T; }

static void tbutton(MMCanvas *c, int x, int y, int w, int h, const char *s, int hot, int on, int primary)
{
    uint32_t face = primary ? C_RED : on ? 0x3C3A34 : hot ? 0xFFFFFF : 0xF2F1EA;
    mm_fill(c, x, y, w, h, face);
    mm_frame(c, x, y, w, h, 1, primary || on ? 0x000000 : hot ? C_RED : mm_opt.bigtext ? 0x3C3A34 : C_LINE);
    draw_text(c, x + 4, y, w - 8, h, s, col_font(1), primary || on ? 0xFFFFFF : hot ? C_RED : 0, TX_CENTER | TX_VCENTER | TX_ELLIPSIS);
}

/* column targets (column units), also used by its hit test: 810+b area, 830+l level, 840+b answer, 850+b refer,
 * 860 hint, 861 new game, 862 settings, 863 quit, 864 stats, 880 speech (close); logon: 900+row player,
 * 970 new player, 971 delete, 972 settings, 973 stats, 975 / 976 list up / down, 977 / 978 1 player / several */
enum { CA_Y = 138, CA_H = 34, CL_Y = 80, CB_Y = 396, CB_H = 38, CQ_Y = 86, CQ_H = 108, CANS_Y = 198, CANS_H = 44 };
static void carea_rect(int b, int *x, int *y, int *w) { *x = 2 + (b / 5) * 149; *y = CA_Y + (b % 5) * (CA_H + 2); *w = 147; }
enum { CM_X = 256, CM_Y = 154 };                             /* the matches beside the map, the map's top */
enum { CLROW_Y = 92, CLROW_H = 30 };                         /* the list below the 1 player / several players switch */
static int col_rows(void) { return g_resume_names[0] ? 7 : 8; }   /* (a saved game: its Continue button under the list) */
enum { CLB1_Y = 354, CLB2_Y = 396 };                        /* rows of buttons under it (the game starts at the castle door) */

static int col_hit(int x, int y)
{
    if (g_screen == SCR_LOGON) {
        if (!g_lg_edit && inside(x, y, 2, 34, 146, 34)) return 977;      /* 1 player */
        if (!g_lg_edit && inside(x, y, 152, 34, 146, 34)) return 978;    /* several players */
        for (int r = 0; r < col_rows(); r++) {
            int i = g_lg_scroll + r - (g_lg_edit ? 1 : 0);
            if (inside(x, y, 2, CLROW_Y + r * CLROW_H, 262, CLROW_H - 2) && i >= 0 && i < g_nlg && !g_lg_edit) return 900 + i;
        }
        if (g_nlg > col_rows() && inside(x, y, 268, CLROW_Y, 30, 40)) return 975;
        if (g_nlg > col_rows() && inside(x, y, 268, CLROW_Y + col_rows() * CLROW_H - 40, 30, 40)) return 976;
        if (g_resume_names[0] && !g_lg_edit && inside(x, y, 2, CLROW_Y + 7 * CLROW_H + 4, 296, 40)) return 979;   /* Continue */
        if (inside(x, y, 2, CLB1_Y, 146, 36)) return 970;
        if (!g_lg_edit && inside(x, y, 152, CLB1_Y, 146, 36)) return 971;
        if (inside(x, y, 2, CLB2_Y, mm_opt.prompts ? 146 : 296, 36)) return 972;
        if (mm_opt.prompts && !g_lg_edit && inside(x, y, 152, CLB2_Y, 146, 36)) return 973;
        return 0;
    }
    Player *p = cur();
    if (!p) return 0;
    if (g_qopen) {
        if (g_inactive) return 0;
        for (int b = 0; b < 4; b++) {
            int ay = CANS_Y + b * (CANS_H + 4);
            if (!g_q.disabled[b] && inside(x, y, 24, ay, 232, CANS_H)) return 840 + b;
            if (inside(x, y, 260, ay, 38, CANS_H)) return 850 + b;
        }
        if (hint_ok() && inside(x, y, 2, CB_Y, 146, CB_H)) return 860;
        return 0;
    }
    if (inside(x, y, 2, CB_Y, 146, CB_H)) return 861;                       /* Main Menu */
    if (inside(x, y, 152, CB_Y, 146, CB_H)) return 862;                     /* Settings */
    if (g_screen != SCR_ROOM) return 0;

    if (inside(x, y, 84, 44, 210, 30)) return 820;                        /* Topic: ... (opens the picker) */
    for (int l = 0; l < 4; l++) if (inside(x, y, 84 + l * 53, CL_Y, 50, 30)) return 830 + l;   /* Difficulty 1-4 */
    if (g_col_areas)
        for (int b = 0; b < 10; b++) {
            int ax, ay, aw;
            carea_rect(b, &ax, &ay, &aw);
            if (inside(x, y, ax, ay, aw, CA_H)) return 810 + b;
        }
    else
        for (int i = 0; i < 5; i++)                                   /* the matches */
            if (p->maze->match[i] && !g_match_end && inside(x, y, CM_X, CM_Y + i * 48, 40, 46)) return 200 + i;
    return 0;
}

static void draw_col(MMCanvas *c)                          /* c = the column's canvas (column units) */
{
    int h = g_hover;
    mm_fill(c, 0, 0, COL_W, COL_H, g_panel_bg);
    mm_frame(c, 0, 0, COL_W, COL_H, 1, C_LINE);
    char t[200], res[32];
    if (g_screen == SCR_LOGON) {
        if (g_title_on) return;
        /* who plays: one player (a tap picks that name) or several (taps add and remove, up to five) */
        int nsel = 0;
        for (int i = 0; i < g_nlg; i++) nsel += g_lg[i].sel;
        draw_text(c, 8, 2, COL_W - 16, 30, "Who plays?", FONT_T_H, 0, TX_LEFT | TX_VCENTER);
        tbutton(c, 2, 34, 146, 34, "1 player", h == 977, !g_lg_mp, 0);
        tbutton(c, 152, 34, 146, 34, "Several players", h == 978, g_lg_mp, 0);
        if (g_lg_mp) snprintf(t, sizeof t, nsel >= 2 ? "%d chosen - now tap the castle door" : "Tap 2 to 5 names (%d chosen)", nsel);
        else snprintf(t, sizeof t, nsel ? "Now tap the castle door" : "Tap your name");
        draw_text(c, 8, 70, COL_W - 16, 20, g_lg_edit ? "Type the new name, then OK" : t, FONT_T, 0x3C3A34, TX_LEFT | TX_VCENTER);
        mm_fill(c, 2, CLROW_Y - 2, 262, col_rows() * CLROW_H + 2, 0xFFFFFF);
        int r = 0;
        if (g_lg_edit) {
            snprintf(t, sizeof t, "%s%s", g_lg_text, (plat_ticks() / 500) & 1 ? "|" : "");
            mm_frame(c, 3, CLROW_Y, 260, CLROW_H - 2, 2, C_RED);
            draw_text(c, 10, CLROW_Y, 246, CLROW_H - 2, t, col_font(0), 0, TX_LEFT | TX_VCENTER);
            r = 1;
        }
        for (int i = g_lg_scroll; i < g_nlg && r < col_rows(); i++, r++) {
            int y = CLROW_Y + r * CLROW_H;
            if (g_lg[i].sel) mm_fill(c, 2, y, 262, CLROW_H - 2, C_RED);
            else if (h == 900 + i) mm_fill(c, 2, y, 262, CLROW_H - 2, 0xF4F3EE);
            if (g_lg_mp) {                                  /* check boxes: several can be on */
                mm_fill(c, 8, y + 7, 14, 14, 0xFFFFFF);
                mm_frame(c, 8, y + 7, 14, 14, 1, 0x3C3A34);
                if (g_lg[i].sel) mm_fill(c, 11, y + 10, 8, 8, 0x3C3A34);
            }
            draw_text(c, g_lg_mp ? 30 : 10, y, 226, CLROW_H - 2, g_lg[i].name, col_font(g_lg[i].sel), g_lg[i].sel ? 0xFFFFFF : 0,
                      TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
            mm_disc(c, 252, y + 14, 5, RANK_COL[g_lg[i].award]);   /* the rank's colour */
        }
        if (g_nlg > col_rows()) {
            tbutton(c, 268, CLROW_Y, 30, 40, "^", h == 975, 0, 0);
            tbutton(c, 268, CLROW_Y + col_rows() * CLROW_H - 40, 30, 40, "v", h == 976, 0, 0);
        }
        if (g_resume_names[0] && !g_lg_edit) {             /* CHANGE: the game left with Main Menu (or an unfinished one) */
            snprintf(t, sizeof t, "Continue: %s", g_resume_names);
            tbutton(c, 2, CLROW_Y + 7 * CLROW_H + 4, 296, 40, t, h == 979, 0, 1);
        }
        tbutton(c, 2, CLB1_Y, 146, 36, g_lg_edit ? "OK" : mm_str(29024), h == 970, 0, 0);
        if (!g_lg_edit) tbutton(c, 152, CLB1_Y, 146, 36, mm_str(29025), h == 971, 0, 0);
        tbutton(c, 2, CLB2_Y, mm_opt.prompts ? 146 : 296, 36, "Settings", h == 972, 0, 0);
        if (mm_opt.prompts && !g_lg_edit) tbutton(c, 152, CLB2_Y, 146, 36, "Stats", h == 973, 0, 0);
        return;
    }
    Player *p = cur();
    if (!p) return;
    /* header: whose turn, and the score - or the points the open question is worth */
    draw_text(c, 8, 4, 190, 30, p->name, FONT_T_H, 0, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
    if (g_qopen) {
        /* CHANGE (user): the points seal of the classic panel, twice the size, centred above the answers; the
         * points run down with the clock, so no separate timer */
        mm_fill(c, 6, 36, COL_W - 12, 1, 0x000000);
        MMCanvas sc = mm_canvas_sub(c, (COL_W - 180) / 2, 38, 90, 80, 2);
        mm_draw_img(&sc, "res/29166.bmp", 0, 0);
        snprintf(t, sizeof t, "%d", q_points());
        draw_text(&sc, -4, 11, 90, 30, t, FONT_POINTS, C_RED, TX_CENTER | TX_VCENTER);
    } else {
        snprintf(t, sizeof t, "Score %d", shown_score(p));
        draw_text(c, 160, 4, 134, 30, t, col_font(1), 0, TX_RIGHT | TX_VCENTER);
        mm_fill(c, 6, 36, COL_W - 12, 1, 0x000000);
    }
    if (g_qopen) {
        if (g_inactive) return;
        int slide = anim_on(AN_ANSWERS) ? (int)((1 - ease(anim_t(AN_ANSWERS))) * 60) : 0;
        MMCanvas acv = *c;
        acv.ox += slide;
        for (int b = 0; b < 4; b++) {
            int ay = CANS_Y + b * (CANS_H + 4), en = !g_q.disabled[b], hv = en && h == 840 + b;
            draw_text(&acv, 2, ay, 20, CANS_H, mm_str(29035 + b), FONT_T_H, en ? 0 : 0xA8A498, TX_CENTER | TX_VCENTER);
            mm_fill(&acv, 24, ay, 232, CANS_H, !en ? g_panel_bg : hv ? 0xFFFFFF : 0xF4F3EE);
            mm_frame(&acv, 24, ay, 232, CANS_H, 1, !en ? 0xC0BCAE : hv ? C_RED : mm_opt.bigtext ? 0x3C3A34 : C_LINE);
            answer_feedback(&acv, b, 24, ay, 232, CANS_H);
            mm_rich(&acv, 30, ay + 2, 220, CANS_H - 4, g_q.q->answer[g_q.order[b]], col_font(0), mm_opt.bigtext ? FONT_T_L : FONT_T,
                    !en ? 0xA8A498 : 0, TX_WRAP | TX_CENTER | TX_VCENTER);
            /* the original "look it up" icon (res 29150 + b) at twice the size, in a button (user: the icons were missing) */
            mm_fill(&acv, 260, ay, 38, CANS_H, h == 850 + b ? 0xFFFFFF : 0xF4F3EE);
            mm_frame(&acv, 260, ay, 38, CANS_H, 1, h == 850 + b ? C_RED : C_LINE);
            MMCanvas ic = mm_canvas_sub(&acv, 261, ay + (CANS_H - 36) / 2, 18, 18, 2);
            snprintf(res, sizeof res, "res/%d.bmp", 29150 + b);
            strip(&ic, res, h == 850 + b ? 1 : 0, 0, 0, 18, 18);
        }
        if (hint_ok()) tbutton(c, 2, CB_Y, 146, CB_H, "50/50 hint", h == 860, 0, 0);
        return;
    }
    if (g_screen != SCR_ROOM) {
        draw_text(c, 12, 60, COL_W - 24, 200, g_screen == SCR_GRATING ? "Tap the castle gate to go in."
                  : "Choose a door: tap one of them.", col_font(0), 0, TX_WRAP);
    } else {                                               /* (a character's speech is in the room: the pop-up) */
        /* CHANGE 2026-11-21 (user): two groups, set apart - "Questions" (the area is one button that opens the
         * picker, then the four levels) and the map (drawn at zoom 2: the same lines and marks, twice the size) */
        /* (user: say plainly what the two rows are) "Topic" = the question area, "Difficulty" = the level */
        mm_fill(c, 2, 40, COL_W - 4, 92, 0xCFCAB6);
        mm_frame(c, 2, 40, COL_W - 4, 92, 1, 0xA09683);
        draw_text(c, 8, 44, 74, 30, "Topic", FONT_T_B, 0x3C3A34, TX_LEFT | TX_VCENTER);
        tbutton(c, 84, 44, 210, 30, mm_str(AREA_TEXT[p->areabtn]), h == 820, g_col_areas, 0);
        draw_text(c, 8, CL_Y, 74, 30, "Difficulty", FONT_T_B, 0x3C3A34, TX_LEFT | TX_VCENTER);
        for (int l = 0; l < 4; l++) {
            char n[2] = {(char)('1' + l), 0};
            tbutton(c, 84 + l * 53, CL_Y, 50, 30, n, h == 830 + l, p->level == l, 0);
        }
        draw_text(c, 84, 112, 210, 18, "1 = easiest    4 = hardest", FONT_TEXT, 0x3C3A34, TX_CENTER | TX_VCENTER);
        if (g_col_areas) {
            for (int b = 0; b < 10; b++) {
                int ax, ay, aw;
                carea_rect(b, &ax, &ay, &aw);
                int on = p->areabtn == b, hv = h == 810 + b;
                mm_fill(c, ax, ay, aw, CA_H, on ? 0x3C3A34 : hv ? 0xFFFFFF : 0xF4F3EE);
                mm_frame(c, ax, ay, aw, CA_H, 1, on ? 0x000000 : hv ? C_RED : mm_opt.bigtext ? 0x3C3A34 : C_LINE);
                const char *s = mm_str(AREA_TEXT[b]);
                int f = mm_rich_height(c, aw - 10, s, FONT_T, FONT_T) <= CA_H - 4 ? FONT_T : FONT_TEXT;
                mm_rich(c, ax + 5, ay + 2, aw - 10, CA_H - 4, s, f, f, on ? 0xFFFFFF : 0, TX_WRAP | TX_CENTER | TX_VCENTER);
            }
        } else {
            /* (user: the torches next to the maze) the map at zoom 2 with 11 x 9 cells, the five matches beside it */
            draw_text(c, 8, 136, 200, 16, "Map", FONT_BOLD, 0x6E655A, TX_LEFT | TX_VCENTER);
            draw_text(c, CM_X - 4, 136, 48, 16, "Light", FONT_BOLD, 0x6E655A, TX_CENTER | TX_VCENTER);
            MMCanvas mc = mm_canvas_sub(c, 8, CM_Y, 122, 102, 2);
            mm_frame(&mc, 0, 0, 122, 102, 1, mm_opt.bigtext ? 0x6E655A : C_LINE);
            draw_map(&mc, p->maze, 1, 1, 112, 92);
            match_glow(&mc, 2, 2, 118, 98);
            MMCanvas tc = mm_canvas_sub(c, CM_X, CM_Y, 20, 5 * 24, 2);
            for (int i = 0; i < 5; i++) {
                snprintf(res, sizeof res, "res/%d.bmp", 29158 + i);
                strip(&tc, res, !p->maze->match[i] ? 2 : h == 200 + i ? 1 : 0, 0, i * 24, 20, 23);
                mm_frame(&tc, 0, i * 24, 20, 23, 1, C_LINE);
            }
        }
    }
    /* CHANGE 2026-11-21 (user): New Game and Quit Game both led back to the gate - one "Main Menu" (the game is kept
     * and can be continued there) */
    tbutton(c, 2, CB_Y, 146, CB_H, "Main Menu", h == 861, 0, 0);
    tbutton(c, 152, CB_Y, 146, CB_H, "Settings", h == 862, 0, 0);
}

/* the strip under the room in the touch layout: big matches (drawn at zoom 2), the award or the points, everyone's
 * rank and score (the map is big in the column) */
enum { SM_X = 8, SM_Y = 310, SM_W = 48, SM_H = 50 };
static int strip_hit(int x, int y)
{
    (void)x; (void)y;
    return 0;                                              /* (the matches moved next to the map, in the column) */
    Player *p = cur();
    if (!p || g_screen == SCR_LOGON) return 0;
    for (int i = 0; i < 5; i++)
        if (p->maze->match[i] && !g_match_end && inside(x, y, SM_X + i * SM_W, SM_Y, SM_W - 4, SM_H)) return 200 + i;
    return 0;
}
/* an award picture with its flat square background keyed out (as the classic panel does, once per picture) */
static MMImage *award_keyed(int rank)
{
    MMImage *aw = mm_image(mm_ranks[rank].image);
    if (!aw || !(aw->px[0] >> 24)) return aw;                /* (already keyed: the corner is clear) */
    uint32_t bgc = aw->px[0] & 0xFFFFFF;
    for (int i = 0; i < aw->w * aw->h; i++) {
        uint32_t v = aw->px[i] & 0xFFFFFF;
        int dr = (int)(v >> 16) - (int)(bgc >> 16), dg = (int)(v >> 8 & 255) - (int)(bgc >> 8 & 255), db = (int)(v & 255) - (int)(bgc & 255);
        if (dr * dr + dg * dg + db * db < 300) aw->px[i] &= 0x00FFFFFF;
    }
    return aw;
}

/* the way to the top award in one bar (CHANGE 2026-11-21, user): filled up to the score, a tick where each award
 * begins, the segment of the next award framed; under it which award comes next and how many points are missing */
static void award_bar(MMCanvas *c, int x, int y, int w, int h, const Player *p)
{
    /* (user: it has to fill visibly) one equal segment per award, the current one filled by the way to the next */
    int top = mm_ranks[5].min > 0 ? mm_ranks[5].min : 20000, sc = shown_score(p), a = p->award < 5 ? p->award : 5;
    double f = 1;
    if (a < 5) {
        int lo = mm_ranks[a].min, hi = mm_ranks[a + 1].min;
        double in = hi > lo ? (double)(sc - lo) / (hi - lo) : 0;
        f = (a + (in < 0 ? 0 : in > 1 ? 1 : in)) / 5;
    }
    mm_fill(c, x, y, w, h, 0xFFFFFF);
    int fw = (int)(w * f);
    if (sc > 0 && fw < 2) fw = 2;
    mm_fill(c, x, y, fw, h, 0xE8C872);
    for (int i = 1; i <= 5; i++) {                          /* the awards: a tick and their colour */
        int tx = x + w * i / 5;
        if (i < 5) mm_fill(c, tx, y, 1, h, 0x6E655A);
        mm_disc(c, tx - (i == 5 ? 1 : 0) + 0.5, y + h + 3, 3, RANK_COL[i]);
    }
    if (a < 5) mm_frame(c, x + w * a / 5, y, w / 5 + 1, h, 2, C_RED);   /* the stretch to the next award */
    mm_frame(c, x, y, w, h, 1, 0x3C3A34);
    char t[128];                                           /* the words under the bar (the ticks cross nothing) */
    if (p->award < 5)
        snprintf(t, sizeof t, "%d / %d  -  %s in %d", sc, top, mm_ranks[p->award + 1].name,
                 mm_ranks[p->award + 1].min - p->score > 0 ? mm_ranks[p->award + 1].min - p->score : 0);
    else snprintf(t, sizeof t, "%d / %d  -  the highest award", sc, top);
    draw_text(c, x, y + h + 8, w, 18, t, FONT_BOLD, 0x3C3A34, TX_CENTER | TX_VCENTER | TX_ELLIPSIS);
}

static void draw_strip(MMCanvas *c)
{
    const int Y = 301;
    Player *p = cur();
    mm_fill(c, -c->ox, Y, MM_W + 2 * c->ox, mm_view_strip(), g_panel_bg);   /* (as tall as the screen leaves) */
    /* the active player's award */
    {
        MMImage *aw = award_keyed(p->award);
        if (aw) {
            rank_glow(c, 312, Y + 50);
            mm_draw_img(c, mm_ranks[p->award].image, 312 - aw->w / 2, Y + 50 - aw->h / 2);
        }
        draw_text(c, 262, Y + 96, 100, 22, mm_ranks[p->award].name, FONT_BOLD, 0, TX_CENTER | TX_VCENTER | TX_ELLIPSIS);
    }
    /* CHANGE 2026-11-21 (user): progress over the whole game (20000) and towards the next award */
    draw_text(c, SM_X, Y + 8, 246, 18, "Way to the top award", FONT_BOLD, 0x3C3A34, TX_LEFT | TX_VCENTER);
    award_bar(c, SM_X, Y + 30, 246, 16, p);                /* (user: one bar, a segment per award) */
    /* CHANGE 2026-11-21 (user): everyone's rank and score, the active player marked; one player: this game's numbers */
    int n = 0, y = Y + 6;
    char t[64];
    for (int i = 0; i < NPLAYERS; i++) n += g_pl[i] != NULL;
    for (int i = 0; i < NPLAYERS; i++) {
        Player *q = g_pl[i];
        if (!q) continue;
        int on = q == p;
        if (on) mm_fill(c, 372, y, 258, 22, 0x3C3A34);
        mm_disc(c, 382, y + 11, 5, RANK_COL[q->award]);
        draw_text(c, 392, y, 70, 22, mm_ranks[q->award].name, FONT_TEXT, on ? 0xE8C872 : 0x6E655A, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
        draw_text(c, 464, y, 110, 22, q->name, on ? FONT_T_B : FONT_T, on ? 0xFFFFFF : 0, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
        snprintf(t, sizeof t, "%d", q == p ? shown_score(q) : q->score);
        draw_text(c, 574, y, 52, 22, t, FONT_T, on ? 0xFFFFFF : 0, TX_RIGHT | TX_VCENTER);
        y += 24;
    }
    if (n == 1) {
        snprintf(t, sizeof t, "This game: %d of %d right", p->right, p->ans);
        draw_text(c, 380, y + 8, 246, 22, t, FONT_T, 0, TX_LEFT | TX_VCENTER);
        snprintf(t, sizeof t, "Streak: %d    Rank: %s", p->streak, mm_ranks[p->award].name);
        draw_text(c, 380, y + 34, 246, 22, t, FONT_T, 0, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
    }
}

void mm_draw(MMCanvas *c)
{
    int touch = touch_view();
    hover_space(0, 0, 1);
    if (g_scr_capture) {                                   /* (Animations) the screen being left, still on the canvas */
        g_scr_capture = 0;
        free(g_scr_old.px);
        g_scr_old = snap(c, -c->ox, 0, MM_W + 2 * c->ox, MM_H);   /* (with the widescreen sides) */
        g_scr_scale = c->scale;
    }
    mm_fill(c, -c->ox, 0, mm_view_w(), mm_view_h(), 0);
    if (g_title_on) {                                       /* TITLESCR.DIB; touch views: in the middle */
        int dx, dy;
        dlg_origin(&dx, &dy);
        MMCanvas tc = touch ? mm_canvas_sub(c, dx, dy, MM_W, MM_H, 1) : *c;
        mm_draw_img(&tc, mm_str(29027), 0, 0);
        return;
    }
    if (g_screen == SCR_LOGON) {
        draw_logon(c);
        if (touch && 301 + mm_view_strip() > MM_H)            /* (the gate picture is 437 tall; a taller view: panel colour below) */
            mm_fill(c, -c->ox, MM_H, MM_W + 2 * c->ox, 301 + mm_view_strip() - MM_H, g_panel_bg);
    } else {
        if (g_screen == SCR_GRATING) {
            mm_draw_img(c, "GRATING.DIB", 0, 0);
            if (anim_on(AN_SHIMMER)) shimmer(c, 0, 0, MM_W, 301, glint_t(0));   /* (user: a tap anywhere goes on: the whole screen glints) */
        } else if (g_screen == SCR_DOORS) {
            mm_draw_img(c, mm_str(29006), 0, 0);
            static const int ORD[5] = {0, 2, 4, 1, 3};      /* the doors left to right (DOOR_HOT) */
            for (int i = 0; i < 5 && anim_on(AN_SHIMMER); i++)
                shimmer(c, DOOR_HOT[ORD[i]][0], DOOR_HOT[ORD[i]][1], DOOR_HOT[ORD[i]][2], DOOR_HOT[ORD[i]][3], glint_t(i));
        }
        else {
            MMCanvas rc = *c;                               /* CHANGE (Animations): the next room slides in */
            int dir = g_an[AN_ROOM].a;
            if (anim_on(AN_ROOM) && (dir == 2 || dir == 3)) rc.ox += (int)((dir == 2 ? 1 : -1) * (1 - ease(anim_t(AN_ROOM))) * 80);
            draw_room(&rc);
            if (anim_on(AN_ROOM))                           /* (the whole width, widescreen sides too) */
                fill_alpha(c, -c->ox, 0, MM_W + 2 * c->ox, 301, 0, (int)((1 - ease(anim_t(AN_ROOM))) * 230));
            if (!g_qopen && !anim_on(AN_ROOM)) {           /* what can be tapped (desktop too, user 2026-11-21) */
                if (mm_opt.anim) {                            /* the gate's glint over each object, left to right */
                    Hot hs[64];
                    int n = 0;
                    for (int i = 0; i < g_nhot; i++) if (g_hot[i].o) hs[n++] = g_hot[i];
                    qsort(hs, (size_t)n, sizeof hs[0], hot_order);
                    for (int i = 0; i < n; i++) obj_glint(c, &hs[i], glint_t(i));
                } else if (touch) {                           /* (no animations, touch: their outlines glow a moment) */
                    int a = taphint_alpha();
                    for (int i = 0; i < g_nhot && a > 0; i++) if (g_hot[i].o) obj_glow(c, &g_hot[i], a);
                }
            }
            if (g_qopen || g_speech) {                      /* the original pop-ups (touch: also big in the column) */
                /* CHANGE (Animations): the banner / speech picture fades in and rises into place */
                int pop = anim_on(AN_POPUP);
                double e = pop ? ease(anim_t(AN_POPUP)) : 1;
                Snap sv = pop ? snap(c, 0, 0, MM_W, 301) : (Snap){0};
                int rise = (int)((1 - e) * 24);
                MMCanvas oc = rise ? mm_canvas_sub(c, 0, rise, MM_W, 301 - rise, 1) : *c;
                if (g_qopen) draw_overlay(&oc, 1, g_q.q->text);
                else draw_overlay(&oc, 0, g_speech_text);         /* (Animations) its lines fade in, top first */
                unsnap(c, &sv, e);
            }
        }
        if (touch) draw_strip(c);
        else draw_panel(c);
        if (anim_on(AN_FADE)) fill_alpha(c, -c->ox, 0, MM_W + 2 * c->ox, 301, 0, (int)((1 - ease(anim_t(AN_FADE))) * 255));
        if (!touch && gear_ok()) draw_gear(c, g_hover == 700);
        if (!touch && hint_ok()) draw_hint(c, g_hover == 701);
    }
    screen_old_draw(c);                                     /* (Animations) the screen being left, growing away */
    if (mute_ok()) draw_mute(c, g_hover == 702);              /* (above the walk-in: it stays put) */
    if (touch) {
        int cx, cy, z;
        col_place(&cx, &cy, &z);
        MMCanvas cc = mm_canvas_sub(c, cx, cy, COL_W, (mm_view_h() - cy) / z, z);
        mm_fill(&cc, 0, 0, COL_W, (mm_view_h() - cy) / z, g_panel_bg);
        hover_space(cx, cy, z);
        draw_col(&cc);
        hover_space(0, 0, 1);
    }
    if (dialog_open()) {                                    /* in the 636 x 437 dialog space, in the middle */
        int dx, dy, slide = 0;
        dlg_origin(&dx, &dy);
        if (mm_opt.anim) {                                  /* (Animations) the view dims and the dialog slides up */
            double e = ease(anim_t(AN_DIALOG));
            fill_alpha(c, -c->ox, 0, mm_view_w(), mm_view_h(), 0, (int)(60 * e));
            slide = (int)((1 - e) * 18);
        }
        Snap ds = anim_on(AN_DIALOG) ? snap(c, dx, dy, MM_W, MM_H + 20) : (Snap){0};   /* (Animations) and fades in */
        MMCanvas dc = mm_canvas_sub(c, dx, dy + slide, MM_W, MM_H, 1);
        hover_space(dx, dy, 1);
        if (mm_settings_on()) mm_settings_draw(&dc, g_mx - dx, g_my - dy);
        if (mm_stats_on()) mm_stats_draw(&dc, g_mx - dx, g_my - dy);
        if (g_story.on) {
            draw_story(&dc);
            int x, y, w, h;
            story_geom(&x, &y, &w, &h);
            confetti(&dc, x, y, w, h);
        }
        if (g_nmsg) draw_msgbox(&dc);
        unsnap(c, &ds, ease(anim_t(AN_DIALOG)));
        hover_space(0, 0, 1);
    }
    g_hover = clickable_at(g_mx, g_my);                                    /* screen may have changed under the pointer */
    plat_set_cursor(g_hover != 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* input                                                                                             */


/* What is clickable under (x, y): 0 = nothing, else an id unique per target (hover highlight and the hand
 * cursor follow it; CHANGE: the original showed the hand only over room hotspots). */
static int clickable_main(int x, int y)
{
    if (g_title_on) return 1;
    if (g_nmsg) {
        int w = 320, h = 130, bx = (MM_W - w) / 2, by = (MM_H - h) / 2 - 20;
        if (g_msg[0].yesno) {
            if (inside(x, y, bx + w / 2 - 80, by + h - 30, 70, 20)) return 1;
            if (inside(x, y, bx + w / 2 + 10, by + h - 30, 70, 20)) return 2;
        } else if (inside(x, y, bx + (w - 70) / 2, by + h - 30, 70, 20)) return 1;
        return 0;
    }
    if (mm_settings_on()) { int h = mm_settings_hit(x, y); return h ? 500 + h : 0; }
    if (mm_stats_on()) { int h = mm_stats_hit(x, y); return h ? 1000 + h : 0; }
    if (g_story.on) {
        int sx, sy, sw, sh;
        story_geom(&sx, &sy, &sw, &sh);
        int bx = sx + (g_story.kind ? 390 : 397), by = sy + (g_story.kind ? 243 : 342);
        if (inside(x, y, sx + sw - 20, sy - 18, 18, 18)) return 10;
        if (g_story.page + 1 < g_story.npages && inside(x, y, bx, by, 18, 17)) return 11;
        return 0;
    }
    if (mute_ok() && inside(x, y, MUTE_X, MUTE_Y, MUTE_S, MUTE_S)) return 702;
    if (g_screen == SCR_LOGON) {
        if (inside(x, y, 253, 29, 123, 200)) return 20;
        if (inside(x, y, 135, 21, 64, 100)) return 21;
        if (touch_view()) return 0;                                       /* (the list is in the column) */
        if (inside(x, y, 470, 404, mm_opt.prompts ? 80 : 143, 17)) return 24;   /* Settings: always (CHANGE 2026-11-21) */
        if (mm_opt.prompts && !g_lg_edit && inside(x, y, 556, 404, 57, 17)) return 25;   /* Stats */
        if (g_lg_edit) return 0;
        if (inside(x, y, 470, 277, 80, 17)) return 22;
        if (inside(x, y, 556, 277, 57, 17)) return 23;
        if (mm_opt.prompts && inside(x, y, 470, 258, 70, 17)) return 26;
        if (mm_opt.prompts && inside(x, y, 543, 258, 70, 17)) return 27;
        if (inside(x, y, 471, 297, 141, 102)) {
            int i = g_lg_scroll + (y - 297) / 16;
            if (i < g_nlg) return 100 + i;
        }
        return 0;
    }
    if (!touch_view() && gear_ok() && inside(x, y, GEAR_X, GEAR_Y, GEAR_S, GEAR_S)) return 700;
    if (!touch_view() && hint_ok() && inside(x, y, HINT_X, HINT_Y, HINT_W, HINT_H)) return 701;
    if (y >= 301) {                                                        /* control panel */
        Player *p = cur();
        int py = y - 301;
        if (!p) return 0;
        for (int i = 0; i < 5; i++)
            if (p->maze->match[i] && !g_match_end && inside(x, py, 6, 8 + 24 * i, 20, 23)) return 200 + i;
        if (mm_opt.resume && inside(x, py, 492, 108, 140, 19)) return 211;
        if (!mm_opt.resume && inside(x, py, 492, 108, 69, 19)) return 210;
        if (!mm_opt.resume && inside(x, py, 563, 108, 69, 19)) return 211;
        if (g_qopen) {
            for (int b = 0; b < 4; b++) {
                if (!g_q.disabled[b] && inside(x, py, 195, ANS_Y[b], 265, 18)) return 220 + b;
                if (inside(x, py, 465, ANS_Y[b], 18, 18)) return 230 + b;
            }
            return 0;
        }
        for (int b = 0; b < 10; b++) {
            int ax, ay, aw;
            area_rect(b, &ax, &ay, &aw);
            if (inside(x, py, ax, ay, aw, 15)) return 240 + b;
        }
        for (int l = 0; l < 4; l++)
            if (inside(x, py, 455, LEVEL_Y[l], 29, 26)) return 260 + l;
        return 0;
    }
    if (g_screen == SCR_GRATING) return 300;
    if (g_screen == SCR_DOORS) {
        for (int i = 0; i < 5; i++)
            if (inside(x, y, DOOR_HOT[i][0], DOOR_HOT[i][1], DOOR_HOT[i][2], DOOR_HOT[i][3])) return 310 + i;
        return 0;
    }
    if (g_ov_btn_on && (g_qopen || g_speech))                          /* Previous / Next (hover redraw) */
        for (int b = 0; b < 2; b++) if (inside(x, y, g_ov_btn[b][0], g_ov_btn[b][1], g_ov_btn[b][2], g_ov_btn[b][3])) return 330 + b;
    if (g_qopen) return 0;
    if (g_speech) return 320;                                              /* any click closes the speech */
    int id = hot_at(x, y);
    return id >= 0 ? 400 + id : 0;
}

/* view coordinates -> the target under them (CHANGE 2026-11-21: touch layout column / strip, dialogs moved) */
static int clickable_at(int x, int y)
{
    if (!touch_view()) return clickable_main(x, y);
    if (g_title_on) return 1;
    if (dialog_open()) {
        int dx, dy;
        dlg_origin(&dx, &dy);
        return clickable_main(x - dx, y - dy);
    }
    int cx, cy;
    if (in_col(x, y, &cx, &cy)) return col_hit(cx, cy);
    if (x < -mm_view_ox() || x >= MM_W + mm_view_ox() || y < 0 || y >= 301 + mm_view_strip()) return 0;
    if (y >= 301 && g_screen != SCR_LOGON) return strip_hit(x, y);
    return clickable_main(x, y);
}

static void update_hover(void)                                             /* redraw only when the target changes */
{
    int h = clickable_at(g_mx, g_my);
    plat_set_cursor(h != 0);
    if (h != g_hover) { g_hover = h; plat_invalidate(); }
}

static void panel_click(int x, int y, int button, int action, int mods)
{
    Player *p = cur();
    int py = y - 301;
    if (action == MA_DOUBLE) {                                             /* debug cheats FUN_00520060 / 0x51ffc0 */
        Maze *m = p->maze;
        if (button == MMB_LEFT && (mods & MMOD_CTRL)) { m->cx = m->tx; m->cy = m->ty; room_at(m, m->cx, m->cy)->visited = 1; }
        else if (button == MMB_LEFT && (mods & MMOD_SHIFT)) p->score += 2001;
        else if (button == MMB_RIGHT && (mods & MMOD_CTRL)) {
            for (int i = 0; i < 100; i++) if (m->grid[i]) m->grid[i]->opened = m->grid[i]->doors;
        } else if (button == MMB_RIGHT && (mods & MMOD_SHIFT)) m->reveal = !m->reveal;
        plat_invalidate();
        return;
    }
    if (action != MA_DOWN || button != MMB_LEFT) return;
    for (int i = 0; i < 5; i++)
        if (inside(x, py, 6, 8 + 24 * i, 20, 23)) { match(i); return; }
    if (mm_opt.resume && inside(x, py, 492, 108, 140, 19)) { mm_cmd(CMD_QUIT); return; }   /* Main Menu */
    if (inside(x, py, 492, 108, 69, 19)) { mm_cmd(CMD_NEW_GAME); return; }
    if (inside(x, py, 563, 108, 69, 19)) { mm_cmd(CMD_QUIT); return; }
    if (g_qopen) {
        for (int b = 0; b < 4; b++) {
            if (inside(x, py, 195, ANS_Y[b], 265, 18)) { answer(b); return; }
            if (inside(x, py, 465, ANS_Y[b], 18, 18)) { refer(b); return; }
        }
        return;
    }
    for (int b = 0; b < 10; b++) {
        int ax, ay, aw;
        area_rect(b, &ax, &ay, &aw);
        if (inside(x, py, ax, ay, aw, 15)) { p->areabtn = b; area_level_changed(p); plat_invalidate(); return; }
    }
    for (int l = 0; l < 4; l++)                                            /* FUN_0051f530 */
        if (inside(x, py, 455, LEVEL_Y[l], 29, 26)) {
            if (p->level != l) { p->level = l; area_level_changed(p); }
            plat_invalidate();
            return;
        }
}

static void logon_click(int x, int y)
{
    if (touch_view()) {                                                    /* only the door and the story banner */
        if (inside(x, y, 253, 29, 123, 200)) logon_start();
        else if (inside(x, y, 135, 21, 64, 100)) open_story(0);
        return;
    }
    if (inside(x, y, 470, 404, mm_opt.prompts ? 80 : 143, 17)) { settings_now(); return; }   /* always (CHANGE 2026-11-21) */
    if (mm_opt.prompts && !g_lg_edit && inside(x, y, 556, 404, 57, 17)) { stats_now(); return; }
    if (inside(x, y, 253, 29, 123, 200)) { logon_start(); return; }
    if (inside(x, y, 135, 21, 64, 100)) { open_story(0); return; }
    if (g_lg_edit) {
        if (inside(x, y, 470, 277, 80, 17)) { if (logon_commit_name()) logon_new_name(); }   /* FUN_005212b0 commits first */
        else if (!inside(x, y, 471, 297, 124, 15)) logon_commit_name();
        return;
    }
    if (inside(x, y, 470, 277, 80, 17)) { logon_new_name(); return; }
    if (inside(x, y, 556, 277, 57, 17)) { logon_delete(); return; }
    if (mm_opt.prompts && inside(x, y, 470, 258, 70, 17)) { lg_mode(0); return; }
    if (mm_opt.prompts && inside(x, y, 543, 258, 70, 17)) { lg_mode(1); return; }
    if (inside(x, y, 471, 297, 141, 102)) {
        int i = g_lg_scroll + (y - 297) / 16;
        if (i < g_nlg) lg_pick(i);
    }
}

static void title_end(void) { g_title_on = 0; plat_invalidate(); }

static void mouse_main(int x, int y, int button, int action, int mods)
{
    if (g_title_on && action == MA_DOWN) { title_end(); return; }
    if (action == MA_MOVE) {
        g_mx = x;
        g_my = y;
        update_hover();
        return;
    }
    if (g_nmsg) {                                                          /* modal message box */
        if (action != MA_DOWN || button != MMB_LEFT) return;
        int w = 320, h = 130, bx = (MM_W - w) / 2, by = (MM_H - h) / 2 - 20;
        if (g_msg[0].yesno) {
            if (inside(x, y, bx + w / 2 - 80, by + h - 30, 70, 20)) msg_close(1);
            else if (inside(x, y, bx + w / 2 + 10, by + h - 30, 70, 20)) msg_close(0);
        } else if (inside(x, y, bx + (w - 70) / 2, by + h - 30, 70, 20)) msg_close(1);
        return;
    }
    if (g_story.on) {
        if (action != MA_DOWN || button != MMB_LEFT) return;
        int sx, sy, sw, sh;
        story_geom(&sx, &sy, &sw, &sh);
        int bx = sx + (g_story.kind ? 390 : 397), by = sy + (g_story.kind ? 243 : 342);
        if (inside(x, y, sx + sw - 20, sy - 18, 18, 18)) close_story();
        else if (g_story.page + 1 < g_story.npages && inside(x, y, bx, by, 18, 17)) { g_story.page++; plat_invalidate(); }
        return;
    }
    if (mm_stats_on()) { mm_stats_mouse(x, y, button, action); return; }
    if (mm_settings_on()) { mm_settings_mouse(x, y, button, action); return; }
    if (action == MA_WHEEL) {                                              /* player list scrolling */
        if (g_screen == SCR_LOGON) {
            g_lg_scroll -= button;
            if (g_lg_scroll > g_nlg - 6) g_lg_scroll = g_nlg - 6;
            if (g_lg_scroll < 0) g_lg_scroll = 0;
            plat_invalidate();
        }
        return;
    }
    if (action == MA_DOWN && button == MMB_LEFT && mute_ok() && inside(x, y, MUTE_X, MUTE_Y, MUTE_S, MUTE_S)) { mute_toggle(); return; }
    if (g_screen == SCR_LOGON) {
        if (action == MA_DOWN && button == MMB_LEFT) logon_click(x, y);
        return;
    }
    if (action == MA_DOWN && button == MMB_LEFT && !touch_view() && gear_ok() && inside(x, y, GEAR_X, GEAR_Y, GEAR_S, GEAR_S)) {
        settings_now();
        return;
    }
    if (action == MA_DOWN && button == MMB_LEFT && !touch_view() && hint_ok() && inside(x, y, HINT_X, HINT_Y, HINT_W, HINT_H)) { hint_5050(); return; }
    if (y >= 301) { panel_click(x, y, button, action, mods); return; }
    if (action != MA_DOWN || button != MMB_LEFT) return;
    if (g_screen == SCR_GRATING) { screen_anim(x, y); g_screen = SCR_DOORS; g_shimmer_next = plat_ticks(); plat_invalidate(); return; }
    if (g_screen == SCR_DOORS) {
        for (int i = 0; i < 5; i++)
            if (inside(x, y, DOOR_HOT[i][0], DOOR_HOT[i][1], DOOR_HOT[i][2], DOOR_HOT[i][3])) {
                enter_castle();                            /* (a fade, not the walk-in: user 2026-11-21) */
                return;
            }
        return;
    }
    if ((g_speech || g_qopen) && ov_page_click(x, y)) return;               /* (Previous / Next of a long pop-up) */
    if (g_speech && !g_qopen) {                                            /* 0x51c7a0 */
        if (typing()) { g_type_done = 1; plat_invalidate(); return; }   /* (a tap first shows all of it) */
        if (g_ov_page + 1 < g_ov_pages) { g_ov_page++; type_start(0); plat_invalidate(); return; }   /* (then the next page) */
        g_speech = 0;
        if (g_atrium_text) { g_atrium_text = 0; open_story(1); }
        plat_invalidate();
        return;
    }
    int id = hot_at(x, y);                                                 /* 0x51c887: hit test, then vtbl+0x16c */
    if (id >= 0) { room_click(id); plat_invalidate(); }
}

static void stats_now(void)
{
    if (!mm_opt.prompts) return;
    if (g_screen == SCR_LOGON && g_lg_edit && !logon_commit_name()) return;
    mm_stats_open();
    dialog_anim();
}

/* a tap in the touch column (column units) */
static void col_click(int x, int y)
{
    int id = col_hit(x, y);
    if (!id) return;
    Player *p = cur();
    if (id >= 900 && id < 900 + 64) lg_pick(id - 900);
    else if (id == 977 || id == 978) lg_mode(id == 978);
    else if (id == 970) { if (!g_lg_edit) logon_new_name(); else logon_commit_name(); }
    else if (id == 971) logon_delete();
    else if (id == 972 || id == 862) settings_now();
    else if (id == 973 || id == 864) stats_now();
    else if (id == 975 || id == 976) {
        g_lg_scroll += id == 975 ? -3 : 3;
        if (g_lg_scroll > g_nlg - col_rows()) g_lg_scroll = g_nlg - col_rows();
        if (g_lg_scroll < 0) g_lg_scroll = 0;
    }
    else if (id >= 810 && id < 820 && p) { p->areabtn = id - 810; area_level_changed(p); g_col_areas = 0; }
    else if (id == 820) g_col_areas = !g_col_areas;
    else if (id >= 200 && id < 205) match(id - 200);
    else if (id >= 830 && id < 834 && p) { if (p->level != id - 830) { p->level = id - 830; area_level_changed(p); } }
    else if (id >= 840 && id < 844) answer(id - 840);
    else if (id >= 850 && id < 854) refer(id - 850);
    else if (id == 860) hint_5050();
    else if (id == 861) mm_cmd(CMD_QUIT);                   /* Main Menu: to the gate, the game kept */
    else if (id == 979) on_resume_box(1, 0);
    else if (id == 880) speech_close();
    plat_invalidate();
}

void mm_mouse(int x, int y, int button, int action, int mods)
{
    if (action == MA_MOVE) {
        g_mx = x;
        g_my = y;
        update_hover();
        return;
    }
    if (!touch_view()) { mouse_main(x, y, button, action, mods); return; }
    if (g_title_on && action == MA_DOWN) { title_end(); return; }
    if (dialog_open()) {
        int dx, dy;
        dlg_origin(&dx, &dy);
        mouse_main(x - dx, y - dy, button, action, mods);
        return;
    }
    int cx, cy;
    if (in_col(x, y, &cx, &cy)) {
        if (action == MA_WHEEL && g_screen == SCR_LOGON) {
            g_lg_scroll -= button;
            if (g_lg_scroll > g_nlg - col_rows()) g_lg_scroll = g_nlg - col_rows();
            if (g_lg_scroll < 0) g_lg_scroll = 0;
            plat_invalidate();
        }
        if (action == MA_DOWN && button == MMB_LEFT) col_click(cx, cy);
        return;
    }
    if (x < -mm_view_ox() || x >= MM_W + mm_view_ox() || y < 0 || y >= 301 + mm_view_strip()) return;
    if (y >= 301 && g_screen != SCR_LOGON) {                 /* the strip: matches */
        int id = action == MA_DOWN && button == MMB_LEFT ? strip_hit(x, y) : 0;
        if (id >= 200 && id < 205) match(id - 200);
        return;
    }
    mouse_main(x, y, button, action, mods);
}

void mm_char(unsigned ch)
{
    if (mm_settings_on() && !g_nmsg) { mm_settings_char(ch); return; }
    if (g_screen != SCR_LOGON || !g_lg_edit || g_nmsg) return;
    size_t n = strlen(g_lg_text);
    int chars = 0;                                                         /* EM_LIMITTEXT 12 (FUN_005212b0) */
    for (const char *s = g_lg_text; *s; s++) chars += (*s & 0xC0) != 0x80;
    if (ch < 32 || chars >= 12) return;
    if (ch < 0x80) g_lg_text[n] = (char)ch, g_lg_text[n + 1] = 0;
    else if (ch < 0x800 && n + 2 < sizeof g_lg_text) {
        g_lg_text[n] = (char)(0xC0 | ch >> 6); g_lg_text[n + 1] = (char)(0x80 | (ch & 63)); g_lg_text[n + 2] = 0;
    }
    plat_invalidate();
}

void mm_key(int key, int mods)
{
    (void)mods;
    if (g_title_on && key != KEY_F2 && key != KEY_F3) { title_end(); return; }
    if (key == KEY_F2) { mm_cmd(CMD_HD); return; }                         /* HD art on/off, any time */
    if (key == KEY_F3) { mm_cmd(CMD_WIDE); return; }                       /* widescreen on/off, any time */
    if (g_nmsg) {
        if (key == KEY_ENTER || key == ' ' || (!g_msg[0].yesno && key == KEY_ESC)) msg_close(1);
        else if (g_msg[0].yesno && (key == 'Y')) msg_close(1);
        else if (g_msg[0].yesno && (key == 'N' || key == KEY_ESC)) msg_close(0);
        return;
    }
    if (mm_stats_on()) { mm_stats_key(key); return; }
    if (mm_settings_on()) { mm_settings_key(key, mods); return; }
    if (g_story.on) {
        if (key == KEY_ESC) close_story();
        else if (key == KEY_ENTER && g_story.page + 1 < g_story.npages) { g_story.page++; plat_invalidate(); }
        return;
    }
    if (g_screen == SCR_LOGON) {
        if (g_lg_edit) {
            size_t n = strlen(g_lg_text);
            if (key == KEY_ENTER) logon_commit_name();
            else if (key == KEY_ESC) { g_lg_edit = 0; plat_invalidate(); }
            else if (key == KEY_BACK && n) {
                while (n && (g_lg_text[n - 1] & 0xC0) == 0x80) n--;
                if (n) g_lg_text[n - 1] = 0;
                plat_invalidate();
            }
        } else if (key == KEY_ENTER) logon_start();
        return;
    }
    if (g_screen == SCR_GRATING && key == KEY_ENTER) { screen_anim(318, 150); g_screen = SCR_DOORS; g_shimmer_next = plat_ticks(); plat_invalidate(); return; }
    if (g_screen == SCR_DOORS && key == KEY_ENTER) { mm_rand(); enter_castle(); return; }   /* FUN_004f4460: random door rand()%4 */
    if (g_screen != SCR_ROOM) return;
    if (g_speech && !g_qopen && (key == KEY_ENTER || key == ' ')) {          /* (pages: Enter / Space = a tap on the speech) */
        if (typing()) g_type_done = 1;
        else if (g_ov_page + 1 < g_ov_pages) { g_ov_page++; type_start(0); }
        else speech_close();
        plat_invalidate();
        return;
    }
    if (g_qopen) {                                                          /* FUN_004f41c0 */
        if (key >= 'A' && key <= 'D') answer(key - 'A');
        else if (key == 'H') hint_5050();                                   /* CHANGE: 50/50 hint */
        else if (key >= '1' && key <= '4') refer(key - '1');
        return;
    }
    if (key == KEY_ENTER) {                                                 /* FUN_004f4460 */
        Room *r = cur_room();                                              /* FUN_0051ce10: only the stairs */
        if (r && (r->doors & 0x10) && cur()->award < 5) stairs();
    }
}

void mm_tick(void)
{
    uint32_t now = plat_ticks();
    anim_tick();
    /* (touch, Animations) glints over what can be tapped, left to right: at the gate now and then, in a room soon after
     * it appears and then now and then, while nothing else is open */
    int n_glint = g_screen == SCR_LOGON ? 2 : g_screen == SCR_GRATING ? 1 : g_screen == SCR_DOORS ? 5 : 0;
    if (g_screen == SCR_ROOM && !g_qopen && !g_speech) for (int i = 0; i < g_nhot; i++) n_glint += g_hot[i].o != NULL;
    if (mm_opt.anim && n_glint && !g_title_on && !dialog_open() && !anim_on(AN_SHIMMER) && !anim_on(AN_SCREEN) &&
        (int32_t)(now - g_shimmer_next) >= 0) {
        int len = GLINT_LEN + (n_glint - 1) * GLINT_STEP;
        anim_start(AN_SHIMMER, len, 0, 0);
        g_shimmer_next = now + (uint32_t)len + 3500;
    }
    if (g_taphint_until && (int32_t)(now - g_taphint_until) >= 0) { g_taphint_until = 0; plat_invalidate(); }
    if (g_title_on && (int32_t)(now - g_title_until) >= 0) title_end();
    if (!mm_hd_preload_step()) mm_wide_preload_step();                     /* HD and wide pictures load ahead of use */
    if (g_qopen && g_q.timer_on && (int32_t)(now - g_q.next_tick) >= 0) {   /* 0x522660 */
        g_q.next_tick += 1000;
        if (g_q.seconds > 0) { g_q.seconds--; plat_invalidate(); }
    }
    if (g_match_end && (int32_t)(now - g_match_end) >= 0) {                /* 0x51e2a0 */
        g_match_end = 0;                                                   /* KillTimer(1) */
        if (cur() && cur()->maze->reveal) { sound(32); cur()->maze->reveal = 0; }
        plat_invalidate();
    }
    if (g_snd.amb_running && (int32_t)(now - g_next_amb) >= 0) amb_fire();   /* panel timer 2 */
    if (mm_settings_on()) {                                                /* text caret */
        static uint32_t blink2;
        if (now - blink2 > 500) { blink2 = now; plat_invalidate(); }
    }
    if (g_screen == SCR_LOGON && g_lg_edit) {
        static uint32_t blink;
        if (now - blink > 500) { blink = now; plat_invalidate(); }
    }
}

/* FUN_004f40f0 (room view) / 0x520a70 (logon), WM_ACTIVATE: inactive -> music state 5 (pause), the question
 * clock stops and the answer, letter and refer buttons are hidden (FUN_005204d0); active -> state 6 (resume),
 * the clock restarts (always, if a question is open) and the buttons come back (FUN_00520550). */
void mm_activate(int active)
{
    if (active == !g_inactive) return;
    g_inactive = !active;
    if (!active) {
        music_update(5);
        if (g_qopen) g_q.timer_on = 0;
    } else {
        music_update(6);
        if (g_qopen && !g_q.timer_on) { g_q.timer_on = 1; g_q.next_tick = plat_ticks() + 1000; }
    }
    plat_invalidate();
}

void mm_cmd(int cmd)
{
    switch (cmd) {
    case CMD_NEW_GAME:                                     /* 0x4f4cc0: nothing while the logon screen is up */
        if (g_screen == SCR_LOGON && !g_story.on) break;
        g_story.on = 0; g_nmsg = 0; game_forget(); show_logon(); break;
    case CMD_QUIT:              /* CHANGE 2026-11-21 (user): "Quit Game" returns to the castle gate instead of ending the
                                   program (the original closed MindMaze). The game stays saved for "continue"; it is
                                   offered at the next start, not right away. */
        if (g_screen == SCR_LOGON) break;
        game_save();
        g_story.on = 0; g_nmsg = 0;
        g_no_offer = 1;
        show_logon();
        g_no_offer = 0;
        break;
    case CMD_EXIT: mm_shutdown(); plat_quit(); break;
    case CMD_SETTINGS: settings_now(); break;            /* CHANGE 2026-11-21: Windows menu > Settings... */
    case CMD_MUSIC:
        mm_music_on = !mm_music_on;
        plat_set_setting("Music", mm_music_on ? "1" : "0");
        music_update(g_music_state);                       /* 0x4f4e90 / 0x4f4ed0: rerun the current state */
        break;
    case CMD_SOUND:
        mm_sound_on = !mm_sound_on;
        plat_set_setting("Sound", mm_sound_on ? "1" : "0");
        music_update(g_music_state);                       /* 0x4f4f10 / 0x4f4f50 do the same (in state 2 the
                                                              background moves on to its next loop) */
        break;
    case CMD_HD:
        if (!mm_hd_available()) break;
        mm_hd_on = !mm_hd_on;
        plat_set_setting("hd_art", mm_hd_on ? "1" : "0");
        plat_invalidate();
        break;
    case CMD_WIDE:                                     /* CHANGE: widescreen view on/off (F3) */
        if (!mm_wide_available()) break;
        mm_wide_on = !mm_wide_on;
        plat_set_setting("widescreen", mm_wide_on ? "1" : "0");
        plat_view_changed();
        break;
    }
}

void mm_shutdown(void) { game_save(); free_players(); }

/* CHANGE 2026-11-21: the platform calls this when the app goes to the background or the system ends it (Android
 * may kill a paused app without any further event); the game goes on unchanged */
void mm_save_players(void)
{
    game_save();
    for (int i = 0; i < NPLAYERS; i++)
        if (g_pl[i]) save_player(g_pl[i]);
}

int mm_init(char *err, int errlen)                                         /* FUN_004f45c0 */
{
    char v[512];
    if (!mm_tables_load(err, errlen)) return 0;               /* data/tables.json (mm_tables.c) */
    if (plat_get_setting("Music", v, sizeof v)) mm_music_on = atoi(v) != 0;
    if (plat_get_setting("Sound", v, sizeof v)) mm_sound_on = atoi(v) != 0;
    if (plat_get_setting("MusicVolume", v, sizeof v)) mm_music_volume = atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
    if (plat_get_setting("SoundVolume", v, sizeof v)) mm_sound_volume = atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
    g_mute = plat_get_setting("Muted", v, sizeof v) && atoi(v) == 1;
    apply_volume();
    mm_hd_on = mm_hd_available() && (!plat_get_setting("hd_art", v, sizeof v) || atoi(v) != 0);
    mm_wide_on = mm_wide_available() && (!plat_get_setting("widescreen", v, sizeof v) || atoi(v) != 0);
    if (plat_get_setting("reference_url", v, sizeof v) && strstr(v, "%s")) snprintf(g_url_tmpl, sizeof g_url_tmpl, "%s", v);
    mm_options_load();                                       /* CHANGE 2026-11-21: Settings > Modern options */
    if (!mm_sources_init(err, errlen)) return 0;              /* CHANGE: question sources (mm_sources.c) */
    MMImage *seal = mm_image("res/29166.bmp");
    if (seal) g_panel_bg = seal->px[0] & 0xFFFFFF;
    if (!mm_image("LOGONSCR.DIB")) { snprintf(err, errlen, "The MindMaze pictures are missing from the data folder."); return 0; }
    if (plat_get_setting("seed", v, sizeof v) && atoi(v)) { g_fixed_seed = 1; mm_srand((unsigned)atoi(v)); }
    else mm_srand(plat_ticks());
    show_logon();
    int secs = TITLE_SECONDS_DEFAULT;
    if (plat_get_setting("title_seconds", v, sizeof v)) secs = atoi(v);
    if (secs > 0 && mm_image(mm_str(29027))) {
        g_title_on = 1;
        g_title_until = plat_ticks() + (uint32_t)secs * 1000u;
    }
    return 1;
}

/* CHANGE 2026-11-21: 1 while typed text goes somewhere (a new player's name on the logon screen, the "Add a Question"
 * fields in Settings), so a touch platform can show its on-screen keyboard (web shell.js MM.syncKeyboard) */
int mm_text_input(void)
{
    if (g_nmsg || g_title_on) return 0;
    if (mm_settings_on()) return mm_settings_typing();
    return g_screen == SCR_LOGON && g_lg_edit && !g_story.on;
}

/* the text being typed and its name, while mm_text_input (the phone shows them in a box above its keyboard, which
 * covers the lower part of the game) */
const char *mm_text_value(void)
{
    const char *label;
    if (!mm_text_input()) return "";
    return mm_settings_on() ? mm_settings_field(&label) : g_lg_text;
}
const char *mm_text_label(void)
{
    const char *label = "";
    if (!mm_text_input()) return "";
    if (mm_settings_on()) mm_settings_field(&label);
    else label = "New player's name";
    return label;
}

