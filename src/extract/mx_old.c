/* Containers and formats of the older Encarta editions (95 to 98), for the built-in extractor:
 *  - WinHelp / MediaView 1.x internal file system (magic 0x00035F3F): Encarta 95 MM95BAG.M12, Encarta 96 (Mac)
 *    "MindMaze Baggage". B-tree directory (magic 0x293B) of {NUL-terminated name, i32 file offset}; each file is
 *    {u32 reserved, u32 used, u8 flags} then its bytes.
 *  - MediaView 2.0 file system (magic 0x01045F3F): Encarta 97 / 98 MMBAG.M20. Directory at the u32 offset in the
 *    header: entry count u32 at +0x2C, entries from +0x3C {u8 name length, name, varint offset, varint size, u8}
 *    (varints: 7-bit groups, low group first, high bit = more).
 *  - KWAJ (MS COMPRESS) method 3 (LZ + Huffman, 4 KB window), the *.DI$ pictures in both: written from the format
 *    description of libmspack's kwajd.c.
 *  - Encarta 95 sprites: double width, the picture on the left and a mask on the right (black = opaque); turned into
 *    the 2006 form (8-bit, palette index 254 = cyan = transparent) so every edition is drawn the same way.
 * Formats found by the surveys of 2026-11-18 (build/versions/<edition>/REPORT.txt). */
#include <stdlib.h>
#include <string.h>
#include "mx.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* ---------------------------------------------------------------------------------------------- file systems */
static int winhelp_files(const uint8_t *d, size_t n, void (*fn)(void *, const char *, const uint8_t *, size_t), void *ctx)
{
    uint32_t dir = le32(d + 4);
    if (dir + 9 + 38 > n) return 0;
    const uint8_t *h = d + dir + 9;
    if (le16(h) != 0x293B) return 0;
    unsigned page = le16(h + 4);
    int root = (int16_t)le16(h + 26), levels = (int16_t)le16(h + 32);
    size_t pages = dir + 9 + 38;
    if (page < 16 || levels < 1 || levels > 8) return 0;
    int p = root;
    for (int l = 0; l < levels - 1; l++) {                       /* down the left edge to the first leaf */
        size_t po = pages + (size_t)p * page;
        if (po + 8 > n) return 0;
        p = (int16_t)le16(d + po + 4);
    }
    int files = 0;
    for (int guard = 0; p != -1 && guard < 4096; guard++) {
        size_t po = pages + (size_t)p * page;
        if (p < 0 || po + 8 > n) return 0;
        int ne = (int16_t)le16(d + po + 2);
        size_t q = po + 8, end = po + page < n ? po + page : n;
        for (int i = 0; i < ne; i++) {
            const uint8_t *z = memchr(d + q, 0, end - q);
            if (!z || (size_t)(z - d) + 5 > end) return 0;
            char name[96];
            size_t nl = (size_t)(z - (d + q));
            if (nl >= sizeof name) nl = sizeof name - 1;
            memcpy(name, d + q, nl);
            name[nl] = 0;
            q = (size_t)(z - d) + 1;
            uint32_t fo = le32(d + q);
            q += 4;
            if (fo + 9 <= n) {
                uint32_t used = le32(d + fo + 4);
                if (used <= n - fo - 9) { fn(ctx, name, d + fo + 9, used); files++; }
            }
        }
        p = (int16_t)le16(d + po + 6);
    }
    return files;
}

static int varint(const uint8_t *d, size_t n, size_t *p, uint32_t *v)
{
    *v = 0;
    for (int s = 0; s < 35; s += 7) {
        if (*p >= n) return 0;
        uint8_t b = d[(*p)++];
        *v |= (uint32_t)(b & 0x7f) << s;
        if (!(b & 0x80)) return 1;
    }
    return 0;
}

static int mv20_files(const uint8_t *d, size_t n, void (*fn)(void *, const char *, const uint8_t *, size_t), void *ctx)
{
    uint32_t dir = le32(d + 4);
    if ((size_t)dir + 0x3C > n) return 0;
    uint32_t count = le32(d + dir + 0x2C);
    size_t p = dir + 0x3C;
    int files = 0;
    for (uint32_t i = 0; i < count && i < 100000; i++) {
        if (p >= n) return 0;
        unsigned nl = d[p++];
        if (p + nl > n) return 0;
        char name[256];
        memcpy(name, d + p, nl);
        name[nl] = 0;
        p += nl;
        uint32_t off, size;
        if (!varint(d, n, &p, &off) || !varint(d, n, &p, &size)) return 0;
        p++;                                                     /* flags */
        if (off <= n && size <= n - off) { fn(ctx, name, d + off, size); files++; }
    }
    return files;
}

