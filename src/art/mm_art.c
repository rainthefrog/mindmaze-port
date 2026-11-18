/* Built-in HD / widescreen art tools: the C port of tools/art/mm_sheets.py, mm_cut.py, mm_wide_sheets.py,
 * mm_wide_cut.py and key_flat_bg.py (those stay in the repository as the reference; tests/art_compare.py checks
 * that both give the same pictures). Float steps follow NumPy 1.26 float32 semantics (pairwise sums, means
 * divided in double, round half to even) and Pillow 10's resampling, so the results match the Python tools.
 * Layout: mm_art.h. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#include "mm_art.h"
#include "mm_art_text.h"

enum { SW = 768, SH = 512, GAP = 8, SCALE = 2, MAXCELLS = 64 };
static const float KEY_IN = 60.0f, KEY_OUT = 150.0f;
static const char OUT_README[] = "The agent's 2x results go here (same file names as the sheets).\n";

/* ------------------------------------------------------------------------------------------------ */
/* text buffer                                                                                       */
typedef struct { char *p; size_t n, cap; } Txt;
static void tput(Txt *t, const char *fmt, ...)
{
    char buf[4096];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    if (t->n + (size_t)n + 1 > t->cap) {
        size_t c = t->cap ? t->cap * 2 : 4096;
        while (c < t->n + (size_t)n + 1) c *= 2;
        char *np = realloc(t->p, c);
        if (!np) return;
        t->p = np;
        t->cap = c;
    }
    memcpy(t->p + t->n, buf, (size_t)n + 1);
    t->n += (size_t)n;
}

static const char *desc_of(const char *file)
{
    for (int i = 0; ART_DESC[i].file; i++) if (!strcmp(ART_DESC[i].file, file)) return ART_DESC[i].desc;
    return "";
}

