/* Image helpers for the built-in art tools (mm_art.c): DIB / PNG in and out, and the resampling, premultiplying
 * and box blur of Pillow 10 (Resample.c, Convert.c, BoxBlur.c) so the results match tools/art, which used
 * Pillow. PNG via stb_image / stb_image_write (public domain, src/third_party). */
#define _USE_MATH_DEFINES
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#include "mm_art.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "../third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../third_party/stb_image_write.h"

AImg *aimg_new(int w, int h, int c)
{
    AImg *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->w = w;
    im->h = h;
    im->c = c;
    im->p = calloc((size_t)w * h * c + 1, 1);
    if (!im->p) { free(im); return NULL; }
    return im;
}

void aimg_free(AImg *im) { if (im) { free(im->p); free(im); } }

/* ---- original pictures: 8-bit DIB / BMP (as Pillow converts them) ---- */
AImg *aimg_load_original(const char *name, int *keyed)
{
    char path[80];
    snprintf(path, sizeof path, "%s", name);
    for (char *p = path; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    int size = 0;
    unsigned char *d = plat_load(path, &size);
    *keyed = 0;
    if (!d) return NULL;
    AImg *im = NULL;
    if (size >= 54 && d[0] == 'B' && d[1] == 'M') {
        unsigned off = d[10] | d[11] << 8 | d[12] << 16 | (unsigned)d[13] << 24, hs = d[14] | d[15] << 8;
        int w = d[18] | d[19] << 8 | d[20] << 16 | d[21] << 24, h = d[22] | d[23] << 8 | d[24] << 16 | d[25] << 24;
        int bpp = d[28] | d[29] << 8, comp = d[30] | d[31] << 8;
        int up = h > 0;
        if (h < 0) h = -h;
        unsigned ncol = d[46] | d[47] << 8;
        if (!ncol) ncol = 256;
        const unsigned char *pal = d + 14 + hs;
        int row = ((w * bpp + 31) / 32) * 4;
        if (w > 0 && h > 0 && w < 4096 && h < 4096 && comp == 0 && (bpp == 8 || bpp == 24) &&
            off + (unsigned)row * h <= (unsigned)size && (bpp != 8 || 14 + hs + 4 * ncol <= (unsigned)size)) {
            im = aimg_new(w, h, 4);
            if (bpp == 8 && strncmp(path, "res/", 4) && ncol > 254)
                *keyed = pal[254 * 4 + 2] == 0 && pal[254 * 4 + 1] == 255 && pal[254 * 4] == 255;   /* BGRx cyan */
            for (int y = 0; im && y < h; y++) {
                const unsigned char *s = d + off + (size_t)row * (up ? h - 1 - y : y);
                unsigned char *o = im->p + (size_t)y * w * 4;
                for (int x = 0; x < w; x++, o += 4) {
                    if (bpp == 8) {
                        unsigned i = s[x];
                        const unsigned char *c = i < ncol ? pal + 4 * i : pal;
                        o[0] = c[2]; o[1] = c[1]; o[2] = c[0];
                        o[3] = (*keyed && i == 254) ? 0 : 255;
                    } else { o[0] = s[3 * x + 2]; o[1] = s[3 * x + 1]; o[2] = s[3 * x]; o[3] = 255; }
                }
            }
        }
    }
    free(d);
    return im;
}

/* ---- PNG ---- */
AImg *aimg_load_png_rgb(const char *rel)
{
    int size = 0;
    unsigned char *d = plat_load(rel, &size);
    if (!d) return NULL;
    int w, h, n;
    unsigned char *px = stbi_load_from_memory(d, size, &w, &h, &n, 3);   /* RGBA -> RGB drops alpha, like Pillow */
    free(d);
    if (!px) return NULL;
    AImg *im = calloc(1, sizeof *im);
    if (!im) { stbi_image_free(px); return NULL; }
    im->w = w;
    im->h = h;
    im->c = 3;
    im->p = malloc((size_t)w * h * 3 + 1);
    if (im->p) memcpy(im->p, px, (size_t)w * h * 3);
    stbi_image_free(px);
    if (!im->p) { free(im); return NULL; }
    return im;
}

typedef struct { unsigned char *d; size_t n, cap; int bad; } Mem;
static void mem_put(void *ctx, void *data, int n)
{
    Mem *m = ctx;
    if (m->bad) return;
    if (m->n + (size_t)n > m->cap) {
        size_t c = m->cap ? m->cap * 2 : 1 << 16;
        while (c < m->n + (size_t)n) c *= 2;
        unsigned char *nd = realloc(m->d, c);
        if (!nd) { m->bad = 1; return; }
        m->d = nd;
        m->cap = c;
    }
    memcpy(m->d + m->n, data, (size_t)n);
    m->n += (size_t)n;
}

int aimg_save_png(const char *rel, const AImg *im)
{
    Mem m = {0};
    stbi_write_png_compression_level = 6;
    int ok = stbi_write_png_to_func(mem_put, &m, im->w, im->h, im->c, im->p, im->w * im->c) && !m.bad;
    if (ok) ok = plat_save_data(rel, m.d, (int)m.n);
    free(m.d);
    return ok;
}

/* ---- Pillow's resampling (8 bits per channel, fixed point with 22 fraction bits) ---- */
#define PRECISION_BITS (32 - 8 - 2)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static double sinc(double x) { if (x == 0.0) return 1.0; x = x * M_PI; return sin(x) / x; }
static double lanczos(double x) { return (-3.0 <= x && x < 3.0) ? sinc(x) * sinc(x / 3) : 0.0; }
static double bicubic(double x)
{
    const double a = -0.5;
    if (x < 0.0) x = -x;
    if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1;
    if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a;
    return 0.0;
}

static int coeffs(int insize, int outsize, int filter, int **bounds, int **kk)
{
    double support0 = filter == A_LANCZOS ? 3.0 : 2.0;
    double scale = (double)insize / outsize, fs = scale < 1.0 ? 1.0 : scale;
    double support = support0 * fs;
    int ksize = (int)ceil(support) * 2 + 1;
    double *pre = calloc((size_t)outsize * ksize, sizeof(double));
    *bounds = calloc((size_t)outsize * 2, sizeof(int));
    *kk = calloc((size_t)outsize * ksize, sizeof(int));
    if (!pre || !*bounds || !*kk) { free(pre); return 0; }
    for (int xx = 0; xx < outsize; xx++) {
        double center = (xx + 0.5) * scale, ww = 0.0, ss = 1.0 / fs;
        int xmin = (int)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = (int)(center + support + 0.5);
        if (xmax > insize) xmax = insize;
        xmax -= xmin;
        double *k = pre + (size_t)xx * ksize;
        for (int x = 0; x < xmax; x++) {
            double w = filter == A_LANCZOS ? lanczos((x + xmin - center + 0.5) * ss) : bicubic((x + xmin - center + 0.5) * ss);
            k[x] = w;
            ww += w;
        }
        for (int x = 0; x < xmax; x++) if (ww != 0.0) k[x] /= ww;
        (*bounds)[xx * 2] = xmin;
        (*bounds)[xx * 2 + 1] = xmax;
    }
    for (int i = 0; i < outsize * ksize; i++)
        (*kk)[i] = pre[i] < 0 ? (int)(-0.5 + pre[i] * (1 << PRECISION_BITS)) : (int)(0.5 + pre[i] * (1 << PRECISION_BITS));
    free(pre);
    return ksize;
}

static unsigned char clip8(int v)
{
    v >>= PRECISION_BITS;                                    /* arithmetic shift, as Pillow's table lookup */
    return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
}

AImg *aimg_resize(const AImg *in, int w, int h, int filter)
{
    int *bx = NULL, *kx = NULL, *by = NULL, *ky = NULL;
    int kw = coeffs(in->w, w, filter, &bx, &kx), kh = coeffs(in->h, h, filter, &by, &ky);
    AImg *tmp = NULL, *out = NULL;
    if (!kw || !kh) goto done;
    int c = in->c;
    int yfirst = by[0], ylast = by[h * 2 - 2] + by[h * 2 - 1];
    /* horizontal pass on the rows the vertical pass uses, then vertical (Pillow ImagingResampleInner) */
    tmp = aimg_new(w, ylast - yfirst, c);
    out = aimg_new(w, h, c);
    if (!tmp || !out) goto done;
    for (int y = 0; y < tmp->h; y++)
        for (int x = 0; x < w; x++) {
            int xmin = bx[x * 2], xmax = bx[x * 2 + 1];
            const int *k = kx + (size_t)x * kw;
            for (int ch = 0; ch < c; ch++) {
                int ss = 1 << (PRECISION_BITS - 1);
                const unsigned char *s = in->p + ((size_t)(y + yfirst) * in->w + xmin) * c + ch;
                for (int i = 0; i < xmax; i++) ss += s[(size_t)i * c] * k[i];
                tmp->p[((size_t)y * w + x) * c + ch] = clip8(ss);
            }
        }
    for (int y = 0; y < h; y++) {
        int ymin = by[y * 2] - yfirst, ymax = by[y * 2 + 1];
        const int *k = ky + (size_t)y * kh;
        for (int x = 0; x < w; x++)
            for (int ch = 0; ch < c; ch++) {
                int ss = 1 << (PRECISION_BITS - 1);
                for (int i = 0; i < ymax; i++) ss += tmp->p[((size_t)(ymin + i) * w + x) * c + ch] * k[i];
                out->p[((size_t)y * w + x) * c + ch] = clip8(ss);
            }
    }
done:
    free(bx); free(kx); free(by); free(ky);
    aimg_free(tmp);
    if (out && (!kw || !kh)) { aimg_free(out); out = NULL; }
    return out;
}

/* Pillow RGBA <-> RGBa (premultiplied) */
void aimg_premultiply(AImg *im)
{
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        unsigned char *p = im->p + 4 * i;
        unsigned a = p[3], t;
        for (int ch = 0; ch < 3; ch++) { t = p[ch] * a + 128; p[ch] = (unsigned char)(((t >> 8) + t) >> 8); }
    }
}
void aimg_unpremultiply(AImg *im)
{
    for (size_t i = 0; i < (size_t)im->w * im->h; i++) {
        unsigned char *p = im->p + 4 * i;
        unsigned a = p[3];
        if (a == 255 || a == 0) continue;
        for (int ch = 0; ch < 3; ch++) { unsigned v = 255 * p[ch] / a; p[ch] = (unsigned char)(v > 255 ? 255 : v); }
    }
}