int mx_old_files(const uint8_t *d, size_t n, void (*fn)(void *, const char *, const uint8_t *, size_t), void *ctx)
{
    if (n < 16) return -1;
    uint32_t magic = le32(d);
    if (magic == 0x00035F3F) return winhelp_files(d, n, fn, ctx);
    if (magic == 0x01045F3F) return mv20_files(d, n, fn, ctx);
    return -1;
}

/* ---------------------------------------------------------------------------------------------- KWAJ */
typedef struct { const uint8_t *d; size_t n, p; uint32_t buf; int bits, over; } KBits;

static unsigned kget(KBits *b, int k)
{
    while (b->bits < k) {
        uint32_t c = 0;
        if (b->p < b->n) c = b->d[b->p++];
        else b->over++;
        b->buf = b->buf << 8 | c;
        b->bits += 8;
    }
    b->bits -= k;
    unsigned v = (b->buf >> b->bits) & ((1u << k) - 1);
    b->buf &= (1u << b->bits) - 1;
    return v;
}

typedef struct { short count[17], sym[256]; } KHuff;
static int khuff(KHuff *h, const uint8_t *len, int n)
{
    short offs[17];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) { if (len[i] > 16) return 0; h->count[len[i]]++; }
    h->count[0] = 0;
    offs[1] = 0;
    for (int l = 1; l < 16; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    return 1;
}
static int ksym(KBits *b, const KHuff *h)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 16; l++) {
        code |= (int)kget(b, 1);
        int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}
static int klens(KBits *b, int type, int n, uint8_t *L)
{
    if (type == 0) { int c = n == 16 ? 4 : n == 32 ? 5 : n == 64 ? 6 : 8; memset(L, c, (size_t)n); return 1; }
    if (type == 3) { for (int i = 0; i < n; i++) L[i] = (uint8_t)kget(b, 4); return 1; }
    if (type != 1 && type != 2) return 0;
    int c = (int)kget(b, 4);
    L[0] = (uint8_t)c;
    for (int i = 1; i < n; i++) {
        if (type == 1) {
            if (kget(b, 1) == 0) {}
            else if (kget(b, 1) == 0) c++;
            else c = (int)kget(b, 4);
        } else {
            int sel = (int)kget(b, 2);
            c = sel == 3 ? (int)kget(b, 4) : (c + sel - 1) & 0xff;
        }
        L[i] = (uint8_t)c;
    }
    return 1;
}

uint8_t *mx_kwaj(const uint8_t *in, size_t n, size_t *outlen)
{
    static const uint8_t SIG[8] = {'K', 'W', 'A', 'J', 0x88, 0xF0, 0x27, 0xD1};
    if (n < 14 || memcmp(in, SIG, 8)) return NULL;
    unsigned method = le16(in + 8), off = le16(in + 10);
    if (off > n) return NULL;
    size_t cap = 1 << 16, o = 0;
    uint8_t *out = malloc(cap);
    if (!out) return NULL;
    if (method == 0) {                                           /* stored */
        free(out);
        *outlen = n - off;
        out = malloc(*outlen + 1);
        if (out) memcpy(out, in + off, *outlen);
        return out;
    }
    if (method != 3) { free(out); return NULL; }
    KBits b = {in, n, off, 0, 0, 0};
    int types[6];
    for (int i = 0; i < 6; i++) types[i] = (int)kget(&b, 4);
    static const int SIZES[5] = {16, 16, 32, 64, 256};
    KHuff T[5];
    uint8_t L[256];
    for (int i = 0; i < 5; i++)
        if (!klens(&b, types[i], SIZES[i], L) || !khuff(&T[i], L, SIZES[i])) { free(out); return NULL; }
    uint8_t win[4096];
    memset(win, ' ', sizeof win);
    unsigned pos = 0;
    int lit_run = 0;
    while (b.over <= 2) {
        int len = ksym(&b, lit_run ? &T[1] : &T[0]);
        if (len < 0) break;
        int count;
        if (len > 0) {
            count = len + 2;
            lit_run = 0;
            int hi = ksym(&b, &T[3]);
            if (hi < 0) break;
            unsigned dist = (unsigned)hi << 6 | kget(&b, 6);
            for (int i = 0; i < count; i++) {
                if (o == cap) { uint8_t *no = realloc(out, cap *= 2); if (!no) { free(out); return NULL; } out = no; }
                uint8_t c = win[(pos + 4096 - dist) & 4095];
                win[pos] = c;
                out[o++] = c;
                pos = (pos + 1) & 4095;
            }
        } else {
            int ll = ksym(&b, &T[2]);
            if (ll < 0) break;
            count = ll + 1;
            lit_run = count == 32 ? 0 : 1;
            for (int i = 0; i < count; i++) {
                int c = ksym(&b, &T[4]);
                if (c < 0) break;
                if (o == cap) { uint8_t *no = realloc(out, cap *= 2); if (!no) { free(out); return NULL; } out = no; }
                win[pos] = (uint8_t)c;
                out[o++] = (uint8_t)c;
                pos = (pos + 1) & 4095;
            }
        }
    }
    if (o >= 6 && out[0] == 'B' && out[1] == 'M') {              /* a BMP: cut the bit-stream padding off */
        uint32_t sz = le32(out + 2);
        if (sz <= o) o = sz;
    }
    *outlen = o;
    return out;
}

