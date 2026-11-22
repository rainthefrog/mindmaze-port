/* Tests for src/core/mm_stats.c (player stats, CHANGE 2026-11-21): recording, streaks, per-area / per-level counts,
 * the stats.txt round trip, damaged lines, Tourist, forget, case-insensitive names, and the stats screen (drawing into
 * a real canvas, hit ids, list -> detail -> back, reset confirmation, Esc, scrolling). Links mm_stats.c + mm_util.c
 * only, with its own plat_* stubs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs ---- */
MMOptions mm_opt = {1, 1, 1, MM_LAYOUT_AUTO, 0, 1};
static char *g_file;                     /* the one user file the stats use */
static int g_file_len, g_saves, g_texts;
void *plat_load_user(const char *name, int *size)
{
    if (strcmp(name, "stats.txt") || !g_file) return NULL;
    char *d = malloc((size_t)g_file_len + 1);
    memcpy(d, g_file, (size_t)g_file_len);
    d[g_file_len] = 0;
    if (size) *size = g_file_len;
    return d;
}
int plat_save_user(const char *name, const void *data, int len)
{
    if (strcmp(name, "stats.txt")) return 0;
    free(g_file);
    g_file = malloc((size_t)len + 1);
    memcpy(g_file, data, (size_t)len);
    g_file[len] = 0;
    g_file_len = len;
    g_saves++;
    return 1;
}
static char *dup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static void set_file(const char *s) { plat_save_user("stats.txt", s, (int)strlen(s)); }
void plat_text(MMCanvas *c, int x, int y, int w, int h, const char *s, int f, uint32_t rgb, int fl) { g_texts++; }
int plat_text_width(const char *s, int f) { return (int)strlen(s) * 6; }
void plat_invalidate(void) {}
int mm_view_ox(void) { return 0; }                    /* (mm_util.c asks it for the widescreen pictures) */
void *plat_load(const char *rel, int *size) { return NULL; }
MMImage *plat_load_png(const char *rel) { return NULL; }
static char g_set[8][2][128];
int plat_get_setting(const char *k, char *buf, int n)
{
    for (int i = 0; i < 8; i++) if (!strcmp(g_set[i][0], k)) { snprintf(buf, (size_t)n, "%s", g_set[i][1]); return 1; }
    return 0;
}
void plat_set_setting(const char *k, const char *v)
{
    for (int i = 0; i < 8; i++)
        if (!g_set[i][0][0] || !strcmp(g_set[i][0], k)) { snprintf(g_set[i][0], 128, "%s", k); snprintf(g_set[i][1], 128, "%s", v ? v : ""); return; }
}
const char *mm_str(int id)
{
    static const char *const AREA[10] = {"All Areas", "Physical Science & Technology", "Life Science", "Geography", "History",
                                         "Social Science", "Religion and Philosophy", "Art, Language, and Literature",
                                         "Performing Arts", "Sports, Hobbies, and Pets"};
    if (id >= 29014 && id <= 29023) return AREA[id - 29014];
    if (id == 29043) return "Tourist";
    return "";
}

/* ---- helpers: read a player's numbers back from the saved text ---- */
typedef struct { int t[7], aa[9], ao[9], la[4]; } Rec;
static int rec_of(const char *name, Rec *r)
{
    if (!g_file) return 0;
    for (char *s = g_file; *s; ) {
        char *nl = strchr(s, '\n');
        size_t n = strlen(name);
        if (!strncmp(s, name, n) && s[n] == '\t') {
            const char *p = s + n + 1;
            int *dst[4] = {r->t, r->aa, r->ao, r->la}, cnt[4] = {7, 9, 9, 4};
            for (int f = 0; f < 4; f++)
                for (int i = 0; i < cnt[f]; i++) { char *e; dst[f][i] = (int)strtol(p, &e, 10); p = e + 1; }
            return 1;
        }
        if (!nl) break;
        s = nl + 1;
    }
    return 0;
}

#define PX 78
#define PY 22
#define PW 480
#define PH 392
#define BTN_Y (PY + PH - 30 + 5)

static uint32_t g_px[(636 * 2) * (437 * 2)];
static MMCanvas canvas(int scale)
{
    MMCanvas c = {g_px, 636 * scale, 437 * scale, 636 * scale, scale, 0, 0, 0};
    return c;
}
static void draw(int mx, int my)
{
    for (int s = 1; s <= 2; s++) { MMCanvas c = canvas(s); mm_stats_draw(&c, mx, my); }
}
static void click(int x, int y) { mm_stats_mouse(x, y, MMB_LEFT, MA_DOWN); }
static int row_y(int r) { return PY + 48 + r * (mm_opt.bigtext ? 22 : 18) + 5; }

