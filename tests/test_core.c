/* Headless tests of the game core (compiled with the host gcc, platform functions stubbed).
 *   make test                          synthetic data (tests/make_fixtures.py), no Encarta files needed
 *   build/test_core data --original    the same tests on a data folder extracted from an Encarta 2006 disc
 * mm_game.c is included directly so its internal functions can be tested.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <sys/stat.h>
#include "mm_art.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../src/core/mm_game.c"
#include "text.h"

static const char *g_data;
static uint32_t g_now = 1000;
static char g_set[64][2][256];
static int g_nset, g_fail, g_sounds[40];
static char g_opened[512];           /* last URL / folder the game asked to open */
static int g_original;               /* --original / --edition: a data folder extracted from a real disc */
static int g_2006;                   /* --original: from the Encarta 2006 disc (its exact counts are checked) */
static int g_total;                  /* questions in the original source */

uint32_t plat_ticks(void) { return g_now; }
void *plat_load(const char *rel, int *size)
{
    char p[512];
    if (rel[0] == '/') snprintf(p, sizeof p, "%s", rel); else snprintf(p, sizeof p, "%s/%s", g_data, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(n + 1);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[n] = 0;
    if (size) *size = (int)n;
    return d;
}
void plat_text(MMCanvas *c, int x, int y, int w, int h, const char *s, int f, uint32_t rgb, int fl) {}
int plat_text_width(const char *s, int f) { return (int)strlen(s) * 6; }
void plat_sound(const char *name)
{
    for (int i = 0; i < 35; i++) if (!strcmp(name, mm_sound_files[i])) g_sounds[i]++;
}
static const char *g_track[2];       /* what each music channel plays (NULL = stopped) */
static int g_paused[2], g_beeps;
void plat_music(int ch, const char *n) { g_track[ch] = n; g_paused[ch] = 0; }
void plat_music_pause(int ch, int p) { g_paused[ch] = p; }
static int g_fade_ms;
void plat_music_fade(int ms) { g_fade_ms = ms; }
static int g_vol[2] = {-1, -1};
int plat_instrument_count(void) { return 0; }
const char *plat_instrument_name(int i) { (void)i; return ""; }
int plat_instrument_get(void) { return -1; }
void plat_instrument_set(int i) { (void)i; }
void plat_volume(int m, int s) { g_vol[0] = m; g_vol[1] = s; }
void plat_beep(void) { g_beeps++; }
int plat_get_setting(const char *k, char *buf, int n)
{
    for (int i = 0; i < g_nset; i++) if (!strcmp(g_set[i][0], k)) { snprintf(buf, n, "%s", g_set[i][1]); return 1; }
    return 0;
}
void plat_set_setting(const char *k, const char *v)
{
    for (int i = 0; i < g_nset; i++) if (!strcmp(g_set[i][0], k)) { snprintf(g_set[i][1], 256, "%s", v ? v : ""); return; }
    snprintf(g_set[g_nset][0], 256, "%s", k);
    snprintf(g_set[g_nset++][1], 256, "%s", v ? v : "");
}
void plat_open_url(const char *u) { snprintf(g_opened, sizeof g_opened, "%s", u); }
int plat_save_data(const char *rel, const void *data, int len)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", g_data, rel);
    for (char *q = p + strlen(g_data) + 1; (q = strchr(q, '/')); q++) { *q = 0; mkdir(p, 0755); *q = '/'; }
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    return fclose(f) == 0 && ok;
}
long plat_data_size(const char *rel)
{
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", g_data, rel);
    return stat(p, &st) || !S_ISREG(st.st_mode) ? -1 : (long)st.st_size;
}
int plat_can_export(void) { return 0; }
void plat_export_data(void) {}
/* "reading a disc": copies build/fixtures/discfx into editions/fx/ as mx_add_edition would */
static int g_add_disc_calls;
enum { PX_ADD_DISC = 78 + 480 - 196 + 20, PY_ADD_DISC = 22 + 25 + 8 };   /* mm_settings.c: PX + PW - 196, PY + 25 */
void plat_add_disc(void)
{
    g_add_disc_calls++;
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
        int n = 0;
        char src[64], dst[64];
        snprintf(src, sizeof src, "discfx/mindmaze.%s", k ? "idx" : "db");
        snprintf(dst, sizeof dst, "editions/fx/mindmaze.%s", k ? "idx" : "db");
        void *d = plat_load(src, &n);
        ok = d && plat_save_data(dst, d, n);
        free(d);
    }
    mm_disc_added(ok, "fx", "Fixture Edition", 150, ok ? NULL : "fixture disc missing");
}
int plat_art_import(int go) { return 0; }
void plat_open_data_folder(const char *rel) { snprintf(g_opened, sizeof g_opened, "folder:%s", rel); }
void plat_set_cursor(int h) {}
static int g_invalidated;
void plat_invalidate(void) { g_invalidated++; }
void plat_view_changed(void) {}
/* user files kept in memory (seen list, custom / online questions) */
static struct { char name[64]; char *data; int len; } g_uf[32];   /* user files: seen, custom, stats, savegame, ... */
static int uf_find(const char *name, int make)
{
    for (int i = 0; i < 32; i++) if (g_uf[i].data && !strcmp(g_uf[i].name, name)) return i;
    if (!make) return -1;
    for (int i = 0; i < 32; i++) if (!g_uf[i].data) { snprintf(g_uf[i].name, 64, "%s", name); g_uf[i].data = calloc(1, 1); g_uf[i].len = 0; return i; }
    return -1;
}
void *plat_load_user(const char *name, int *size)
{
    int i = uf_find(name, 0);
    if (i < 0) return NULL;
    char *d = malloc((size_t)g_uf[i].len + 1);
    memcpy(d, g_uf[i].data, (size_t)g_uf[i].len);
    d[g_uf[i].len] = 0;
    if (size) *size = g_uf[i].len;
    return d;
}
int plat_save_user(const char *name, const void *data, int len)
{
    int i = uf_find(name, 1);
    if (i < 0) return 0;
    free(g_uf[i].data);
    g_uf[i].data = malloc((size_t)len + 1);
    memcpy(g_uf[i].data, data, (size_t)len);
    g_uf[i].len = len;
    return 1;
}
void plat_append_user(const char *name, const char *text)
{
    int i = uf_find(name, 1), n = (int)strlen(text);
    if (i < 0) return;
    g_uf[i].data = realloc(g_uf[i].data, (size_t)(g_uf[i].len + n + 1));
    memcpy(g_uf[i].data + g_uf[i].len, text, (size_t)n);
    g_uf[i].len += n;
}
void plat_http_get(const char *url, int tag) { (void)url; (void)tag; }
void plat_quit(void) {}
MMImage *plat_load_png(const char *rel) { return NULL; }
/* text: stand-in metrics, or the real fonts (text.c, as the web and SDL builds use) with MM_FONTS=<fonts folder> */
static int g_real_fonts;
int plat_text_px_width(MMCanvas *c, const char *s, int f) { return g_real_fonts ? text_px_width(c, s, f) : (int)strlen(s) * 6 * c->scale; }
int plat_text_px_height(MMCanvas *c, int f) { return g_real_fonts ? text_px_height(c, f) : 13 * c->scale; }
int plat_text_px_ascent(MMCanvas *c, int f) { return g_real_fonts ? text_px_ascent(c, f) : 11 * c->scale; }
void plat_text_px(MMCanvas *c, int X, int Y, const char *s, int f, uint32_t rgb, int a, int b, int d, int e)
{
    if (g_real_fonts) text_px(c, X, Y, s, f, rgb, a, b, d, e);
}


#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_text(void)
{
    char err[256], plain[512];
    CHECK(mm_questions_load("original", err, sizeof err), "load");
    int ents = 0, tags = 0;
    for (int a = 0; a < 9; a++) for (int l = 0; l < 4; l++) for (int i = 0; i < mm_question_count(a, l); i++) {
        const MMQuestion *q = mm_question_at(a, l, i);
        const char *t[5] = {q->text, q->answer[0], q->answer[1], q->answer[2], q->answer[3]};
        for (int k = 0; k < 5; k++) {
            const char *amp = strchr(t[k], '&'), *e = amp ? amp + 1 : NULL;   /* (also typos: "Moli&egrave:re", "&mdash.") */
            while (e && (isalnum((unsigned char)*e) || *e == '#')) e++;
            if ((amp && e - amp > 2 && e - amp < 10 && (*e == ';' || *e == ':' || *e == '.')) || strstr(t[k], "&z.")) { ents++; if (ents < 4) printf("  entity left: %s\n", t[k]); }
            mm_plain(t[k], plain, sizeof plain);
            if (strchr(plain, '<') || strchr(plain, '>')) { tags++; if (tags < 4) printf("  tag left: %s\n", plain); }
        }
    }
    CHECK(ents == 0, "%d texts still contain HTML entities", ents);
    CHECK(tags == 0, "%d texts still contain tags after mm_plain", tags);
    mm_plain("<it>Whistler\xE2\x80\x99s Mother</it> and <Hamlet</it>", plain, sizeof plain);
    CHECK(!strcmp(plain, "Whistler\xE2\x80\x99s Mother and Hamlet"), "mm_plain: %s", plain);
    printf("text: entities decoded, tags handled\n");
}