static void hd_key(const char *file, char *key, int n)       /* "res/29150.bmp" -> "res_29150", "alchem.dib" -> "alchem" */
{
    int o = 0;
    if (!strncmp(file, "res/", 4)) { o = snprintf(key, (size_t)n, "res_"); file += 4; }
    for (; *file && *file != '.' && o < n - 1; file++) key[o++] = *file;
    key[o] = 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* NumPy-like float32 helpers                                                                        */
static float pw_sum(const float *a, size_t n)                /* numpy pairwise summation (float32) */
{
    if (n < 8) {
        float r = 0.f;
        for (size_t i = 0; i < n; i++) r += a[i];
        return r;
    }
    if (n <= 128) {
        float r[8];
        size_t i;
        for (int j = 0; j < 8; j++) r[j] = a[j];
        for (i = 8; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; j++) r[j] += a[i + j];
        float res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; i++) res += a[i];
        return res;
    }
    size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pw_sum(a, n2) + pw_sum(a + n2, n - n2);
}
static float np_mean(const float *a, size_t n) { return (float)((double)pw_sum(a, n) / (double)n); }
static float np_std(const float *a, size_t n, float *tmp)
{
    float m = pw_sum(a, n) / (float)n;
    for (size_t i = 0; i < n; i++) { float d = a[i] - m; tmp[i] = d * d; }
    return sqrtf((float)((double)pw_sum(tmp, n) / (double)n));
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static unsigned char round8(float v) { float r = nearbyintf(v); return (unsigned char)(r < 0 ? 0 : r > 255 ? 255 : r); }

static float key_alpha3(float r, float g, float b)            /* 0..1 share of picture from the distance to magenta */
{
    float dr = r - 255.f, dg = g - 0.f, db = b - 255.f;
    float d = sqrtf(((0.f + dr * dr) + dg * dg) + db * db);
    return clampf((d - KEY_IN) / (KEY_OUT - KEY_IN), 0.f, 1.f);
}
static int light_pink(float r, float g, float b)
{
    int R = (int)r, G = (int)g, B = (int)b;
    return R > 150 && B > 150 && (R < B ? R : B) - G > 30;
}
static int tinted(float r, float g, float b)
{
    int R = (int)r, G = (int)g, B = (int)b;
    return (R < B ? R : B) - G > 25;
}

static void dilate(unsigned char *m, int w, int h, int r)       /* 4-neighbour, r times (in place) */
{
    unsigned char *o = malloc((size_t)w * h);
    if (!o) return;
    for (int it = 0; it < r; it++) {
        memcpy(o, m, (size_t)w * h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                if (m[i]) continue;
                if ((y > 0 && m[i - w]) || (y < h - 1 && m[i + w]) || (x > 0 && m[i - 1]) || (x < w - 1 && m[i + 1])) o[i] = 1;
            }
        memcpy(m, o, (size_t)w * h);
    }
    free(o);
}
static void erode(unsigned char *m, int w, int h, int r)
{
    for (size_t i = 0; i < (size_t)w * h; i++) m[i] = !m[i];
    dilate(m, w, h, r);
    for (size_t i = 0; i < (size_t)w * h; i++) m[i] = !m[i];
}

/* ------------------------------------------------------------------------------------------------ */
/* originals and the HD sheet layout (mm_sheets.py)                                                  */
typedef struct { const char *file; int x, y, w, h, keyed; } Cell;
typedef struct { char name[32]; Cell cell[MAXCELLS]; int n; const char *note; } Sheet;

static int load_size(const char *file, int *w, int *h, int *keyed)
{
    AImg *im = aimg_load_original(file, keyed);
    if (!im) return 0;
    *w = im->w;
    *h = im->h;
    aimg_free(im);
    return 1;
}

/* shelf packing, tallest first (stable sort by -h, -w) */
static int hd_layout(Sheet *sheets, int max, char *err, int errlen)
{
    int ns = 0;
    for (int g = 0; ART_GROUPS[g].prefix; g++) {
        Cell items[MAXCELLS];
        int ni = 0;
        for (int i = ART_GROUPS[g].first; ART_GROUP_FILES[i] && ni < MAXCELLS; i++) {
            Cell c = {ART_GROUP_FILES[i], 0, 0, 0, 0, 0};
            if (!load_size(c.file, &c.w, &c.h, &c.keyed)) continue;              /* not in this data folder: left out */
            items[ni++] = c;
        }
        for (int i = 1; i < ni; i++) {                              /* insertion sort = stable */
            Cell t = items[i];
            int j = i - 1;
            while (j >= 0 && (items[j].h < t.h || (items[j].h == t.h && items[j].w < t.w))) { items[j + 1] = items[j]; j--; }
            items[j + 1] = t;
        }
        struct { int x, y, h; } shelf[8][32];
        int nshelf[8] = {0}, sy[8] = {0}, first = ns;
        for (int i = 0; i < ni; i++) {
            Cell *it = &items[i];
            if (it->w + 2 * GAP > SW || it->h + 2 * GAP > SH) { snprintf(err, (size_t)errlen, "%s too big for a sheet", it->file); return -1; }
            int placed = 0;
            for (int s = first; s < ns && !placed; s++) {
                int k = s - first;
                for (int f = 0; f < nshelf[k] && !placed; f++)
                    if (shelf[k][f].x + it->w + GAP <= SW && it->h <= shelf[k][f].h) {
                        it->x = shelf[k][f].x;
                        it->y = shelf[k][f].y;
                        sheets[s].cell[sheets[s].n++] = *it;
                        shelf[k][f].x += it->w + GAP;
                        placed = 1;
                    }
                if (placed) break;
                if (sy[k] + it->h + GAP <= SH && nshelf[k] < 32) {
                    shelf[k][nshelf[k]].x = GAP + it->w + GAP;
                    shelf[k][nshelf[k]].y = sy[k];
                    shelf[k][nshelf[k]].h = it->h;
                    nshelf[k]++;
                    it->x = GAP;
                    it->y = sy[k];
                    sheets[s].cell[sheets[s].n++] = *it;
                    sy[k] += it->h + GAP;
                    placed = 1;
                }
            }
            if (!placed) {
                if (ns >= max || ns - first >= 8) { snprintf(err, (size_t)errlen, "too many sheets"); return -1; }
                int k = ns - first;
                Sheet *s = &sheets[ns++];
                memset(s, 0, sizeof *s);
                snprintf(s->name, sizeof s->name, "%s_%03d", ART_GROUPS[g].prefix, k);
                s->note = ART_GROUPS[g].note;
                shelf[k][0].x = GAP + it->w + GAP;
                shelf[k][0].y = GAP;
                shelf[k][0].h = it->h;
                nshelf[k] = 1;
                it->x = GAP;
                it->y = GAP;
                s->cell[s->n++] = *it;
                sy[k] = GAP + it->h + GAP;
            }
        }
        for (int s = first; s < ns; s++)                                /* listed / drawn by y, then x */
            for (int i = 1; i < sheets[s].n; i++) {
                Cell t = sheets[s].cell[i];
                int j = i - 1;
                while (j >= 0 && (sheets[s].cell[j].y > t.y || (sheets[s].cell[j].y == t.y && sheets[s].cell[j].x > t.x))) {
                    sheets[s].cell[j + 1] = sheets[s].cell[j];
                    j--;
                }
                sheets[s].cell[j + 1] = t;
            }
    }
    return ns;
}

static int hd_prepare(char *err, int errlen)
{
    static Sheet sheets[40];
    int ns = hd_layout(sheets, 40, err, errlen);
    if (ns < 0) return -1;
    for (int s = 0; s < ns; s++) {
        AImg *sheet = aimg_new(SW, SH, 3);
        if (!sheet) return -1;
        for (size_t i = 0; i < (size_t)SW * SH; i++) { sheet->p[3 * i] = 255; sheet->p[3 * i + 1] = 0; sheet->p[3 * i + 2] = 255; }
        Txt t = {0};
        tput(&t, "%s.png (%dx%d; positions in sheet pixels, x then y)\nNOTE: %s\n", sheets[s].name, SW, SH, sheets[s].note);
        for (int i = 0; i < sheets[s].n; i++) {
            Cell *c = &sheets[s].cell[i];
            int keyed;
            AImg *im = aimg_load_original(c->file, &keyed);
            if (!im) { aimg_free(sheet); free(t.p); return -1; }
            for (int y = 0; y < im->h; y++)
                for (int x = 0; x < im->w; x++) {
                    const unsigned char *p = im->p + ((size_t)y * im->w + x) * 4;
                    if (keyed && !p[3]) continue;
                    memcpy(sheet->p + ((size_t)(c->y + y) * SW + c->x + x) * 3, p, 3);
                }
            aimg_free(im);
            tput(&t, "- x %d-%d, y %d-%d: %s (%s, %dx%d, %s)\n", c->x, c->x + c->w - 1, c->y, c->y + c->h - 1, desc_of(c->file),
                 c->file, c->w, c->h, keyed ? "transparent sprite on magenta" : "opaque picture, fills its box");
        }
        char path[96];
        snprintf(path, sizeof path, "art/work/hd/sheets/%s.png", sheets[s].name);
        int ok = aimg_save_png(path, sheet);
        snprintf(path, sizeof path, "art/work/hd/sheets/%s.txt", sheets[s].name);
        ok = ok && plat_save_data(path, t.p, (int)t.n);
        aimg_free(sheet);
        free(t.p);
        if (!ok) { snprintf(err, (size_t)errlen, "could not write %s", path); return -1; }
    }
    plat_save_data("art/work/hd/out_gen/README.txt", OUT_README, (int)strlen(OUT_README));
    return ns;
}

/* ------------------------------------------------------------------------------------------------ */
/* HD cutter (mm_cut.py)                                                                             */
typedef struct { int w, h; float *rgb; float *a; } FImg;     /* float32 RGB + alpha */

static void match_colours(float *rgb, const float *ref, const unsigned char *mask, int n)
{
    int cnt = 0;
    for (int i = 0; i < n; i++) cnt += mask[i];
    if (cnt < 50) return;
    float *h = malloc(sizeof(float) * cnt), *o = malloc(sizeof(float) * cnt), *tmp = malloc(sizeof(float) * cnt);
    if (!h || !o || !tmp) { free(h); free(o); free(tmp); return; }
    for (int ch = 0; ch < 3; ch++) {
        int k = 0;
        for (int i = 0; i < n; i++) if (mask[i]) { h[k] = rgb[3 * i + ch]; o[k] = ref[3 * i + ch]; k++; }
        float sh = np_std(h, (size_t)cnt, tmp), so = np_std(o, (size_t)cnt, tmp);
        float g = sh > 1 ? clampf(so / sh, 0.85f, 1.15f) : 1.0f;
        float hm = np_mean(h, (size_t)cnt), om = np_mean(o, (size_t)cnt);
        for (int i = 0; i < n; i++) rgb[3 * i + ch] = (rgb[3 * i + ch] - hm) * g + om;
    }
    for (int i = 0; i < 3 * n; i++) rgb[i] = clampf(rgb[i], 0.f, 255.f);
    free(h); free(o); free(tmp);
}

static int cmp_float(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

/* repaint the HD background in the original's flat edge colour; returns 1 and the colour when it has one */
static int match_flat_background(float *rgb, float *a, int w, int h, const float *o2rgb, const unsigned char *o2solid,
                                 const AImg *orig, unsigned char c0[3])
{
    int ow = orig->w, oh = orig->h, nb = 2 * ow + 2 * oh;
    unsigned char *col = malloc((size_t)nb * 3);
    if (!col) return 0;
    int k = 0;
    for (int x = 0; x < ow; x++) memcpy(col + 3 * k++, orig->p + 4 * x, 3);
    for (int x = 0; x < ow; x++) memcpy(col + 3 * k++, orig->p + 4 * ((size_t)(oh - 1) * ow + x), 3);
    for (int y = 0; y < oh; y++) memcpy(col + 3 * k++, orig->p + 4 * ((size_t)y * ow), 3);
    for (int y = 0; y < oh; y++) memcpy(col + 3 * k++, orig->p + 4 * ((size_t)y * ow + ow - 1), 3);
    int best = -1, bestn = 0;                                   /* most common; ties: lowest (r, g, b) */
    for (int i = 0; i < nb; i++) {
        int n = 0;
        for (int j = 0; j < nb; j++) n += !memcmp(col + 3 * i, col + 3 * j, 3);
        if (n > bestn || (n == bestn && memcmp(col + 3 * i, col + 3 * best, 3) < 0)) { bestn = n; best = i; }
    }
    memcpy(c0, col + 3 * best, 3);
    free(col);
    if (bestn < 0.85 * nb || (c0[0] == 0 && c0[1] == 255 && c0[2] == 255)) return 0;

    int hb = 2 * w + 2 * h;
    float *v = malloc(sizeof(float) * hb), chd[3];
    unsigned char *bg = calloc((size_t)w * h, 1), *seen = calloc((size_t)w * h, 1);
    int *q = malloc(sizeof(int) * ((size_t)w * h + hb));
    float *wgt = malloc(sizeof(float) * w * h), *t2 = malloc(sizeof(float) * w * h);
    if (!v || !bg || !seen || !q || !wgt || !t2) { free(v); free(bg); free(seen); free(q); free(wgt); free(t2); return 0; }
    for (int ch = 0; ch < 3; ch++) {
        int n = 0;
        for (int x = 0; x < w; x++) v[n++] = rgb[3 * x + ch];
        for (int x = 0; x < w; x++) v[n++] = rgb[3 * ((size_t)(h - 1) * w + x) + ch];
        for (int y = 0; y < h; y++) v[n++] = rgb[3 * ((size_t)y * w) + ch];
        for (int y = 0; y < h; y++) v[n++] = rgb[3 * ((size_t)y * w + w - 1) + ch];
        qsort(v, (size_t)n, sizeof(float), cmp_float);
        chd[ch] = n % 2 ? v[n / 2] : (float)((double)(0.f + v[n / 2 - 1] + v[n / 2]) / 2.0);
    }
    for (int i = 0; i < w * h; i++)
        bg[i] = o2solid[i] && o2rgb[3 * i] == c0[0] && o2rgb[3 * i + 1] == c0[1] && o2rgb[3 * i + 2] == c0[2];
    erode(bg, w, h, 2);
    for (int i = 0; i < w * h; i++) {
        float *p = rgb + 3 * i, d = 0;
        for (int ch = 0; ch < 3; ch++) { float e = fabsf(p[ch] - chd[ch]); if (e > d) d = e; }
        unsigned char u[3] = {(unsigned char)p[0], (unsigned char)p[1], (unsigned char)p[2]};
        if (d <= 30 || key_alpha3(u[0], u[1], u[2]) < 1.0f || light_pink(p[0], p[1], p[2])) bg[i] = 1;
    }
    int qh = 0, qt = 0;
    for (int y = 0; y < h; y++) { int xs[2] = {0, w - 1}; for (int j = 0; j < 2; j++) { int i = y * w + xs[j]; if (bg[i]) { seen[i] = 1; q[qt++] = i; } } }
    for (int x = 0; x < w; x++) { int ys[2] = {0, h - 1}; for (int j = 0; j < 2; j++) { int i = ys[j] * w + x; if (bg[i] && !seen[i]) { seen[i] = 1; q[qt++] = i; } } }
    while (qh < qt) {
        int i = q[qh++], y = i / w, x = i % w;
        int nb4[4][2] = {{y - 1, x}, {y + 1, x}, {y, x - 1}, {y, x + 1}};
        for (int j = 0; j < 4; j++) {
            int ny = nb4[j][0], nx = nb4[j][1];
            if (ny < 0 || nx < 0 || ny >= h || nx >= w) continue;
            int ni = ny * w + nx;
            if (bg[ni] && !seen[ni]) { seen[ni] = 1; q[qt++] = ni; }
        }
    }
    for (int i = 0; i < w * h; i++) wgt[i] = seen[i];
    for (int it = 0; it < 2; it++) {                            /* 3x3 mean, edges extended */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                #define W(yy, xx) wgt[(size_t)((yy) < 0 ? 0 : (yy) >= h ? h - 1 : (yy)) * w + ((xx) < 0 ? 0 : (xx) >= w ? w - 1 : (xx))]
                float s = W(y - 1, x - 1) + W(y - 1, x) + W(y - 1, x + 1) + W(y, x - 1) + W(y, x) + W(y, x + 1) +
                          W(y + 1, x - 1) + W(y + 1, x) + W(y + 1, x + 1);
                #undef W
                t2[(size_t)y * w + x] = s / 9.f;
            }
        memcpy(wgt, t2, sizeof(float) * w * h);
    }
    for (int i = 0; i < w * h; i++) {
        float g = wgt[i] > (float)seen[i] ? wgt[i] : (float)seen[i];
        for (int ch = 0; ch < 3; ch++) rgb[3 * i + ch] = rgb[3 * i + ch] * (1.f - g) + (float)c0[ch] * g;
        if (seen[i] && o2solid[i]) a[i] = 1.0f;
    }
    free(v); free(bg); free(seen); free(q); free(wgt); free(t2);
    return 1;
}

/* one picture: part = HD pixels of its bounds (uint8 RGB), mine = pixels that belong to it; returns RGB or RGBA */
static AImg *finish(const unsigned char *part, int w, int h, int keyed, const unsigned char *mine, const AImg *orig, int dx, int dy)
{
    size_t n = (size_t)w * h;
    float *o2rgb = calloc(n * 3, sizeof(float)), *rgb = malloc(sizeof(float) * n * 3), *a = malloc(sizeof(float) * n);
    unsigned char *o2solid = calloc(n, 1), *inner = malloc(n), *hole = calloc(n, 1), *fringe = calloc(n, 1), *tmp = malloc(n),
                  *mask = malloc(n);
    AImg *out = NULL;
    if (!o2rgb || !rgb || !a || !o2solid || !inner || !hole || !fringe || !tmp || !mask) goto done;
    int oy = -dy, ox = -dx;
    for (int y = 0; y < h; y++)                                  /* original at 2x, placed at (-dx, -dy) */
        for (int x = 0; x < w; x++) {
            int sy = y - oy, sx = x - ox;
            if (sy < 0 || sx < 0 || sy >= 2 * orig->h || sx >= 2 * orig->w) continue;
            const unsigned char *p = orig->p + ((size_t)(sy / 2) * orig->w + sx / 2) * 4;
            size_t i = (size_t)y * w + x;
            o2rgb[3 * i] = p[0]; o2rgb[3 * i + 1] = p[1]; o2rgb[3 * i + 2] = p[2];
            o2solid[i] = p[3] > 0;
        }
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = part + 3 * i;
        float al = keyed ? key_alpha3(p[0], p[1], p[2]) : 1.0f;
        a[i] = al;
        for (int ch = 0; ch < 3; ch++) {                         /* unmix */
            float f = p[ch], m = ch == 1 ? 0.f : 255.f;
            float safe = al > 1e-3f ? al : 1e-3f;
            rgb[3 * i + ch] = al >= 1.0f ? f : clampf((f - (1.0f - al) * m) / safe, 0.f, 255.f);
        }
    }
    memcpy(inner, o2solid, n);
    erode(inner, w, h, 2);
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = part + 3 * i;
        float raw = key_alpha3(p[0], p[1], p[2]);
        if (keyed && inner[i]) a[i] = 1.0f;
        hole[i] = inner[i] && raw < 1.0f;
        fringe[i] = keyed && o2solid[i] && !inner[i] && raw < 1.0f;
        if (hole[i]) memcpy(rgb + 3 * i, o2rgb + 3 * i, sizeof(float) * 3);
        mask[i] = inner[i] && !hole[i];
    }
    match_colours(rgb, o2rgb, mask, (int)n);
    for (size_t i = 0; i < n; i++) if (hole[i] || fringe[i]) memcpy(rgb + 3 * i, o2rgb + 3 * i, sizeof(float) * 3);
    unsigned char flat[3];
    int has_flat = match_flat_background(rgb, a, w, h, o2rgb, o2solid, orig, flat);
    if (keyed)
        for (size_t i = 0; i < n; i++)
            if (!o2solid[i] && light_pink(part[3 * i], part[3 * i + 1], part[3 * i + 2])) a[i] = 0.f;
    for (size_t i = 0; i < n; i++) tmp[i] = tinted(o2rgb[3 * i], o2rgb[3 * i + 1], o2rgb[3 * i + 2]) && o2solid[i];
    dilate(tmp, w, h, 3);
    for (size_t i = 0; i < n; i++) {
        float *p = rgb + 3 * i;
        int nt = tinted(clampf(p[0], 0, 255), clampf(p[1], 0, 255), clampf(p[2], 0, 255)) && !tmp[i];
        if (!nt) continue;
        if (has_flat) { p[0] = flat[0]; p[1] = flat[1]; p[2] = flat[2]; }
        else {
            if (o2solid[i] && !inner[i]) memcpy(p, o2rgb + 3 * i, sizeof(float) * 3);
            if (!o2solid[i]) a[i] = 0.f;
        }
    }
    out = aimg_new(w, h, keyed ? 4 : 3);
    if (!out) goto done;
    for (size_t i = 0; i < n; i++) {
        unsigned char *o = out->p + i * out->c;
        for (int ch = 0; ch < 3; ch++) o[ch] = round8(rgb[3 * i + ch]);
        if (keyed) {
            o[3] = mine[i] ? round8(a[i] * 255.f) : 0;
            if (!o[3]) o[0] = o[1] = o[2] = 0;
        }
    }
