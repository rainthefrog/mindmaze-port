/* Settings screen on the logon page (CHANGE, not in the original): question sources on/off, "skip questions
 * already seen", forget the seen list, a form to add your own questions (mm_sources.c does the work), and the
 * Pictures page (HD / widescreen switches and how to make that art: docs/HD-ART.md, data/art/START-HERE.txt).
 * Drawn over the logon picture in logical 636x437 coordinates, in the panel's colours. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#include "mm_art.h"

#ifndef MM_REPO_URL
#define MM_REPO_URL "https://github.com/OWNER/mindmaze-port"
#endif
#define HD_ART_URL MM_REPO_URL "/blob/main/docs/HD-ART.md"

#define C_TEXT 0x34322D
#define C_RED 0x972938
#define C_LINE 0xA09683
#define C_BG 0xD9D6C5
#define C_GREY 0x6E655A
#define PX 78
#define PY 22
#define PW 480
#define PH 392

static const char *AREAS[9] = {"Physical Science & Technology", "Life Science", "Geography", "History",
                               "Social Science", "Religion and Philosophy", "Art, Language, and Literature",
                               "Performing Arts", "Sports, Hobbies, and Pets"};
enum { F_QUESTION, F_CORRECT, F_WRONG1, F_WRONG2, F_WRONG3, NFIELD };
static const int FIELD_MAX[NFIELD] = {300, 40, 40, 40, 40};   /* characters */

static struct {
    int on, page, focus, area, level, err;
    char f[NFIELD][640];
    char status[256];
    int art_waiting;                                     /* agent results not installed yet (checked on opening) */
    int busy;                                            /* a disc is being read (web: in a worker) */
} S;