static void test_mazes(void)
{
    int chars = 0, rooms_total = 0, furn_over = 0;
    int pairs = 0, same_all = 0, same_furn = 0, same_chr = 0, over_door = 0, rooms_door = 0;                  /* neighbouring rooms that look alike */
    for (unsigned seed = 1; seed <= 2000; seed++) {
        mm_srand(seed);
        Maze *m = maze_new();
        int n = 0, stairs = 0;
        for (int i = 0; i < 100; i++) {
            Room *r = m->grid[i];
            if (!r) continue;
            n++;
            int x = i % 10, y = i / 10;
            for (int d = 0; d < 4; d++)
                if (r->doors & (1 << d)) {
                    Room *o = room_at(m, x + DX[d], y + DY[d]);
                    CHECK(o && (o->doors & (1 << OPP[d])), "seed %u: one-way door at %d,%d dir %d", seed, x, y, d);
                }
            if (r->doors & 0x10) {
                stairs++;
                CHECK(!(r->doors & 1), "seed %u: stairs room has a north door", seed);
                CHECK(x == m->tx && y == m->ty, "seed %u: stairs position", seed);
                CHECK(r->chr == NONE && !r->furnA && !r->furnB, "seed %u: stairs room decorated", seed);
            } else {
                CHECK(r->wall >= 0 && r->wall < MM_N_WALLS && (r->floor == NONE || r->floor < MM_N_FLOORS) && r->ceil < MM_N_CEILINGS, "seed %u: bad layers", seed);
                CHECK(r->chr == NONE || r->chr < MM_N_CHARACTERS, "seed %u: bad character", seed);
                chars += r->chr != NONE;
            }
            CHECK(MM_N_FURNITURE == 64 || (!(r->furnA >> MM_N_FURNITURE) && !(r->furnB >> MM_N_FURNITURE)), "seed %u: furniture bit out of range", seed);
            furn_over += __builtin_popcountll(r->furnA) > 5;
            if (r->doors & 12) {                                  /* side door + furniture on that door's slot */
                rooms_door++;
                int hit = 0;
                for (int f = 0; f < MM_N_FURNITURE; f++)
                    if ((r->furnA | r->furnB) >> f & 1) {
                        const MMObject *o = obj(MM_G_FURNITURE, f);
                        if (((r->doors & 4) && o->slots[3]) || ((r->doors & 8) && o->slots[2])) hit = 1;
                    }
                over_door += hit;
            }
            for (int d = 1; d <= 2; d++)                          /* east / south neighbour through a door */
                if ((r->doors & (1 << (d == 1 ? 2 : 1))) && !(r->doors & 0x10)) {
                    Room *o = room_at(m, x + (d == 1), y + (d == 2));
                    if (!o || (o->doors & 0x10)) continue;
                    pairs++;
                    int sf = o->furnA == r->furnA && o->furnB == r->furnB;
                    same_furn += sf;
                    same_chr += o->chr != NONE && o->chr == r->chr;
                    same_all += sf && o->wall == r->wall && o->floor == r->floor && o->ceil == r->ceil && o->chr == r->chr;
                }
        }
        rooms_total += n;
        CHECK(n == 41, "seed %u: %d rooms", seed, n);
        CHECK(stairs == 1, "seed %u: %d stairs rooms", seed, stairs);
        CHECK(m->sy == 9 && room_at(m, m->sx, m->sy) && room_at(m, m->sx, m->sy)->visited, "seed %u: start", seed);
        /* all rooms reachable from the start */
        int seen[100] = {0}, stack[100], sp = 0, reach = 0;
        stack[sp++] = m->sy * 10 + m->sx;
        seen[stack[0]] = 1;
        while (sp) {
            int i = stack[--sp];
            reach++;
            for (int d = 0; d < 4; d++)
                if (m->grid[i]->doors & (1 << d)) {
                    int j = (i / 10 + DY[d]) * 10 + i % 10 + DX[d];
                    if (!seen[j]) { seen[j] = 1; stack[sp++] = j; }
                }
        }
        CHECK(reach == 41, "seed %u: only %d rooms reachable", seed, reach);
        CHECK(m->tx != m->sx || m->ty != m->sy, "seed %u: stairs in the start room", seed);
        maze_free(m);
    }
    CHECK(furn_over == 0, "more than 5 back furniture items in %d rooms", furn_over);
    printf("mazes: 2000 seeds, %d rooms, %.1f%% of rooms have a character\n", rooms_total, 100.0 * chars / rooms_total);
    printf("mazes: %d connected neighbour pairs, %.2f%% same furniture, %.2f%% look identical\n", pairs,
           100.0 * same_furn / pairs, 100.0 * same_all / pairs);
    CHECK(same_chr == 0, "%d connected neighbours with the same character", same_chr);   /* CHANGES 2026-11-17 */
    CHECK(over_door == 0, "%d rooms with furniture on a side door's slot", over_door);
    printf("mazes: %.1f%% of neighbours have the same character; %.1f%% of rooms with a side door have furniture on its slot\n",
           100.0 * same_chr / pairs, 100.0 * over_door / rooms_door);
}

static void test_questions(void)
{
    char err[256];
    CHECK(mm_questions_load("original", err, sizeof err), "original: %s", err);
    int total = 0;
    for (int a = 0; a < 9; a++) for (int l = 0; l < 4; l++) total += mm_question_count(a, l);
    CHECK(total > 0 && (!g_2006 || total == 8020), "original has %d questions", total);
    g_total = total;
    const MMQuestion *q = mm_question_at(0, 0, 0);
    CHECK(q && (q->id == 1 || (g_original && !g_2006)) && q->answer[0][0] && q->answer[3][0], "first question");
    /* keep copies, then load the JSON export and compare (the export is only in the developer's 2006 folder) */
    if (plat_data_size("questions.json") <= 0) { printf("questions: %d (no questions.json to compare)\n", total); return; }
    char (*texts)[64] = malloc((size_t)total * 64);
    int k = 0;
    for (int a = 0; a < 9; a++) for (int l = 0; l < 4; l++) for (int i = 0; i < mm_question_count(a, l); i++)
        snprintf(texts[k++], 64, "%s|%s", mm_question_at(a, l, i)->text, mm_question_at(a, l, i)->answer[3]);
    CHECK(mm_questions_load("json:questions.json", err, sizeof err), "json: %s", err);
    int same = 0;
    k = 0;
    for (int a = 0; a < 9; a++) for (int l = 0; l < 4; l++) for (int i = 0; i < mm_question_count(a, l); i++) {
        char t[64];
        snprintf(t, 64, "%s|%s", mm_question_at(a, l, i)->text, mm_question_at(a, l, i)->answer[3]);
        same += !strcmp(t, texts[k++]);
    }
    CHECK(same == total, "json differs from original in %d questions", total - same);
    free(texts);
    printf("questions: original and json identical (%d)\n", same);
}

static void set_seed(void) { plat_set_setting("seed", "99"); plat_set_setting("questions", "original"); plat_set_setting("title_seconds", "0"); plat_set_setting("opt_resume", "0");
                             plat_set_setting("opt_hints", "0"); plat_set_setting("opt_prompts", "0"); plat_set_setting("opt_anim", "0"); plat_set_setting("opt_layout", "1"); }

/* title screen (CHANGE): shown at start for title_seconds, ends on time, a click or a key; logon underneath */
static void test_title(void);
/* CHANGE 2026-11-18: entering the castle while INTRO.MID still plays starts a maze loop at once */
static void test_castle_music(void)
{
    char err[256];
    set_seed();
    plat_set_setting("Music", "1");
    CHECK(mm_init(err, sizeof err) && g_track[0] && !strcmp(g_track[0], "INTRO.MID"), "intro on the logon screen");
    g_lg[g_nlg - 1].sel = 1;
    g_fade_ms = 0;
    logon_start();
    CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6) && g_fade_ms > 0, "the castle door click fades into a loop (%s)",
          g_track[0] ? g_track[0] : "-");
    enter_castle();
    CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6), "the loop plays on in the maze (%s)", g_track[0] ? g_track[0] : "-");
    const char *loop = g_track[0];
    mm_music_ended(0);
    CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6) && strcmp(g_track[0], loop), "next loop differs");
    /* loop transitions (Encarta 95-98 discs): "<from>-<to>.mid" between two loops when the file is there */
    static const char *const TR[6] = {"1-2.mid", "1-3.mid", "2-1.mid", "2-3.mid", "3-1.mid", "3-2.mid"};
    for (int i = 0; i < 6 && !g_original; i++) plat_save_data(TR[i], "MThd", 4);
    if (!g_original) {
        const char *from = g_track[0];
        mm_music_ended(0);
        char want_prefix[4] = {from[6], '-', 0, 0};               /* "BGLOOPn.MID" -> "n-" */
        CHECK(g_track[0] && !strncmp(g_track[0], want_prefix, 2) && strstr(g_track[0], ".mid"), "transition after %s (%s)", from,
              g_track[0] ? g_track[0] : "-");
        char to = g_track[0] ? g_track[0][2] : 0;
        mm_music_ended(0);
        CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6) && g_track[0][6] == to, "the loop of the transition follows (%s)",
              g_track[0] ? g_track[0] : "-");
        char cmd2[600];
        snprintf(cmd2, sizeof cmd2, "rm -f '%s'/[123]-[123].mid", g_data);
        if (system(cmd2) != 0) printf("  (could not remove the test transitions)\n");
    }
    show_logon();                                            /* a second game: intro again, switch again */
    CHECK(g_track[0] && !strcmp(g_track[0], "INTRO.MID"), "intro again on the logon screen");
    g_lg[g_nlg - 1].sel = 1;
    logon_start();
    enter_castle();
    CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6), "second game: entering starts a loop (%s)", g_track[0] ? g_track[0] : "-");
    free_players();
    printf("castle music: ok\n");
}
static void test_title(void)
{
    char err[256];
    set_seed();
    plat_set_setting("title_seconds", "5");
    if (plat_data_size("titlescr.dib") <= 0) {                      /* Encarta 95 has no title picture */
        CHECK(mm_init(err, sizeof err) && !g_title_on && g_screen == SCR_LOGON, "no title picture: the logon screen at once");
        printf("title: none in this data folder\n");
        return;
    }
    CHECK(mm_init(err, sizeof err) && g_title_on && g_screen == SCR_LOGON, "title shown at start");
    CHECK(g_track[0] && !strcmp(g_track[0], "INTRO.MID"), "intro music plays during the title");
    g_now += 4900; mm_tick();
    CHECK(g_title_on, "title still shown after 4.9 s");
    g_now += 200; mm_tick();
    CHECK(!g_title_on, "title gone after 5 s");
    mm_init(err, sizeof err);
    mm_mouse(300, 200, MMB_LEFT, MA_DOWN, 0);
    CHECK(!g_title_on && g_nlg >= 1 && !g_lg[0].sel, "a click ends the title without clicking the logon screen");
    mm_init(err, sizeof err);
    mm_key(KEY_ENTER, 0);
    CHECK(!g_title_on, "a key ends the title");
    set_seed();
    printf("title: ok\n");
}

static void test_game(void)
{
    char err[256];
    set_seed();
    CHECK(mm_init(err, sizeof err), "init: %s", err);
    /* two players: a saved one and Tourist */
    plat_set_setting("MindmazePlayer1", "2600,0,Ann");
    show_logon();
    CHECK(g_nlg == 2 && !strcmp(g_lg[0].name, "Ann") && g_lg[1].tourist, "logon list");
    logon_start();
    CHECK(g_nmsg == 1, "no selection -> message");
    msg_close(1);
    g_lg[0].sel = g_lg[1].sel = 1;
    logon_start();
    CHECK(g_screen == SCR_GRATING && g_pl[0] && g_pl[1] && g_pl[0]->score == 2600, "start");
    mm_key(KEY_ENTER, 0);
    mm_key(KEY_ENTER, 0);
    CHECK(g_screen == SCR_ROOM, "entered the castle");

    /* correct answer: points, move, turn passes */
    Player *a = g_pl[0];
    Room *r = cur_room();
    int dir = -1;
    for (int d = 0; d < 4; d++) if (r->doors & (1 << d)) { dir = d; break; }
    int x0 = a->maze->cx, y0 = a->maze->cy;
    door_click(dir);
    CHECK(g_qopen && g_q.seconds == 66, "question opened");
    g_now += 3000;
    for (int i = 0; i < 60; i++) { g_now += 50; mm_tick(); }
    int secs = g_q.seconds;
    CHECK(secs == 60, "clock counts down 66 -> 60 in 6 s (%d)", secs);
    int right = 0;
    for (int b = 0; b < 4; b++) if (g_q.order[b] == 0) right = b;
    answer(right);
    CHECK(g_nmsg == 1 && strstr(g_msg[0].text, "points added"), "correct message");
    msg_close(1);
    CHECK(a->score == 2600 + secs, "score %d", a->score);
    CHECK(a->maze->cx == x0 + DX[dir] && a->maze->cy == y0 + DY[dir], "moved");
    CHECK(g_cur == 1, "turn passed to player 2");
    CHECK(room_at(a->maze, x0, y0)->opened & (1 << dir), "door stays open");

    /* player 2: wrong, then wrong again -> no move, turn passes */
    Player *b = g_pl[1];
    r = cur_room();
    for (int d = 0; d < 4; d++) if (r->doors & (1 << d)) { dir = d; break; }
    x0 = b->maze->cx;
    y0 = b->maze->cy;
    door_click(dir);
    int wrong[2], nw = 0;
    for (int k = 0; k < 4; k++) if (g_q.order[k] != 0 && nw < 2) wrong[nw++] = k;
    answer(wrong[0]);
    msg_close(1);
    CHECK(g_qopen && g_q.disabled[wrong[0]] && g_q.tried, "first wrong answer");
    answer(wrong[0]);
    CHECK(g_nmsg == 0, "disabled answer ignored");
    answer(wrong[1]);
    CHECK(g_nmsg == 1 && strstr(g_msg[0].text, "correct answer is"), "second wrong: %s", g_nmsg ? g_msg[0].text : "-");
    msg_close(1);
    CHECK(!g_qopen && b->maze->cx == x0 && b->maze->cy == y0 && b->score == 0, "no move, no points");
    CHECK(g_cur == 0, "turn back to player 1");

    /* walking back through the opened door costs no question */
    a = g_pl[0];
    int back = -1;
    for (int d = 0; d < 4; d++) if (cur_room()->opened & (1 << d)) back = d;
    CHECK(back >= 0, "opened door on the other side");
    door_click(back);
    CHECK(!g_qopen && g_cur == 1, "free move through an opened door");

    /* stairs: award re-ranked, new maze, same player */
    g_cur = 0;
    a->maze->cx = a->maze->tx;
    a->maze->cy = a->maze->ty;
    Maze *old = a->maze;
    stairs();
    (void)old;   /* the new maze may reuse the old allocation: check the position instead */
    int fresh = a->maze->cx == a->maze->sx && a->maze->cy == a->maze->sy && a->maze->sy == 9;
    CHECK(fresh && a->award == 1 && g_nmsg == 1, "stairs -> Pupil award, new maze (fresh %d, award %d, score %d, msgs %d)",
          fresh, a->award, a->score, g_nmsg);
    msg_close(1);
    CHECK(g_cur == 0, "same player after the stairs");

    /* matches */
    match(0);
    CHECK(a->maze->reveal && !a->maze->match[0], "match lit");
    match(1);
    CHECK(a->maze->match[1], "second match blocked while one burns");
    g_now += 8100;
    mm_tick();
    CHECK(!a->maze->reveal, "match out after 8 s");

    /* winning: 20000 points, stairs -> award 5, atrium, ending resets the player */
    a->score = 20500;
    a->maze->cx = a->maze->tx;
    a->maze->cy = a->maze->ty;
    stairs();
    msg_close(1);
    CHECK(a->award == 5, "master scholar");
    room_click(0x1E);
    CHECK(g_speech && g_atrium_text, "atrium text");
    mm_mouse(300, 100, MMB_LEFT, MA_DOWN, 0);
    CHECK(g_story.on && g_story.kind == 1, "ending story");
    close_story();
    CHECK(g_screen == SCR_LOGON, "back at the logon");
    char v[256];
    plat_get_setting("MindmazePlayer1", v, sizeof v);
    CHECK(!strcmp(v, "0,0,Ann"), "winner reset and saved: %s", v);
    CHECK(g_sounds[33] >= 1 && g_sounds[34] >= 2 && g_sounds[31] == 1 && g_sounds[32] >= 1, "sounds right/wrong/match");
    printf("game flow: ok\n");
}

