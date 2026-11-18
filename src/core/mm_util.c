/* Utilities: the original's random generator, bitmap loading, drawing primitives, strings. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"

/* ---- MSVC CRT rand(): the original uses srand()/rand() from the C runtime ---- */
static unsigned g_seed = 1;
void mm_srand(unsigned seed) { g_seed = seed; }
int mm_rand(void)
{
    g_seed = g_seed * 214013u + 2531011u;
    return (int)((g_seed >> 16) & 0x7fff);
}

/* ---- strings ---- */
char *mm_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    memcpy(p, s, n);
    return p;
}

static const unsigned short cp1252_hi[32] = {
    0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,
    0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
    0x0153, 0x9D, 0x017E, 0x0178};

char *mm_cp1252_to_utf8(const unsigned char *s, int len)
{
    char *out = malloc(len * 3 + 1), *o = out;
    for (int i = 0; i < len; i++) {
        unsigned c = s[i];
        if (c >= 0x80 && c < 0xA0) c = cp1252_hi[c - 0x80];
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
        else { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | ((c >> 6) & 63)); *o++ = (char)(0x80 | (c & 63)); }
    }
    *o = 0;
    return out;
}

/* ---- bitmaps: 8-bit BMP/DIB (palette index 0xFE = transparent, as FUN_0052f390 draws them) and 24-bit ---- */
static MMImage *decode_bmp(const unsigned char *d, int size, int keyed)
{
    if (size < 54 || d[0] != 'B' || d[1] != 'M') return NULL;
    unsigned off = d[10] | d[11] << 8 | d[12] << 16 | (unsigned)d[13] << 24;
    unsigned hs = d[14] | d[15] << 8;
    int w = d[18] | d[19] << 8 | d[20] << 16 | d[21] << 24;
    int h = d[22] | d[23] << 8 | d[24] << 16 | d[25] << 24;
    int bpp = d[28] | d[29] << 8;
    int topdown = h < 0;
    if (topdown) h = -h;
    if (w <= 0 || h <= 0 || (bpp != 8 && bpp != 24)) return NULL;
    const unsigned char *pal = d + 14 + hs;
    int stride = ((w * bpp / 8) + 3) & ~3;
    if (off + (unsigned)stride * h > (unsigned)size) return NULL;
    MMImage *im = malloc(sizeof *im);
    im->w = w;
    im->h = h;
    im->px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const unsigned char *row = d + off + (size_t)stride * (topdown ? y : h - 1 - y);
        uint32_t *o = im->px + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            if (bpp == 8) {
                int i = row[x];
                const unsigned char *p = pal + i * 4;
                o[x] = (keyed && i == 0xFE) ? 0 : 0xFF000000u | p[2] << 16 | p[1] << 8 | p[0];
            } else {
                const unsigned char *p = row + x * 3;
                o[x] = 0xFF000000u | p[2] << 16 | p[1] << 8 | p[0];
            }
        }
    }
    return im;
}

static struct { char name[40]; MMImage *im; } g_cache[160];
static int g_ncache;

MMImage *mm_image(const char *name)
{
    for (int i = 0; i < g_ncache; i++)
        if (!strcmp(g_cache[i].name, name)) return g_cache[i].im;
    int size = 0;
    char path[80];
    strcpy(path, name);
    for (char *p = path; *p; p++)                       /* assets are stored with lower-case names */
        if (*p >= 'A' && *p <= 'Z') *p += 32;
    unsigned char *d = plat_load(path, &size);
    MMImage *im = d ? decode_bmp(d, size, strncmp(path, "res/", 4) != 0) : NULL;
    free(d);
    if (g_ncache < (int)(sizeof g_cache / sizeof g_cache[0])) {
        strncpy(g_cache[g_ncache].name, name, sizeof g_cache[0].name - 1);
        g_cache[g_ncache++].im = im;
    }
    return im;
}

/* ---- drawing (logical coordinates, scaled by c->scale) ---- */
void mm_fill(MMCanvas *c, int x, int y, int w, int h, uint32_t rgb)
{
    x += c->ox;
    int s = c->scale, x0 = x * s, y0 = y * s, x1 = (x + w) * s, y1 = (y + h) * s;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *p = c->px + (size_t)yy * c->stride;
        for (int xx = x0; xx < x1; xx++) p[xx] = rgb;
    }
}

