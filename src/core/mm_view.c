/* MindMaze - Modern options and the view (layout) the platform draws (CHANGE 2026-11-21, user: continue games, hints,
 * turn prompts + stats, touch layout incl. portrait, large text / high contrast, animations; each can be switched off
 * in Settings > Modern options, all off = the classic game). See mm.h for the view sizes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"

MMOptions mm_opt = {1, 1, 1, MM_LAYOUT_AUTO, 0, 1};

static const char *const KEY[6] = {"opt_resume", "opt_hints", "opt_prompts", "opt_layout", "opt_bigtext", "opt_anim"};

static int *field(int i)
{
    int *f[6] = {&mm_opt.resume, &mm_opt.hints, &mm_opt.prompts, &mm_opt.layout, &mm_opt.bigtext, &mm_opt.anim};
    return f[i];
}

static int g_view = MM_VIEW_CLASSIC, g_scr_w = 4, g_scr_h = 3, g_touch;

static int choose_view(void)
{
    int touch = mm_opt.layout == MM_LAYOUT_TOUCH_ONLY || (mm_opt.layout == MM_LAYOUT_AUTO && g_touch);
    if (!touch) return MM_VIEW_CLASSIC;
    /* (user 2026-11-21: portrait only if the game can take at least half the screen - it cannot: an upright phone
     * shows the room at about a fifth of its height - so portrait is not supported; the landscape layout is used and
     * the platform asks to turn the device. MM_VIEW_TOUCH_TALL stays in the code, unused.) */
    return MM_VIEW_TOUCH_WIDE;
}

/* the view's size (CHANGE 2026-11-21, user: the touch layout uses the widescreen art where it helps, and no empty
 * space): landscape 636 or 764 (widescreen art, when made and switched on) + the 300 column, 437 tall; portrait 636 x
 * 1311 (the room, the strip, the column at zoom 2). The rest of the screen is the platform's letterbox. */
static void geom(int *w, int *h, int *ext)
{
    *ext = 0;
    if (g_view == MM_VIEW_CLASSIC) { *ext = mm_view_ext(); *w = MM_W + 2 * *ext; *h = MM_H; return; }
    if (g_view == MM_VIEW_TOUCH_TALL) { *w = MM_W; *h = MM_H + 874; return; }
    /* (user 2026-11-21: no stretched, empty panel space) the height stays 437; of the original and the widescreen room
     * the one shown bigger on this screen wins - the original on most phones, the wide pictures on very wide screens */
    double best = -1;
    int cand[2] = {0, mm_view_ext()};
    for (int k = 0; k < (cand[1] ? 2 : 1); k++) {
        int W = MM_W + 2 * cand[k] + 300, H = MM_H;
        double s = (double)g_scr_w / W < (double)g_scr_h / H ? (double)g_scr_w / W : (double)g_scr_h / H;
        double room = s * s * (MM_W + 2 * cand[k]);           /* the room's area on the screen */
        if (room > best) { best = room; *w = W; *h = H; *ext = cand[k]; }
    }
}

static int g_lw, g_lh, g_lext;
static void relayout(void)
{
    int v = choose_view(), w, h, e;
    int changed = v != g_view;
    g_view = v;
    geom(&w, &h, &e);
    if (changed || w != g_lw || h != g_lh || e != g_lext) {
        g_lw = w; g_lh = h; g_lext = e;
        plat_view_changed();
    }
    plat_invalidate();
}

void mm_options_load(void)
{
    char v[32];
    for (int i = 0; i < 6; i++)
        if (plat_get_setting(KEY[i], v, sizeof v)) *field(i) = atoi(v);
    if (mm_opt.layout < MM_LAYOUT_AUTO || mm_opt.layout > MM_LAYOUT_TOUCH_ONLY) mm_opt.layout = MM_LAYOUT_AUTO;
    g_view = choose_view();
}

