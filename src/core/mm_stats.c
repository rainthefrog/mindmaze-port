/* MindMaze - player stats (CHANGE 2026-11-21, not in the original; Modern options > "Turn prompts and player stats").
 *
 * Per player name (case-insensitive like the logon list; "Tourist" is never recorded): questions answered, right at
 * the first / second try, wrong (incl. time ran out), the current and best streak of first-try answers, mazes finished,
 * answered / right per area (DB area 0..8) and answered per level (0..3). Recorded only while mm_opt.prompts is on.
 *
 * Kept in the user file "stats.txt" (plat_load_user / plat_save_user), saved after every change. Format (UTF-8):
 *   line 1:  "MindMaze player stats 1"   (version 1; a missing / damaged header does not stop the reading)
 *   then one line per player, five fields separated by tabs:
 *     name \t answered first second wrong streak best mazes \t 9 x area answered \t 9 x area right \t 4 x level answered
 *   (numbers separated by single spaces). A line that does not parse exactly (wrong count, not a number, negative,
 *   right > answered, ...) is skipped, as is a second line for the same name. Names never hold tabs or line breaks
 *   (replaced by spaces when recorded).
 *
 * The stats screen (mm_stats_open) is an overlay in the 636 x 437 dialog space in the Settings screen's look
 * (mm_settings.c): a list of the players with stats (most answered first), and a player's detail page. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"

#define STATS_FILE "stats.txt"
#define STATS_HEADER "MindMaze player stats 1"
#define TOURIST_STR 29043                  /* "Tourist" */
#define AREA_STR0 29014                    /* + button 1..9: the area button labels (29014 = "All Areas") */
#define NAREA 9
#define NLEVEL 4
#define MAXPLAYERS 1000
#define MAXCOUNT 1000000000

#define C_TEXT 0x34322D
#define C_RED 0x972938
#define C_LINE 0xA09683
#define C_BG 0xD9D6C5
#define C_GREY 0x6E655A
#define C_HOVER 0xE8E5D6
#define C_BUTTON 0xF2F1EA
#define C_BUTTON_FRAME 0x2E2B25
#define PX 78
#define PY 22
#define PW 480
#define PH 392

/* the game's area buttons 1..9 -> DB area (mm_game.c AREA_OF_BUTTON, FUN_00520270); the table lists them in button order */
static const int AREA_OF_BUTTON[10] = {-1, 1, 2, 3, 0, 4, 5, 6, 7, 8};

typedef struct {
    char name[64];
    int answered, first, second, wrong, streak, best, mazes;
    int area_ans[NAREA], area_ok[NAREA];
    int level_ans[NLEVEL];
} PStat;

static PStat *g_p;
static int g_n, g_cap, g_loaded;

