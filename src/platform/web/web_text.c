/* Text for platforms without GDI (web / Android): open fonts rasterised with stb_truetype, laid out like the
 * Windows build's DrawText (alignment, word wrap, end ellipsis) so the panel layout of the core stays the same.
 *
 * Fonts (fonts/, open licences): Liberation Sans for Tahoma and Arial, Liberation Serif for Times New Roman (both
 * metric-compatible with the Windows fonts), TeX Gyre Pagella Italic for Palatino Linotype Italic (level numerals).
 * Sizes are the Windows build's: the em height in pixels (CreateFont with a negative height), times the scale. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#include "web_text.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb_truetype.h"

enum { F_SANS, F_SANS_B, F_SANS_I, F_SERIF, F_SERIF_B, F_PAGELLA_I, NFACE };
static const char *const FACE_FILE[NFACE] = {"LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf",
                                             "LiberationSans-Italic.ttf", "LiberationSerif-Regular.ttf",
                                             "LiberationSerif-Bold.ttf", "texgyrepagella-italic.otf"};
static struct { stbtt_fontinfo info; unsigned char *data; int ok; } g_face[NFACE];

/* the Windows build's fonts (win32_main.c make_fonts) */
static const struct { int face, px; } FONTDEF[FONT_COUNT] = {
    [FONT_TEXT] = {F_SANS, 11},    [FONT_BOLD] = {F_SANS_B, 11},   [FONT_LIST] = {F_SANS, 13},
    [FONT_SMALL] = {F_SANS, 9},    [FONT_NAME] = {F_SERIF, 18},    [FONT_LEVEL] = {F_PAGELLA_I, 25},
    [FONT_ITALIC] = {F_SANS_I, 11}, [FONT_POINTS] = {F_SERIF_B, 24}, [FONT_SCORE] = {F_SANS, 12},
    [FONT_ANSWER] = {F_SANS, 11},  [FONT_ANSWER_I] = {F_SANS_I, 11},
};

int text_init(const char *dir)
{
    int ok = 1;
    for (int i = 0; i < NFACE; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, FACE_FILE[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { ok = 0; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        g_face[i].data = malloc((size_t)n);
        if (g_face[i].data && fread(g_face[i].data, 1, (size_t)n, f) == (size_t)n &&
            stbtt_InitFont(&g_face[i].info, g_face[i].data, stbtt_GetFontOffsetForIndex(g_face[i].data, 0)))
            g_face[i].ok = 1;
        else ok = 0;
        fclose(f);
    }
    return ok;
}

/* ---- metrics and glyph cache ---- */
typedef struct { int face, px, cp; int w, h, xoff, yoff, adv; unsigned char *bmp; } Glyph;
#define GCACHE 8192
static Glyph g_cache[GCACHE];

static int face_of(int font) { int f = FONTDEF[font].face; return g_face[f].ok ? f : F_SANS; }

static void vmetrics(int font, int scale, int *ascent, int *height)
{
    int f = face_of(font), px = FONTDEF[font].px * scale, a, d, g;
    if (!g_face[f].ok) { *ascent = px; *height = px + px / 4; return; }
    float s = stbtt_ScaleForMappingEmToPixels(&g_face[f].info, (float)px);
    stbtt_GetFontVMetrics(&g_face[f].info, &a, &d, &g);
    *ascent = (int)lroundf(a * s);
    *height = *ascent + (int)lroundf(-d * s);
}

static const Glyph *glyph(int font, int scale, int cp)
{
    int f = face_of(font), px = FONTDEF[font].px * scale;
    unsigned h = ((unsigned)cp * 2654435761u ^ (unsigned)px * 40503u ^ (unsigned)f * 9973u) & (GCACHE - 1);
    for (int n = 0; n < 64; n++, h = (h + 1) & (GCACHE - 1)) {
        Glyph *g = &g_cache[h];
        if (g->px && g->face == f && g->px == px && g->cp == cp) return g;
        if (!g->px) {
            g->face = f;
            g->px = px;
            g->cp = cp;
            if (!g_face[f].ok) { g->adv = px / 2; return g; }
            stbtt_fontinfo *fi = &g_face[f].info;
            float s = stbtt_ScaleForMappingEmToPixels(fi, (float)px);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(fi, cp, &adv, &lsb);
            g->adv = (int)lroundf(adv * s);                     /* whole-pixel advances, as GDI */
            g->bmp = stbtt_GetCodepointBitmap(fi, s, s, cp, &g->w, &g->h, &g->xoff, &g->yoff);
            return g;
        }
    }
    static Glyph none;                                          /* cache crowded: draw nothing */
    none.adv = px / 2;
    return &none;
}

static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}

static int measure_n(const char *s, int n, int font, int scale)       /* first n bytes */
{
    int w = 0;
    const char *e = s + n;
    while (s < e && *s) w += glyph(font, scale, utf8_next(&s))->adv;
    return w;
}

static void draw_run(MMCanvas *c, int X, int Y, const char *s, int n, int font, uint32_t rgb, int cx0, int cy0, int cx1, int cy1)
{
    int asc, hgt;
    vmetrics(font, c->scale, &asc, &hgt);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > c->w) cx1 = c->w;
    if (cy1 > c->h) cy1 = c->h;
    int r0 = rgb >> 16 & 255, g0 = rgb >> 8 & 255, b0 = rgb & 255;
    const char *e = s + n;
    while (s < e && *s) {
        const Glyph *g = glyph(font, c->scale, utf8_next(&s));
        int gx = X + g->xoff, gy = Y + asc + g->yoff;
        for (int y = 0; g->bmp && y < g->h; y++) {
            int py = gy + y;
            if (py < cy0 || py >= cy1) continue;
            uint32_t *row = c->px + (size_t)py * c->stride;
            for (int x = 0; x < g->w; x++) {
                int px = gx + x, a = g->bmp[y * g->w + x];
                if (!a || px < cx0 || px >= cx1) continue;
                uint32_t d = row[px];
                int r = d >> 16 & 255, gg = d >> 8 & 255, b = d & 255;
                r += (r0 - r) * a / 255;
                gg += (g0 - gg) * a / 255;
                b += (b0 - b) * a / 255;
                row[px] = (uint32_t)r << 16 | (uint32_t)gg << 8 | (uint32_t)b;
            }
        }
        X += g->adv;
    }
}