/* flows checked against the decomp on 2026-11-16 (music FUN_0051adb0, hotspots FUN_0051c190/FUN_0052f840,
 * door without a question FUN_0051ccb0, matches 0x51e250, logon FUN_005215d0) */
static void test_flows(void)
{
    char err[256];
    set_seed();
    g_nset = 0;
    set_seed();
    plat_set_setting("Music", "1");
    plat_set_setting("Sound", "1");
    CHECK(mm_init(err, sizeof err), "init: %s", err);
    /* logon: INTRO.MID once; its end starts a background loop, still on the logon screen */
    CHECK(g_track[0] && !strcmp(g_track[0], "INTRO.MID") && g_music_state == 2, "logon plays INTRO (%s)", g_track[0]);
    mm_music_ended(0);
    CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6) && g_snd.mode >= 1, "INTRO end -> BGLOOP (%s)", g_track[0]);
    const char *first = g_track[0];
    mm_music_ended(0);
    CHECK(g_track[0] && strcmp(g_track[0], first) != 0, "next loop differs from the last (%s %s)", first, g_track[0]);

    /* new player names: case-insensitive duplicates refused, a new name is saved at once, 12 characters */
    mm_cmd(CMD_NEW_GAME);
    CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6), "New Game on the logon screen does nothing");
    logon_new_name();
    for (const char *s = "Abcdefghijklmnop"; *s; s++) mm_char((unsigned char)*s);
    CHECK(strlen(g_lg_text) == 12, "name limited to 12 (%d)", (int)strlen(g_lg_text));
    mm_key(KEY_ENTER, 0);
    char v[256];
    int saved = 0;
    for (int n = 1; n < 10; n++) {
        char k[32];
        snprintf(k, sizeof k, "MindmazePlayer%d", n);
        if (plat_get_setting(k, v, sizeof v) && strstr(v, "Abcdefghijkl")) saved = 1;
    }
    CHECK(saved && g_lg[0].sel && g_lg[0].n, "new player saved at once");
    logon_new_name();
    for (const char *s = "ABCDEFGHIJKL"; *s; s++) mm_char((unsigned char)*s);
    mm_key(KEY_ENTER, 0);
    CHECK(g_nmsg == 1 && g_lg_edit, "duplicate (other case) refused, edit box reopened");
    msg_close(1);
    mm_key(KEY_ESC, 0);
    for (int i = 0; i < g_nlg; i++) g_lg[i].sel = g_lg[i].tourist;
    logon_start();
    mm_key(KEY_ENTER, 0);
    mm_key(KEY_ENTER, 0);
    CHECK(g_screen == SCR_ROOM && g_track[0] && !strncmp(g_track[0], "BGLOOP", 6), "entering the castle does not switch the music");
    CHECK(g_snd.amb_running, "ambient timer runs after entering");

    /* question music pauses the loop; closing it resumes the loop */
    Room *r = cur_room();
    int dir = -1;
    for (int d = 0; d < 4; d++) if (r->doors & (1 << d)) { dir = d; break; }
    door_click(dir);
    CHECK(g_qopen && g_track[1] && !strcmp(g_track[1], "QUESTION.MID") && g_paused[0] && !g_snd.amb_running, "question music");
    mm_music_ended(1);
    CHECK(g_track[1] && !strcmp(g_track[1], "QUESTION.MID"), "question music loops");
    room_click(dir);
    CHECK(g_beeps == 1, "hotspot click during a question beeps");
    int wrong[2], nw = 0;
    for (int k = 0; k < 4; k++) if (g_q.order[k] != 0 && nw < 2) wrong[nw++] = k;
    answer(wrong[0]);
    msg_close(1);
    answer(wrong[1]);
    CHECK(g_qopen && g_track[1], "second wrong: question (and its music) open while the box shows");
    msg_close(1);
    CHECK(!g_qopen && !g_track[1] && !g_paused[0] && g_music_state == 2, "question closed, loop resumed");

    /* matches: per player flag; the timer end only sounds if the current player's map is lit */
    Player *p = cur();
    match(0);
    CHECK(p->maze->reveal, "match lit");
    int m2 = g_sounds[32];
    p->maze->reveal = 0;                                     /* as if the turn refresh had put it out */
    g_now += 8100;
    mm_tick();
    CHECK(g_sounds[32] == m2, "no MATCH2 when nothing is lit");

    /* hotspots: first rect in the original's order wins */
    g_nhot = 0;
    add_hot(0, 0, 100, 100, 31);
    add_hot(0, 0, 200, 200, 15);
    CHECK(hot_at(50, 50) == 31 && hot_at(150, 150) == 15 && hot_at(250, 250) == -1, "hotspot order");

    /* a door with no question left: message, then the player walks through anyway */
    mm_skip_seen = 0;                                        /* the original behaviour (skip seen is a CHANGE) */
    mm_question_count(0, 0);
    Maze *mz = p->maze;
    r = cur_room();
    for (int d = 0; d < 4; d++) if ((r->doors & (1 << d)) && !(r->opened & (1 << d))) { dir = d; break; }
    int x0 = mz->cx, y0 = mz->cy, c0 = g_cur;
    p->areabtn = 1;
    g_slot[p->slot].n = 0;                                   /* empty list */
    door_click(dir);
    CHECK(!g_qopen && g_nmsg == 1, "out of questions message");
    msg_close(1);
    CHECK(mz->cx == x0 + DX[dir] && mz->cy == y0 + DY[dir] && g_cur == c0, "moved through without a question");

    /* music off: everything stops, the ambient timer is not restarted by room redraws */
    mm_cmd(CMD_MUSIC);
    CHECK(!g_track[0] && !g_track[1] && g_snd.mode == 0, "music off");
    next_turn();
    CHECK(!g_snd.amb_running, "no ambient after a room redraw with music off");
    mm_cmd(CMD_MUSIC);
    CHECK(g_track[0] && !strncmp(g_track[0], "BGLOOP", 6), "music on again: a loop");
    free_players();
    printf("flows: ok\n");
}

#include <time.h>
/* question sources (CHANGE): duplicates across sources, seen list, start over, level fallback, custom add */
static void test_sources(void)
{
    char err[256];
    CHECK(mm_questions_load("original|json:questions.json", err, sizeof err), "two sources: %s", err);
    CHECK(mm_questions_count_src(0) == g_total && mm_questions_count_src(1) == 0, "json duplicates dropped (%d + %d)",
          mm_questions_count_src(0), mm_questions_count_src(1));
    /* seen list: all but 3 questions of (area 2, level 1) seen -> the list holds the 3 */
    mm_srand(7);
    g_pl[0] = calloc(1, sizeof(Player));
    snprintf(g_pl[0]->name, sizeof g_pl[0]->name, "Tester");
    g_pl[0]->slot = 0;
    int total = mm_question_count(2, 1);
    for (int i = 3; i < total; i++) mm_mark_seen("Tester", mm_question_at(2, 1, i)->key);
    mm_skip_seen = 1;
    list_build(1, 0, 2);
    CHECK(g_slot[0].n == 3, "unseen only: %d (of %d)", g_slot[0].n, total);
    for (int i = 0; i < 3; i++) mm_mark_seen("Tester", mm_question_at(2, 1, i)->key);
    list_build(1, 0, 2);
    /* all seen: a fresh round; the newest quarter of the marks stays (CHANGE 2026-11-18) */
    int want = total - total / 4;                     /* real question sets can hold a duplicate text: one off */
    CHECK(g_slot[0].n == want || (g_original && abs(g_slot[0].n - want) <= 2), "all seen: fresh round without the newest quarter (%d of %d)", g_slot[0].n, total);
    for (int i = 0; i < 3 && i < total / 4; i++)
        CHECK(mm_seen("Tester", mm_question_at(2, 1, i)->key), "question %d (seen last) still marked", i);
    /* only this player, this area / level */
    int other = mm_question_count(3, 1) > 0;
    if (other) mm_mark_seen("Tester", mm_question_at(3, 1, 0)->key);
    mm_mark_seen("Other", mm_question_at(2, 1, 0)->key);
    for (int i = 0; i < total; i++) mm_mark_seen("Tester", mm_question_at(2, 1, i)->key);
    list_build(1, 0, 2);
    int still = 0;
    for (int i = 0; i < total; i++) still += mm_seen("Tester", mm_question_at(2, 1, i)->key);
    int nkeys = 0;                                           /* distinct keys (near-identical texts share one) */
    for (int i = 0; i < total; i++) {
        int dup = 0;
        for (int j = 0; j < i && !dup; j++) dup = mm_question_at(2, 1, j)->key == mm_question_at(2, 1, i)->key;
        nkeys += !dup;
    }
    CHECK(still >= nkeys / 4 && still <= nkeys / 4 + (total - nkeys), "exhausted list: %d marks kept (expected %d)", still, nkeys / 4);
    CHECK(g_slot[0].n == total - still, "fresh round holds the rest (%d)", g_slot[0].n);
    CHECK(!other || mm_seen("Tester", mm_question_at(3, 1, 0)->key), "other areas keep their marks");
    CHECK(mm_seen("Other", mm_question_at(2, 1, 0)->key), "other players keep their marks");
    char *sf = plat_load_user("seen.txt", NULL);
    int lines = 0;
    for (char *q = sf; q && (q = strstr(q, "Tester	")); q++) lines++;
    CHECK(sf && strstr(sf, "Other	") && lines == nkeys / 4 + other, "seen file rewritten (%d Tester lines)", lines);
    free(sf);
    mm_seen_forget_all();
    /* a question added while running joins its list and the picker rebuilds */
    unsigned gen = mm_questions_generation();
    const char *const a[4] = {"Yes", "No", "Maybe", "Never"};
    CHECK(mm_questions_add("Is this a test question added while the game runs?", a, 2, 1, 0) &&
          mm_question_count(2, 1) == total + 1 && mm_questions_generation() != gen, "added question");
    CHECK(!mm_questions_add("Is this a TEST question added while the game runs", a, 2, 1, 0), "duplicate text refused");
    mm_skip_seen = 0;
    list_build(1, 0, 2);
    CHECK(g_slot[0].n == total + 1, "list includes the new question");
    /* level fallback: an empty (area, level) uses the nearest level */
    FILE *f = fopen("/tmp/mm_one.json", "w");
    fputs("{\"questions\": [{\"id\": 1, \"area\": 4, \"level\": 0, \"question\": \"Only one\", \"answers\": "
          "[{\"text\": \"a\"}, {\"text\": \"b\"}, {\"text\": \"c\"}, {\"text\": \"d\"}]}]}", f);
    fclose(f);
    CHECK(mm_questions_load("json:/tmp/mm_one.json", err, sizeof err), "one-question file: %s", err);
    list_build(3, 0, 4);
    CHECK(g_slot[0].level == 0 && g_slot[0].n == 1, "level 4 falls back to level 1 (level %d, n %d)", g_slot[0].level, g_slot[0].n);
    remove("/tmp/mm_one.json");
    free(g_pl[0]);
    g_pl[0] = NULL;
    CHECK(mm_questions_load("original", err, sizeof err), "reload original");
    printf("sources: ok\n");
}