int main(void)
{
    Rec r;
    /* recording, streaks, areas, levels */
    mm_stats_answer("Anna", 0, 0, 1, 10);
    mm_stats_answer("anna", 0, 1, 1, 5);           /* same player, other case */
    mm_stats_answer("ANNA", 3, 2, 1, 0);
    mm_stats_answer("Anna", 3, 3, 2, 0);           /* second try: streak broken */
    mm_stats_answer("Anna", 8, 3, 0, 0);           /* wrong */
    mm_stats_answer("Anna", 8, 0, 1, 3);
    mm_stats_stairs("anna");
    mm_stats_answer("Ben", 5, 1, 0, 0);
    CHECK(g_saves == 8);
    CHECK(g_file && !strncmp(g_file, "MindMaze player stats 1\n", 24));
    CHECK(rec_of("Anna", &r));
    CHECK(r.t[0] == 6 && r.t[1] == 4 && r.t[2] == 1 && r.t[3] == 1);   /* answered, first, second, wrong */
    CHECK(r.t[4] == 1 && r.t[5] == 3 && r.t[6] == 1);                    /* streak, best, mazes */
    CHECK(r.aa[0] == 2 && r.ao[0] == 2 && r.aa[3] == 2 && r.ao[3] == 2 && r.aa[8] == 2 && r.ao[8] == 1 && r.aa[5] == 0);
    CHECK(r.la[0] == 2 && r.la[1] == 1 && r.la[2] == 1 && r.la[3] == 2);
    CHECK(rec_of("Ben", &r) && r.t[0] == 1 && r.t[3] == 1 && r.t[4] == 0 && r.aa[5] == 1 && r.ao[5] == 0 && r.la[1] == 1);
    CHECK(!rec_of("anna", &r));                    /* one line, the first spelling kept */
    /* out-of-range area / level still count the answer */
    mm_stats_answer("Ben", 12, -1, 1, 0);
    CHECK(rec_of("Ben", &r) && r.t[0] == 2 && r.t[1] == 1 && r.t[4] == 1 && r.t[5] == 1);
    /* Tourist, empty names, NULL and recording off are ignored */
    int saves = g_saves;
    mm_stats_answer("Tourist", 0, 0, 1, 0);
    mm_stats_answer("tOURIST", 0, 0, 1, 0);
    mm_stats_stairs("Tourist");
    mm_stats_answer("", 0, 0, 1, 0);
    mm_stats_answer(NULL, 0, 0, 1, 0);
    mm_opt.prompts = 0;
    mm_stats_answer("Anna", 0, 0, 1, 0);
    mm_stats_stairs("Anna");
    mm_opt.prompts = 1;
    CHECK(g_saves == saves && !strstr(g_file, "Tourist") && !strstr(g_file, "ourist"));
    /* names with a tab / line break are stored with spaces */
    mm_stats_answer("Tab\tName\n", 1, 1, 1, 0);
    CHECK(rec_of("Tab Name ", &r) && r.t[0] == 1);

    /* round trip: the saved text read back (mm_stats_open reads the file again) gives the same file */
    char *before = dup(g_file);
    mm_stats_open();
    mm_stats_key(KEY_ESC);
    CHECK(!mm_stats_on());
    mm_stats_stairs("Ben");                        /* forces a save from the reloaded data */
    CHECK(rec_of("Ben", &r) && r.t[6] == 1);
    CHECK(rec_of("Anna", &r) && r.t[0] == 6 && r.t[5] == 3 && r.aa[8] == 2 && r.la[3] == 2);
    {   /* everything except Ben's maze count unchanged */
        char *b = strstr(before, "Ben\t"), *a = strstr(g_file, "Ben\t");
        CHECK(b && a && !strncmp(before, g_file, (size_t)(b - before)));
    }
    free(before);

    /* damaged lines are skipped, good ones kept */
    set_file("garbage header\r\n"
             "Good\t3 2 1 0 2 2 1\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\r\n"
             "Short\t3 2 1 0 2 2\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\n"
             "Neg\t3 2 1 0 -2 2 1\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\n"
             "Sum\t3 2 2 0 2 2 1\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\n"
             "Okbig\t3 2 1 0 2 2 1\t1 1 1 0 0 0 0 0 0\t2 1 1 0 0 0 0 0 0\t1 1 1 0\n"
             "Letters\t3 2 1 0 2 x 1\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\n"
             "Extra\t3 2 1 0 2 2 1\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\textra\n"
             "\t3 2 1 0 2 2 1\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\n"
             "Huge\t99999999999 2 1 0 2 2 1\t1 1 1 0 0 0 0 0 0\t1 1 1 0 0 0 0 0 0\t1 1 1 0\n"
             "GOOD\t9 9 0 0 9 9 0\t0 0 0 0 0 0 0 0 0\t0 0 0 0 0 0 0 0 0\t0 0 0 0\n"
             "Truncated\t3 2 1 0 2 2 1\t1 1 1");
    mm_stats_open();
    mm_stats_stairs("Good");
    CHECK(rec_of("Good", &r) && r.t[0] == 3 && r.t[6] == 2);
    CHECK(!rec_of("GOOD", &r));                    /* duplicate name skipped */
    CHECK(!strstr(g_file, "Short") && !strstr(g_file, "Neg") && !strstr(g_file, "Sum") && !strstr(g_file, "Okbig") &&
          !strstr(g_file, "Letters") && !strstr(g_file, "Extra") && !strstr(g_file, "Huge") && !strstr(g_file, "Truncated"));
    CHECK(!strstr(g_file, "\n\t"));
    mm_stats_key(KEY_ESC);
    /* a missing file: nothing, no crash */
    free(g_file); g_file = NULL; g_file_len = 0;
    mm_stats_open();
    draw(300, 200);
    CHECK(mm_stats_hit(PX + 30, row_y(0)) == 0);   /* empty list: no rows */
    mm_stats_key(KEY_ESC);

    /* forget, case-insensitive */
    mm_stats_answer("Carl", 0, 0, 1, 0);
    mm_stats_answer("Dora", 0, 0, 1, 0);
    mm_stats_forget("CARL");
    CHECK(!strstr(g_file, "Carl") && rec_of("Dora", &r));
    mm_stats_forget("nobody");
    mm_stats_forget(NULL);

    /* the screen: three players, most answered first */
    free(g_file); g_file = NULL; g_file_len = 0;
    mm_stats_open(); mm_stats_key(KEY_ESC);          /* reload: empty */
    for (int i = 0; i < 3; i++) mm_stats_answer("Few", 1, 0, 1, 0);
    for (int i = 0; i < 7; i++) mm_stats_answer("Most", 2, 1, i & 1 ? 2 : 1, 0);
    mm_stats_stairs("Mazer");                      /* mazes only: listed last */
    CHECK(!mm_stats_on() && mm_stats_hit(PX + 30, row_y(0)) == 0);
    mm_stats_open();
    CHECK(mm_stats_on());
    g_texts = 0;
    draw(0, 0);
    CHECK(g_texts > 10);
    {   /* the panel was drawn: title bar red, background panel colour */
        MMCanvas c = canvas(1);
        mm_stats_draw(&c, 0, 0);
        CHECK(g_px[(PY + 5) * 636 + PX + 100] == 0x972938);
        CHECK(g_px[(PY + 200) * 636 + PX + 2] == 0xD9D6C5);
    }
    CHECK(mm_stats_hit(PX + PW - 12, PY + 6) == 1);      /* X */
    CHECK(mm_stats_hit(PX + PW - 60, BTN_Y) == 2);       /* Done */
    CHECK(mm_stats_hit(PX + 30, row_y(0)) == 100);
    CHECK(mm_stats_hit(PX + 30, row_y(2)) == 102);
    CHECK(mm_stats_hit(PX + 30, row_y(3)) == 0);         /* no fourth row */
    CHECK(mm_stats_hit(PX + 40, BTN_Y) == 0);            /* no scroll buttons for three */
    CHECK(mm_stats_hit(10, 10) == 0);
    draw(PX + 30, row_y(1));                             /* hover */
    /* list -> detail of the first row (Most) */
    click(PX + 30, row_y(0));
    CHECK(mm_stats_on() && mm_stats_hit(PX + 30, row_y(0)) == 0);   /* detail: no rows */
    CHECK(mm_stats_hit(PX + 30, BTN_Y) == 3);            /* Back */
    CHECK(mm_stats_hit(PX + 150, BTN_Y) == 4);           /* Reset */
    CHECK(mm_stats_hit(PX + PW - 60, BTN_Y) == 2);
    draw(PX + 150, BTN_Y);
    /* Back -> list; Esc in the detail -> list, Esc in the list closes */
    click(PX + 30, BTN_Y);
    CHECK(mm_stats_hit(PX + 30, row_y(0)) == 100);
    click(PX + 30, row_y(1));                            /* Few */
    mm_stats_key(KEY_ESC);
    CHECK(mm_stats_on() && mm_stats_hit(PX + 30, row_y(0)) == 100);
    mm_stats_key(KEY_ESC);
    CHECK(!mm_stats_on());
    /* reset needs a second click; Back in between cancels it */
    mm_stats_open();
    click(PX + 30, row_y(0));                            /* Most */
    saves = g_saves;
    click(PX + 150, BTN_Y);
    CHECK(g_saves == saves && rec_of("Most", &r));       /* first click: only asks */
    draw(0, 0);
    click(PX + 30, BTN_Y);                               /* Back */
    click(PX + 30, row_y(0));                            /* Most again */
    click(PX + 150, BTN_Y);
    CHECK(rec_of("Most", &r));                           /* the confirmation did not carry over */
    click(PX + 150, BTN_Y);                              /* confirmed */
    CHECK(!rec_of("Most", &r) && rec_of("Few", &r) && g_saves == saves + 1);
    CHECK(mm_stats_on() && mm_stats_hit(PX + 30, row_y(0)) == 100 && mm_stats_hit(PX + 30, row_y(2)) == 0);
    /* Done closes; X closes */
    click(PX + PW - 60, BTN_Y);
    CHECK(!mm_stats_on());
    mm_stats_open();
    click(PX + PW - 12, PY + 6);
    CHECK(!mm_stats_on());

    /* many players: scroll by wheel and buttons; large text */
    for (int i = 0; i < 30; i++) {
        char n[16];
        snprintf(n, sizeof n, "P%02d", i);
        for (int k = 0; k < 40 - i; k++) mm_stats_answer(n, k % 9, k % 4, k % 3, 0);   /* P00 most */
    }
    for (int big = 0; big <= 1; big++) {
        mm_opt.bigtext = big;
        mm_stats_open();
        draw(0, 0);
        int fit = big ? (PH - 88) / 22 : (PH - 88) / 18;
        CHECK(mm_stats_hit(PX + 30, row_y(fit - 1)) == 100 + fit - 1);
        CHECK(mm_stats_hit(PX + 30, row_y(fit)) == 0);
        CHECK(mm_stats_hit(PX + 40, BTN_Y) == 0);        /* at the top: no Up */
        CHECK(mm_stats_hit(PX + 100, BTN_Y) == 6);       /* Down */
        mm_stats_mouse(300, 200, -1, MA_WHEEL);
        CHECK(mm_stats_hit(PX + 30, row_y(0)) == 103);
        CHECK(mm_stats_hit(PX + 40, BTN_Y) == 5);        /* Up */
        mm_stats_mouse(300, 200, 1, MA_WHEEL);
        CHECK(mm_stats_hit(PX + 30, row_y(0)) == 100);
        for (int i = 0; i < 20; i++) mm_stats_mouse(300, 200, -1, MA_WHEEL);
        int n = 32;                                      /* 30 + Few + Mazer */
        CHECK(mm_stats_hit(PX + 30, row_y(0)) == 100 + n - fit);   /* clamped at the end */
        CHECK(mm_stats_hit(PX + 100, BTN_Y) == 0);
        draw(PX + 40, BTN_Y);
        click(PX + 40, BTN_Y);                           /* Up */
        CHECK(mm_stats_hit(PX + 30, row_y(0)) == 100 + n - fit - (fit - 1));
        click(PX + 30, row_y(0));
        draw(0, 0);                                      /* detail, both text sizes */
        mm_stats_key(KEY_ESC);
        mm_stats_key(KEY_ESC);
        CHECK(!mm_stats_on());
    }
    mm_opt.bigtext = 0;
    /* the order: P00 (40 answered) first */
    mm_stats_open();
    click(PX + 30, row_y(0));
    mm_stats_forget("p00");                              /* forgotten while its detail is shown: back to the list */
    draw(0, 0);
    CHECK(mm_stats_hit(PX + 30, row_y(0)) == 100);
    mm_stats_key(KEY_ESC);
    CHECK(!mm_stats_on());
    /* the file is still valid after all that: reading it back gives the same text */
    before = dup(g_file);
    mm_stats_open();
    saves = g_saves;
    mm_stats_forget("not there");
    CHECK(g_saves == saves);
    mm_stats_forget("few");                              /* reloaded data saved again: the same text minus that line */
    {
        char *a = strstr(before, "\nFew\t"), *e = a ? strchr(a + 1, '\n') : NULL;
        CHECK(a && e);
        if (a && e) {
            memmove(a + 1, e + 1, strlen(e + 1) + 1);
            CHECK(!strcmp(before, g_file));
        }
    }
    free(before);
    mm_stats_key(KEY_ESC);
    free(g_file);

    printf("test_stats: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