/* ---- the platform text functions (mm.h) ---- */
int text_px_width(MMCanvas *c, const char *s, int font) { return measure_n(s, (int)strlen(s), font, c->scale); }
int text_px_height(MMCanvas *c, int font) { int a, h; vmetrics(font, c->scale, &a, &h); return h; }
int text_px_ascent(MMCanvas *c, int font) { int a, h; vmetrics(font, c->scale, &a, &h); return a; }

void text_px(MMCanvas *c, int X, int Y, const char *s, int font, uint32_t rgb, int cx0, int cy0, int cx1, int cy1)
{
    int m = c->ox * c->scale;                                   /* widescreen margin (callers use view pixels) */
    draw_run(c, X + m, Y, s, (int)strlen(s), font, rgb, cx0 + m, cy0, cx1 + m, cy1);
}

int text_width(const char *s, int font, int scale)
{
    return measure_n(s, (int)strlen(s), font, scale) / (scale ? scale : 1);
}

/* DrawText: single line (left / centre / right, top or vertically centred, optional end ellipsis) or word wrap */
void text_box(MMCanvas *c, int x, int y, int w, int h, const char *s, int font, uint32_t rgb, int flags)
{
    int sc = c->scale;
    int X0 = (x + c->ox) * sc, Y0 = y * sc, W = w * sc, H = h * sc;
    int asc, lh;
    vmetrics(font, sc, &asc, &lh);
    int cx1 = X0 + W, cy1 = Y0 + H;
    if (!(flags & TX_WRAP)) {
        int n = (int)strlen(s), tw = measure_n(s, n, font, sc);
        char buf[1024];
        if ((flags & TX_ELLIPSIS) && tw > W) {
            int ew = measure_n("...", 3, font, sc);
            while (n > 0) {
                do n--; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80);
                if (measure_n(s, n, font, sc) + ew <= W) break;
            }
            if (n > (int)sizeof buf - 4) n = (int)sizeof buf - 4;
            memcpy(buf, s, (size_t)n);
            memcpy(buf + n, "...", 4);
            s = buf;
            n += 3;
            tw = measure_n(s, n, font, sc);
        }
        int X = flags & TX_CENTER ? X0 + (W - tw) / 2 : flags & TX_RIGHT ? X0 + W - tw : X0;
        int Y = flags & TX_VCENTER ? Y0 + (H - lh) / 2 : Y0;
        draw_run(c, X, Y, s, n, font, rgb, X0, Y0, cx1, cy1);
        return;
    }
    /* word wrap: break at spaces, at '\n', and inside a word that alone is wider than the box */
    int Y = Y0;
    const char *p = s;
    while (*p && Y < cy1) {
        const char *line = p, *brk = NULL, *q = p;
        int width = 0;
        while (*q && *q != '\n') {
            const char *nq = q;
            int adv = glyph(font, sc, utf8_next(&nq))->adv;
            if (width + adv > W && q > line) {
                if (!brk) brk = q;                              /* no space on the line: break the word */
                break;
            }
            if (*q == ' ') brk = q;
            width += adv;
            q = nq;
        }
        const char *end = (*q && *q != '\n') ? brk : q;
        int n = (int)(end - line);
        while (n > 0 && line[n - 1] == ' ') n--;
        int tw = measure_n(line, n, font, sc);
        int X = flags & TX_CENTER ? X0 + (W - tw) / 2 : flags & TX_RIGHT ? X0 + W - tw : X0;
        draw_run(c, X, Y, line, n, font, rgb, X0, Y0, cx1, cy1);
        Y += lh;
        p = end;
        if (*p == '\n') p++;
        else while (*p == ' ') p++;
    }
}