static int inside(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

int mm_settings_on(void) { return S.on; }
int mm_settings_typing(void) { return S.on && S.page == 1; }   /* "Add a Question": its fields take text */
/* the field being typed in and its name (the phone shows them above its keyboard, CHANGE 2026-11-21) */
const char *mm_settings_field(const char **label)
{
    static const char *const NAME[NFIELD] = {"Question (the clue)", "Correct answer", "Wrong answer 1", "Wrong answer 2", "Wrong answer 3"};
    *label = NAME[S.focus];
    return S.f[S.focus];
}

void mm_settings_open(void)
{
    S.on = 1;
    S.page = 0;
    S.status[0] = 0;
    for (int i = 0; i < mm_source_count(); i++) mm_source_set(i, mm_source_enabled(i));
    plat_invalidate();
}

/* ---- geometry ---- */
/* source rows: two lines each, or one line when there are many (added discs, CHANGE 2026-11-18) */
static int src_h(void) { return mm_source_count() <= 5 ? 30 : 17; }
static int src_y(int i) { return PY + 50 + src_h() * i; }
static int skip_y(void) { return src_y(mm_source_count()) + 8; }
/* volume bars (CHANGE): 0 music, 1 sound effects; click on the bar sets the level (steps of 5) */
static int vol_y(int k) { return skip_y() + 48 + 24 * k; }
enum { VOL_X = PX + 180, VOL_W = 200 };
/* "Export data for the phone app" (CHANGE 2026-11-18): below the instruments row when the platform has both (SDL) */
static int export_y(void) { return plat_instrument_count() > 0 ? vol_y(3) - 2 : vol_y(2) + 6; }
static int vol_from_x(int x) { int v = (x - VOL_X) * 100 / VOL_W; v = (v + 2) / 5 * 5; return v < 0 ? 0 : v > 100 ? 100 : v; }
static const int FY[NFIELD] = {PY + 46, PY + 130, PY + 168, PY + 190, PY + 212};
static int field_h(int f) { return f == F_QUESTION ? 64 : 18; }

/* clickable targets: 1 close, 10+i source, 30 skip seen, 31 forget seen, 32 add, 33 done, 35 pictures page,
 * 40+f field, 50/51 area </>, 60+l level, 70 save, 71 back, 80/81 volume, 82 instruments, 83 export,
 * 84 add questions from a disc,
 * pictures page: 34 widescreen, 36 HD, 37 how-to web page, 38 open the art kit folder, 39 back, 90 install */
enum { PAGE_SOURCES, PAGE_ADD, PAGE_PICTURES, PAGE_MODERN };
/* Modern options page (CHANGE 2026-11-21): 100 resume, 101 hints, 102 prompts + stats, 104 large text, 105 animations,
 * 110..112 layout auto / classic / touch, 120 "Classic MindMaze" (all off), 121 back; main page: 122 opens it */
static const int MOD_Y[6] = {PY + 50, PY + 86, PY + 122, PY + 184, PY + 238, PY + 274};
static const int LAY_X[3] = {PX + 90, PX + 200, PX + 300};
static int pic_y(int row) { return PY + 50 + 26 * row; }
int mm_settings_hit(int x, int y)
{
    if (!S.on) return 0;
    if (inside(x, y, PX + PW - 20, PY, 18, 18)) return 1;
    if (S.page == 0) {
        for (int i = 0; i < mm_source_count(); i++)
            if (inside(x, y, PX + 12, src_y(i), PW - 24, src_h() - 2)) return 10 + i;
        if (!S.busy && inside(x, y, PX + PW - 196, PY + 25, 180, 18)) return 84;
        if (inside(x, y, PX + 12, skip_y(), 300, 16)) return 30;
        if (inside(x, y, PX + PW - 150, skip_y() - 2, 134, 20)) return 31;
        for (int k = 0; k < 2; k++) if (inside(x, y, VOL_X - 6, vol_y(k) - 2, VOL_W + 12, 20)) return 80 + k;
        if (plat_instrument_count() > 1 && inside(x, y, PX + 12, vol_y(2) - 2, PW - 24, 20)) return 82;
        if (plat_can_export() && !S.status[0] && inside(x, y, PX + 16, export_y(), 220, 20)) return 83;
        if (inside(x, y, PX + 16, PY + PH - 30, 116, 20)) return 32;
        if (inside(x, y, PX + 138, PY + PH - 30, 116, 20)) return 35;
        if (inside(x, y, PX + 260, PY + PH - 30, 116, 20)) return 122;
        if (inside(x, y, PX + PW - 96, PY + PH - 30, 80, 20)) return 33;
        return 0;
    }
    if (S.page == PAGE_MODERN) {
        for (int i = 0; i < 6; i++)
            if (i != 3 && inside(x, y, PX + 12, MOD_Y[i], PW - 24, 30)) return 100 + i;
        for (int k = 0; k < 3; k++) if (inside(x, y, LAY_X[k], MOD_Y[3], 96, 16)) return 110 + k;
        if (inside(x, y, PX + 16, PY + PH - 30, PW - 128, 22)) return 120;
        if (inside(x, y, PX + PW - 96, PY + PH - 30, 80, 20)) return 121;
        return 0;
    }
    if (S.page == PAGE_PICTURES) {
        if (mm_hd_available() && inside(x, y, PX + 12, pic_y(0), 300, 18)) return 36;
        if (mm_wide_available() && inside(x, y, PX + 12, pic_y(1), 300, 18)) return 34;
        if (inside(x, y, PX + 16, PY + PH - 30, 200, 20)) return 37;
        if (inside(x, y, PX + 224, PY + PH - 30, 150, 20)) return 38;
        if (inside(x, y, PX + PW - 96, PY + PH - 30, 80, 20)) return 39;
        if (inside(x, y, PX + 16, PY + PH - 56, 200, 20)) return 90;
        return 0;
    }
    for (int f = 0; f < NFIELD; f++)
        if (inside(x, y, PX + 16, FY[f], PW - 32, field_h(f))) return 40 + f;
    if (inside(x, y, PX + 16, PY + 256, 20, 20)) return 50;
    if (inside(x, y, PX + 262, PY + 256, 20, 20)) return 51;
    for (int l = 0; l < 4; l++)
        if (inside(x, y, PX + 312 + 32 * l, PY + 254, 26, 24)) return 60 + l;
    if (inside(x, y, PX + PW - 196, PY + PH - 30, 90, 20)) return 70;
    if (inside(x, y, PX + PW - 100, PY + PH - 30, 84, 20)) return 71;
    return 0;
}

/* ---- drawing ---- */
static void draw_pictures(MMCanvas *c, int hv);
static void draw_modern(MMCanvas *c, int hv);
static void button(MMCanvas *c, int x, int y, int w, int h, const char *s, int hv)
{
    mm_fill(c, x, y, w, h, 0xF2F1EA);
    mm_frame(c, x, y, w, h, 1, 0x2E2B25);
    plat_text(c, x, y, w, h, s, FONT_TEXT, hv ? C_RED : 0, TX_CENTER | TX_VCENTER);
}

static void checkbox(MMCanvas *c, int x, int y, int on, int hv)
{
    mm_fill(c, x, y, 12, 12, 0xFFFFFF);
    mm_frame(c, x, y, 12, 12, 1, hv ? C_RED : C_LINE);
    if (on) mm_fill(c, x + 3, y + 3, 6, 6, C_TEXT);
}

static void field(MMCanvas *c, int f, int hv)
{
    int x = PX + 16, y = FY[f], w = PW - 32, h = field_h(f);
    mm_fill(c, x, y, w, h, 0xFFFFFF);
    mm_frame(c, x, y, w, h, 1, S.focus == f ? C_RED : hv ? C_GREY : C_LINE);
    char t[700];
    snprintf(t, sizeof t, "%s%s", S.f[f], S.focus == f && (plat_ticks() / 500) & 1 ? "|" : "");
    if (f == F_QUESTION) { plat_text(c, x + 4, y + 2, w - 8, h - 4, t, FONT_TEXT, 0, TX_WRAP); return; }
    const char *s = t;                                        /* single line: show the end while typing */
    while (*s && plat_text_width(s, FONT_TEXT) > w - 8) { s++; while ((*s & 0xC0) == 0x80) s++; }
    plat_text(c, x + 4, y, w - 8, h, s, FONT_TEXT, 0, TX_LEFT | TX_VCENTER);
}

void mm_settings_draw(MMCanvas *c, int mx, int my)
{
    if (!S.on) return;
    int hv = mm_settings_hit(mx, my);
    mm_fill(c, PX - 2, PY - 2, PW + 4, PH + 4, C_TEXT);
    mm_fill(c, PX, PY, PW, 18, C_RED);
    static const char *const TITLE[4] = {"Settings", "Add a Question", "Pictures: Original, HD and Widescreen", "Modern options"};
    plat_text(c, PX + 6, PY, PW - 30, 18, TITLE[S.page], FONT_BOLD, 0xFFFFFF, TX_LEFT | TX_VCENTER);
    plat_text(c, PX + PW - 20, PY, 18, 18, "X", FONT_BOLD, hv == 1 ? 0xFFFF80 : 0xFFFFFF, TX_CENTER | TX_VCENTER);
    mm_fill(c, PX, PY + 18, PW, PH - 18, C_BG);
    char buf[256];
    if (S.page == 0) {
        plat_text(c, PX + 16, PY + 28, 300, 16, "Question sources", FONT_BOLD, C_TEXT, TX_LEFT);
        if (S.busy) plat_text(c, PX + PW - 196, PY + 25, 180, 18, "Reading the disc...", FONT_TEXT, C_GREY, TX_RIGHT | TX_VCENTER);
        else button(c, PX + PW - 196, PY + 25, 180, 18, "Add questions from a disc...", hv == 84);
        mm_fill(c, PX + 16, PY + 44, PW - 32, 1, 0);
        for (int i = 0; i < mm_source_count(); i++) {
            int y = src_y(i);
            checkbox(c, PX + 16, y + 2, mm_source_enabled(i), hv == 10 + i);
            if (src_h() < 30) {                                    /* one line: name, count on the right */
                plat_text(c, PX + 36, y, 230, 15, mm_source_name(i), FONT_BOLD, hv == 10 + i ? C_RED : C_TEXT, TX_LEFT | TX_ELLIPSIS);
                plat_text(c, PX + 270, y, PW - 286, 15, mm_source_info(i), FONT_TEXT, C_GREY, TX_RIGHT | TX_ELLIPSIS);
                continue;
            }
            plat_text(c, PX + 36, y, PW - 52, 14, mm_source_name(i), FONT_BOLD, hv == 10 + i ? C_RED : C_TEXT, TX_LEFT | TX_ELLIPSIS);
            plat_text(c, PX + 36, y + 14, PW - 52, 14, mm_source_info(i), FONT_TEXT, C_GREY, TX_LEFT | TX_ELLIPSIS);
        }
        int y = skip_y();
        mm_fill(c, PX + 16, y - 5, PW - 32, 1, C_LINE);
        checkbox(c, PX + 16, y + 2, mm_skip_seen, hv == 30);
        plat_text(c, PX + 36, y, 280, 16, "Skip questions a player has already seen", FONT_TEXT, hv == 30 ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER);
        snprintf(buf, sizeof buf, "Forget seen (%d)", mm_seen_total());
        button(c, PX + PW - 150, y - 2, 134, 20, buf, hv == 31);
        plat_text(c, PX + 16, vol_y(0) - 22, 300, 16, "Volume", FONT_BOLD, C_TEXT, TX_LEFT);
        mm_fill(c, PX + 16, vol_y(0) - 6, PW - 32, 1, C_LINE);
        for (int k = 0; k < 2; k++) {
            int vy = vol_y(k), v = k ? mm_sound_volume : mm_music_volume, on = k ? mm_sound_on : mm_music_on;
            plat_text(c, PX + 16, vy, 160, 16, k ? "Sound effects" : "Music", FONT_TEXT, hv == 80 + k ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER);
            mm_fill(c, VOL_X, vy + 4, VOL_W, 8, 0xFFFFFF);
            mm_fill(c, VOL_X, vy + 4, VOL_W * v / 100, 8, on ? C_RED : C_GREY);
            mm_frame(c, VOL_X, vy + 4, VOL_W, 8, 1, hv == 80 + k ? C_RED : C_LINE);
            mm_fill(c, VOL_X + VOL_W * v / 100 - 2, vy + 1, 5, 14, C_TEXT);           /* knob */
            snprintf(buf, sizeof buf, on ? "%d%%" : "%d%% (off)", v);
            plat_text(c, VOL_X + VOL_W + 12, vy, 90, 16, buf, FONT_TEXT, on ? C_TEXT : C_GREY, TX_LEFT | TX_VCENTER);
        }
        if (plat_instrument_count() > 0) {                       /* music instruments (web / Android) */
            int vy = vol_y(2), n = plat_instrument_count();
            plat_text(c, PX + 16, vy, 160, 16, "Music instruments", FONT_TEXT, hv == 82 ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER);
            snprintf(buf, sizeof buf, n > 1 ? "%s  (click to change, %d of %d)" : "%s", plat_instrument_name(plat_instrument_get()),
                     plat_instrument_get() + 1, n);
            plat_text(c, VOL_X, vy, PW - (VOL_X - PX) - 16, 16, buf, FONT_TEXT, hv == 82 ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
        }
        /* PC (Windows, Linux, macOS): hand the data to the phone app; hidden while a status message uses that line */
        if (plat_can_export() && !S.status[0]) {
            button(c, PX + 16, export_y(), 220, 20, "Export data for the phone app...", hv == 83);
            plat_text(c, PX + 244, export_y(), PW - 260, 20, "one .zip for the Android app's import", FONT_TEXT, C_GREY, TX_LEFT | TX_VCENTER);
        }
        button(c, PX + 16, PY + PH - 30, 116, 20, "Add a question...", hv == 32);
        button(c, PX + 138, PY + PH - 30, 116, 20, "Pictures / HD...", hv == 35);
        button(c, PX + 260, PY + PH - 30, 116, 20, "Modern options...", hv == 122);
        button(c, PX + PW - 96, PY + PH - 30, 80, 20, "Done", hv == 33);
    } else if (S.page == PAGE_PICTURES) {
        draw_pictures(c, hv);
    } else if (S.page == PAGE_MODERN) {
        draw_modern(c, hv);
    } else {
        static const char *LABEL[NFIELD] = {"Question (the clue)", "Correct answer", "Wrong answers", NULL, NULL};
        static const int LY[NFIELD] = {PY + 30, PY + 114, PY + 152, 0, 0};
        for (int f = 0; f < NFIELD; f++) {
            if (LABEL[f]) plat_text(c, PX + 16, LY[f], 300, 16, LABEL[f], FONT_BOLD, C_TEXT, TX_LEFT);
            field(c, f, hv == 40 + f);
        }
        plat_text(c, PX + 16, PY + 238, 200, 16, "Area", FONT_BOLD, C_TEXT, TX_LEFT);
        button(c, PX + 16, PY + 256, 20, 20, "<", hv == 50);
        plat_text(c, PX + 40, PY + 256, 220, 20, AREAS[S.area], FONT_TEXT, C_TEXT, TX_CENTER | TX_VCENTER);
        button(c, PX + 262, PY + 256, 20, 20, ">", hv == 51);
        plat_text(c, PX + 312, PY + 238, 120, 16, "Level", FONT_BOLD, C_TEXT, TX_LEFT);
        for (int l = 0; l < 4; l++) {
            int x = PX + 312 + 32 * l, yy = PY + 254;
            mm_fill(c, x, yy, 26, 24, 0xECEBE4);
            char n[2] = {(char)('1' + l), 0};
            plat_text(c, x, yy, 26, 24, n, FONT_NAME, hv == 60 + l ? C_RED : 0, TX_CENTER | TX_VCENTER);
            if (S.level == l) mm_frame(c, x, yy, 26, 24, 1, C_RED);
        }
        snprintf(buf, sizeof buf, "Saved in custom_questions.json next to MindMaze.exe (%d so far). "
                 "Switch on \"My questions\" in the settings to play them.", mm_custom_count());
        plat_text(c, PX + 16, PY + 290, PW - 32, 30, buf, FONT_TEXT, C_GREY, TX_WRAP);
        button(c, PX + PW - 196, PY + PH - 30, 90, 20, "Save", hv == 70);
        button(c, PX + PW - 100, PY + PH - 30, 84, 20, "Back", hv == 71);
    }
    if (S.status[0])
        plat_text(c, PX + 16, PY + PH - (S.page == PAGE_PICTURES ? 92 : 62), PW - 32, 28, S.status, FONT_TEXT, S.err ? C_RED : C_TEXT, TX_WRAP);
}

/* Pictures page: the switches (same as F2 / F3 and the menu) and how to make the HD / widescreen art */
static void pic_switch(MMCanvas *c, int row, int avail, int on, int id, const char *label, int hv)
{
    int y = pic_y(row);
    char t[160];
    if (avail) checkbox(c, PX + 16, y + 3, on, hv == id);
    else mm_frame(c, PX + 16, y + 3, 12, 12, 1, C_LINE);
    snprintf(t, sizeof t, "%s%s", label, avail ? "" : "  -  not made yet");
    plat_text(c, PX + 36, y, PW - 52, 18, t, FONT_TEXT, !avail ? C_GREY : hv == id ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER);
}

static void draw_pictures(MMCanvas *c, int hv)
{
    plat_text(c, PX + 16, PY + 28, 300, 16, "Show", FONT_BOLD, C_TEXT, TX_LEFT);
    mm_fill(c, PX + 16, PY + 44, PW - 32, 1, 0);
    pic_switch(c, 0, mm_hd_available(), mm_hd_on, 36, "HD pictures, sharper (F2); off = the original pictures", hv);
    pic_switch(c, 1, mm_wide_available(), mm_view_ext() > 0, 34, "Widescreen view, wider rooms (F3)", hv);

    int y = pic_y(2) + 10;
    plat_text(c, PX + 16, y, 300, 16, "Make HD and widescreen pictures", FONT_BOLD, C_TEXT, TX_LEFT);
    mm_fill(c, PX + 16, y + 16, PW - 32, 1, 0);
    int kit = 0;
    void *d = plat_load("art/START-HERE.txt", NULL);
    if (d) { kit = 1; free(d); }
    char t[1000];
    snprintf(t, sizeof t,
             "The sharper and wider pictures are not on the disc: you make them once, on your own computer, from your "
             "own Encarta pictures with an AI agent that can draw images (the instructions are written for OpenAI "
             "Codex). MindMaze has already prepared everything in its data folder.\n\n"
             "1. Open the agent on MindMaze's data folder and give it the file art/START-HERE.txt from there%s.\n"
             "2. It asks which pictures you want (HD, widescreen or both) and paints them.\n"
             "3. Restart MindMaze (it installs the new pictures by itself) or press Install below; then switch them on.\n\n"
             "Note: this takes many image generations (28 for HD, 20 more for widescreen) and can use a large part "
             "of an AI plan's limits or cost money on an API. It can be stopped and continued later. The pictures are "
             "for your own use.",
             kit ? "" : " (missing: delete the data folder and start MindMaze again to get it)");
    plat_text(c, PX + 16, y + 22, PW - 32, PY + PH - 62 - (y + 22), t, FONT_TEXT, C_TEXT, TX_WRAP);
    button(c, PX + 16, PY + PH - 56, 200, 20, S.art_waiting ? "Install the new pictures now" : "Install pictures again", hv == 90);
    button(c, PX + 16, PY + PH - 30, 200, 20, "How to make HD art (web page)", hv == 37);
    /* the phone app has no file manager: there the HD pictures come as a zip, a file or a link (CHANGE 2026-11-18) */
    button(c, PX + 224, PY + PH - 30, 150, 20, plat_art_import(0) ? "HD pictures: file or link..." : "Open the art kit folder", hv == 38);
    button(c, PX + PW - 96, PY + PH - 30, 80, 20, "Back", hv == 39);
}

/* Modern options page (CHANGE 2026-11-21): six switches, grouped; "Classic MindMaze" is a switch that turns them all off (and back) */
static void mod_row(MMCanvas *c, int i, int on, const char *title, const char *info, int hv)
{
    int y = MOD_Y[i], h = hv == 100 + i;
    checkbox(c, PX + 16, y + 2, on, h);
    plat_text(c, PX + 36, y, PW - 52, 15, title, FONT_BOLD, h ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER);
    plat_text(c, PX + 36, y + 15, PW - 52, 15, info, FONT_TEXT, C_GREY, TX_LEFT | TX_ELLIPSIS);
}

static void draw_modern(MMCanvas *c, int hv)
{
    plat_text(c, PX + 16, PY + 28, 300, 16, "Game", FONT_BOLD, C_TEXT, TX_LEFT);
    mm_fill(c, PX + 16, PY + 44, PW - 32, 1, 0);
    mod_row(c, 0, mm_opt.resume, "Continue unfinished games",
            "saved after every turn; the castle gate offers to continue", hv);
    mod_row(c, 1, mm_opt.hints, "Hints", "50/50 removes wrong answers; then one chance, half the points", hv);
    mod_row(c, 2, mm_opt.prompts, "Turn prompts and player stats",
            "\"Your turn\" between players; Stats on the castle-gate page", hv);
    plat_text(c, PX + 16, PY + 160, 300, 16, "Display", FONT_BOLD, C_TEXT, TX_LEFT);
    mm_fill(c, PX + 16, PY + 176, PW - 32, 1, 0);
    int y = MOD_Y[3];
    plat_text(c, PX + 16, y, 70, 16, "Layout", FONT_BOLD, C_TEXT, TX_LEFT | TX_VCENTER);
    static const char *const LAY[3] = {"Auto", "Classic", "Touch"};
    for (int k = 0; k < 3; k++) {
        int h = hv == 110 + k;
        mm_disc(c, LAY_X[k] + 7, y + 8, 6, h ? C_RED : C_LINE);
        mm_disc(c, LAY_X[k] + 7, y + 8, 5, 0xFFFFFF);
        if (mm_opt.layout == k) mm_disc(c, LAY_X[k] + 7, y + 8, 3, C_TEXT);
        plat_text(c, LAY_X[k] + 18, y, 78, 16, LAY[k], FONT_TEXT, h ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER);
    }
    plat_text(c, PX + 36, y + 17, PW - 52, 30, "Touch: big buttons beside the room, below it on an upright screen. "
              "Auto: touch on phones and tablets, classic with a mouse.", FONT_TEXT, C_GREY, TX_WRAP);
    mod_row(c, 4, mm_opt.bigtext, "Large text and high contrast", "bigger question and message text, darker text and lines", hv);
    mod_row(c, 5, mm_opt.anim, "Animations", "room changes, answers, dialogs and rewards move", hv);
    {                                                       /* the "Classic MindMaze" switch (user: toggled, all off) */
        int on = mm_options_classic_on(), h = hv == 120, y = PY + PH - 30;
        checkbox(c, PX + 16, y + 3, on, h);
        plat_text(c, PX + 36, y, 130, 22, "Classic MindMaze", FONT_BOLD, h ? C_RED : C_TEXT, TX_LEFT | TX_VCENTER);
        plat_text(c, PX + 158, y, PW - 270, 22, "all modern options off", FONT_TEXT, C_GREY, TX_LEFT | TX_VCENTER | TX_ELLIPSIS);
    }
    button(c, PX + PW - 96, PY + PH - 30, 80, 20, "Back", hv == 121);
}

/* ---- actions ---- */
static void with_commas(int n, char *o, size_t k)
{
    if (n >= 1000) snprintf(o, k, "%d,%03d", n / 1000, n % 1000);
    else snprintf(o, k, "%d", n);
}

/* the player chose a disc and the platform reads it (web: in a worker, can take a while) */
void mm_disc_reading(void)
{
    S.busy = 1;
    S.status[0] = 0;
    plat_invalidate();
}

/* the platform has read the disc (or the player cancelled): register the source and say what it added */
void mm_disc_added(int ok, const char *id, const char *name, int count, const char *msg)
{
    S.busy = 0;
    S.status[0] = 0;
    S.err = 0;
    if (!ok) {
        if (msg && *msg) { snprintf(S.status, sizeof S.status, "%s", msg); S.err = 1; }
        plat_invalidate();
        return;
    }
    char err[256], all[32];
    int i = mm_sources_add_edition(id, name, count, err, sizeof err);
    if (i < 0) { snprintf(S.status, sizeof S.status, "%s", err); S.err = 1; plat_invalidate(); return; }
    with_commas(count, all, sizeof all);
    char nb[32];                                             /* the info line starts with the number loaded */
    const char *info = mm_source_info(i);
    int k = 0;
    for (const char *p = info; *p && k < 30 && ((*p >= '0' && *p <= '9') || *p == ','); p++) nb[k++] = *p;
    nb[k] = 0;
    if (!strcmp(nb, "0"))
        snprintf(S.status, sizeof S.status, "Added %s: all of its %s questions are already in a source above, so it "
                 "adds nothing new.", name, all);
    else if (strstr(info, " new of "))
        snprintf(S.status, sizeof S.status, "Added %s: %s questions, %s of them new (the others are already in a source "
                 "above and are left out). Switched on.", name, all, nb);
    else snprintf(S.status, sizeof S.status, "Added %s: %s questions. Switched on.", name, all);
    plat_invalidate();
}
static void close_settings(void)
{
    char err[256];
    if (!mm_sources_apply(err, sizeof err)) {                 /* stays open with the reason */
        snprintf(S.status, sizeof S.status, "%s", err);
        S.err = 1;
        plat_invalidate();
        return;
    }
    S.on = 0;
    plat_invalidate();
}

static void save_question(void)
{
    char err[256];
    const char *const a[4] = {S.f[F_CORRECT], S.f[F_WRONG1], S.f[F_WRONG2], S.f[F_WRONG3]};
    if (!mm_custom_add(S.f[F_QUESTION], a, S.area, S.level, err, sizeof err)) {
        snprintf(S.status, sizeof S.status, "%s", err);
        S.err = 1;
    } else {
        snprintf(S.status, sizeof S.status, "Saved. You have %d question%s of your own. Type the next one, or Back.",
                 mm_custom_count(), mm_custom_count() == 1 ? "" : "s");
        S.err = 0;
        for (int f = 0; f < NFIELD; f++) S.f[f][0] = 0;
        S.focus = F_QUESTION;
    }
    plat_invalidate();
}

static void click(int id)
{
    if (!id) return;
    if (id == 1) { if (S.page) { S.page = 0; S.status[0] = 0; plat_invalidate(); } else close_settings(); return; }
    if (id >= 10 && id < 10 + mm_source_count()) { mm_source_set(id - 10, !mm_source_enabled(id - 10)); S.status[0] = 0; }
    else if (id == 30) mm_set_skip_seen(!mm_skip_seen);
    else if (id == 83) plat_export_data();
    else if (id == 84) {                                     /* questions from another Encarta disc */
        S.status[0] = 0;
        plat_invalidate();
        plat_add_disc();             /* asks for the disc; mm_disc_reading while it reads, then mm_disc_added */
        return;
    }
    else if (id == 82) plat_instrument_set((plat_instrument_get() + 1) % plat_instrument_count());   /* next SoundFont */
    else if (id == 34) mm_cmd(CMD_WIDE);                     /* resizes the window at once, saved like F3 */
    else if (id == 36) mm_cmd(CMD_HD);                       /* saved like F2 */
    else if (id == 35) { S.page = PAGE_PICTURES; S.status[0] = 0; S.art_waiting = mm_art_results_waiting(); }
    else if (id == 37) plat_open_url(HD_ART_URL);
    else if (id == 38) { if (!plat_art_import(1)) plat_open_data_folder("art"); }
    else if (id == 39) { S.page = PAGE_SOURCES; S.status[0] = 0; }
    else if (id == 122) { S.page = PAGE_MODERN; S.status[0] = 0; }
    else if (id >= 100 && id <= 105 && id != 103) {
        int *o[6] = {&mm_opt.resume, &mm_opt.hints, &mm_opt.prompts, NULL, &mm_opt.bigtext, &mm_opt.anim};
        *o[id - 100] = !*o[id - 100];
        mm_options_save();
    }
    else if (id >= 110 && id <= 112) { mm_opt.layout = id - 110; mm_options_save(); }
    else if (id == 120) {
        mm_options_classic_set(!mm_options_classic_on());
        snprintf(S.status, sizeof S.status, "%s", mm_options_classic_on() ? "All modern options are off: MindMaze plays and looks like the original."
                 : "Modern options are back as you had them.");
        S.err = 0;
    }
    else if (id == 121) { S.page = PAGE_SOURCES; S.status[0] = 0; }
    else if (id == 90) {                                     /* cut the agent's results into hd/ and wide/ */
        plat_set_cursor(0);
        char rep[600];
        int n = mm_art_install(rep, sizeof rep);
        if (n > 0) {
            if (mm_hd_available() && !mm_hd_on) mm_cmd(CMD_HD);
        }
        snprintf(S.status, sizeof S.status, "%s", n > 0 ? rep : n == 0 ? "No results from the agent yet (data/art/work/*/out_gen)." : rep);
        for (char *q = S.status; *q; q++) if (*q == '\n') *q = ' ';
        S.err = n < 0;
        S.art_waiting = 0;
    }
    else if (id == 31) {
        mm_seen_forget_all();
        snprintf(S.status, sizeof S.status, "The list of seen questions is empty: every question can come up again.");
        S.err = 0;
    } else if (id == 32) { S.page = 1; S.focus = F_QUESTION; S.status[0] = 0; }
    else if (id == 33) { close_settings(); return; }
    else if (id >= 40 && id < 40 + NFIELD) S.focus = id - 40;
    else if (id == 50) S.area = (S.area + 8) % 9;
    else if (id == 51) S.area = (S.area + 1) % 9;
    else if (id >= 60 && id < 64) S.level = id - 60;
    else if (id == 70) { save_question(); return; }
    else if (id == 71) { S.page = 0; S.status[0] = 0; }
    plat_invalidate();
}

void mm_settings_mouse(int x, int y, int button, int action)
{
    if (action != MA_DOWN || button != MMB_LEFT) return;
    int id = mm_settings_hit(x, y);
    if (id == 80 || id == 81) {                               /* volume bar: set from the click position */
        int v = vol_from_x(x);
        mm_set_volume(id == 80 ? v : mm_music_volume, id == 81 ? v : mm_sound_volume);
        plat_invalidate();
        return;
    }
    click(id);
}

static int chars(const char *s) { int n = 0; for (; *s; s++) n += (*s & 0xC0) != 0x80; return n; }

void mm_settings_key(int key, int mods)
{
    if (key == KEY_ESC) { click(1); return; }
    if (S.page != PAGE_ADD) { if (key == KEY_ENTER && S.page == PAGE_SOURCES) close_settings(); return; }
    char *t = S.f[S.focus];
    if (key == KEY_TAB) S.focus = (S.focus + ((mods & MMOD_SHIFT) ? NFIELD - 1 : 1)) % NFIELD;
    else if (key == KEY_ENTER) { if (S.focus == F_WRONG3) { save_question(); return; } S.focus++; }
    else if (key == KEY_BACK) {
        size_t n = strlen(t);
        while (n && (t[n - 1] & 0xC0) == 0x80) n--;
        if (n) t[n - 1] = 0;
    } else return;
    plat_invalidate();
}

void mm_settings_char(unsigned ch)
{
    if (!S.on || S.page != PAGE_ADD || ch < 32) return;
    char *t = S.f[S.focus];
    size_t n = strlen(t);
    if (chars(t) >= FIELD_MAX[S.focus] || n + 4 >= sizeof S.f[0]) return;
    if (ch < 0x80) t[n++] = (char)ch;
    else if (ch < 0x800) { t[n++] = (char)(0xC0 | ch >> 6); t[n++] = (char)(0x80 | (ch & 63)); }
    else { t[n++] = (char)(0xE0 | ch >> 12); t[n++] = (char)(0x80 | ((ch >> 6) & 63)); t[n++] = (char)(0x80 | (ch & 63)); }
    t[n] = 0;
    plat_invalidate();
}