/* Anti-aliased filled circle, centre and radius in logical pixels (drawn at full canvas resolution). */
void mm_disc(MMCanvas *c, double cx, double cy, double r, uint32_t rgb)
{
    cx += c->ox;
    int s = c->scale;
    int X0 = (int)((cx - r - 1) * s), X1 = (int)((cx + r + 1) * s) + 1;
    int Y0 = (int)((cy - r - 1) * s), Y1 = (int)((cy + r + 1) * s) + 1;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > c->w) X1 = c->w;
    if (Y1 > c->h) Y1 = c->h;
    double R = r * s, CX = cx * s, CY = cy * s;
    for (int Y = Y0; Y < Y1; Y++)
        for (int X = X0; X < X1; X++) {
            double dx = X + 0.5 - CX, dy = Y + 0.5 - CY;
            double cov = R + 0.5 - __builtin_sqrt(dx * dx + dy * dy);
            if (cov <= 0) continue;
            uint32_t *d = &c->px[(size_t)Y * c->stride + X];
            if (cov >= 1) { *d = rgb; continue; }
            uint32_t a = (uint32_t)(cov * 255), na = 255 - a, p = *d;
            *d = ((((rgb >> 16) & 255) * a + ((p >> 16) & 255) * na) / 255) << 16 |
                 ((((rgb >> 8) & 255) * a + ((p >> 8) & 255) * na) / 255) << 8 |
                 (((rgb & 255) * a + (p & 255) * na) / 255);
        }
}

/* Filled ellipse, anti-aliased by 4x4 supersampling per canvas pixel (smooth at every scale). (cx, cy) and the radii
 * are in logical pixels with pixel edges on whole numbers, like mm_disc. */
void mm_ellipse(MMCanvas *c, double cx, double cy, double rx, double ry, uint32_t rgb)
{
    cx += c->ox;
    int s = c->scale;
    double CX = cx * s, CY = cy * s, RX = rx * s, RY = ry * s;
    int X0 = (int)(CX - RX) - 1, X1 = (int)(CX + RX) + 2, Y0 = (int)(CY - RY) - 1, Y1 = (int)(CY + RY) + 2;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > c->w) X1 = c->w;
    if (Y1 > c->h) Y1 = c->h;
    for (int Y = Y0; Y < Y1; Y++)
        for (int X = X0; X < X1; X++) {
            int n = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    double dx = (X + (i + 0.5) / 4 - CX) / RX, dy = (Y + (j + 0.5) / 4 - CY) / RY;
                    n += dx * dx + dy * dy <= 1.0;
                }
            if (!n) continue;
            uint32_t *d = &c->px[(size_t)Y * c->stride + X];
            if (n == 16) { *d = rgb; continue; }
            uint32_t a = (uint32_t)(n * 255 / 16), na = 255 - a, p = *d;
            *d = ((((rgb >> 16) & 255) * a + ((p >> 16) & 255) * na) / 255) << 16 |
                 ((((rgb >> 8) & 255) * a + ((p >> 8) & 255) * na) / 255) << 8 |
                 (((rgb & 255) * a + (p & 255) * na) / 255);
        }
}

void mm_frame(MMCanvas *c, int x, int y, int w, int h, int t, uint32_t rgb)
{
    mm_fill(c, x, y, w, t, rgb);
    mm_fill(c, x, y + h - t, w, t, rgb);
    mm_fill(c, x, y, t, h, rgb);
    mm_fill(c, x + w - t, y, t, h, rgb);
}

/* Draw image region (sx, sy, sw, sh) whose pixels are 1/k original pixels, with its top-left at (ox, oy) in
 * 1/k units, onto the canvas (scale S canvas pixels per original pixel). Canvas pixel X shows unit
 * t = X*k/S (nearest). Alpha is blended (HD pictures have soft edges; originals are 0 or 255). */
/* Smooth (bilinear) enlargement of an image region: used for HD pictures when the canvas scale is larger
 * than their own (2x art shown at 3x/4x), so they are not drawn as hard 2x2 blocks. Samples stay inside the
 * region (button strips: no bleeding from the next frame); colours are alpha-weighted. */