/* ---------------------------------------------------------------------------------------------- 95 masks */
/* A double-width 8-bit BMP (picture | mask) -> a single-width 8-bit BMP with palette index 254 = cyan for the
 * transparent pixels (mask white). Returns NULL if the picture is not of that kind (caller keeps it as it is). */
uint8_t *mx_unmask95(const uint8_t *bmp, size_t n, size_t *outlen)
{
    if (n < 54 || bmp[0] != 'B' || bmp[1] != 'M') return NULL;
    uint32_t off = le32(bmp + 10), hs = le32(bmp + 14);
    int w = (int)le32(bmp + 18), h = (int)le32(bmp + 22);
    unsigned bpp = le16(bmp + 28), comp = le32(bmp + 30), ncol = le32(bmp + 46);
    if (bpp != 8 || comp != 0 || w < 2 || (w & 1) || h == 0 || hs < 40) return NULL;
    int ah = h < 0 ? -h : h, hw = w / 2;
    size_t row = ((size_t)w + 3) & ~(size_t)3, orow = ((size_t)hw + 3) & ~(size_t)3;
    if (!ncol) ncol = 256;
    if (14 + hs + 4 * ncol > n || off + row * ah > n || ncol > 256) return NULL;
    const uint8_t *pal = bmp + 14 + hs, *px = bmp + off;
    /* the mask half must be only black and white */
    int light[256];
    for (unsigned i = 0; i < 256; i++) light[i] = i < ncol ? (pal[4 * i] + pal[4 * i + 1] + pal[4 * i + 2]) > 384 : 0;
    for (int y = 0; y < ah; y++)
        for (int x = hw; x < w; x++) {
            const uint8_t *c = pal + 4 * px[(size_t)y * row + x];
            int s = c[0] + c[1] + c[2];
            if (s > 30 && s < 735) return NULL;
        }
    size_t osz = 14 + 40 + 1024 + orow * ah;
    uint8_t *o = calloc(1, osz);
    if (!o) return NULL;
    o[0] = 'B'; o[1] = 'M';
    uint32_t v[] = {(uint32_t)osz, 0, 14 + 40 + 1024};
    memcpy(o + 2, v, 12);
    uint8_t *ih = o + 14;
    ih[0] = 40;
    int32_t W = hw, H = h;
    memcpy(ih + 4, &W, 4);
    memcpy(ih + 8, &H, 4);
    ih[12] = 1;
    ih[14] = 8;
    uint32_t isz = (uint32_t)(orow * ah), used = 256;
    memcpy(ih + 20, &isz, 4);
    memcpy(ih + 32, &used, 4);
    uint8_t *op = o + 54;
    memcpy(op, pal, 4 * ncol);
    op[4 * 254] = 255; op[4 * 254 + 1] = 255; op[4 * 254 + 2] = 0; op[4 * 254 + 3] = 0;   /* BGR: cyan */
    uint8_t *dst = o + 54 + 1024;
    for (int y = 0; y < ah; y++)
        for (int x = 0; x < hw; x++) {
            uint8_t ix = px[(size_t)y * row + x], m = px[(size_t)y * row + hw + x];
            dst[(size_t)y * orow + x] = light[m] ? 254 : (ix == 254 ? 253 : ix);   /* keep 254 for transparency */
        }
    *outlen = osz;
    return o;
}