/* Pillow ImageFilter.BoxBlur(radius) on one 8-bit channel (one pass each way, edges extended) */
void aimg_box_blur(unsigned char *ch, int w, int h, float radius)
{
    int r = (int)radius;
    unsigned ww = (unsigned)((float)(1 << 24) / (radius * 2 + 1));
    unsigned fw = ((1u << 24) - (unsigned)(r * 2 + 1) * ww) / 2;
    unsigned char *tmp = malloc((size_t)w * h);
    if (!tmp) return;
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? h : w, lines = pass ? w : h;
        for (int l = 0; l < lines; l++) {
            for (int i = 0; i < n; i++) {
                #define AT(j) (pass ? ch[(size_t)((j) < 0 ? 0 : (j) >= n ? n - 1 : (j)) * w + l] \
                                    : ch[(size_t)l * w + ((j) < 0 ? 0 : (j) >= n ? n - 1 : (j))])
                unsigned acc = 0;
                for (int j = i - r; j <= i + r; j++) acc += AT(j) * ww;
                acc += (AT(i - r - 1) + AT(i + r + 1)) * fw;
                #undef AT
                unsigned char v = (unsigned char)((acc + (1u << 23)) >> 24);
                if (pass) tmp[(size_t)i * w + l] = v; else tmp[(size_t)l * w + i] = v;
            }
        }
        memcpy(ch, tmp, (size_t)w * h);
    }
    free(tmp);
}