static void blit_smooth(MMCanvas *c, const MMImage *im, int sx, int sy, int sw, int sh, int ox, int oy, int k)
{
    ox += c->ox * k;
    int s = c->scale;
    int X0 = ox * s / k, X1 = (ox + sw) * s / k, Y0 = oy * s / k, Y1 = (oy + sh) * s / k;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > c->w) X1 = c->w;
    if (Y1 > c->h) Y1 = c->h;
    if (X1 <= X0 || Y1 <= Y0) return;
    static int *u0, *fu;
    static int cap;
    if (X1 - X0 > cap) { cap = X1 - X0; u0 = realloc(u0, sizeof(int) * cap); fu = realloc(fu, sizeof(int) * cap); }
    int maxu = sw - 1 < im->w - 1 - sx ? sw - 1 : im->w - 1 - sx, maxv = sh - 1 < im->h - 1 - sy ? sh - 1 : im->h - 1 - sy;
    for (int X = X0; X < X1; X++) {                         /* source position of the pixel centre, 8-bit fraction */
        int f = ((2 * X + 1) * k * 128) / s - 128 - ox * 256;
        if (f < 0) f = 0;
        if (f > maxu * 256) f = maxu * 256;
        u0[X - X0] = f >> 8;
        fu[X - X0] = f & 255;
    }
    for (int Y = Y0; Y < Y1; Y++) {
        int f = ((2 * Y + 1) * k * 128) / s - 128 - oy * 256;
        if (f < 0) f = 0;
        if (f > maxv * 256) f = maxv * 256;
        int v = f >> 8, fv = f & 255, v1 = v < maxv ? v + 1 : v;
        const uint32_t *r0 = im->px + (size_t)(sy + v) * im->w + sx, *r1 = im->px + (size_t)(sy + v1) * im->w + sx;
        uint32_t *dst = c->px + (size_t)Y * c->stride;
        for (int X = X0; X < X1; X++) {
            int u = u0[X - X0], u1 = u < maxu ? u + 1 : u, wu = fu[X - X0];
            uint32_t p[4] = {r0[u], r0[u1], r1[u], r1[u1]};
            int w[4] = {(256 - wu) * (256 - fv), wu * (256 - fv), (256 - wu) * fv, wu * fv};
            unsigned A = 0, R = 0, G = 0, B = 0;
            for (int i = 0; i < 4; i++) {
                unsigned a = (p[i] >> 24) * (unsigned)w[i];
                A += a;
                R += ((p[i] >> 16) & 255) * (a >> 8);
                G += ((p[i] >> 8) & 255) * (a >> 8);
                B += (p[i] & 255) * (a >> 8);
            }
            if (!A) continue;
            unsigned al = A >> 16, aw = A >> 8;                  /* alpha 0..255; colour = sum(c*a)/sum(a) */
            uint32_t r = R / aw, g = G / aw, b = B / aw;
            if (al >= 255) dst[X] = r << 16 | g << 8 | b;
            else {
                uint32_t d = dst[X], na = 255 - al;
                dst[X] = ((r * al + ((d >> 16) & 255) * na) / 255) << 16 | ((g * al + ((d >> 8) & 255) * na) / 255) << 8 |
                         ((b * al + (d & 255) * na) / 255);
            }
        }
    }
}

static void blit_k(MMCanvas *c, const MMImage *im, int sx, int sy, int sw, int sh, int ox, int oy, int k)
{
    if (!im) return;
    if (k > 1 && c->scale > k) { blit_smooth(c, im, sx, sy, sw, sh, ox, oy, k); return; }   /* adds the margin itself */
    ox += c->ox * k;
    int s = c->scale;
    int X0 = ox * s / k - 1, X1 = (ox + sw) * s / k + 1, Y0 = oy * s / k - 1, Y1 = (oy + sh) * s / k + 1;
    if (X0 < 0) X0 = 0;
    if (Y0 < 0) Y0 = 0;
    if (X1 > c->w) X1 = c->w;
    if (Y1 > c->h) Y1 = c->h;
    /* source column for each canvas column, computed once (no divisions in the pixel loop) */
    static int *utab;
    static int ucap;
    if (X1 <= X0) return;
    if (X1 - X0 > ucap) { ucap = X1 - X0; utab = realloc(utab, sizeof(int) * ucap); }
    int ux0 = -1, ux1 = -1;
    for (int X = X0; X < X1; X++) {
        int u = X * k / s - ox;
        utab[X - X0] = (u < 0 || u >= sw || sx + u >= im->w) ? -1 : u;
        if (utab[X - X0] >= 0) { if (ux0 < 0) ux0 = X; ux1 = X + 1; }
    }
    if (ux0 < 0) return;
    for (int Y = Y0; Y < Y1; Y++) {
        int v = Y * k / s - oy;
        if (v < 0 || v >= sh || sy + v >= im->h) continue;
        uint32_t *dst = c->px + (size_t)Y * c->stride;
        const uint32_t *src = im->px + (size_t)(sy + v) * im->w + sx;
        for (int X = ux0; X < ux1; X++) {
            int u = utab[X - X0];
            if (u < 0) continue;
            uint32_t p = src[u], a = p >> 24;
            if (a == 255) dst[X] = p & 0xFFFFFF;
            else if (a) {
                uint32_t d = dst[X], na = 255 - a;
                uint32_t r = (((p >> 16) & 255) * a + ((d >> 16) & 255) * na) / 255;
                uint32_t g = (((p >> 8) & 255) * a + ((d >> 8) & 255) * na) / 255;
                uint32_t b = ((p & 255) * a + (d & 255) * na) / 255;
                dst[X] = r << 16 | g << 8 | b;
            }
        }
    }
}