/* the award picture for each rank is drawn in the panel (529, 336) when no question is open */
static void test_awards(void)
{
    char err[256];
    set_seed();
    mm_init(err, sizeof err);
    g_lg[g_nlg - 1].sel = 1;
    logon_start();
    enter_castle();
    MMCanvas c = {0};
    c.scale = 1;
    c.w = MM_W;
    c.h = MM_H;
    c.stride = c.w;
    c.px = malloc(sizeof(uint32_t) * c.w * c.h);
    for (int a = 1; a < 6; a++) {
        cur()->award = a;
        mm_fill(&c, 0, 0, MM_W, MM_H, 0);
        mm_draw(&c);
        uint32_t bg = c.px[(301 + 60) * c.stride + 630];         /* panel background */
        int x0 = MM_W, x1 = -1, y0 = MM_H, y1 = -1, n = 0;
        for (int y = 301 + 27; y < 301 + 105; y++)               /* above the New Game / Quit Game buttons */
            for (int x = 500; x < 630; x++)
                if (c.px[y * c.stride + x] != bg) {
                    n++;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        /* (the test platform draws no text, so everything here is the award picture) */
        CHECK(n > 300, "award %d (%s): only %d pixels drawn", a, mm_ranks[a].image, n);
        CHECK(y1 < 301 + 90, "award %d reaches into the Progress line (to y %d)", a, y1 - 301);
        CHECK(abs((x0 + x1 + 1) / 2 - 562) <= 1, "award %d not centred (x %d-%d)", a, x0, x1);
    }
    free(c.px);
    printf("awards: ranks 1-5 drawn, centred on x 562, clear of the Progress line\n");
}

static void test_draw_speed(void)
{
    /* full room + panel at scale 4 (a 4K screen), original pictures */
    char err[256];
    set_seed();
    mm_init(err, sizeof err);
    g_lg[g_nlg - 1].sel = 1;
    logon_start();
    enter_castle();
    MMCanvas c = {0};
    c.scale = 4;
    c.w = MM_W * 4;
    c.h = MM_H * 4;
    c.stride = c.w;
    c.px = malloc(sizeof(uint32_t) * c.w * c.h);
    mm_draw(&c);                                          /* first draw loads the pictures */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 20; i++) mm_draw(&c);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / 20;
    printf("draw speed: %.1f ms per full frame at 2544x1748\n", ms);
#ifndef __SANITIZE_ADDRESS__                              /* the sanitizer build is several times slower */
    CHECK(ms < 60, "full redraw too slow (%.1f ms)", ms);
#endif
    free(c.px);
}

/* Settings > Pictures: page opens, the how-to link goes to docs/HD-ART.md, the art kit folder opens, Back returns */
static void test_settings_pictures(void)
{
    mm_settings_open();
    int px = 78 + 156 + 10, py = 22 + 392 - 25;               /* "Pictures / HD art..." (PX + 156, PY + PH - 30) */
    CHECK(mm_settings_hit(px, py) == 35, "pictures button (%d)", mm_settings_hit(px, py));
    mm_settings_mouse(px, py, MMB_LEFT, MA_DOWN);
    CHECK(mm_settings_hit(78 + 20, 22 + 392 - 25) == 37, "how-to button on the pictures page");
    MMCanvas c = {0};
    c.scale = 1; c.w = MM_W; c.h = MM_H; c.stride = c.w;
    c.px = calloc((size_t)c.w * c.h, sizeof(uint32_t));
    mm_settings_draw(&c, 0, 0);                                /* draws without crashing (text is stubbed) */
    free(c.px);
    g_opened[0] = 0;
    mm_settings_mouse(78 + 20, 22 + 392 - 25, MMB_LEFT, MA_DOWN);
    CHECK(strstr(g_opened, "/blob/main/docs/HD-ART.md") != NULL, "how-to link: %s", g_opened);
    mm_settings_mouse(78 + 230, 22 + 392 - 25, MMB_LEFT, MA_DOWN);
    CHECK(!strcmp(g_opened, "folder:art"), "art folder: %s", g_opened);
    CHECK(mm_hd_available() || mm_settings_hit(78 + 20, 22 + 50 + 5) == 0, "HD switch disabled without HD art");
    mm_settings_mouse(78 + 480 - 90, 22 + 392 - 25, MMB_LEFT, MA_DOWN);   /* Back */
    CHECK(mm_settings_hit(px, py) == 35, "back on the main settings page");
    /* volume bars: a click at the middle of the music bar sets 50 %, applied and saved */
    int vy = 0;
    for (int y = 22; y < 22 + 392 && !vy; y++) if (mm_settings_hit(78 + 180 + 100, y) == 80) vy = y + 4;
    CHECK(vy > 0, "music volume bar found");
    mm_settings_mouse(78 + 180 + 100, vy, MMB_LEFT, MA_DOWN);
    char v[16] = "";
    plat_get_setting("MusicVolume", v, sizeof v);
    CHECK(mm_music_volume == 50 && g_vol[0] == 50 && !strcmp(v, "50"), "music volume 50 (%d, %d, %s)", mm_music_volume, g_vol[0], v);
    mm_settings_mouse(78 + 180 + 200, vy + 24, MMB_LEFT, MA_DOWN);
    CHECK(mm_sound_volume == 100 && g_vol[1] == 100, "sound volume 100 (%d)", mm_sound_volume);
    mm_set_volume(100, 100);
    mm_settings_key(KEY_ESC, 0);
    CHECK(!mm_settings_on(), "settings closed");
    printf("settings: pictures page and volume ok\n");
}

/* built-in art tools: sheets made, an "agent" result (each sheet doubled) installed, the game sees the pictures */
#include "../src/art/mm_art_text.h"
static void test_art(void)
{
    char rep[2048], path[160];
    CHECK(mm_art_prepare(1, rep, sizeof rep), "art prepare: %s", rep);
    CHECK(plat_data_size("art/work/hd/sheets/backgrounds_000.png") > 0 && plat_data_size("art/work/hd/sheets/backgrounds_000.txt") > 0,
          "HD sheet written");
    CHECK(plat_data_size("art/work/wide/sheets/backgrounds_000.png") > 0 && plat_data_size("art/work/wide/masks/backgrounds_000.png") > 0,
          "widescreen sheet and mask written");
    CHECK(!mm_art_results_waiting(), "nothing waiting before the agent ran");
    int results = 0;
    for (int kind = 0; kind < 2; kind++)
        for (int g = 0; ART_GROUPS[g].prefix; g++)
            for (int k = 0; k < 8; k++) {
                snprintf(path, sizeof path, "art/work/%s/sheets/%s_%03d.png", kind ? "wide" : "hd", ART_GROUPS[g].prefix, k);
                AImg *s = aimg_load_png_rgb(path), *d = s ? aimg_new(s->w * 2, s->h * 2, 3) : NULL;
                if (!d) { aimg_free(s); continue; }
                for (int y = 0; y < d->h; y++)
                    for (int x = 0; x < d->w; x++) memcpy(d->p + ((size_t)y * d->w + x) * 3, s->p + ((size_t)(y / 2) * s->w + x / 2) * 3, 3);
                snprintf(path, sizeof path, "art/work/%s/out_gen/%s_%03d.png", kind ? "wide" : "hd", ART_GROUPS[g].prefix, k);
                results += aimg_save_png(path, d);
                aimg_free(s);
                aimg_free(d);
            }
    CHECK(results >= 4 && mm_art_results_waiting(), "agent results waiting (%d)", results);
    int n = mm_art_install(rep, sizeof rep);
    CHECK(n > 0, "installed %d pictures: %s", n, rep);
    CHECK(plat_data_size("hd/logonscr.png") > 0 && plat_data_size("hd/bounds.txt") > 0, "HD logon picture installed");
    CHECK(plat_data_size("wide/logonscr.png") > 0 && plat_data_size("wide/logonscr_1x.png") > 0 && plat_data_size("wide/wide.txt") > 0,
          "widescreen logon picture installed");
    CHECK(plat_data_size("art/work/REPORT.txt") > 0 && !mm_art_results_waiting(), "report written, nothing waiting");
    CHECK(mm_hd_available() && mm_wide_available(), "the game sees the new pictures without a restart");
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s/hd' '%s/wide' '%s/art'", g_data, g_data, g_data);
    if (!g_original && system(cmd) != 0) printf("  (could not remove the test pictures)\n");
    mm_pictures_reload();
    printf("art tools: %d sheets doubled, %d pictures installed\n", results, n);
}

/* data/tables.json: loaded, counts as declared, strings found, JSON escapes decoded */
static void test_tables(void)
{
    char err[256] = "";
    CHECK(mm_tables_load(err, sizeof err), "tables: %s", err);
    if (!mm_objects) return;
    CHECK(MM_N_DOORS == 5 && MM_N_OBJECTS == MM_G_FURNITURE + MM_N_FURNITURE && MM_G_FLOORS == 5, "group layout");
    for (int i = 0; i < MM_N_OBJECTS; i++) CHECK(mm_objects[i].name && strstr(mm_objects[i].name, ".DIB"), "object %d name", i);
    for (int i = 0; i < MM_N_CHARACTERS; i++) {
        int id = obj(MM_G_CHARACTERS, i)->id;
        CHECK(id >= 7 && id < 7 + MM_N_SPEECH && mm_speech_count[id - 7] > 0, "character %d (id %d) has no speech text", i, id);
    }
    for (int i = 1; i < MM_N_RANKS; i++) CHECK(mm_ranks[i].min == mm_ranks[i - 1].max + 1, "rank %d range", i);
    CHECK(atoi(mm_str(29034)) == 66 && strstr(mm_str(29041), "%u") && strstr(mm_str(29040), "%s") && !*mm_str(1), "strings");
    CHECK(!strcmp(mm_music_files[0], "INTRO.MID") && !strcmp(mm_music_files[4], "QUESTION.MID"), "music names");
    if (!g_original)                                          /* the fixture writes its e-acute as a JSON \u escape */
        CHECK(strstr(mm_speech[0][0], "caf\xC3\xA9") != NULL, "\\u escape decoded to UTF-8: %s", mm_speech[0][0]);
    if (g_original) CHECK(MM_N_CHARACTERS == 22 && MM_N_FURNITURE == 34 && MM_N_OBJECTS == 70, "original object counts");
    printf("tables: %d objects (%d characters, %d furniture)\n", MM_N_OBJECTS, MM_N_CHARACTERS, MM_N_FURNITURE);
}

