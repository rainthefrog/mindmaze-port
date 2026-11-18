/* MindMaze game core, ported from ENCARTA.EXE (Encarta 2006). See SPEC.md / NOTES.md for the analysis.
 * Each routine names the original function it follows. Deliberate differences are marked CHANGE.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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

void mm_set_volume(int music, int sound)
{
    mm_music_volume = music < 0 ? 0 : music > 100 ? 100 : music;
    mm_sound_volume = sound < 0 ? 0 : sound > 100 ? 100 : sound;
    char v[8];
    snprintf(v, sizeof v, "%d", mm_music_volume);
    plat_set_setting("MusicVolume", v);
    snprintf(v, sizeof v, "%d", mm_sound_volume);
    plat_set_setting("SoundVolume", v);
    plat_volume(mm_music_volume, mm_sound_volume);
}

static int g_speech;                  /* speech overlay shown (mode 0) */
static char g_speech_text[2048];
static int g_atrium_text;             /* room +0x8c */
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
typedef struct { int x, y, w, h, id; } Hot;
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
static char g_lg_text[64];

static void next_turn(void);
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
            if (!conflict(obj(MM_G_FURNITURE, f), occ)) {
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
            if (!conflict(obj(MM_G_FURNITURE, f), occ)) {
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
/* game flow                                                                                         */

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
    a->visited = b->visited = 1;
}

static void next_turn(void)                                                /* FUN_004f3670 / FUN_004f3da0 */
{
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
    plat_invalidate();
}

static void on_correct_box(int yes, int points)
{
    (void)yes;
    Player *p = cur();
    p->score += points;
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

static void answer(int button)                                             /* FUN_0051f750 */
{
    if (!g_qopen || button < 0 || button > 3 || g_q.disabled[button]) return;
    Player *p = cur();
    char buf[1024];
    if (g_q.order[button] == 0) {
        sound(33);                                                         /* RIGHT */
        int points = (p->level + 1) * g_q.seconds;
        question_close();
        snprintf(buf, sizeof buf, mm_str(points != 1 ? 29041 : 29057), points);
        msgbox(buf, 0, on_correct_box, points);
    } else if (!g_q.tried) {
        sound(34);                                                         /* WRONG */
        g_q.timer_on = 0;                                                  /* FUN_00522620 */
        msgbox(mm_str(29039), 0, on_wrong1_box, button);
    } else {
        sound(34);
        g_q.timer_on = 0;
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
    g_qopen = 1;
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
            if (i != p->award) { p->award = i; msgbox("Congratulations!  You now have a new award!", 0, NULL, 0); }
            return;
        }
}

static void stairs(void)                                                   /* FUN_0051c940 */
{
    Player *p = cur();
    rerank(p);
    maze_free(p->maze);
    p->maze = maze_new();
    room_redraw_music();                                                   /* FUN_0051c190 */
    plat_invalidate();
}

static void ending_closed(void)                                            /* FUN_004f3840 */
{
    Player *p = cur();
    if (p && p->award == 5) { p->score = 0; p->award = 0; }
    show_logon();
}

static void open_story(int kind)                                           /* FUN_0052fec0 */
{
    memset(&g_story, 0, sizeof g_story);
    g_story.on = 1;
    g_story.kind = kind;
    if (kind == 0) { g_story.pages[0] = "OPEN1.DIB"; g_story.pages[1] = "OPEN2.DIB"; g_story.npages = 2; }
    else {
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
    if (g_qopen) { plat_beep(); return; }                                  /* 0x51c6fc: MessageBeep(0) */
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
    g_screen = SCR_GRATING;
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
    Player tmp;                                  /* FUN_0051b880 + FUN_0051bb80: the new player is saved at once */
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.name, sizeof tmp.name, "%s", g_lg_text);
    save_player(&tmp);
    g_lg[0].n = tmp.saved_n;
    plat_invalidate();
    return 1;
}

static void logon_delete(void)
{
    char key[32];
    for (int i = 0; i < g_nlg;) {
        if (g_lg[i].sel && !g_lg[i].tourist) {
            if (g_lg[i].n) { snprintf(key, sizeof key, "MindmazePlayer%u", g_lg[i].n); plat_set_setting(key, "deleted"); }
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
static int hover(int rx, int ry, int rw, int rh) { return inside(g_mx, g_my, rx, ry, rw, rh); }

static void add_hot(int x, int y, int w, int h, int id)
{
    if (g_nhot < 64) g_hot[g_nhot++] = (Hot){x, y, w, h, id};
}

static void draw_object(MMCanvas *c, const MMObject *o) { mm_draw_img(c, o->name, o->x, o->y); }

static void hot_object(const MMObject *o, int mode)                        /* FUN_0051c410 */
{
    int l = o->rect[0], t = o->rect[1], r = o->rect[2], b = o->rect[3];
    if (mode == 0) {
        if (l && t) add_hot(o->x + l, o->y + t, abs(r - l), abs(b - t), o->id);
        return;
    }
    if (!l || !t || !r || !b) {
        MMImage *im = mm_image(o->name);
        if (!im) return;
        add_hot(o->x, o->y, o->half == 1 ? im->w / 2 : im->w, im->h, o->id);
    } else add_hot(o->x + l, o->y + t, abs(r - l), abs(b - t), o->id);
}

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
}

static int hot_at(int x, int y)                                            /* FUN_0052f840: first match, or -1 */
{
    for (int i = 0; i < g_nhot; i++)
        if (inside(x, y, g_hot[i].x, g_hot[i].y, g_hot[i].w, g_hot[i].h)) return g_hot[i].id;
    return -1;
}

static void draw_overlay(MMCanvas *c, int mode, const char *text)          /* FUN_0051ced0 / FUN_0051d0d0 */
{
    if (mode == 0) {
        mm_draw_img(c, "DIALOG.DIB", 67, 0);
        draw_text(c, 99, 44, 230, 172, text, FONT_TEXT, 0, TX_WRAP);
    } else {
        mm_draw_img(c, "BANNER.DIB", 67, 0);
        mm_rich(c, 194, 95, 191, 158, text, FONT_TEXT, FONT_ITALIC, 0, TX_WRAP);        /* <it> italics */
    }
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
            if (i == m->cx && j == m->cy) mm_ellipse(c, x + 6, y + 5.5, 3, 2.5, C_RED);
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
    mm_frame(c, 29, Y + 7, 141, 121, 1, C_LINE);
    draw_map(c, p->maze, 30, Y + 8, 162, Y + 126);
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
                mm_draw_img(c, mm_ranks[p->award].image, ax, ay);
            }
        }
        /* labels 0x7167 / 0x7166 / 0x7168: font RCDATA 4916 = Arial 12 (was Tahoma 11), top-aligned like the static
         * controls (screenshot digits at y 393..402 = Arial 12 from y 90; the rebuild's VCENTER put them 3-4 px low),
         * alignment table 0x6b4518: "Progress:" centred, score right, "/20000" left; black */
        draw_text(c, 492, Y + 90, 55, 20, mm_str(29031), FONT_SCORE, 0, TX_CENTER);
        char sc[16];
        snprintf(sc, sizeof sc, "%d", p->score);
        draw_text(c, 551, Y + 90, 35, 20, sc, FONT_SCORE, 0, TX_RIGHT);
        draw_text(c, 586, Y + 90, 40, 20, mm_str(29032), FONT_SCORE, 0, TX_LEFT);
    } else {
        for (int b = 0; b < 4 && !g_inactive; b++) {                       /* hidden while inactive (FUN_005204d0) */
            int en = !g_q.disabled[b];
            /* letters (group 0x3178, FUN_0051edb0 WM_SETFONT 0x1343): Times New Roman 18, top-aligned static, black
             * (was Tahoma 11 bold) */
            draw_text(c, 175, Y + ANS_Y[b], 17, 18, mm_str(29035 + b), FONT_NAME, 0, TX_LEFT);
            int hv = en && !g_nmsg && hover(195, Y + ANS_Y[b], 265, 18);
            int ay = Y + ANS_Y[b];
            if (hv) {
                /* hover as in a screenshot of the original (user, 2026-11-18): the button stands out as a raised
                 * button - lighter face, dark outline, white top/left and grey bottom/right bevel; text stays black */
                mm_fill(c, 195, ay, 265, 18, 0xF4F3EE);
                mm_frame(c, 195, ay, 265, 18, 1, 0x3C3A34);
                mm_fill(c, 196, ay + 1, 263, 1, 0xFFFFFF);
                mm_fill(c, 196, ay + 1, 1, 16, 0xFFFFFF);
                mm_fill(c, 196, ay + 16, 263, 1, 0x9A968A);
                mm_fill(c, 458, ay + 1, 1, 16, 0x9A968A);
            } else {
                mm_fill(c, 195, ay, 265, 18, g_panel_bg);
                mm_frame(c, 195, ay, 265, 18, 1, en ? C_LINE : 0xC0BCAE);
            }
            /* answer buttons (group 0x88c): FONT_ANSWER = Tahoma 11, centred, like the user's screenshot of the original
             * (FUN_0051edb0 WM_SETFONT 0x1341 suggested Times New Roman 14; the screenshot wins, 2026-11-18) */
            mm_rich(c, 199, ay, 259, 18, g_q.q->answer[g_q.order[b]], FONT_ANSWER, FONT_ANSWER_I,
                      !en ? 0xA8A498 : C_TEXT, TX_VCENTER | TX_CENTER);   /* centred, user 2026-11-18 */
            snprintf(res, sizeof res, "res/%d.bmp", 29150 + b);
            strip(c, res, hover(465, Y + ANS_Y[b], 18, 18) ? 1 : 0, 465, Y + ANS_Y[b], 18, 18);
        }
        mm_draw_img(c, "res/29166.bmp", 522, Y + 17);
        char pts[16];
        snprintf(pts, sizeof pts, "%d", (p->level + 1) * g_q.seconds);
        /* bigger than the original; centred on the seal's circle (bitmap 29166 x 13-68 -> centre 562.5, not the bitmap's 567) */
        /* maroon as in a screenshot of the original (user, 2026-11-18); the decomp does not show the colour (set
         * in the timer callback LAB_00522660, not decompiled) */
        draw_text(c, 518, Y + 28, 90, 30, pts, FONT_POINTS, C_RED, TX_CENTER | TX_VCENTER);
    }
    panel_button(c, 492, Y + 108, 69, 19, mm_str(29068));
    panel_button(c, 563, Y + 108, 69, 19, mm_str(29069));
    /* name: label 0x7169 at (488,4,148,20), centred, top-aligned, font RCDATA 4931 = Times New Roman 18 (was 16 px,
     * vertically centred in a 22 px box); screenshot "Tourist" spans y 308..321 = TNR 18 from y 4; black.
     * Box 2 px taller than the original so descenders are not clipped. Drawn last: never covered. */
    draw_text(c, 488, Y + 4, 148, 22, p->name, FONT_NAME, 0, TX_CENTER | TX_ELLIPSIS);
}

static void draw_logon(MMCanvas *c)
{
    mm_draw_img(c, "LOGONSCR.DIB", 0, 0);
    text_button(c, 470, 277, 80, 17, mm_str(29024), !g_lg_edit);
    text_button(c, 556, 277, 57, 17, mm_str(29025), !g_lg_edit);
    text_button(c, 470, 404, 143, 17, "Settings...", !g_lg_edit);      /* CHANGE: question settings (mm_settings.c) */
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
    draw_text(c, x + 14, y + 28, w - 28, h - 64, m->text, FONT_TEXT, C_TEXT, TX_WRAP);
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

void mm_draw(MMCanvas *c)
{
    mm_fill(c, -c->ox, 0, MM_W + 2 * c->ox, MM_H, 0);
    if (g_title_on) { mm_draw_img(c, mm_str(29027), 0, 0); return; }       /* TITLESCR.DIB */
    if (g_screen == SCR_LOGON) draw_logon(c);
    else {
        if (g_screen == SCR_GRATING) mm_draw_img(c, "GRATING.DIB", 0, 0);
        else if (g_screen == SCR_DOORS) mm_draw_img(c, mm_str(29006), 0, 0);
        else {
            draw_room(c);
            if (g_qopen) draw_overlay(c, 1, g_q.q->text);
            else if (g_speech) draw_overlay(c, 0, g_speech_text);
        }
        draw_panel(c);
    }
    if (mm_settings_on()) mm_settings_draw(c, g_mx, g_my);
    if (g_story.on) draw_story(c);
    if (g_nmsg) draw_msgbox(c);
    g_hover = clickable_at(g_mx, g_my);                                    /* screen may have changed under the pointer */
    plat_set_cursor(g_hover != 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* input                                                                                             */

static const int DOOR_HOT[5][4] = {{0, 0, 125, 300}, {349, 51, 76, 191}, {197, 76, 59, 108}, {501, 32, 118, 245}, {200, 232, 237, 69}};

/* What is clickable under (x, y): 0 = nothing, else an id unique per target (hover highlight and the hand
 * cursor follow it; CHANGE: the original showed the hand only over room hotspots). */
static int clickable_at(int x, int y)
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
    if (g_story.on) {
        int sx, sy, sw, sh;
        story_geom(&sx, &sy, &sw, &sh);
        int bx = sx + (g_story.kind ? 390 : 397), by = sy + (g_story.kind ? 243 : 342);
        if (inside(x, y, sx + sw - 20, sy - 18, 18, 18)) return 10;
        if (g_story.page + 1 < g_story.npages && inside(x, y, bx, by, 18, 17)) return 11;
        return 0;
    }
    if (g_screen == SCR_LOGON) {
        if (inside(x, y, 253, 29, 123, 200)) return 20;
        if (inside(x, y, 135, 21, 64, 100)) return 21;
        if (g_lg_edit) return 0;
        if (inside(x, y, 470, 277, 80, 17)) return 22;
        if (inside(x, y, 470, 404, 143, 17)) return 24;
        if (inside(x, y, 556, 277, 57, 17)) return 23;
        if (inside(x, y, 471, 297, 141, 102)) {
            int i = g_lg_scroll + (y - 297) / 16;
            if (i < g_nlg) return 100 + i;
        }
        return 0;
    }
    if (y >= 301) {                                                        /* control panel */
        Player *p = cur();
        int py = y - 301;
        if (!p) return 0;
        for (int i = 0; i < 5; i++)
            if (p->maze->match[i] && !g_match_end && inside(x, py, 6, 8 + 24 * i, 20, 23)) return 200 + i;
        if (inside(x, py, 492, 108, 69, 19)) return 210;
        if (inside(x, py, 563, 108, 69, 19)) return 211;
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
    if (g_qopen) return 0;
    if (g_speech) return 320;                                              /* any click closes the speech */
    int id = hot_at(x, y);
    return id >= 0 ? 400 + id : 0;
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
    if (inside(x, y, 253, 29, 123, 200)) { logon_start(); return; }
    if (inside(x, y, 135, 21, 64, 100)) { open_story(0); return; }
    if (g_lg_edit) {
        if (inside(x, y, 470, 277, 80, 17)) { if (logon_commit_name()) logon_new_name(); }   /* FUN_005212b0 commits first */
        else if (!inside(x, y, 471, 297, 124, 15)) logon_commit_name();
        return;
    }
    if (inside(x, y, 470, 277, 80, 17)) { logon_new_name(); return; }
    if (inside(x, y, 470, 404, 143, 17)) { mm_settings_open(); return; }
    if (inside(x, y, 556, 277, 57, 17)) { logon_delete(); return; }
    if (inside(x, y, 471, 297, 141, 102)) {
        int i = g_lg_scroll + (y - 297) / 16;
        if (i < g_nlg) { g_lg[i].sel = !g_lg[i].sel; plat_invalidate(); }
    }
}

static void title_end(void) { g_title_on = 0; plat_invalidate(); }

void mm_mouse(int x, int y, int button, int action, int mods)
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
    if (g_screen == SCR_LOGON) {
        if (action == MA_DOWN && button == MMB_LEFT) logon_click(x, y);
        return;
    }
    if (y >= 301) { panel_click(x, y, button, action, mods); return; }
    if (action != MA_DOWN || button != MMB_LEFT) return;
    if (g_screen == SCR_GRATING) { g_screen = SCR_DOORS; plat_invalidate(); return; }
    if (g_screen == SCR_DOORS) {
        for (int i = 0; i < 5; i++)
            if (inside(x, y, DOOR_HOT[i][0], DOOR_HOT[i][1], DOOR_HOT[i][2], DOOR_HOT[i][3])) { enter_castle(); return; }
        return;
    }
    if (g_speech && !g_qopen) {                                            /* 0x51c7a0 */
        g_speech = 0;
        if (g_atrium_text) { g_atrium_text = 0; open_story(1); }
        plat_invalidate();
        return;
    }
    int id = hot_at(x, y);                                                 /* 0x51c887: hit test, then vtbl+0x16c */
    if (id >= 0) { room_click(id); plat_invalidate(); }
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
    if (g_screen == SCR_GRATING && key == KEY_ENTER) { g_screen = SCR_DOORS; plat_invalidate(); return; }
    if (g_screen == SCR_DOORS && key == KEY_ENTER) { mm_rand(); enter_castle(); return; }   /* FUN_004f4460: random door rand()%4 */
    if (g_screen != SCR_ROOM) return;
    if (g_qopen) {                                                          /* FUN_004f41c0 */
        if (key >= 'A' && key <= 'D') answer(key - 'A');
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
        g_story.on = 0; g_nmsg = 0; show_logon(); break;
    case CMD_QUIT: mm_shutdown(); plat_quit(); break;
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

void mm_shutdown(void) { free_players(); }

int mm_init(char *err, int errlen)                                         /* FUN_004f45c0 */
{
    char v[512];
    if (!mm_tables_load(err, errlen)) return 0;               /* data/tables.json (mm_tables.c) */
    if (plat_get_setting("Music", v, sizeof v)) mm_music_on = atoi(v) != 0;
    if (plat_get_setting("Sound", v, sizeof v)) mm_sound_on = atoi(v) != 0;
    if (plat_get_setting("MusicVolume", v, sizeof v)) mm_music_volume = atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
    if (plat_get_setting("SoundVolume", v, sizeof v)) mm_sound_volume = atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
    plat_volume(mm_music_volume, mm_sound_volume);
    mm_hd_on = mm_hd_available() && (!plat_get_setting("hd_art", v, sizeof v) || atoi(v) != 0);
    mm_wide_on = mm_wide_available() && (!plat_get_setting("widescreen", v, sizeof v) || atoi(v) != 0);
    if (plat_get_setting("reference_url", v, sizeof v) && strstr(v, "%s")) snprintf(g_url_tmpl, sizeof g_url_tmpl, "%s", v);
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