void mm_blit_part(MMCanvas *c, const MMImage *im, int sx, int sy, int w, int h, int x, int y)
{
    blit_k(c, im, sx, sy, w, h, x, y, 1);
}

void mm_blit(MMCanvas *c, const MMImage *im, int x, int y)
{
    if (im) blit_k(c, im, 0, 0, im->w, im->h, x, y, 1);
}

/* ---- HD pictures (data/hd/<key>.png + bounds.txt "key dx dy" in 2x pixels) ---- */
int mm_hd_on = 1;
static int g_hd_checked, g_hd_avail;
static struct { char key[40]; MMImage *im; int dx, dy, tried; } g_hd[160];
static int g_nhd;

static void hd_key(const char *name, char *key, int n)
{
    int o = 0;
    if (!strncmp(name, "res/", 4)) { o = snprintf(key, n, "res_"); name += 4; }
    for (; *name && *name != '.' && o < n - 1; name++) key[o++] = (*name >= 'A' && *name <= 'Z') ? *name + 32 : *name;
    key[o] = 0;
}

static void hd_init(void)
{
    if (g_hd_checked) return;
    g_hd_checked = 1;
    int size = 0;
    char *t = plat_load("hd/bounds.txt", &size);
    if (!t) return;
    g_hd_avail = 1;
    for (char *line = strtok(t, "\r\n"); line && g_nhd < 160; line = strtok(NULL, "\r\n")) {
        char k[40];
        int dx, dy;
        if (sscanf(line, "%39s %d %d", k, &dx, &dy) == 3) {
            snprintf(g_hd[g_nhd].key, sizeof g_hd[0].key, "%s", k);
            g_hd[g_nhd].dx = dx;
            g_hd[g_nhd].dy = dy;
            g_nhd++;
        }
    }
    free(t);
}

int mm_hd_available(void) { hd_init(); return g_hd_avail; }

static void free_image(MMImage *im) { if (im) { free(im->px); free(im); } }
static void wide_forget(void);

/* new HD / widescreen pictures were installed (mm_art.c): read the lists again, load the pictures again */
void mm_pictures_reload(void)
{
    for (int i = 0; i < g_nhd; i++) free_image(g_hd[i].im);
    memset(g_hd, 0, sizeof g_hd);
    g_nhd = g_hd_checked = g_hd_avail = 0;
    wide_forget();
}

static int hd_get(const char *name, MMImage **im, int *dx, int *dy)
{
    if (!mm_hd_on || !mm_hd_available()) return 0;
    char key[40];
    hd_key(name, key, sizeof key);
    for (int i = 0; i < g_nhd; i++)
        if (!strcmp(g_hd[i].key, key)) {
            if (!g_hd[i].tried) {
                char path[64];
                snprintf(path, sizeof path, "hd/%s.png", key);
                g_hd[i].im = plat_load_png(path);
                g_hd[i].tried = 1;
            }
            if (!g_hd[i].im) return 0;
            *im = g_hd[i].im;
            *dx = g_hd[i].dx;
            *dy = g_hd[i].dy;
            return 1;
        }
    return 0;
}

/* Load one HD picture that has not been loaded yet (called from the game tick, so pictures are ready before
 * they are first shown). Returns 0 when everything is loaded. */