/* ---- names ---- */
static int same_name(const char *a, const char *b)                         /* ASCII case-insensitive, like the logon list */
{
    for (; *a && *b; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

/* copy a name for storing: control characters (tabs, line breaks) become spaces; cut at a UTF-8 character boundary */
static void clean_name(const char *s, char *out, int n)
{
    int i = 0;
    for (; *s && i < n - 1; s++) out[i++] = (unsigned char)*s < 32 ? ' ' : *s;
    if (((unsigned char)*s & 0xC0) == 0x80) {            /* cut in the middle of a character: drop all of it */
        while (i > 0 && ((unsigned char)out[i - 1] & 0xC0) == 0x80) i--;
        if (i > 0) i--;
    }
    out[i] = 0;
}

static int find(const char *name)
{
    for (int i = 0; i < g_n; i++) if (same_name(g_p[i].name, name)) return i;
    return -1;
}

static PStat *add(const char *name)
{
    if (g_n >= MAXPLAYERS) return NULL;
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 16;
        PStat *p = realloc(g_p, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        g_p = p;
        g_cap = cap;
    }
    PStat *p = &g_p[g_n++];
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", name);
    return p;
}

/* ---- file ---- */
/* exactly n numbers 0..MAXCOUNT separated by single spaces, nothing else */
static int parse_ints(const char *s, const char *end, int *out, int n)
{
    for (int i = 0; i < n; i++) {
        if (i && (s >= end || *s++ != ' ')) return 0;
        if (s >= end || *s < '0' || *s > '9') return 0;
        long v = 0;
        while (s < end && *s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (v > MAXCOUNT) return 0; }
        out[i] = (int)v;
    }
    return s == end;
}

static int parse_line(const char *s, const char *end, PStat *p)
{
    const char *f[6];
    int nf = 0;
    f[nf++] = s;
    for (const char *q = s; q < end; q++)
        if (*q == '\t') { if (nf == 5) return 0; f[nf++] = q + 1; }
    if (nf != 5) return 0;
    f[5] = end + 1;
    int len = (int)(f[1] - 1 - f[0]);
    if (len <= 0 || len >= (int)sizeof p->name) return 0;
    memset(p, 0, sizeof *p);
    memcpy(p->name, f[0], (size_t)len);
    p->name[len] = 0;
    for (int i = 0; i < len; i++) if ((unsigned char)p->name[i] < 32) return 0;
    int t[7];
    if (!parse_ints(f[1], f[2] - 1, t, 7) || !parse_ints(f[2], f[3] - 1, p->area_ans, NAREA) ||
        !parse_ints(f[3], f[4] - 1, p->area_ok, NAREA) || !parse_ints(f[4], f[5] - 1, p->level_ans, NLEVEL)) return 0;
    p->answered = t[0]; p->first = t[1]; p->second = t[2]; p->wrong = t[3]; p->streak = t[4]; p->best = t[5]; p->mazes = t[6];
    if ((long)p->first + p->second + p->wrong != p->answered || p->streak > p->best || p->best > p->first) return 0;
    long sa = 0, sl = 0;
    for (int a = 0; a < NAREA; a++) { if (p->area_ok[a] > p->area_ans[a]) return 0; sa += p->area_ans[a]; }
    for (int l = 0; l < NLEVEL; l++) sl += p->level_ans[l];
    return sa <= p->answered && sl <= p->answered;
}

static void load(void)
{
    g_n = 0;
    g_loaded = 1;
    int size = 0;
    char *d = plat_load_user(STATS_FILE, &size);
    if (!d) return;
    const char *s = d, *e = d + size;
    while (s < e) {
        const char *nl = memchr(s, '\n', (size_t)(e - s)), *le = nl ? nl : e;
        const char *end = le > s && le[-1] == '\r' ? le - 1 : le;
        PStat p;
        if (parse_line(s, end, &p) && find(p.name) < 0) {
            PStat *q = add(p.name);
            if (q) *q = p;
        }
        s = nl ? nl + 1 : e;
    }
    free(d);
}

static void ensure(void) { if (!g_loaded) load(); }

static void save(void)
{
    size_t cap = 64 + (size_t)g_n * 512, len = 0;   /* a line: name < 64 + 29 numbers of at most 10 digits */
    char *b = malloc(cap);
    if (!b) return;
    len += (size_t)snprintf(b, cap, "%s\n", STATS_HEADER);
    for (int i = 0; i < g_n; i++) {
        const PStat *p = &g_p[i];
        len += (size_t)snprintf(b + len, cap - len, "%s\t%d %d %d %d %d %d %d\t", p->name, p->answered, p->first, p->second,
                                p->wrong, p->streak, p->best, p->mazes);
        for (int a = 0; a < NAREA; a++) len += (size_t)snprintf(b + len, cap - len, a ? " %d" : "%d", p->area_ans[a]);
        b[len++] = '\t';
        for (int a = 0; a < NAREA; a++) len += (size_t)snprintf(b + len, cap - len, a ? " %d" : "%d", p->area_ok[a]);
        b[len++] = '\t';
        for (int l = 0; l < NLEVEL; l++) len += (size_t)snprintf(b + len, cap - len, l ? " %d" : "%d", p->level_ans[l]);
        b[len++] = '\n';
    }
    plat_save_user(STATS_FILE, b, (int)len);
    free(b);
}

/* the stats record of a player that is recorded (NULL: none, Tourist, recording off), created if new */
static PStat *record_of(const char *player)
{
    if (!mm_opt.prompts || !player) return NULL;
    char name[64];
    clean_name(player, name, sizeof name);
    if (!name[0] || same_name(name, mm_str(TOURIST_STR))) return NULL;
    ensure();
    int i = find(name);
    return i >= 0 ? &g_p[i] : add(name);
}

static void bump(int *v) { if (*v < MAXCOUNT) (*v)++; }

void mm_stats_answer(const char *player, int area, int level, int result, int seconds_left)
{
    (void)seconds_left;
    PStat *p = record_of(player);
    if (!p || p->answered >= MAXCOUNT) return;
    bump(&p->answered);
    if (result == 1) { bump(&p->first); bump(&p->streak); if (p->streak > p->best) p->best = p->streak; }
    else { if (result == 2) bump(&p->second); else bump(&p->wrong); p->streak = 0; }
    if (area >= 0 && area < NAREA) { bump(&p->area_ans[area]); if (result == 1 || result == 2) bump(&p->area_ok[area]); }
    if (level >= 0 && level < NLEVEL) bump(&p->level_ans[level]);
    save();
    plat_invalidate();
}

void mm_stats_stairs(const char *player)
{
    PStat *p = record_of(player);
    if (!p) return;
    bump(&p->mazes);
    save();
    plat_invalidate();
}

static void remove_at(int i)
{
    memmove(&g_p[i], &g_p[i + 1], (size_t)(g_n - i - 1) * sizeof *g_p);
    g_n--;
}

void mm_stats_forget(const char *player)
{
    if (!player) return;
    char name[64];
    clean_name(player, name, sizeof name);
    ensure();
    int i = find(name);
    if (i < 0) return;
    remove_at(i);
    save();
    plat_invalidate();
}

/* ---- the stats screen ---- */
enum { PAGE_LIST, PAGE_DETAIL };
/* clickable targets: 1 close (X), 2 Done, 3 Back, 4 Reset, 5 scroll up, 6 scroll down, 100 + i player row i of the
 * sorted list (only the visible rows) */
enum { ID_CLOSE = 1, ID_DONE, ID_BACK, ID_RESET, ID_UP, ID_DOWN, ID_ROW = 100 };

static struct {
    int on, page, scroll, confirm;
    char name[64];                         /* the detail page's player */
} S;

static int big(void) { return mm_opt.bigtext; }
static int font_n(void) { return big() ? FONT_T : FONT_TEXT; }
static int font_b(void) { return big() ? FONT_T_B : FONT_BOLD; }
static int inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }
static int pct(int part, int whole) { return whole > 0 ? (int)(((long long)part * 200 + whole) / (2LL * whole)) : 0; }

static int has_stats(const PStat *p) { return p->answered > 0 || p->mazes > 0; }

/* the players with stats, most answered first (then by name) */
static int cmp_idx(const void *a, const void *b)
{
    const PStat *p = &g_p[*(const int *)a], *q = &g_p[*(const int *)b];
    if (p->answered != q->answered) return p->answered > q->answered ? -1 : 1;
    if (p->mazes != q->mazes) return p->mazes > q->mazes ? -1 : 1;
    return *(const int *)a - *(const int *)b;
}
static int sorted(int *out)
{
    int n = 0;
    for (int i = 0; i < g_n; i++) if (has_stats(&g_p[i])) out[n++] = i;
    qsort(out, (size_t)n, sizeof *out, cmp_idx);
    return n;
}

/* list geometry */
enum { LIST_Y = PY + 48, LIST_BOTTOM = PY + PH - 40, BTN_Y = PY + PH - 30 };
static int row_h(void) { return big() ? 22 : 18; }
static int rows_fit(void) { return (LIST_BOTTOM - LIST_Y) / row_h(); }
static int max_scroll(int n) { return n > rows_fit() ? n - rows_fit() : 0; }
static void clamp_scroll(int n) { if (S.scroll > max_scroll(n)) S.scroll = max_scroll(n); if (S.scroll < 0) S.scroll = 0; }

static PStat *shown(void) { int i = find(S.name); return i >= 0 ? &g_p[i] : NULL; }

int mm_stats_on(void) { return S.on; }

void mm_stats_open(void)
{
    load();                                /* read again: the file is the truth */
    S.on = 1;
    S.page = PAGE_LIST;
    S.scroll = 0;
    S.confirm = 0;
    plat_invalidate();
}

int mm_stats_hit(int x, int y)
{
    if (!S.on) return 0;
    if (inside(x, y, PX + PW - 20, PY, 18, 18)) return ID_CLOSE;
    if (inside(x, y, PX + PW - 96, BTN_Y, 80, 20)) return ID_DONE;
    if (S.page == PAGE_DETAIL) {
        if (inside(x, y, PX + 16, BTN_Y, 80, 20)) return ID_BACK;
        if (inside(x, y, PX + 104, BTN_Y, 150, 20)) return ID_RESET;
        return 0;
    }
    int idx[MAXPLAYERS], n = sorted(idx);
    clamp_scroll(n);
    if (max_scroll(n)) {
        if (S.scroll > 0 && inside(x, y, PX + 16, BTN_Y, 60, 20)) return ID_UP;
        if (S.scroll < max_scroll(n) && inside(x, y, PX + 82, BTN_Y, 60, 20)) return ID_DOWN;
    }
    for (int r = 0; r < rows_fit() && S.scroll + r < n; r++)
        if (inside(x, y, PX + 12, LIST_Y + r * row_h(), PW - 24, row_h())) return ID_ROW + S.scroll + r;
    return 0;
}

static void button(MMCanvas *c, int x, int y, int w, int h, const char *s, int hv)
{
    mm_fill(c, x, y, w, h, C_BUTTON);
    mm_frame(c, x, y, w, h, 1, C_BUTTON_FRAME);
    plat_text(c, x, y, w, h, s, FONT_TEXT, hv ? C_RED : 0, TX_CENTER | TX_VCENTER);
}

static void num(MMCanvas *c, int x, int y, int w, int h, int v, const char *suffix, int font, uint32_t rgb)
{
    char t[32];
    snprintf(t, sizeof t, "%d%s", v, suffix);
    plat_text(c, x, y, w, h, t, font, rgb, TX_RIGHT | TX_VCENTER);
}

/* list columns: player, answered, % right first try, best streak, mazes (right edges PX + 464) */
static const int COL_X[5] = {PX + 16, PX + 186, PX + 256, PX + 334, PX + 404};
static const int COL_W[5] = {166, 66, 74, 66, 60};

static void draw_list(MMCanvas *c, int hv)
{
    static const char *const HEAD[5] = {"Player", "Answered", "1st try", "Best streak", "Mazes"};
    for (int k = 0; k < 5; k++)
        plat_text(c, COL_X[k], PY + 26, COL_W[k], 16, HEAD[k], FONT_BOLD, C_TEXT, (k ? TX_RIGHT : TX_LEFT) | TX_VCENTER);
    mm_fill(c, PX + 16, PY + 44, PW - 32, 1, 0);
    int idx[MAXPLAYERS], n = sorted(idx), rh = row_h(), fn = font_n();
    clamp_scroll(n);
    if (!n)
        plat_text(c, PX + 16, LIST_Y + 8, PW - 32, 60, "No stats yet. Answers and finished mazes are counted for every "
                  "saved player (not the Tourist) while \"Turn prompts and player stats\" is on in Settings > Modern options.",
                  fn, C_GREY, TX_WRAP);
    for (int r = 0; r < rows_fit() && S.scroll + r < n; r++) {
        const PStat *p = &g_p[idx[S.scroll + r]];
        int y = LIST_Y + r * rh, on = hv == ID_ROW + S.scroll + r;
        uint32_t col = on ? C_RED : C_TEXT;
        if (on) mm_fill(c, PX + 12, y, PW - 24, rh, C_HOVER);
        plat_text(c, COL_X[0], y, COL_W[0], rh, p->name, font_b(), col, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
        num(c, COL_X[1], y, COL_W[1], rh, p->answered, "", fn, col);
        num(c, COL_X[2], y, COL_W[2], rh, pct(p->first, p->answered), "%", fn, col);
        num(c, COL_X[3], y, COL_W[3], rh, p->best, "", fn, col);
        num(c, COL_X[4], y, COL_W[4], rh, p->mazes, "", fn, col);
        mm_fill(c, PX + 16, y + rh - 1, PW - 32, 1, C_LINE);
    }
    if (max_scroll(n)) {
        if (S.scroll > 0) button(c, PX + 16, BTN_Y, 60, 20, "Up", hv == ID_UP);
        if (S.scroll < max_scroll(n)) button(c, PX + 82, BTN_Y, 60, 20, "Down", hv == ID_DOWN);
        char t[64];
        int last = S.scroll + rows_fit() < n ? S.scroll + rows_fit() : n;
        snprintf(t, sizeof t, "%d-%d of %d players", S.scroll + 1, last, n);
        plat_text(c, PX + 150, BTN_Y, 180, 20, t, FONT_TEXT, C_GREY, TX_LEFT | TX_VCENTER);
    } else if (n)
        plat_text(c, PX + 16, BTN_Y, 300, 20, "Click a player for the details.", FONT_TEXT, C_GREY, TX_LEFT | TX_VCENTER);
}

static void draw_detail(MMCanvas *c, int hv, const PStat *p)
{
    int fn = font_n(), fb = font_b();
    char t[128];
    /* summary: two columns of four lines */
    const int SY = PY + 26, LH = 18;
    snprintf(t, sizeof t, "Questions answered: %d", p->answered);
    plat_text(c, PX + 16, SY, 260, LH, t, fb, C_TEXT, TX_LEFT | TX_VCENTER);
    snprintf(t, sizeof t, "Right at the first try: %d (%d%%)", p->first, pct(p->first, p->answered));
    plat_text(c, PX + 16, SY + LH, 260, LH, t, fn, C_TEXT, TX_LEFT | TX_VCENTER);
    snprintf(t, sizeof t, "Right at the second try: %d", p->second);
    plat_text(c, PX + 16, SY + 2 * LH, 260, LH, t, fn, C_TEXT, TX_LEFT | TX_VCENTER);
    snprintf(t, sizeof t, "Wrong or out of time: %d", p->wrong);
    plat_text(c, PX + 16, SY + 3 * LH, 260, LH, t, fn, C_TEXT, TX_LEFT | TX_VCENTER);
    snprintf(t, sizeof t, "Mazes finished: %d", p->mazes);
    plat_text(c, PX + 290, SY, 174, LH, t, fb, C_TEXT, TX_LEFT | TX_VCENTER);
    snprintf(t, sizeof t, "Current streak: %d", p->streak);
    plat_text(c, PX + 290, SY + LH, 174, LH, t, fn, C_TEXT, TX_LEFT | TX_VCENTER);
    snprintf(t, sizeof t, "Best streak: %d", p->best);
    plat_text(c, PX + 290, SY + 2 * LH, 174, LH, t, fn, C_TEXT, TX_LEFT | TX_VCENTER);
    plat_text(c, PX + 290, SY + 3 * LH, 174, LH, "(right at the first try in a row)", FONT_TEXT, C_GREY, TX_LEFT | TX_VCENTER);
    /* per area, in the order of the game's area buttons */
    const int TY = SY + 4 * LH + 6, RH = 20, BX = PX + 352, BW = 76;
    mm_fill(c, PX + 16, TY - 2, PW - 32, 1, 0);
    plat_text(c, PX + 16, TY, 200, 16, "Area", FONT_BOLD, C_TEXT, TX_LEFT | TX_VCENTER);
    plat_text(c, PX + 226, TY, 60, 16, "Answered", FONT_BOLD, C_TEXT, TX_RIGHT | TX_VCENTER);
    plat_text(c, PX + 290, TY, 52, 16, "Right", FONT_BOLD, C_TEXT, TX_RIGHT | TX_VCENTER);
    plat_text(c, BX, TY, PX + 464 - BX, 16, "% right", FONT_BOLD, C_TEXT, TX_LEFT | TX_VCENTER);
    for (int b = 1; b <= NAREA; b++) {
        int a = AREA_OF_BUTTON[b], y = TY + 18 + (b - 1) * RH, ans = p->area_ans[a], ok = p->area_ok[a], v = pct(ok, ans);
        uint32_t col = ans ? C_TEXT : C_GREY;
        plat_text(c, PX + 16, y, 206, RH, mm_str(AREA_STR0 + b), fn, col, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
        num(c, PX + 226, y, 60, RH, ans, "", fn, col);
        num(c, PX + 290, y, 52, RH, ok, "", fn, col);
        mm_fill(c, BX, y + 6, BW, 8, 0xFFFFFF);
        if (ans) mm_fill(c, BX, y + 6, BW * v / 100, 8, C_RED);
        mm_frame(c, BX, y + 6, BW, 8, 1, C_LINE);
        if (ans) num(c, BX + BW, y, PX + 464 - BX - BW, RH, v, "%", fn, col);
        mm_fill(c, PX + 16, y + RH - 1, PW - 32, 1, C_LINE);
    }
    /* per level */
    int ly = TY + 18 + NAREA * RH + 6;
    plat_text(c, PX + 16, ly, 150, LH, "Answered per level", FONT_BOLD, C_TEXT, TX_LEFT | TX_VCENTER);
    for (int l = 0; l < NLEVEL; l++) {
        snprintf(t, sizeof t, "Level %d: %d", l + 1, p->level_ans[l]);
        plat_text(c, PX + 160 + 76 * l, ly, 74, LH, t, fn, C_TEXT, TX_LEFT | TX_VCENTER);
    }
    button(c, PX + 16, BTN_Y, 80, 20, "Back", hv == ID_BACK);
    button(c, PX + 104, BTN_Y, 150, 20, S.confirm ? "Click again to reset" : "Reset", hv == ID_RESET);
}

void mm_stats_draw(MMCanvas *c, int mx, int my)
{
    if (!S.on) return;
    ensure();
    const PStat *p = S.page == PAGE_DETAIL ? shown() : NULL;
    if (S.page == PAGE_DETAIL && !p) S.page = PAGE_LIST;          /* forgotten meanwhile */
    int hv = mm_stats_hit(mx, my);
    mm_fill(c, PX - 2, PY - 2, PW + 4, PH + 4, C_TEXT);
    mm_fill(c, PX, PY, PW, 18, C_RED);
    char title[96];
    if (p) snprintf(title, sizeof title, "Player Stats: %s", p->name);
    else snprintf(title, sizeof title, "Player Stats");
    plat_text(c, PX + 6, PY, PW - 30, 18, title, font_b(), 0xFFFFFF, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
    plat_text(c, PX + PW - 20, PY, 18, 18, "X", FONT_BOLD, hv == ID_CLOSE ? 0xFFFF80 : 0xFFFFFF, TX_CENTER | TX_VCENTER);
    mm_fill(c, PX, PY + 18, PW, PH - 18, C_BG);
    if (p) draw_detail(c, hv, p);
    else draw_list(c, hv);
    button(c, PX + PW - 96, BTN_Y, 80, 20, "Done", hv == ID_DONE);
}

static void close_stats(void) { S.on = 0; S.confirm = 0; plat_invalidate(); }
static void back(void) { S.page = PAGE_LIST; S.confirm = 0; plat_invalidate(); }

static void scroll_by(int d)
{
    int idx[MAXPLAYERS], n = sorted(idx);
    S.scroll += d;
    clamp_scroll(n);
    plat_invalidate();
}

static void click(int id)
{
    if (!id) return;
    if (id == ID_CLOSE || id == ID_DONE) { close_stats(); return; }
    if (id == ID_BACK) { back(); return; }
    if (id == ID_UP) { scroll_by(-(rows_fit() - 1)); return; }
    if (id == ID_DOWN) { scroll_by(rows_fit() - 1); return; }
    if (id == ID_RESET) {
        if (!S.confirm) { S.confirm = 1; plat_invalidate(); return; }
        int i = find(S.name);
        if (i >= 0) { remove_at(i); save(); }
        back();
        return;
    }
    if (id >= ID_ROW) {
        int idx[MAXPLAYERS], n = sorted(idx);
        if (id - ID_ROW >= n) return;
        snprintf(S.name, sizeof S.name, "%s", g_p[idx[id - ID_ROW]].name);
        S.page = PAGE_DETAIL;
        S.confirm = 0;
        plat_invalidate();
    }
}

void mm_stats_mouse(int x, int y, int button, int action)
{
    if (!S.on) return;
    ensure();
    if (action == MA_WHEEL) { if (S.page == PAGE_LIST) scroll_by(button > 0 ? -3 : 3); return; }
    if (action != MA_DOWN || button != MMB_LEFT) return;
    click(mm_stats_hit(x, y));
}

void mm_stats_key(int key)
{
    if (!S.on) return;
    if (key == KEY_ESC) { if (S.page == PAGE_DETAIL) back(); else close_stats(); }
    else if (key == KEY_ENTER && S.page == PAGE_LIST) close_stats();
}