/* Settings > "Add questions from a disc" (CHANGE 2026-11-18): the fake platform "reads a disc" (build/fixtures/discfx:
 * 100 questions the original already has + 50 new ones) into editions/fx/; the new source is listed after the
 * original, switched on, and only its 50 new questions are loaded */
static void test_add_disc(void)
{
    char err[256];
    set_seed();
    CHECK(mm_init(err, sizeof err), "init: %s", err);
    int before = mm_source_count(), total0 = 0;
    for (int a = 0; a < 9; a++) for (int l = 0; l < 4; l++) total0 += mm_question_count(a, l);
    mm_settings_open();
    mm_settings_mouse(PX_ADD_DISC, PY_ADD_DISC, MMB_LEFT, MA_DOWN);
    CHECK(g_add_disc_calls == 1, "the button asks the platform for a disc");
    CHECK(mm_source_count() == before + 1 && strstr(mm_source_name(1), "Fixture Edition"), "source listed after the original (%s)",
          mm_source_name(1));
    CHECK(mm_source_enabled(1) && strstr(mm_source_info(1), "50 new of 150"), "switched on, duplicates left out: %s",
          mm_source_info(1));
    int total = 0;
    for (int a = 0; a < 9; a++) for (int l = 0; l < 4; l++) total += mm_question_count(a, l);
    CHECK(total == total0 + 50, "questions loaded: %d (before %d)", total, total0);
    mm_disc_added(1, "fx", "Fixture Edition", 150, NULL);    /* the same disc again: replaced, not listed twice */
    CHECK(mm_source_count() == before + 1, "adding the same disc twice keeps one entry");
    mm_disc_added(0, NULL, NULL, 0, "No MindMaze questions found here.");
    CHECK(mm_source_count() == before + 1, "a failed read changes nothing");
    mm_settings_key(KEY_ESC, 0);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'/editions", g_data);
    if (system(cmd) != 0) printf("  (could not remove the test edition)\n");
    plat_set_setting("sources", "original,custom");
    printf("add disc: ok (50 new of 150)\n");
}


/* CHANGE 2026-11-21: players saved at once (score after every right answer, rank at the stairs, mm_save_players when
 * the app goes to the background); Settings always on the logon screen and through the gear in the game; the phone
 * keyboard flag (mm_text_input) */
static void test_save_settings(void)
{
    char err[256], v[256];
    g_nset = 0;
    set_seed();
    CHECK(mm_init(err, sizeof err), "init: %s", err);
    plat_set_setting("MindmazePlayer1", "100,0,Ann");
    show_logon();

    /* typing a new name: keyboard wanted; Settings works and keeps the typed name */
    CHECK(!mm_text_input(), "no typing on the logon screen at first");
    logon_click(475, 280);                                       /* New player */
    CHECK(g_lg_edit && mm_text_input(), "new player name: keyboard wanted");
    mm_char('B'); mm_char('o'); mm_char('b');
    CHECK(clickable_at(480, 410) == 24, "Settings clickable while typing");
    logon_click(480, 410);                                       /* Settings... */
    CHECK(!g_lg_edit && mm_settings_on() && !strcmp(g_lg[0].name, "Bob"), "Settings opens, Bob kept (%s)", g_lg[0].name);
    CHECK(!mm_text_input(), "settings main page: no keyboard");
    mm_key(KEY_ESC, 0);
    CHECK(!mm_settings_on(), "settings closed");

    /* a game with Ann and Bob; Bob is new */
    for (int i = 0; i < g_nlg; i++) g_lg[i].sel = !strcmp(g_lg[i].name, "Ann") || !strcmp(g_lg[i].name, "Bob");
    logon_start();
    mm_key(KEY_ENTER, 0);
    mm_key(KEY_ENTER, 0);
    CHECK(g_screen == SCR_ROOM, "in the castle");

    /* gear: opens Settings in a room, not while a question is open */
    CHECK(clickable_at(GEAR_X + 5, GEAR_Y + 5) == 700, "gear clickable in a room");
    mm_mouse(GEAR_X + 5, GEAR_Y + 5, MMB_LEFT, MA_DOWN, 0);
    CHECK(mm_settings_on(), "gear opens Settings in the game");
    mm_key(KEY_ESC, 0);
    CHECK(!mm_settings_on() && g_screen == SCR_ROOM, "closed, still in the room");

    Player *p = cur();
    Room *r = cur_room();
    int dir = -1;
    for (int d = 0; d < 4; d++) if (r->doors & (1 << d)) { dir = d; break; }
    door_click(dir);
    CHECK(g_qopen && !gear_ok() && clickable_at(GEAR_X + 5, GEAR_Y + 5) != 700, "no gear while a question is open");
    /* the app to the background (web/Android: MM.suspend -> mm_activate(0)): the clock waits, the music pauses */
    int s0 = g_q.seconds;
    mm_activate(0);
    for (int i = 0; i < 100; i++) { g_now += 100; mm_tick(); }
    CHECK(g_q.seconds == s0 && (!mm_music_on || g_paused[0] || g_paused[1] || !g_track[1]), "suspended: clock stopped at %d (%d)", s0, g_q.seconds);
    mm_activate(1);
    for (int i = 0; i < 25; i++) { g_now += 100; mm_tick(); }
    CHECK(g_q.seconds < s0, "back: the clock runs again (%d -> %d)", s0, g_q.seconds);
    int beeps = g_beeps;
    mm_cmd(CMD_SETTINGS);
    CHECK(!mm_settings_on() && g_beeps == beeps + 1, "menu Settings refused (beep) during a question");

    /* right answer: saved at once, before any quit */
    int right = 0;
    for (int b = 0; b < 4; b++) if (g_q.order[b] == 0) right = b;
    answer(right);
    msg_close(1);
    char want[64];
    snprintf(want, sizeof want, "%d,%d,%s", p->score, p->award, p->name);
    snprintf(v, sizeof v, "MindmazePlayer%d", p->saved_n);
    char got[256] = "";
    plat_get_setting(v, got, sizeof got);
    CHECK(p->saved_n && !strcmp(got, want), "score saved right away (%s = %s, want %s)", v, got, want);

    /* the rank at the stairs */
    g_cur = p->slot;
    p->score = 2500;
    p->maze->cx = p->maze->tx;
    p->maze->cy = p->maze->ty;
    stairs();
    msg_close(1);
    snprintf(want, sizeof want, "2500,%d,%s", p->award, p->name);
    plat_get_setting(v, got, sizeof got);
    CHECK(p->award >= 1 && !strcmp(got, want), "rank saved at the stairs (%s, want %s)", got, want);

    /* the app goes to the background: everyone saved, the game goes on */
    for (int i = 0; i < NPLAYERS; i++) if (g_pl[i]) g_pl[i]->score += 7;
    mm_save_players();
    int ok = 1;
    for (int i = 0; i < NPLAYERS; i++) {
        if (!g_pl[i]) continue;
        snprintf(v, sizeof v, "MindmazePlayer%d", g_pl[i]->saved_n);
        snprintf(want, sizeof want, "%d,%d,%s", g_pl[i]->score, g_pl[i]->award, g_pl[i]->name);
        if (!plat_get_setting(v, got, sizeof got) || strcmp(got, want)) ok = 0;
    }
    CHECK(ok && g_screen == SCR_ROOM && g_pl[0], "mm_save_players saves all and the game goes on");
    printf("saving, settings everywhere, keyboard flag: ok\n");
}


/* Modern options (CHANGE 2026-11-21): views, the touch column, hints, stats, turn prompts, continue a game,
 * animations, the options page, drawing every layout */
static MMCanvas view_canvas(int scale)
{
    MMCanvas c = {0};
    c.scale = scale;
    c.w = mm_view_w() * scale;
    c.h = mm_view_h() * scale;
    c.stride = c.w;
    c.ox = mm_view_ox();
    c.px = calloc((size_t)c.w * c.h, sizeof(uint32_t));
    return c;
}
static void draw_all_views(const char *what)
{
    static const int SCR[3][3] = {{1280, 720, 0}, {844, 390, 1}, {390, 844, 1}};
    for (int v = 0; v < 3; v++) {
        mm_set_screen(SCR[v][0], SCR[v][1], SCR[v][2]);
        for (int s = 1; s <= 2; s++) {
            MMCanvas c = view_canvas(s);
            mm_draw(&c);
            if (v) {                                        /* the column is drawn (panel colour, not black) */
                int cx, cy, z;
                col_place(&cx, &cy, &z);
                uint32_t px = c.px[(size_t)((cy + 2 * z) * s) * c.stride + (size_t)((cx + 150 * z) * s)];
                CHECK(px != 0 || g_title_on, "%s: column drawn in view %d at scale %d", what, v, s);
            }
            free(c.px);
        }
    }
}
static void tap_view(int x, int y) { mm_mouse(x, y, MMB_LEFT, MA_DOWN, 0); mm_mouse(x, y, MMB_LEFT, MA_UP, 0); }
static void tap_col(int x, int y) { int cx, cy, z; col_place(&cx, &cy, &z); tap_view(cx + x * z, cy + y * z); }