int mm_hd_preload_step(void)
{
    if (!mm_hd_on || !mm_hd_available()) return 0;
    for (int i = 0; i < g_nhd; i++)
        if (!g_hd[i].tried) {
            char path[64];
            snprintf(path, sizeof path, "hd/%s.png", g_hd[i].key);
            g_hd[i].im = plat_load_png(path);
            g_hd[i].tried = 1;
            return 1;
        }
    return 0;
}

/* ---- widescreen pictures (CHANGE): data/wide/<key>.png (2x, HD centre) and <key>_1x.png (1x, original centre),
 * wide.txt "key left right" = extension in original pixels on each side (tools/mm_wide_cut.py) ---- */
int mm_wide_on = 1;
static int g_wide_checked, g_wide_avail;
static struct { char key[40]; MMImage *im[2]; int left, right, tried[2]; } g_wide[40];
static int g_nwide;

static void wide_forget(void)
{
    for (int i = 0; i < g_nwide; i++) { free_image(g_wide[i].im[0]); free_image(g_wide[i].im[1]); }
    memset(g_wide, 0, sizeof g_wide);
    g_nwide = g_wide_checked = g_wide_avail = 0;
}

static void wide_init(void)
{
    if (g_wide_checked) return;
    g_wide_checked = 1;
    int size = 0;
    char *t = plat_load("wide/wide.txt", &size);
    if (!t) return;
    g_wide_avail = 1;
    for (char *line = strtok(t, "\r\n"); line && g_nwide < 40; line = strtok(NULL, "\r\n")) {
        char k[40];
        int l, r;
        if (sscanf(line, "%39s %d %d", k, &l, &r) == 3) {
            snprintf(g_wide[g_nwide].key, sizeof g_wide[0].key, "%s", k);
            g_wide[g_nwide].left = l;
            g_wide[g_nwide].right = r;
            g_nwide++;
        }
    }
    free(t);
}

int mm_wide_available(void) { wide_init(); return g_wide_avail; }
int mm_view_ext(void) { return mm_wide_on && mm_wide_available() ? MM_EXT : 0; }

static MMImage *wide_load(int i, int hd)
{
    if (!g_wide[i].tried[hd]) {
        char path[80];
        snprintf(path, sizeof path, hd ? "wide/%s.png" : "wide/%s_1x.png", g_wide[i].key);
        g_wide[i].im[hd] = plat_load_png(path);
        g_wide[i].tried[hd] = 1;
    }
    return g_wide[i].im[hd];
}

/* wide version of a picture for the current HD setting: image, its scale (1 or 2) and left extension */
static int wide_get(const char *name, MMImage **im, int *k, int *left)
{
    if (!mm_view_ext()) return 0;
    char key[40];
    hd_key(name, key, sizeof key);
    for (int i = 0; i < g_nwide; i++)
        if (!strcmp(g_wide[i].key, key)) {
            int hd = mm_hd_on && mm_hd_available();
            MMImage *w = wide_load(i, hd);
            if (!w && hd) { w = wide_load(i, 0); hd = 0; }
            if (!w) return 0;
            *im = w;
            *k = hd ? 2 : 1;
            *left = g_wide[i].left;
            return 1;
        }
    return 0;
}

/* Load one wide picture for the current HD setting (game tick, like mm_hd_preload_step). */
int mm_wide_preload_step(void)
{
    if (!mm_view_ext()) return 0;
    int hd = mm_hd_on && mm_hd_available();
    for (int i = 0; i < g_nwide; i++)
        if (!g_wide[i].tried[hd]) { wide_load(i, hd); return 1; }
    return 0;
}
void mm_draw_img(MMCanvas *c, const char *name, int x, int y)
{
    MMImage *hi;
    int dx, dy, k, left;
    if (wide_get(name, &hi, &k, &left)) blit_k(c, hi, 0, 0, hi->w, hi->h, (x - left) * k, y * k, k);
    else if (hd_get(name, &hi, &dx, &dy)) blit_k(c, hi, 0, 0, hi->w, hi->h, x * 2 + dx, y * 2 + dy, 2);
    else mm_blit(c, mm_image(name), x, y);
}

void mm_draw_img_part(MMCanvas *c, const char *name, int sx, int sy, int w, int h, int x, int y)
{
    MMImage *hi;
    int dx, dy;
    if (hd_get(name, &hi, &dx, &dy)) blit_k(c, hi, sx * 2 - dx, sy * 2 - dy, w * 2, h * 2, x * 2, y * 2, 2);
    else mm_blit_part(c, mm_image(name), sx, sy, w, h, x, y);
}