done:
    free(o2rgb); free(rgb); free(a); free(o2solid); free(inner); free(hole); free(fringe); free(tmp); free(mask);
    return out;
}

/* award pictures: the flat square around them (close to the panel colour, joined to the border) -> transparent,
 * 1 px soft edge (key_flat_bg.py) */
static void key_flat_bg(AImg *im)
{
    static const int PANEL[3] = {217, 214, 197};
    int w = im->w, h = im->h;
    unsigned char *near = malloc((size_t)w * h), *out = calloc((size_t)w * h, 1), *al = malloc((size_t)w * h);
    int *st = malloc(sizeof(int) * ((size_t)w * h * 4 + 4 * (w + h)));
    if (!near || !out || !al || !st) { free(near); free(out); free(al); free(st); return; }
    for (int i = 0; i < w * h; i++) {
        const unsigned char *p = im->p + 4 * i;
        int d0 = p[0] - PANEL[0], d1 = p[1] - PANEL[1], d2 = p[2] - PANEL[2];
        near[i] = sqrt((double)(d0 * d0 + d1 * d1 + d2 * d2)) < 34;
    }
    int sp = 0;
    for (int x = 0; x < w; x++) { st[sp++] = x; st[sp++] = (h - 1) * w + x; }
    for (int y = 0; y < h; y++) { st[sp++] = y * w; st[sp++] = y * w + w - 1; }
    while (sp) {
        int i = st[--sp];
        if (out[i] || !near[i]) continue;
        out[i] = 1;
        int y = i / w, x = i % w;
        if (y + 1 < h) st[sp++] = i + w;
        if (y > 0) st[sp++] = i - w;
        if (x + 1 < w) st[sp++] = i + 1;
        if (x > 0) st[sp++] = i - 1;
    }
    for (int i = 0; i < w * h; i++) al[i] = out[i] ? 0 : im->p[4 * i + 3];
    aimg_box_blur(al, w, h, 0.6f);
    for (int i = 0; i < w * h; i++) if (al[i] < im->p[4 * i + 3]) im->p[4 * i + 3] = al[i];
    free(near); free(out); free(al); free(st);
}