static void test_modern(void)
{
    char err[256], v[256];
    g_nset = 0;
    set_seed();
    static const char *const ON[] = {"opt_resume", "opt_hints", "opt_prompts", "opt_anim"};
    for (int i = 0; i < 4; i++) plat_set_setting(ON[i], "1");
    plat_set_setting("opt_layout", "0");
    plat_set_setting("MindmazePlayer1", "100,0,Ann");
    plat_set_setting("MindmazePlayer2", "0,0,Bob");
    for (int i = 0; i < 32; i++) { free(g_uf[i].data); g_uf[i].data = NULL; }
    CHECK(mm_init(err, sizeof err), "init: %s", err);
    CHECK(mm_opt.resume && mm_opt.hints && mm_opt.prompts && mm_opt.layout == MM_LAYOUT_AUTO && !mm_opt.bigtext && mm_opt.anim,
          "defaults: all on, layout auto, large text off");
    mm_set_screen(1280, 720, 0);
    CHECK(mm_view() == MM_VIEW_CLASSIC && mm_view_w() == MM_W + 2 * mm_view_ext() && mm_view_h() == MM_H, "mouse: classic view");
    mm_set_screen(844, 390, 1);
    CHECK(mm_view() == MM_VIEW_TOUCH_WIDE && mm_view_w() == 936 && mm_view_h() == 437 && mm_view_ox() == 0, "touch, landscape: 936x437");
    mm_set_screen(390, 844, 1);
    CHECK(mm_view() == MM_VIEW_TOUCH_WIDE && mm_view_w() == 936 && mm_view_h() == 437, "touch, portrait: the landscape layout (portrait not supported, %d x %d)", mm_view_w(), mm_view_h());
    draw_all_views("logon");

    /* the castle gate in the column (touch): select both, enter */
    mm_set_screen(844, 390, 1);
    int ia = -1, ib = -1;
    for (int i = 0; i < g_nlg; i++) { if (!strcmp(g_lg[i].name, "Ann")) ia = i; if (!strcmp(g_lg[i].name, "Bob")) ib = i; }
    tap_col(20, CLROW_Y + ia * CLROW_H + 5);                    /* 1 player: a tap picks that name only */
    tap_col(20, CLROW_Y + ib * CLROW_H + 5);
    CHECK(!g_lg[ia].sel && g_lg[ib].sel, "1 player: the last tapped name only");
    tap_col(160, 40);                                          /* Several players */
    tap_col(20, CLROW_Y + ia * CLROW_H + 5);
    CHECK(g_lg_mp && g_lg[ia].sel && g_lg[ib].sel, "several players: both chosen");
    draw_all_views("multiplayer gate");
    mm_set_screen(844, 390, 1);
    tap_view(314, 129);                                        /* the castle door starts the game */
    CHECK(g_screen == SCR_GRATING && anim_on(AN_SCREEN), "castle door: in, with the walk-in (screen %d)", g_screen);
    tap_view(300, 150);
    tap_view(60, 150);
    CHECK(g_screen == SCR_ROOM, "into the castle by taps");
    mm_set_screen(844, 390, 1);                                 /* landscape from here */
    Player *a = cur();
    CHECK(a && clickable_at(636 + 150, 55) == 820 && clickable_at(636 + 20, CA_Y + 5) == 0, "column: Topic button, map below");
    tap_col(150, 55);                                          /* Topic: ... opens the picker */
    CHECK(g_col_areas && clickable_at(636 + 20, CA_Y + 3 * (CA_H + 2) + 5) == 813, "area picker open");
    draw_all_views("area picker");
    mm_set_screen(844, 390, 1);
    tap_col(20, CA_Y + 3 * (CA_H + 2) + 5);
    tap_col(84 + 2 * 53 + 10, CL_Y + 10);
    CHECK(a->areabtn == 3 && a->level == 2 && !g_col_areas, "column: area (picker closed) and level (%d %d)", a->areabtn, a->level);
    a->areabtn = 0;
    draw_all_views("room");
    mm_set_screen(844, 390, 1);

    /* question: hint, right answer, stats, turn prompt */
    Room *r = cur_room();
    int dir = -1;
    for (int d = 0; d < 4; d++) if (r->doors & (1 << d)) { dir = d; break; }
    door_click(dir);
    CHECK(g_qopen, "question open");
    CHECK(g_an[AN_POPUP].on && g_an[AN_ANSWERS].on, "the question pops up (animation)");
    draw_all_views("question");
    mm_set_screen(844, 390, 1);
    int full = q_points();
    tap_col(20, CB_Y + 5);                                     /* 50/50 */
    int off = 0;
    for (int b = 0; b < 4; b++) off += g_q.disabled[b] && g_q.order[b] != 0;
    CHECK(g_q.hint && off == 2 && q_points() == full / 2, "50/50: two wrong answers gone, half the points (%d of %d)", q_points(), full);
    int right = 0;
    for (int b = 0; b < 4; b++) if (g_q.order[b] == 0) right = b;
    int before = a->score, pts = q_points();
    tap_col(100, CANS_Y + right * (CANS_H + 4) + 10);
    CHECK(g_nmsg == 1 && mm_animating(), "right answer: message, feedback animation");
    draw_all_views("feedback");
    mm_set_screen(844, 390, 1);
    msg_close(1);
    CHECK(a->score == before + pts, "points added (%d)", a->score);
    CHECK(g_nmsg == 1 && strstr(g_msg[0].text, "your turn") && strstr(g_msg[0].text, cur()->name), "turn prompt: %s",
          g_nmsg ? g_msg[0].text : "-");
    msg_close(1);
    int si = uf_find("stats.txt", 0);
    CHECK(si >= 0 && strstr(g_uf[si].data, "Ann\t1 1 0 0 1 1 0"), "stats recorded: %s", si >= 0 ? g_uf[si].data : "-");

    /* the hint after a wrong try: one more answer goes, and only one chance is left (user 2026-11-21) */
    r = cur_room();
    for (int d = 0; d < 4; d++) if ((r->doors & (1 << d)) && !(r->opened & (1 << d))) dir = d;
    door_click(dir);
    int w1 = -1, okb = -1;
    for (int b = 0; b < 4; b++) { if (g_q.order[b] == 0) okb = b; else if (w1 < 0) w1 = b; }
    answer(w1);
    msg_close(1);
    hint_5050();
    int open = 0;
    for (int b = 0; b < 4; b++) open += !g_q.disabled[b];
    CHECK(g_qopen && g_q.hint && open == 2 && !g_q.disabled[okb], "hint after a wrong try removes one (%d open)", open);
    int w2 = -1;
    for (int b = 0; b < 4; b++) if (!g_q.disabled[b] && b != okb) w2 = b;
    answer(w2);
    CHECK(g_nmsg == 1 && strstr(g_msg[0].text, "correct answer is"), "after the hint a wrong answer ends the question: %s",
          g_nmsg ? g_msg[0].text : "-");
    while (g_nmsg) msg_close(1);

    /* continue the game after a restart */
    int sg = uf_find("savegame.txt", 0);
    CHECK(sg >= 0 && g_uf[sg].len > 100, "game saved after the turn");
    int cx0 = a->maze->cx, cy0 = a->maze->cy, sc0 = a->score, cur0 = g_cur;
    show_logon();                                              /* (a restart shows the gate) */
    CHECK(g_nmsg == 0 && strstr(g_resume_names, "Ann") && strstr(g_resume_names, "Bob") &&
          clickable_at(636 + 150, CLROW_Y + 7 * CLROW_H + 20) == 979, "touch: the gate offers Continue (%s)", g_resume_names);
    tap_col(150, CLROW_Y + 7 * CLROW_H + 20);
    Player *a2 = NULL;
    for (int i = 0; i < NPLAYERS; i++) if (g_pl[i] && !strcmp(g_pl[i]->name, "Ann")) a2 = g_pl[i];
    CHECK(g_screen == SCR_ROOM && a2 && a2->maze->cx == cx0 && a2->maze->cy == cy0 && a2->score == sc0 && g_cur == cur0,
          "game continued in the same room with the same score");
    CHECK(a2 && room_at(a2->maze, cx0, cy0)->opened, "the opened door is still open");
    tap_col(20, CB_Y + 5);                                     /* Main Menu: back to the gate, the game kept */
    CHECK(g_screen == SCR_LOGON && g_nmsg == 0 && g_uf[sg].len > 100 && g_resume_names[0], "Main Menu: castle gate, game kept");
    tap_col(150, CLROW_Y + 7 * CLROW_H + 20);
    CHECK(g_screen == SCR_ROOM, "continued after Main Menu");
    mm_set_screen(1280, 720, 0);                               /* classic: the question at the start */
    show_logon();
    CHECK(g_nmsg == 1 && g_msg[0].yesno && strstr(g_msg[0].text, "Ann"), "classic: continue offered at the start");
    msg_close(1);
    CHECK(g_screen == SCR_ROOM, "classic: continued");
    mm_set_screen(844, 390, 1);
    mm_cmd(CMD_NEW_GAME);
    CHECK(g_screen == SCR_LOGON && g_nmsg == 0 && g_uf[sg].len == 0, "New Game drops the saved game, no offer");

    /* animations: on -> something moves after a step; off -> never */
    for (int i = 0; i < g_nlg; i++) g_lg[i].sel = !strcmp(g_lg[i].name, "Ann");
    logon_start();
    enter_castle();
    g_now += 5000;
    mm_tick();
    r = cur_room();
    for (int d = 0; d < 4; d++) if (r->doors & (1 << d)) dir = d;
    r->opened |= 1 << dir;
    door_click(dir);
    CHECK(mm_animating(), "animation after walking through a door");
    g_now += 2000;
    mm_tick();
    while (g_nmsg) msg_close(1);
    g_now += 3000;
    mm_tick();
    CHECK(!mm_animating(), "animations end");

    /* Settings > Modern options > Classic MindMaze: all off, classic view even on a touch screen */
    settings_now();
    mm_settings_mouse(343 + 5, 22 + 392 - 25, MMB_LEFT, MA_DOWN);   /* "Modern options..." */
    draw_all_views("options page");
    mm_set_screen(844, 390, 1);
    MMOptions opt_was = mm_opt;
    mm_settings_mouse(78 + 30, 22 + 392 - 25, MMB_LEFT, MA_DOWN);   /* "Classic MindMaze (all off)" */
    CHECK(!mm_opt.resume && !mm_opt.hints && !mm_opt.prompts && !mm_opt.anim && !mm_opt.bigtext && mm_opt.layout == MM_LAYOUT_CLASSIC_ONLY
          && mm_view() == MM_VIEW_CLASSIC, "Classic MindMaze: everything off, classic view");
    plat_get_setting("opt_layout", v, sizeof v);
    CHECK(!strcmp(v, "1"), "options saved (%s)", v);
    mm_settings_mouse(78 + 30, 22 + 392 - 25, MMB_LEFT, MA_DOWN);   /* the switch again: the choices come back */
    CHECK(!memcmp(&mm_opt, &opt_was, sizeof opt_was) && !mm_options_classic_on(), "Classic MindMaze off: the modern options as they were");
    mm_settings_mouse(78 + 30, 22 + 392 - 25, MMB_LEFT, MA_DOWN);   /* (and on again for the rest) */
    CHECK(mm_options_classic_on(), "Classic MindMaze on again");
    mm_key(KEY_ESC, 0);
    r = cur_room();
    for (int d = 0; d < 4; d++) if (r->doors & (1 << d)) dir = d;
    r->opened |= 1 << dir;
    door_click(dir);
    CHECK(!mm_animating(), "animations off: nothing moves");
    mm_opt.bigtext = 1;
    mm_opt.layout = MM_LAYOUT_AUTO;
    draw_all_views("large text");
    mm_opt.bigtext = 0;
    mm_set_screen(1280, 720, 0);
    printf("modern options: ok\n");
}


/* Overlay check of the tap areas (2026-11-21, user: "some are offset on purpose but some look like accidents"): every
 * object that can be tapped - furniture with a click rect, characters, doors - drawn on grey with
 * red = the tap area the game uses (hot_object: the original tables), green = the box of the picture's visible
 * pixels, yellow = where the glint runs (glint_area). Sheet: <data>/../build/overlay/sheet.png (3 per row, 636 x 301
 * each), report lines "overlay ..." with how much of the tap area is picture and how much of the picture is
 * tappable. Real data only (make test-data). */
