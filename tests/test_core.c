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
#include "../src/core/mm_game.c"

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
void plat_invalidate(void) {}
void plat_view_changed(void) {}
/* user files kept in memory (seen list, custom / online questions) */
static struct { char name[64]; char *data; int len; } g_uf[8];
static int uf_find(const char *name, int make)
{
    for (int i = 0; i < 8; i++) if (g_uf[i].data && !strcmp(g_uf[i].name, name)) return i;
    if (!make) return -1;
    for (int i = 0; i < 8; i++) if (!g_uf[i].data) { snprintf(g_uf[i].name, 64, "%s", name); g_uf[i].data = calloc(1, 1); g_uf[i].len = 0; return i; }
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
    free(g_uf[i].data);
    g_uf[i].data = malloc((size_t)len + 1);
    memcpy(g_uf[i].data, data, (size_t)len);
    g_uf[i].len = len;
    return 1;
}
void plat_append_user(const char *name, const char *text)
{
    int i = uf_find(name, 1), n = (int)strlen(text);
    g_uf[i].data = realloc(g_uf[i].data, (size_t)(g_uf[i].len + n + 1));
    memcpy(g_uf[i].data + g_uf[i].len, text, (size_t)n);
    g_uf[i].len += n;
}
void plat_http_get(const char *url, int tag) { (void)url; (void)tag; }
void plat_quit(void) {}
MMImage *plat_load_png(const char *rel) { return NULL; }
int plat_text_px_width(MMCanvas *c, const char *s, int f) { return (int)strlen(s) * 6 * c->scale; }
int plat_text_px_height(MMCanvas *c, int f) { return 13 * c->scale; }
int plat_text_px_ascent(MMCanvas *c, int f) { return 11 * c->scale; }
void plat_text_px(MMCanvas *c, int X, int Y, const char *s, int f, uint32_t rgb, int a, int b, int d, int e) {}


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
            const char *amp = strchr(t[k], '&');
            if (amp && strchr(amp, ';') && strchr(amp, ';') - amp < 9 && amp[1] != ' ') { ents++; if (ents < 4) printf("  entity left: %s\n", t[k]); }
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

static void set_seed(void) { plat_set_setting("seed", "99"); plat_set_setting("questions", "original"); plat_set_setting("title_seconds", "0"); }

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

int main(int argc, char **argv)
{
    g_data = argc > 1 ? argv[1] : "build/fixtures";
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
    if (!g_original) test_art();                             /* writes into the data folder: fixtures only */
    if (!g_original) test_add_disc();                        /* the same */
    test_draw_speed();
    printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
    return g_fail != 0;
}