/* owner cell per pixel: inside a box = that cell; drawn pixels outside all boxes = the cell they connect to */
static int *owners(const unsigned char *drawn, int w, int h, const int (*box)[4], int nb)
{
    int *own = malloc(sizeof(int) * w * h), *q = malloc(sizeof(int) * ((size_t)w * h * 2 + 16));
    unsigned char *inbox = calloc((size_t)w * h, 1);
    if (!own || !q || !inbox) { free(own); free(q); free(inbox); return NULL; }
    for (int i = 0; i < w * h; i++) own[i] = -1;
    for (int b = 0; b < nb; b++)
        for (int y = box[b][1]; y < box[b][3]; y++)
            for (int x = box[b][0]; x < box[b][2]; x++) { own[y * w + x] = b; inbox[y * w + x] = 1; }
    size_t qh = 0, qt = 0;
    for (int b = 0; b < nb; b++) {
        int x0 = box[b][0], y0 = box[b][1], x1 = box[b][2], y1 = box[b][3];
        for (int y = y0; y < y1; y++) { if (drawn[y * w + x0]) q[qt++] = y * w + x0; if (drawn[y * w + x1 - 1]) q[qt++] = y * w + x1 - 1; }
        for (int x = x0; x < x1; x++) { if (drawn[y0 * w + x]) q[qt++] = y0 * w + x; if (drawn[(y1 - 1) * w + x]) q[qt++] = (y1 - 1) * w + x; }
    }
    while (qh < qt) {
        int i = q[qh++], y = i / w, x = i % w, o = own[i];
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int ny = y + dy, nx = x + dx;
                if (ny < 0 || nx < 0 || ny >= h || nx >= w) continue;
                int ni = ny * w + nx;
                if (drawn[ni] && own[ni] < 0 && !inbox[ni]) { own[ni] = o; q[qt++] = ni; }
            }
    }
    free(q);
    free(inbox);
    return own;
}

typedef struct { char key[40]; int dx, dy; } Bound;

static void bounds_load(Bound *b, int *nb)
{
    *nb = 0;
    char *t = plat_load("hd/bounds.txt", NULL);
    if (!t) return;
    for (char *line = strtok(t, "\r\n"); line && *nb < 200; line = strtok(NULL, "\r\n"))
        if (sscanf(line, "%39s %d %d", b[*nb].key, &b[*nb].dx, &b[*nb].dy) == 3) (*nb)++;
    free(t);
}
static void bounds_set(Bound *b, int *nb, const char *key, int dx, int dy)
{
    for (int i = 0; i < *nb; i++) if (!strcmp(b[i].key, key)) { b[i].dx = dx; b[i].dy = dy; return; }
    if (*nb >= 200) return;
    snprintf(b[*nb].key, sizeof b[0].key, "%s", key);
    b[*nb].dx = dx;
    b[*nb].dy = dy;
    (*nb)++;
}
static int cmp_bound(const void *x, const void *y) { return strcmp(((const Bound *)x)->key, ((const Bound *)y)->key); }