static void test_overlay(void)
{
    char err[256];
    set_seed();
    if (!mm_init(err, sizeof err)) return;
    struct { int group, n, mode; const char *kind; } G[3] = {{MM_G_FURNITURE, MM_N_FURNITURE, 0, "furniture"},
                                                              {MM_G_CHARACTERS, MM_N_CHARACTERS, 1, "character"},
                                                              {MM_G_DOORS, 5, 1, "door"}};
    int total = 0;
    for (int g = 0; g < 3; g++) total += G[g].n;
    int cols = 3, rows = (total + cols - 1) / cols, W = 636, H = 301;
    MMCanvas c = {0};
    c.scale = 1;
    c.w = W;
    c.h = H;
    c.stride = W;
    c.px = calloc((size_t)W * H, sizeof(uint32_t));
    AImg *sheet = aimg_new(W * cols, H * rows, 3);
    int k = 0;
    for (int g = 0; g < 3; g++)
        for (int i = 0; i < G[g].n; i++, k++) {
            const MMObject *o = obj(G[g].group, i);
            mm_fill(&c, 0, 0, W, H, 0x505050);
            mm_draw_img(&c, o->name, o->x, o->y);
            g_nhot = 0;
            hot_object(o, G[g].mode);
            MMImage *im = mm_image(o->name);
            int bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1, opq = 0, inhot = 0, hotpx = 0, hotopq = 0;
            int w = im ? im->w : 0;                              /* (the 2006 pictures are drawn whole, half or not) */
            for (int y = 0; im && y < im->h; y++)
                for (int x = 0; x < w; x++)
                    if ((im->px[y * im->w + x] >> 24) > 128) {
                        int X = o->x + x, Y = o->y + y;
                        opq++;
                        if (X < bx0) bx0 = X;
                        if (X > bx1) bx1 = X;
                        if (Y < by0) by0 = Y;
                        if (Y > by1) by1 = Y;
                        if (g_nhot && inside(X, Y, g_hot[0].x, g_hot[0].y, g_hot[0].w, g_hot[0].h)) inhot++;
                    }
            if (g_nhot) {
                Hot *hh = &g_hot[0];
                for (int y = hh->y; y < hh->y + hh->h; y++)
                    for (int x = hh->x; x < hh->x + hh->w; x++) {
                        hotpx++;
                        int lx = x - o->x, ly = y - o->y;
                        if (im && lx >= 0 && ly >= 0 && lx < w && ly < im->h && (im->px[ly * im->w + lx] >> 24) > 128) hotopq++;
                    }
                obj_glint(&c, &g_hot[0], 0.5);                     /* the glint, halfway */
                mm_frame(&c, hh->x, hh->y, hh->w, hh->h, 2, 0xFF2020);
            }
            if (bx1 >= 0) mm_frame(&c, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1, 1, 0x20FF20);
            if (g_nhot) {
                Hot *hh = &g_hot[0];
                int ol = bx0 - hh->x, ot = by0 - hh->y, orr = hh->x + hh->w - 1 - bx1, ob = hh->y + hh->h - 1 - by1;
                printf("overlay %-9s %2d %-13s pic %3d,%3d %3dx%3d  tap %3d,%3d %3dx%3d  past the picture: left %3d top %3d right %3d"
                       " bottom %3d  tap on picture %3d%%  picture tappable %3d%%\n", G[g].kind, i, o->name, bx0, by0, bx1 - bx0 + 1,
                       by1 - by0 + 1, hh->x, hh->y, hh->w, hh->h, ol > 0 ? ol : 0, ot > 0 ? ot : 0, orr > 0 ? orr : 0, ob > 0 ? ob : 0,
                       hotpx ? hotopq * 100 / hotpx : 0, opq ? inhot * 100 / opq : 0);
            }
            int sx = (k % cols) * W, sy = (k / cols) * H;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    uint32_t v = c.px[y * W + x];
                    unsigned char *d = sheet->p + ((size_t)(sy + y) * sheet->w + sx + x) * 3;
                    d[0] = v >> 16 & 255; d[1] = v >> 8 & 255; d[2] = v & 255;
                }
        }
    aimg_save_png("../build/overlay/sheet.png", sheet);
    aimg_free(sheet);
    free(c.px);
    printf("overlay: build/overlay/sheet.png (%d objects)\n", total);
}


/* pop-up pages (CHANGE 2026-11-21, user: the typewriter cut off the end; Next / Previous for a larger font; the size is
 * fixed before typing): every speech and question, in each layout, shown page by page has every letter on some page;
 * the typewriter's partial pages keep the final layout; Previous / Next and taps turn the pages. */
static void canvas_png(const MMCanvas *c, const char *path)     /* (MM_SHOT=<folder>: pictures to look at by eye) */
{
    AImg *a = aimg_new(c->w, c->h, 3);
    for (int i = 0; i < c->w * c->h; i++) {
        uint32_t v = c->px[(i / c->w) * c->stride + i % c->w];
        a->p[i * 3] = v >> 16 & 255; a->p[i * 3 + 1] = v >> 8 & 255; a->p[i * 3 + 2] = v & 255;
    }
    aimg_save_png(path, a);
    aimg_free(a);
}
static void shot(const MMCanvas *c, const char *name)
{
    char p[512];
    if (!getenv("MM_SHOT")) return;
    snprintf(p, sizeof p, "%s/%s.png", getenv("MM_SHOT"), name);
    canvas_png(c, p);
    printf("shot: %s\n", p);
}

/* the mute button (CHANGE 2026-11-21, user): top left, every screen, both layouts; saved; volumes kept */
static void test_mute(void)
{
    char err[256], v[16];
    set_seed();
    if (!mm_init(err, sizeof err)) return;
    mm_set_volume(70, 40);
    for (int touch = 0; touch < 2; touch++) {
        mm_set_screen(touch ? 844 : 1280, touch ? 390 : 720, touch);
        show_logon();
        CHECK(!g_mute && clickable_at(MUTE_X + 5, MUTE_Y + 5) != 0, "mute (%s): clickable on the gate", touch ? "touch" : "classic");
        tap_view(MUTE_X + 5, MUTE_Y + 5);
        CHECK(g_mute && plat_get_setting("Muted", v, sizeof v) && !strcmp(v, "1") && mm_music_volume == 70 && mm_sound_volume == 40,
              "mute: on, saved, volume levels kept");
        g_lg[0].sel = 1;
        logon_start();
        for (int i = 0; i < 4 && g_screen != SCR_ROOM; i++) { if (g_nmsg) msg_close(1); else mm_key(KEY_ENTER, 0); }
        int scr = g_screen, q = g_qopen, sp = g_speech;
        tap_view(MUTE_X + 5, MUTE_Y + 5);
        CHECK(!g_mute && g_screen == scr && g_qopen == q && g_speech == sp, "mute: off again in the room, nothing else tapped");
        MMCanvas c = view_canvas(1);
        mm_draw(&c);
        free(c.px);
        mm_cmd(CMD_QUIT);
        if (g_nmsg) msg_close(1);
    }
    set_seed();
    printf("mute: ok\n");
}

/* glint coverage (CHANGE 2026-11-21, user: "the soldier ... does half the object"): in the rooms of 8 mazes, every
 * tappable object's glint is swept from start to end; it must light its whole visible picture (the pixels not covered
 * by an object drawn in front). Real data only. */
static void test_glint_cover(void)
{
    char err[256];
    set_seed();
    if (!mm_init(err, sizeof err)) return;
    int objs = 0, low = 0;
    MMCanvas c = {0};
    c.scale = 1; c.w = MM_W; c.h = MM_H; c.stride = MM_W;
    c.px = calloc((size_t)MM_W * MM_H, sizeof(uint32_t));
    for (unsigned seed = 1; seed <= 8; seed++) {
        mm_srand(seed);
        Maze *m = maze_new();
        for (int i = 0; i < 100; i++) {
            Room *r = m->grid[i];
            if (!r || (r->doors & 0x10)) continue;
            room_hots(r);
            for (int h = 0; h < g_nhot; h++) {
                const MMObject *o = g_hot[h].o;
                MMImage *im = o ? mm_image(o->name) : NULL;
                if (!im) continue;
                memset(c.px, 0, (size_t)MM_W * MM_H * sizeof(uint32_t));
                for (double t = 0.02; t < 1; t += 0.02) obj_glint(&c, &g_hot[h], t);   /* (band 32 px, steps < 16 px) */
                long all = 0, lit = 0;
                Front fr[160];
                int nf = fronts_of(o, fr);
                for (int y = 0; y < im->h; y++)
                    for (int x = 0; x < im->w; x++) {
                        int X = o->x + x, Y = o->y + y;
                        if ((im->px[y * im->w + x] >> 24) <= 128 || X < 0 || Y < 0 || X >= MM_W || Y >= MM_H || covered_by(fr, nf, X, Y)) continue;
                        all++;
                        lit += c.px[Y * MM_W + X] != 0;
                    }
                objs++;
                if (all && lit * 100 / all < 90 && low++ < 5) printf("  glint lights %ld of %ld pixels of %s\n", lit, all, o->name);
            }
        }
        maze_free(m);
    }
    free(c.px);
    CHECK(low == 0, "glint: every tappable object lit over its whole visible picture (%d of %d below 90%%)", low, objs);
    printf("glint: %d objects in rooms, %d lit less than 90%%\n", objs, low);
}
static int letters(const char *t)
{
    char p[4096];
    mm_plain(t, p, sizeof p);
    int n = 0;
    for (const char *s = p; *s; s++) n += (*s & 0xC0) != 0x80 && *s != ' ' && *s != '\n';
    return n;
}
extern int mm_rich_overflow;
static void test_pages(void)
{
    char err[256];
    set_seed();
    if (!mm_init(err, sizeof err)) return;
    static const int SCR[3][3] = {{1280, 720, 0}, {1280, 720, 0}, {844, 390, 1}};
    int bad = 0, maxpages = 0, texts = 0, paged = 0;
    for (int v = 0; v < 3; v++) {
        mm_opt.bigtext = v == 1;
        mm_set_screen(SCR[v][0], SCR[v][1], SCR[v][2]);
        MMCanvas c = view_canvas(2);
        for (int mode = 0; mode < 2; mode++)
            for (int k = 0; k < (mode ? 10 * 4 : MM_N_SPEECH); k++) {
                int nt = mode ? mm_question_count(k / 4, k % 4) : mm_speech_count[k];
                if (mode && nt > 60) nt = 60;
                for (int i = 0; i < nt; i++) {
                    const char *t = mode ? mm_question_at(k / 4, k % 4, i)->text : mm_speech[k][i];
                    char buf[2048];
                    if (!mode) {                            /* (as show_speech: a literal \n is a line break) */
                        char *o = buf;
                        for (; *t && o < buf + sizeof buf - 1; t++) { if (t[0] == '\\' && t[1] == 'n') { *o++ = '\n'; t++; } else *o++ = *t; }
                        *o = 0;
                        t = buf;
                    }
                    ov_reset();
                    int sum = 0;
                    draw_overlay(&c, mode, t);
                    int pages = g_ov_pages;
                    int ov0 = mm_rich_overflow;
                    for (g_ov_page = 0; g_ov_page < pages; g_ov_page++) sum += draw_overlay(&c, mode, t);
                    if (mm_rich_overflow > ov0) printf("  past the edge (view %d mode %d): %.120s\n", v, mode, t);
                    texts++;
                    if (pages > 1) paged++;
                    if (pages > maxpages) maxpages = pages;
                    if (sum != letters(t) && bad++ < 3) printf("  letters lost (view %d mode %d: %d of %d): %.60s\n", v, mode, sum, letters(t), t);
                }
            }
        free(c.px);
    }
    mm_opt.bigtext = 0;
    CHECK(!mm_rich_overflow, "pop-ups: %d words past the right edge of the box", mm_rich_overflow);
    CHECK(!bad, "pop-ups: every letter of %d texts on a page (%d paged, at most %d pages)", texts, paged, maxpages);
    printf("pop-ups: %d texts, %d on more than one page, at most %d pages\n", texts, paged, maxpages);

    /* a long speech in the touch layout: pages, lines fading in page by page, Next / Previous / Close / tap */
    mm_set_screen(844, 390, 1);
    plat_set_setting("MindmazePlayer1", "0,0,Ann");
    show_logon();
    g_lg[0].sel = 1;
    logon_start();
    for (int i = 0; i < 4 && g_screen != SCR_ROOM; i++) { if (g_nmsg) msg_close(1); else mm_key(KEY_ENTER, 0); }
    CHECK(g_screen == SCR_ROOM, "pages: in a room (screen %d, title %d, msg %d, pl %p, nlg %d)", g_screen, g_title_on, g_nmsg, (void *)g_pl[0], g_nlg);
    mm_opt.anim = 1;
    char *o = g_speech_text;
    for (int i = 0; i < 40; i++) o += sprintf(o, "Word%d is here. ", i);
    g_speech = 1;
    g_qopen = 0;
    ov_reset();
    type_start(1);
    MMCanvas c1 = view_canvas(1);
    mm_draw(&c1);
    g_type_done = 1;
    MMCanvas c2 = view_canvas(2);
    g_an[AN_SHIMMER].on = 1; g_an[AN_SHIMMER].t0 = plat_ticks() - 900; g_an[AN_SHIMMER].dur = 6000;
    mm_draw(&c2);
    shot(&c2, "pages-touch");
    g_an[AN_SHIMMER].on = 0;
    g_speech = 0;
    mm_draw(&c2);
    g_an[AN_SHIMMER].on = 1; g_an[AN_SHIMMER].t0 = plat_ticks() - 1200;
    mm_draw(&c2);
    shot(&c2, "room-glint");
    if (getenv("MM_SHOT")) {                                /* rooms with their tap areas (red) and the glints halfway */
        Maze *m = cur()->maze;
        int ox = m->cx, oy = m->cy, k = 0;
        for (int i = 0; i < 100 && k < 8; i++) {
            Room *r = m->grid[i];
            if (!r || (r->doors & 0x10)) continue;
            m->cx = i % 10; m->cy = i / 10;
            g_an[AN_SHIMMER].on = 1; g_an[AN_SHIMMER].t0 = plat_ticks() - 1500;
            mm_draw(&c2);
            for (int h = 0; h < g_nhot; h++) mm_frame(&c2, g_hot[h].x, g_hot[h].y, g_hot[h].w, g_hot[h].h, 1, 0xFF2020);
            char nm[32];
            snprintf(nm, sizeof nm, "room-%d", k++);
            shot(&c2, nm);
        }
        m->cx = ox; m->cy = oy;
        g_an[AN_SHIMMER].on = 0;
    }
    g_an[AN_SHIMMER].on = 0;
    free(c2.px);
    g_speech = 1;
    type_start(1);
    mm_draw(&c1);
    CHECK(g_ov_pages > 1 && g_ov_btn_on, "long speech: %d pages with Previous / Next", g_ov_pages);
    CHECK(typing() && g_ov_lines > 1 && g_ov_lines < 64, "the first page's lines fade in (%d lines)", g_ov_lines);
    {                                                       /* line by line, top first */
        uint32_t t0 = g_now;
        g_now = g_type_t0 + LINE_DELAY + LINE_FADE;
        CHECK(line_alpha(0) == 1 && line_alpha(1) < 1 && line_alpha(g_ov_lines - 1) == 0, "the top line first, the others after it");
        g_now += 60000;                                     /* fading ends between frames: one more frame is asked for */
        g_invalidated = 0;
        int anim = mm_animating();
        CHECK(!anim && g_invalidated > 0, "lines done: a last redraw with the whole page");
        g_now = t0;
        type_start(1);
        mm_draw(&c1);
    }
    tap_view(g_ov_btn[1][0] + 5, g_ov_btn[1][1] + 5);      /* Next while lines appear: all of them, same page */
    CHECK(!typing() && g_ov_page == 0 && g_speech, "Next while the lines appear: the page shown whole, no page turn");
    tap_view(g_ov_btn[1][0] + 5, g_ov_btn[1][1] + 5);      /* then Next turns the page */
    CHECK(g_ov_page == 1 && typing(), "Next: the next page, its lines fade in");
    tap_view(300, 100);                                     /* a tap elsewhere: all of the page */
    CHECK(!typing() && g_ov_page == 1, "tap: the page shown whole");
    mm_draw(&c1);
    tap_view(g_ov_btn[0][0] + 5, g_ov_btn[0][1] + 5);      /* Previous */
    CHECK(g_ov_page == 0 && g_speech, "Previous: back a page");
    g_type_done = 1;
    for (int i = 0; i < 20 && g_speech; i++) mm_key(KEY_ENTER, 0);
    CHECK(!g_speech, "Enter: through the pages, then the speech closes");
    g_speech = 1; g_qopen = 0; ov_reset(); g_type_done = 1;  /* Close on the last page (user: Next turns into Close) */
    mm_draw(&c1);
    g_ov_page = g_ov_pages - 1;
    mm_draw(&c1);
    tap_view(g_ov_btn[1][0] + 5, g_ov_btn[1][1] + 5);
    CHECK(!g_speech, "Close on the last page closes the speech");
    snprintf(g_speech_text, sizeof g_speech_text, "Short words.");   /* a one-page speech: Close too (user) */
    g_speech = 1; ov_reset(); type_start(1);
    mm_draw(&c1);
    CHECK(g_ov_pages == 1 && g_ov_btn_on, "one-page speech: buttons shown");
    tap_view(g_ov_btn[1][0] + 5, g_ov_btn[1][1] + 5);      /* (lines still appearing: all of them first) */
    CHECK(g_speech && !typing(), "one-page speech: Close while the lines appear shows them first");
    tap_view(g_ov_btn[1][0] + 5, g_ov_btn[1][1] + 5);
    CHECK(!g_speech, "one-page speech: Close closes it");
    free(c1.px);
    set_seed();
    printf("pop-up pages: ok\n");
}