void mm_options_save(void)
{
    char v[16];
    for (int i = 0; i < 6; i++) {
        snprintf(v, sizeof v, "%d", *field(i));
        plat_set_setting(KEY[i], v);
    }
    relayout();
}

/* "Classic MindMaze" is a switch (CHANGE 2026-11-21, user: "a classic option that can be toggled to disable all options"):
 * on = every modern option off and the classic layout, the choices before kept in "opt_before"; off = those choices
 * back (or the defaults when there are none). On whenever everything is off, however it got there. */
int mm_options_classic_on(void)
{
    return !mm_opt.resume && !mm_opt.hints && !mm_opt.prompts && !mm_opt.bigtext && !mm_opt.anim && mm_opt.layout == MM_LAYOUT_CLASSIC_ONLY;
}

void mm_options_classic_set(int on)
{
    char v[64];
    if (on == mm_options_classic_on()) return;
    if (on) {
        snprintf(v, sizeof v, "%d,%d,%d,%d,%d,%d", mm_opt.resume, mm_opt.hints, mm_opt.prompts, mm_opt.layout, mm_opt.bigtext, mm_opt.anim);
        plat_set_setting("opt_before", v);
        mm_opt.resume = mm_opt.hints = mm_opt.prompts = mm_opt.bigtext = mm_opt.anim = 0;
        mm_opt.layout = MM_LAYOUT_CLASSIC_ONLY;
    } else {
        MMOptions d = {1, 1, 1, MM_LAYOUT_AUTO, 0, 1}, b;
        if (plat_get_setting("opt_before", v, sizeof v) &&
            sscanf(v, "%d,%d,%d,%d,%d,%d", &b.resume, &b.hints, &b.prompts, &b.layout, &b.bigtext, &b.anim) == 6 &&
            b.layout >= MM_LAYOUT_AUTO && b.layout <= MM_LAYOUT_TOUCH_ONLY) d = b;
        mm_opt = d;
        if (mm_options_classic_on()) mm_opt = (MMOptions){1, 1, 1, MM_LAYOUT_AUTO, 0, 1};   /* (saved all off: the defaults) */
    }
    mm_options_save();
}

void mm_options_classic(void) { mm_options_classic_set(1); }

void mm_set_screen(int w, int h, int touch)
{
    if (w <= 0 || h <= 0) return;
    g_scr_w = w;
    g_scr_h = h;
    g_touch = touch != 0;
    relayout();
}

int mm_view(void) { return g_view; }
int mm_view_w(void) { int w, h, e; geom(&w, &h, &e); return w; }
int mm_view_h(void) { int w, h, e; geom(&w, &h, &e); return h; }
int mm_view_ox(void) { int w, h, e; geom(&w, &h, &e); return e; }
/* the strip under the room (touch views): 136 units, more when the screen's shape leaves room */
int mm_view_strip(void)
{
    int w, h, e;
    geom(&w, &h, &e);
    return g_view == MM_VIEW_TOUCH_WIDE ? h - 301 : g_view == MM_VIEW_TOUCH_TALL ? h - 874 - 301 : 136;   /* (136 now) */
}

MMCanvas mm_canvas_sub(const MMCanvas *c, int x, int y, int w, int h, int zoom)
{
    MMCanvas s = *c;
    int X = (x + c->ox) * c->scale, Y = y * c->scale;
    if (X < 0) X = 0;
    if (Y < 0) Y = 0;
    if (X > c->w) X = c->w;
    if (Y > c->h) Y = c->h;
    s.px = c->px + (size_t)Y * c->stride + X;
    if (zoom < 1) zoom = 1;
    s.scale = c->scale * zoom;                                /* w, h: in the sub-canvas' own units */
    s.w = w * s.scale < c->w - X ? w * s.scale : c->w - X;
    s.h = h * s.scale < c->h - Y ? h * s.scale : c->h - Y;
    s.ox = 0;
    s.orgx = c->orgx + X;
    s.orgy = c->orgy + Y;
    return s;
}