static int hd_install(Txt *rep, int *installed, Txt *stamp)
{
    static Sheet sheets[40];
    char err[200];
    int ns = hd_layout(sheets, 40, err, sizeof err);
    if (ns < 0) { tput(rep, "HD: %s\n", err); return -1; }
    static Bound b[200];
    int nb;
    bounds_load(b, &nb);
    int done = 0, missing = 0;
    for (int s = 0; s < ns; s++) {
        char path[96];
        snprintf(path, sizeof path, "art/work/hd/out_gen/%s.png", sheets[s].name);
        long sz = plat_data_size(path);
        AImg *res = sz > 0 ? aimg_load_png_rgb(path) : NULL;
        if (!res) { missing++; continue; }
        tput(stamp, "%s %ld\n", path, sz);
        if (res->w != SW * SCALE || res->h != SH * SCALE) {
            AImg *r2 = aimg_resize(res, SW * SCALE, SH * SCALE, A_LANCZOS);
            aimg_free(res);
            res = r2;
            if (!res) continue;
            tput(rep, "HD %s: result was not 1536x1024, resized\n", sheets[s].name);
        }
        int W = res->w, H = res->h;
        unsigned char *drawn = malloc((size_t)W * H);
        int (*box)[4] = malloc(sizeof(int[4]) * sheets[s].n);
        if (!drawn || !box) { free(drawn); free(box); aimg_free(res); return -1; }
        for (int i = 0; i < W * H; i++) drawn[i] = key_alpha3(res->p[3 * i], res->p[3 * i + 1], res->p[3 * i + 2]) > 0;
        for (int c = 0; c < sheets[s].n; c++) {
            Cell *ce = &sheets[s].cell[c];
            box[c][0] = ce->x * SCALE; box[c][1] = ce->y * SCALE;
            box[c][2] = (ce->x + ce->w) * SCALE; box[c][3] = (ce->y + ce->h) * SCALE;
            if (!ce->keyed)                                          /* opaque pictures do not spill */
                for (int y = box[c][1]; y < box[c][3]; y++) memset(drawn + (size_t)y * W + box[c][0], 0, (size_t)(box[c][2] - box[c][0]));
        }
        int *own = owners(drawn, W, H, (const int (*)[4])box, sheets[s].n);
        for (int c = 0; own && c < sheets[s].n; c++) {
            Cell *ce = &sheets[s].cell[c];
            int x0 = box[c][0], y0 = box[c][1], x1 = box[c][2], y1 = box[c][3];
            int bx0 = x0, by0 = y0, bx1 = x1, by1 = y1;
            if (ce->keyed)
                for (int i = 0; i < W * H; i++)
                    if (own[i] == c && drawn[i]) {
                        int y = i / W, x = i % W;
                        if (x < bx0) bx0 = x;
                        if (y < by0) by0 = y;
                        if (x + 1 > bx1) bx1 = x + 1;
                        if (y + 1 > by1) by1 = y + 1;
                    }
            int w = bx1 - bx0, h = by1 - by0;
            unsigned char *part = malloc((size_t)w * h * 3), *mine = malloc((size_t)w * h);
            int keyed;
            AImg *orig = aimg_load_original(ce->file, &keyed);
            if (!part || !mine || !orig) { free(part); free(mine); aimg_free(orig); continue; }
            for (int y = 0; y < h; y++) {
                memcpy(part + (size_t)y * w * 3, res->p + ((size_t)(by0 + y) * W + bx0) * 3, (size_t)w * 3);
                for (int x = 0; x < w; x++) mine[y * w + x] = own[(by0 + y) * W + bx0 + x] == c;
            }
            AImg *img = finish(part, w, h, ce->keyed, mine, orig, bx0 - x0, by0 - y0);
            char key[40];
            hd_key(ce->file, key, sizeof key);
            if (img && (!strcmp(key, "award") || !strcmp(key, "ribbon") || !strcmp(key, "medal") || !strcmp(key, "ring") ||
                        !strcmp(key, "trophy")) && img->c == 4)
                key_flat_bg(img);
            snprintf(path, sizeof path, "hd/%s.png", key);
            if (img && aimg_save_png(path, img)) { bounds_set(b, &nb, key, bx0 - x0, by0 - y0); done++; }
            aimg_free(img);
            aimg_free(orig);
            free(part);
            free(mine);
        }
        free(own);
        free(drawn);
        free(box);
        aimg_free(res);
    }
    if (done) {
        qsort(b, (size_t)nb, sizeof b[0], cmp_bound);
        Txt t = {0};
        for (int i = 0; i < nb; i++) tput(&t, "%s %d %d\n", b[i].key, b[i].dx, b[i].dy);
        plat_save_data("hd/bounds.txt", t.p ? t.p : "", (int)t.n);
        free(t.p);
    }
    tput(rep, "HD: %d pictures installed from %d of %d sheets%s\n", done, ns - missing, ns,
         missing ? " (the other sheets have no result yet)" : "");
    *installed += done;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
/* widescreen sheets and cutter (mm_wide_sheets.py, mm_wide_cut.py)                                  */
typedef struct {
    const char *file, *kind, *task;
    char name[32];
    int vx, w, h, keyed, opaque, sides, tw, x0, y0, px;   /* sides: 1 left, 2 right */
    int nb, band[4][4];                                   /* y0, y1, split_left, split_right (STRETCH) */
} WideItem;

static int wide_items(WideItem *it, int max)
{
    int n = 0, count[3] = {0};
    for (int i = 0; WIDE_ITEMS[i].file && n < max; i++) {
        WideItem *e = &it[n];
        memset(e, 0, sizeof *e);
        e->file = WIDE_ITEMS[i].file;
        e->kind = WIDE_ITEMS[i].kind;
        e->task = WIDE_ITEMS[i].task;
        e->vx = WIDE_ITEMS[i].view_x;
        AImg *im = aimg_load_original(e->file, &e->keyed);
        if (!im) continue;
        e->w = im->w;
        e->h = im->h;
        int amin = 255;
        for (int p = 0; p < im->w * im->h; p++) if (im->p[4 * p + 3] < amin) amin = im->p[4 * p + 3];
        e->opaque = !(e->keyed && amin == 0);
        for (int side = 0; side < 2; side++) {                 /* reaches the view edge with opaque pixels */
            int col = side ? im->w - 1 : 0, at_edge = side ? e->vx + im->w >= WIDE_VIEW_W - 1 : e->vx == 0;
            if (!at_edge) continue;
            int any = !e->keyed;
            for (int y = 0; y < im->h && !any; y++) any = im->p[((size_t)y * im->w + col) * 4 + 3] != 0;
            if (any) e->sides |= 1 << side;
        }
        aimg_free(im);
        if (!e->sides) continue;
        int ns = (e->sides & 1) + ((e->sides >> 1) & 1);
        e->tw = e->w + WIDE_EXT * ns;
        if (e->tw > SW || e->h > SH) continue;
        int k = !strcmp(e->kind, "backgrounds") ? 0 : !strcmp(e->kind, "layers") ? 1 : 2;
        snprintf(e->name, sizeof e->name, "%s_%03d", e->kind, count[k]++);
        e->x0 = (SW - e->tw) / 2;
        e->y0 = e->h > SH - 16 - 64 ? (SH - e->h) / 2 : 8;
        e->px = e->x0 + ((e->sides & 1) ? WIDE_EXT : 0);
        e->nb = WIDE_ITEMS[i].nbands;
        memcpy(e->band, WIDE_ITEMS[i].bands, sizeof e->band);
        if (!e->nb) { e->nb = 1; e->band[0][0] = 0; e->band[0][1] = e->h; e->band[0][2] = 0; e->band[0][3] = e->w; }
        n++;
    }
    return n;
}

static long pyround(double v) { return (long)nearbyint(v); }      /* Python round(): half to even */
static void fmt(char *o, size_t n, double x, double y, double sx, double sy)
{
    snprintf(o, n, "(%ld, %ld) [2x result: (%ld, %ld)]", pyround(x + sx), pyround(y + sy), pyround(2 * (x + sx)), pyround(2 * (y + sy)));
}

static int runs(const AImg *im, int x, int (*r)[2], int max)    /* opaque runs (y0, y1) of one column */
{
    int n = 0, s = -1;
    for (int y = 0; y < im->h; y++) {
        int o = im->p[((size_t)y * im->w + x) * 4 + 3] > 0;
        if (o && s < 0) s = y;
        if (!o && s >= 0) { if (n < max) { r[n][0] = s; r[n][1] = y - 1; n++; } s = -1; }
    }
    if (s >= 0 && n < max) { r[n][0] = s; r[n][1] = im->h - 1; n++; }
    return n;
}

/* straight-line continuations of the layer outline edges crossing the view border (outline_guides) */
static void outline_guides(Txt *t, const AImg *im, int side, int bx, int y0)
{
    int w = im->w, h = im->h, fit = 30;
    int r0[64][2];
    int n0 = runs(im, side ? w - 1 : 0, r0, 64);
    for (int k = 0; k < n0; k++)
        for (int kind = 0; kind < 2; kind++) {                 /* 0 top, 1 bottom */
            int yb = r0[k][kind];
            if ((kind == 0 && yb == 0) || (kind == 1 && yb == h - 1)) continue;
            double px[64], py[64];
            int np = 0, y = yb;
            for (int d = 0; d < fit; d++) {
                int rr[64][2];
                int nr = runs(im, side ? w - 1 - d : d, rr, 64);
                if (!nr) break;
                int yn = rr[0][kind];
                for (int j = 1; j < nr; j++) if (abs(rr[j][kind] - y) < abs(yn - y)) yn = rr[j][kind];
                if (np && abs(yn - y) > 3) break;
                y = yn;
                px[np] = d;
                py[np] = y;
                np++;
            }
            double mx = 0, my = 0, sxx = 0, sxy = 0;
            for (int i = 0; i < np; i++) mx += px[i];
            for (int i = 0; i < np; i++) my += py[i];
            mx /= np;
            my /= np;
            for (int i = 0; i < np; i++) sxx += (px[i] - mx) * (px[i] - mx);
            if (sxx == 0) sxx = 1;
            for (int i = 0; i < np; i++) sxy += (px[i] - mx) * (py[i] - my);
            double kk = sxy / sxx, b = my - kk * mx, yo = b + kk * (-WIDE_EXT);
            int has_yo = 1, has_leave = 0, edge = 0;
            double leave = 0;
            if (yo < 0 || yo > h - 1) {
                edge = yo < 0 ? 0 : h - 1;
                if (kk != 0) { leave = (b - edge) / kk; has_leave = 1; }
                has_yo = 0;
            }
            int sgn = side ? 1 : -1;
            char f1[96], f2[96];
            const char *what = kind ? "bottom" : "top", *sd = side ? "right" : "left";
            fmt(f1, sizeof f1, bx, yb, 0, y0);
            if (has_yo) {
                fmt(f2, sizeof f2, bx + sgn * WIDE_EXT, yo, 0, y0);
                tput(t, "- Straight line, %s edge of the layer on the %s side: crosses the border at %s and must reach the "
                        "strip's outer edge at %s.\n", what, sd, f1, f2);
            } else if (has_leave && leave > 0) {
                fmt(f2, sizeof f2, bx + sgn * leave, edge, 0, y0);
                tput(t, "- Straight line, %s edge of the layer on the %s side: crosses the border at %s and runs straight to "
                        "%s, where it leaves the picture: the layer ends there, everything further out is flat magenta.\n",
                     what, sd, f1, f2);
            }
        }
}

static int wide_prepare(char *err, int errlen)
{
    static WideItem it[32];
    int n = wide_items(it, 32);
    for (int i = 0; i < n; i++) {
        WideItem *e = &it[i];
        int keyed;
        AImg *im = aimg_load_original(e->file, &keyed), *sheet = aimg_new(SW, SH, 3), *mask = aimg_new(SW, SH, 1);
        if (!im || !sheet || !mask) { aimg_free(im); aimg_free(sheet); aimg_free(mask); return -1; }
        for (size_t p = 0; p < (size_t)SW * SH; p++) { sheet->p[3 * p] = 255; sheet->p[3 * p + 1] = 0; sheet->p[3 * p + 2] = 255; }
        int L = e->sides & 1, R = (e->sides >> 1) & 1, gl = L ? WIDE_EXT : 0;
        for (int b = 0; b < e->nb; b++) {
            int ya = e->band[b][0], yb = e->band[b][1], sl = L ? e->band[b][2] : 0, sr = R ? e->band[b][3] : e->w;
            int seg[3][3] = {{0, sl, 0}, {sl, sr, gl}, {sr, e->w, gl + (R ? WIDE_EXT : 0)}};
            for (int s = 0; s < 3; s++)
                for (int y = ya; y < yb; y++)
                    for (int x = seg[s][0]; x < seg[s][1]; x++) {
                        const unsigned char *p = im->p + ((size_t)y * e->w + x) * 4;
                        if (!p[3]) continue;
                        memcpy(sheet->p + ((size_t)(e->y0 + y) * SW + e->x0 + x + seg[s][2]) * 3, p, 3);
                    }
            for (int y = ya; y < yb; y++) {
                if (L) memset(mask->p + (size_t)(e->y0 + y) * SW + e->x0 + sl, 255, WIDE_EXT);
                if (R) memset(mask->p + (size_t)(e->y0 + y) * SW + e->x0 + sr + gl, 255, WIDE_EXT);
            }
        }
        for (size_t p = 0; p < (size_t)SW * SH; p++)
            if (mask->p[p]) { sheet->p[3 * p] = 0; sheet->p[3 * p + 1] = 255; sheet->p[3 * p + 2] = 0; }
        Txt t = {0};
        const char *desc = desc_of(e->file);
        int is_stretch = 0;
        for (int k = 0; WIDE_ITEMS[k].file; k++) if (!strcmp(WIDE_ITEMS[k].file, e->file)) is_stretch = WIDE_ITEMS[k].nbands > 0;
        tput(&t, "%s.png (%dx%d; positions in sheet pixels, x then y)\n", e->name, SW, SH);
        if (!is_stretch) {
            tput(&t, "ACTION: FILL IN DETAILS - the picture keeps its size and place; paint what lies beyond its edge into the "
                     "green strip(s).\n");
            tput(&t, "PICTURE: x %d-%d, y %d-%d: %s (%s, %dx%d, %s). Keep it exactly as it is (only redrawn at 2x).\n", e->px,
                 e->px + e->w - 1, e->y0, e->y0 + e->h - 1, desc, e->file, e->w, e->h,
                 e->opaque ? "opaque, fills its box" : "transparent parts on magenta");
            if (L) tput(&t, "PAINT (left side): the GREEN strip x %d-%d, y %d-%d.\n", e->x0, e->x0 + WIDE_EXT - 1, e->y0, e->y0 + e->h - 1);
            if (R) tput(&t, "PAINT (right side): the GREEN strip x %d-%d, y %d-%d.\n", e->x0 + e->tw - WIDE_EXT, e->x0 + e->tw - 1,
                        e->y0, e->y0 + e->h - 1);
        } else {
            tput(&t, "ACTION: STRETCH - the picture's own border frame has been moved outwards to the new, wider edges; the "
                     "green gaps between the moved frame and the rest of the picture are the paint area. Close them so the "
                     "picture looks as if it had always been this wide.\n");
            tput(&t, "PICTURE: x %d-%d, y %d-%d (now %d px wide): %s (%s, originally %dx%d, opaque).\n", e->x0, e->x0 + e->tw - 1,
                 e->y0, e->y0 + e->h - 1, e->tw, desc, e->file, e->w, e->h);
            tput(&t, "PAINT: every GREEN area: a vertical gap %d px wide inside each side of the frame%s.\n", WIDE_EXT,
                 e->nb > 1 ? ", which reaches further in at the top and bottom where the old corner ornaments were"
                           : ", over the full height");
        }
        const char *task = e->task;
        for (int k = 0; !task && WIDE_WHAT[k].kind; k++) if (!strcmp(WIDE_WHAT[k].kind, e->kind)) task = WIDE_WHAT[k].what;
        tput(&t, "TASK: %s\n", task ? task : "");
        if (!strcmp(e->file, "dunrm.dib") || !strcmp(e->file, "plnwalls.dib") || !strcmp(e->file, "atrium.dib") ||
            !strcmp(e->file, "rmofdoor.dib"))
            tput(&t, "Extra: the side walls run towards the viewer; in the green strips they come closer, so continue their "
                     "stone courses / panels, floor and ceiling lines outwards along the same vanishing lines.\n");
        Txt g = {0};
        int room = 0;
        for (int k = 0; ROOM_SHEETS[k]; k++) room |= !strcmp(ROOM_SHEETS[k], e->file);
        char f1[96], f2[96];
        if (strcmp(e->kind, "backgrounds") || room) {
            fmt(f1, sizeof f1, ROOM_VP_X - e->vx, ROOM_VP_Y, e->px, e->y0);
            tput(&g, "- Vanishing point of the room (all receding lines meet here; lines parallel to the back wall stay exactly "
                     "horizontal, upright edges stay exactly vertical): %s.\n", f1);
        }
        if (room)
            for (int k = 0; ROOM_LINES[k].name; k++) {
                fmt(f1, sizeof f1, ROOM_LINES[k].bx, ROOM_LINES[k].by, e->px, e->y0);
                fmt(f2, sizeof f2, ROOM_LINES[k].ox, ROOM_LINES[k].oy, e->px, e->y0);
                tput(&g, "- Straight line, %s: crosses the strip border at %s and must reach the strip's outer edge at %s.\n",
                     ROOM_LINES[k].name, f1, f2);
            }
        if (!strcmp(e->kind, "layers"))
            for (int side = 0; side < 2; side++)
                if (e->sides >> side & 1) outline_guides(&g, im, side, side ? e->px + e->w - 1 : e->px, e->y0);
        if (!strcmp(e->kind, "objects"))
            for (int side = 0; side < 2; side++)
                if (e->sides >> side & 1) {
                    int bx = side ? e->px + e->w - 1 : e->px, rr[64][2];
                    int nr = runs(im, side ? e->w - 1 : 0, rr, 64);
                    for (int k = 0; k < nr; k++)
                        tput(&g, "- The visible outline crosses the border at x %d from y %d to y %d [2x result: x %d, y %d-%d]; "
                                 "the continuation starts exactly there, with the same width and direction.\n",
                             bx, rr[k][0] + e->y0, rr[k][1] + e->y0, 2 * bx, 2 * (rr[k][0] + e->y0), 2 * (rr[k][1] + e->y0));
                }
        if (g.n) { tput(&t, "GEOMETRY (strict; check the result against it):\n"); tput(&t, "%s", g.p); }
        char path[96];
        snprintf(path, sizeof path, "art/work/wide/sheets/%s.png", e->name);
        int ok = aimg_save_png(path, sheet);
        snprintf(path, sizeof path, "art/work/wide/masks/%s.png", e->name);
        ok = ok && aimg_save_png(path, mask);
        snprintf(path, sizeof path, "art/work/wide/sheets/%s.txt", e->name);
        ok = ok && plat_save_data(path, t.p, (int)t.n);
        free(t.p);
        free(g.p);
        aimg_free(im);
        aimg_free(sheet);
        aimg_free(mask);
        if (!ok) { snprintf(err, (size_t)errlen, "could not write %s", path); return -1; }
    }
    plat_save_data("art/work/wide/out_gen/README.txt", OUT_README, (int)strlen(OUT_README));
    return n;
}

static const char *check_bounds(const AImg *R, const WideItem *e, char *buf, size_t n)
{
    AImg *s = aimg_resize(R, SW, SH, A_BICUBIC);
    if (!s) return "";
    int x0 = SW, y0 = SH, x1 = -1, y1 = -1;
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            const unsigned char *p = s->p + ((size_t)y * SW + x) * 3;
            if (p[0] > 200 && p[2] > 200 && p[1] < 60) continue;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x > x1) x1 = x;
            if (y > y1) y1 = y;
        }
    aimg_free(s);
    if (x1 < 0) return "empty result";
    int want[4] = {e->x0, e->y0, e->x0 + e->tw - 1, e->y0 + e->h - 1};
    int over[4] = {want[0] - x0, want[1] - y0, x1 - want[2], y1 - want[3]};
    for (int i = 0; i < 4; i++)
        if (over[i] > 12) {
            snprintf(buf, n, "painted area (%d, %d, %d, %d), sheet box (%d, %d, %d, %d) (sheet px)", x0, y0, x1, y1, want[0], want[1],
                     want[2], want[3]);
            return buf;
        }
    return "";
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int wide_install(Txt *rep, int *installed, Txt *stamp)
{
    static WideItem it[32];
    int n = wide_items(it, 32), done = 0, missing = 0;
    char *lines[32];
    int nl = 0;
    for (int i = 0; i < n; i++) {
        WideItem *e = &it[i];
        char path[96], key[40];
        snprintf(path, sizeof path, "art/work/wide/out_gen/%s.png", e->name);
        long sz = plat_data_size(path);
        AImg *R = sz > 0 ? aimg_load_png_rgb(path) : NULL;
        if (!R) { missing++; continue; }
        tput(stamp, "%s %ld\n", path, sz);
        if (R->w != 2 * SW || R->h != 2 * SH) { AImg *r2 = aimg_resize(R, 2 * SW, 2 * SH, A_LANCZOS); aimg_free(R); R = r2; if (!R) continue; }
        char warn[200];
        const char *wmsg = check_bounds(R, e, warn, sizeof warn);
        if (*wmsg) tput(rep, "REGENERATE wide %s (%s): %s\n", e->name, e->file, wmsg);
        int w2 = 2 * e->tw, h2 = 2 * e->h;
        size_t np = (size_t)w2 * h2;
        float *rgb = malloc(sizeof(float) * np * 3), *a = malloc(sizeof(float) * np);
        AImg *im2 = aimg_new(w2, h2, 4);
        if (!rgb || !a || !im2) { free(rgb); free(a); aimg_free(im2); aimg_free(R); continue; }
        for (int y = 0; y < h2; y++)
            for (int x = 0; x < w2; x++)
                for (int ch = 0; ch < 3; ch++)
                    rgb[((size_t)y * w2 + x) * 3 + ch] = R->p[((size_t)(2 * e->y0 + y) * R->w + 2 * e->x0 + x) * 3 + ch];
        aimg_free(R);
        if (e->opaque) {
            const int E = 4;                                         /* edges mixed with the magenta around the box */
            for (int y = 0; y < E; y++) memcpy(rgb + (size_t)y * w2 * 3, rgb + (size_t)E * w2 * 3, sizeof(float) * w2 * 3);
            for (int y = h2 - E; y < h2; y++) memcpy(rgb + (size_t)y * w2 * 3, rgb + (size_t)(h2 - E - 1) * w2 * 3, sizeof(float) * w2 * 3);
            for (int y = 0; y < h2; y++)
                for (int x = 0; x < E; x++) {
                    memcpy(rgb + ((size_t)y * w2 + x) * 3, rgb + ((size_t)y * w2 + E) * 3, sizeof(float) * 3);
                    memcpy(rgb + ((size_t)y * w2 + w2 - E + x) * 3, rgb + ((size_t)y * w2 + w2 - E - 1) * 3, sizeof(float) * 3);
                }
            for (size_t p = 0; p < np; p++) a[p] = 255.f;
        } else {
            for (size_t p = 0; p < np; p++) {
                float *c = rgb + 3 * p, al = key_alpha3(c[0], c[1], c[2]);
                if (al < 0.08f) al = 0;
                for (int ch = 0; ch < 3; ch++) {
                    float m = ch == 1 ? 0.f : 255.f, safe = al > 1e-3f ? al : 1e-3f;
                    if (al < 1.0f) c[ch] = clampf((c[ch] - (1.0f - al) * m) / safe, 0.f, 255.f);
                }
                a[p] = al * 255.f;
            }
        }
        for (size_t p = 0; p < np; p++) {
            for (int ch = 0; ch < 3; ch++) im2->p[4 * p + ch] = (unsigned char)clampf(rgb[3 * p + ch] + 0.5f, 0.f, 255.f);
            im2->p[4 * p + 3] = (unsigned char)clampf(a[p] + 0.5f, 0.f, 255.f);
        }
        free(rgb);
        free(a);
        AImg *pre = aimg_new(w2, h2, 4);
        AImg *w1 = NULL;
        if (pre) {
            memcpy(pre->p, im2->p, np * 4);
            aimg_premultiply(pre);
            w1 = aimg_resize(pre, e->tw, e->h, A_LANCZOS);
            aimg_free(pre);
        }
        if (w1) {
            aimg_unpremultiply(w1);
            for (size_t p = 0; p < (size_t)e->tw * e->h; p++) w1->p[4 * p + 3] = e->opaque ? 255 : (w1->p[4 * p + 3] < 128 ? 0 : 255);
        }
        int k = 0;
        for (const char *f = e->file; *f && *f != '.' && k < 39; f++) key[k++] = (*f >= 'A' && *f <= 'Z') ? *f + 32 : *f;
        key[k] = 0;
        snprintf(path, sizeof path, "wide/%s.png", key);
        int ok = aimg_save_png(path, im2);
        snprintf(path, sizeof path, "wide/%s_1x.png", key);
        ok = ok && w1 && aimg_save_png(path, w1);
        aimg_free(im2);
        aimg_free(w1);
        if (!ok) continue;
        char line[80];
        snprintf(line, sizeof line, "%s %d %d", key, (e->sides & 1) ? WIDE_EXT : 0, (e->sides & 2) ? WIDE_EXT : 0);
        lines[nl++] = mm_strdup(line);
        done++;
    }
    if (nl) {
        qsort(lines, (size_t)nl, sizeof lines[0], cmp_str);
        Txt t = {0};
        for (int i = 0; i < nl; i++) { tput(&t, "%s\n", lines[i]); free(lines[i]); }
        plat_save_data("wide/wide.txt", t.p, (int)t.n);
        free(t.p);
    }
    tput(rep, "Widescreen: %d pictures installed from %d of %d sheets%s\n", done, n - missing, n,
         missing ? " (the other sheets have no result yet)" : "");
    *installed += done;
    return 0;
}