/* room overlap check (CHANGE 2026-11-21, user: paintings must not overlap other objects; tap areas fitted to their
 * picture where nothing overlaps): over the rooms of many mazes, the visible pixels of each drawn object (doors,
 * furniture, the character) are compared. Real data only. */
static int vis(const MMObject *o, int X, int Y)                /* the object's picture is solid at screen X, Y */
{
    MMImage *im = mm_image(o->name);
    int x = X - o->x, y = Y - o->y;
    return im && x >= 0 && y >= 0 && x < im->w && y < im->h && (im->px[y * im->w + x] >> 24) > 128;
}
static void test_room_overlap(void)
{
    char err[256];
    set_seed();
    if (!mm_init(err, sizeof err)) return;
    static int pair[256][256];
    memset(pair, 0, sizeof pair);
    static int other[256][256];
    memset(other, 0, sizeof other);
    int rooms = 0, painted = 0, bad_rooms = 0, taps = 0, full = 0;
    double cover = 0;
    for (unsigned seed = 1; seed <= 300; seed++) {
        mm_srand(seed);
        Maze *m = maze_new();
        for (int i = 0; i < 100; i++) {
            Room *r = m->grid[i];
            if (!r || (r->doors & 0x10)) continue;
            rooms++;
            const MMObject *L[80];
            int n = 0;
            for (int d = 0; d < 5; d++) if (r->doors & (1 << d)) L[n++] = obj(MM_G_DOORS, d);
            for (int f = 0; f < MM_N_FURNITURE; f++) if ((r->furnA | r->furnB) >> f & 1) L[n++] = obj(MM_G_FURNITURE, f);
            if (r->chr != NONE) L[n++] = obj(MM_G_CHARACTERS, r->chr);
            int any = 0, bad = 0;
            for (int a = 0; a < n; a++) {
                if (strncmp(L[a]->name, "PIC", 3)) continue;
                any = 1;
                MMImage *im = mm_image(L[a]->name);
                for (int b = 0; b < n && im; b++) {
                    if (b == a) continue;
                    int px = 0;
                    for (int y = 0; y < im->h; y++)
                        for (int x = 0; x < im->w; x++)
                            if (vis(L[a], L[a]->x + x, L[a]->y + y) && vis(L[b], L[a]->x + x, L[a]->y + y)) px++;
                    if (px) { pair[L[a] - mm_objects][L[b] - mm_objects]++; bad = 1; }
                }
            }
            painted += any;
            bad_rooms += bad;
            for (int a = 0; a < n; a++)                     /* (any two objects: what else overlaps) */
                for (int b = a + 1; b < n; b++)
                    if (!is_painting(L[a]) && !is_painting(L[b]) && pix_overlap(L[a], L[b])) other[L[a] - mm_objects][L[b] - mm_objects]++;
            room_hots(r);                                   /* tap areas: how much of each picture they hold */
            for (int h = 0; h < g_nhot; h++) {
                const MMObject *o = g_hot[h].o;
                MMImage *im = o ? mm_image(o->name) : NULL;
                if (!im) continue;
                long all = 0, in = 0;
                for (int y = 0; y < im->h; y++)
                    for (int x = 0; x < im->w; x++)
                        if ((im->px[y * im->w + x] >> 24) > 128) { all++; in += inside(o->x + x, o->y + y, g_hot[h].x, g_hot[h].y, g_hot[h].w, g_hot[h].h); }
                taps++;
                full += in == all;
                cover += all ? (double)in / all : 1;
            }
        }
        maze_free(m);
    }
    printf("paintings: %d rooms, %d with a painting, %d where a painting overlaps another object\n", rooms, painted, bad_rooms);
    for (int a = 0; a < MM_N_OBJECTS; a++)
        for (int b = 0; b < MM_N_OBJECTS; b++)
            if (pair[a][b]) printf("  painting %-12s over %-12s in %d rooms\n", mm_objects[a].name, mm_objects[b].name, pair[a][b]);
    CHECK(bad_rooms == 0, "no painting overlaps another object (%d rooms)", bad_rooms);
    for (int a = 0; a < MM_N_OBJECTS; a++)
        for (int b = 0; b < MM_N_OBJECTS; b++)
            if (other[a][b]) printf("  overlap (not a painting) %-12s %-12s in %d rooms\n", mm_objects[a].name, mm_objects[b].name, other[a][b]);
    Room t = {0};                                           /* a saved room from before: QUEEN with PIC1-1 over her */
    int q = -1, p1 = -1, p2 = -1;
    for (int i = 0; i < MM_N_CHARACTERS; i++) if (!strcmp(obj(MM_G_CHARACTERS, i)->name, "QUEEN.DIB")) q = i;
    for (int f = 0; f < MM_N_FURNITURE; f++) { if (!strcmp(obj(MM_G_FURNITURE, f)->name, "PIC1-1.DIB")) p1 = f; if (!strcmp(obj(MM_G_FURNITURE, f)->name, "PIC2-1.DIB")) p2 = f; }
    t.chr = q; t.floor = NONE; t.furnA = 1ull << p1;
    int clash = q >= 0 && p1 >= 0 && painting_clash(&t, obj(MM_G_FURNITURE, p1));
    room_fix_paintings(&t);
    CHECK(!clash || (t.furnA == (p2 >= 0 && !painting_clash(&t, obj(MM_G_FURNITURE, p2)) ? 1ull << p2 : 0)), "saved room: the painting moved to the other wall (%llx)", t.furnA);
    printf("tap areas: %d tappable objects in those rooms, %.0f%% hold their whole picture, on average %.0f%% of it\n", taps,
           100.0 * full / (taps ? taps : 1), 100.0 * cover / (taps ? taps : 1));
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);                       /* nothing lost if a check crashes */
    g_data = argc > 1 ? argv[1] : "build/fixtures";
    if (getenv("MM_FONTS")) g_real_fonts = text_init(getenv("MM_FONTS"));
    g_2006 = argc > 2 && !strcmp(argv[2], "--original");
    g_original = g_2006 || (argc > 2 && !strcmp(argv[2], "--edition"));   /* Encarta 95-98 disc */
    test_tables();
    if (!mm_objects) { printf("no tables: stopped\n"); return 1; }
    test_mazes();
    test_questions();
    test_game();
    test_flows();
    test_sources();
    test_awards();
    test_text();
    test_settings_pictures();
    test_title();
    test_castle_music();
    test_save_settings();
    test_modern();
    test_pages();
    test_mute();
    if (g_2006) test_overlay();                              /* (real data: the tap areas over the pictures) */
    if (g_2006) test_room_overlap();
    if (g_2006) test_glint_cover();
    if (!g_original) test_art();                             /* writes into the data folder: fixtures only */
    if (!g_original) test_add_disc();                        /* the same */
    test_draw_speed();
    printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
    return g_fail != 0;
}