/* ------------------------------------------------------------------------------------------------ */
int mm_art_prepare(int force, char *report, int n)
{
    if (!force && plat_data_size("art/work/hd/sheets/backgrounds_000.txt") > 0 &&
        plat_data_size("art/work/wide/sheets/backgrounds_000.txt") > 0) {
        snprintf(report, (size_t)n, "sheets already there");
        return 1;
    }
    char err[200] = "";
    int a = hd_prepare(err, sizeof err), b = a < 0 ? -1 : wide_prepare(err, sizeof err);
    if (a < 0 || b < 0) { snprintf(report, (size_t)n, "could not make the sheets: %s", err); return 0; }
    snprintf(report, (size_t)n, "%d HD sheets and %d widescreen sheets written to art/work", a, b);
    return 1;
}

int mm_art_results_waiting(void)
{
    char *old = plat_load("art/work/installed.txt", NULL);
    int waiting = 0;
    for (int kind = 0; kind < 2 && !waiting; kind++) {
        static Sheet sheets[40];
        static WideItem it[32];
        char err[100];
        int ns = kind ? wide_items(it, 32) : hd_layout(sheets, 40, err, sizeof err);
        for (int s = 0; s < ns && !waiting; s++) {
            char path[96], line[120];
            snprintf(path, sizeof path, "art/work/%s/out_gen/%s.png", kind ? "wide" : "hd", kind ? it[s].name : sheets[s].name);
            long sz = plat_data_size(path);
            if (sz <= 0) continue;
            snprintf(line, sizeof line, "%s %ld\n", path, sz);
            waiting = !old || !strstr(old, line);
        }
    }
    free(old);
    return waiting;
}

int mm_art_install(char *report, int n)
{
    Txt rep = {0}, stamp = {0};
    int installed = 0;
    tput(&rep, "MindMaze art install\n");
    int r1 = hd_install(&rep, &installed, &stamp), r2 = wide_install(&rep, &installed, &stamp);
    if (!strstr(rep.p, "REGENERATE")) tput(&rep, "No sheet needs to be generated again.\n");
    plat_save_data("art/work/REPORT.txt", rep.p, (int)rep.n);
    plat_save_data("art/work/installed.txt", stamp.p ? stamp.p : "", (int)stamp.n);
    mm_pictures_reload();
    snprintf(report, (size_t)n, "%s", rep.p + strlen("MindMaze art install\n"));
    free(rep.p);
    free(stamp.p);
    return r1 < 0 || r2 < 0 ? -1 : installed;
}
