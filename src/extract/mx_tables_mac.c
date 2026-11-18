/* MindMaze's tables from the Encarta 96 Macintosh application (CHANGE 2026-11-18): mx_tables_mac (see mx_tables.h).
 *
 * Input: the resource fork of "/Installer Files/Encarta 96" (APPL/ENCA, 68k, built with Microsoft's Windows-on-Mac
 * toolchain; known build: 4398903 bytes, sha256 5ec7cb51...). mx_hfs_file (mx_edition.c) reads it from the disc
 * image. Reference: build/versions/enc96/tables/extract_tables96.py (same results).
 *
 * Format facts (all big-endian):
 *   resource fork  header u32 dataOff, mapOff, dataLen, mapLen; map +24 u16 type list offset; type list u16 n-1,
 *                  n x {char[4] type, u16 count-1, u16 ref list offset (from the type list)}; ref
 *                  {i16 id, u16 name, u8 attr, u24 data offset (from dataOff)}; data = u32 length + bytes.
 *   CODE 0 +4      u32 below-A5 size (0x47ac0).
 *   DATA 0         the globals below A5: u32 near_bss, near_size, far_bss, far_size (sum = below-A5 size), then
 *                  near_size bytes of near data (placed so that it ends at A5-near_bss, i.e. starts at A5-0x2df0)
 *                  and far_size bytes of far data (starting at A5-below = A5-0x47ac0). A pointer in the globals is
 *                  a signed 32-bit A5 offset.
 *   tables (A5 offsets): music char*[5] -0x172c, group pointers obj*[7] -0x1718 (6 groups + end) and counts u32[6]
 *                  -0x16fc, picture areas char*[9] -0x1e000, sounds char*[35] -0x1d800, ranks 6 x {name*, image*,
 *                  u32 min, u32 max} -0x1c3bc, speech (char**)[23] -0x10fe8 and counts u32[23] -0x10f8c.
 *   room object    0x38 bytes: +0 name*, +4 i32 x, +8 i32 y, +0x14 i32 rect[4], +0x24 i32 half, +0x28 u8 slots[7],
 *                  +0x30 u16 flags, +0x32 u16 id.
 *   STR# n         a Windows STRINGTABLE block: u16 count, Pascal strings (Mac Roman); string id = (n-1)*16 + index.
 *                  MindMaze's ids are in STR# 1813..1816 (1817 is read too if present, ids up to 29069).
 *   WBMP id        a Windows BITMAP resource (BITMAPINFOHEADER + palette + pixels) whose 11 header fields are
 *                  byte-swapped to big-endian: swapped back, then mx_dib_to_bmp. Ids 29150..29164, 29166.
 * The program's own spellings are kept ("NDOOR.DI$", lower-case music, CR line breaks, "<squawk!>");
 * mx_tables_json normalises them. Every read is bounds-checked; counts and pointers are validated.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_tables.h"

enum {
    A5_MUSIC = -0x172C, A5_GROUP_PTRS = -0x1718, A5_GROUP_COUNTS = -0x16FC, A5_PICTURE_AREAS = -0x1E000,
    A5_SOUNDS = -0x1D800, A5_RANKS = -0x1C3BC, A5_SPEECH = -0x10FE8, A5_SPEECH_COUNTS = -0x10F8C,
    OBJECT_SIZE = 0x38, RANK_SIZE = 16, MAX_SPEECH = 1000, MAX_BELOW_A5 = 16 << 20, MAX_TEXT = 4096
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static unsigned be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* ---------------------------------------------------------------------------------------------- resource fork */
typedef struct { const uint8_t *d; size_t n; uint32_t doff, dlen; const uint8_t *map; uint32_t mlen; } Fork;

static int fork_open(Fork *f, const uint8_t *d, size_t n)
{
    if (!d || n < 16) return 0;
    f->d = d;
    f->n = n;
    f->doff = be32(d);
    uint32_t moff = be32(d + 4);
    f->dlen = be32(d + 8);
    f->mlen = be32(d + 12);
    if (f->doff > n || f->dlen > n - f->doff || moff > n || f->mlen > n - moff || f->mlen < 30) return 0;
    f->map = d + moff;
    return 1;
}

/* resource (type, id) -> its bytes, NULL if missing or out of range */
static const uint8_t *fork_get(const Fork *f, const char *type, int id, size_t *len)
{
    unsigned tl = be16(f->map + 24);
    if (tl + 2 > f->mlen) return NULL;
    unsigned ntypes = (be16(f->map + tl) + 1) & 0xFFFF;
    for (unsigned i = 0; i < ntypes; i++) {
        unsigned te = tl + 2 + 8 * i;
        if (te + 8 > f->mlen) return NULL;
        if (memcmp(f->map + te, type, 4)) continue;
        unsigned cnt = be16(f->map + te + 4) + 1, rl = tl + be16(f->map + te + 6);
        for (unsigned j = 0; j < cnt; j++) {
            unsigned re = rl + 12 * j;
            if (re + 12 > f->mlen) return NULL;
            if ((int16_t)be16(f->map + re) != id) continue;
            uint32_t off = (uint32_t)f->map[re + 5] << 16 | f->map[re + 6] << 8 | f->map[re + 7];
            if (off > f->dlen || f->dlen - off < 4) return NULL;
            uint32_t n = be32(f->d + f->doff + off);
            if (n > f->dlen - off - 4) return NULL;
            *len = n;
            return f->d + f->doff + off + 4;
        }
        return NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- A5 world */
typedef struct { uint8_t *img; int64_t below; int bad; } A5;

static const uint8_t *a5_at(A5 *g, int64_t a5, size_t n)
{
    int64_t o = a5 + g->below;
    if (o < 0 || o > g->below || (int64_t)n > g->below - o) { g->bad = 1; return NULL; }
    return g->img + o;
}
static uint32_t a5_u32(A5 *g, int64_t a5) { const uint8_t *p = a5_at(g, a5, 4); return p ? be32(p) : 0; }
static int64_t a5_ptr(A5 *g, int64_t a5) { const uint8_t *p = a5_at(g, a5, 4); return p ? (int32_t)be32(p) : 0; }

/* C string at A5 offset a5 -> malloc'd UTF-8; marks g bad if out of range, unterminated, too long, holds control
 * characters other than line breaks / tabs, or is empty while nonempty is required */
static char *a5_cstr(A5 *g, int64_t a5, int nonempty)
{
    const uint8_t *p = a5_at(g, a5, 1);
    if (!p) return NULL;
    size_t room = (size_t)(g->img + g->below - p), n = 0;
    while (n < room && n <= MAX_TEXT && p[n]) {
        if (p[n] < 0x20 && p[n] != '\r' && p[n] != '\n' && p[n] != '\t') { g->bad = 1; return NULL; }
        n++;
    }
    if (n >= room || n > MAX_TEXT || (nonempty && !n)) { g->bad = 1; return NULL; }
    char *s = mx_strdup_macroman(p, n);
    if (!s) g->bad = 1;
    return s;
}
static char *a5_pstr(A5 *g, int64_t a5, int nonempty)          /* string through the pointer stored at a5 */
{
    int64_t q = a5_ptr(g, a5);
    if (g->bad) return NULL;
    if (q >= 0) { g->bad = 1; return NULL; }                     /* the globals all lie below A5 */
    return a5_cstr(g, q, nonempty);
}

/* ---------------------------------------------------------------------------------------------- reader */
int mx_tables_mac(const uint8_t *rsrc, size_t rsrclen, MXTables *t, char *err, int errlen)
{
    const char *why = NULL;
    A5 g = {0};
    Fork f;
    size_t n0 = 0, nd = 0;
    memset(t, 0, sizeof *t);
    if (err && errlen > 0) err[0] = 0;
    if (!fork_open(&f, rsrc, rsrclen)) { why = "its resource fork is missing or damaged"; goto fail; }

    /* the A5 world, rebuilt from DATA 0 */
    const uint8_t *code0 = fork_get(&f, "CODE", 0, &n0), *data0 = fork_get(&f, "DATA", 0, &nd);
    if (!code0 || n0 < 8 || !data0 || nd < 16) { why = "its program code (CODE 0 / DATA 0) is missing"; goto fail; }
    uint32_t below = be32(code0 + 4), near_bss = be32(data0), near_size = be32(data0 + 4), far_bss = be32(data0 + 8),
             far_size = be32(data0 + 12);
    uint64_t sum = (uint64_t)near_bss + near_size + far_bss + far_size;
    if (!below || below > MAX_BELOW_A5 || sum != below || 16 + (uint64_t)near_size + far_size > nd) {
        why = "its global data (DATA 0) does not have the expected layout";
        goto fail;
    }
    g.below = below;
    g.img = calloc(below, 1);
    if (!g.img) { why = "out of memory"; goto fail; }
    memcpy(g.img, data0 + 16 + near_size, far_size);                         /* far data from A5-below */
    memcpy(g.img + far_size + far_bss, data0 + 16, near_size);               /* near data, ends at A5-near_bss */

    /* room objects: 6 groups, one table of 0x38-byte records */
    int64_t ptr[7];
    int total = 0;
    for (int i = 0; i < 7; i++) ptr[i] = a5_ptr(&g, A5_GROUP_PTRS + 4 * i);
    for (int i = 0; i < 6; i++) {
        uint32_t c = a5_u32(&g, A5_GROUP_COUNTS + 4 * i);
        if (g.bad || c > MXT_MAX_OBJECTS || total + (int)c > MXT_MAX_OBJECTS || ptr[i + 1] != ptr[i] + (int64_t)c * OBJECT_SIZE) {
            why = "its room object table is not where this reader expects it (an unknown build?)";
            goto fail;
        }
        t->counts[i] = (int)c;
        total += (int)c;
    }
    if (!total || !a5_at(&g, ptr[0], (size_t)total * OBJECT_SIZE)) { why = "its room object table is out of range"; goto fail; }
    t->nobjects = total;
    for (int i = 0; i < total; i++) {
        int64_t o = ptr[0] + (int64_t)i * OBJECT_SIZE;
        const uint8_t *r = a5_at(&g, o, OBJECT_SIZE);
        MXObject *ob = &t->obj[i];
        ob->name = a5_pstr(&g, o, 1);
        if (g.bad) { why = "a room object's picture name is unreadable"; goto fail; }
        ob->x = (int32_t)be32(r + 4);
        ob->y = (int32_t)be32(r + 8);
        for (int k = 0; k < 4; k++) ob->rect[k] = (int32_t)be32(r + 0x14 + 4 * k);
        ob->half = (int32_t)be32(r + 0x24);
        for (int k = 0; k < 7; k++) ob->slots[k] = r[0x28 + k];
        ob->flags = (int)be16(r + 0x30);
        ob->id = (int)be16(r + 0x32);
    }

    /* ranks, name lists */
    for (int i = 0; i < 6; i++) {
        int64_t a = A5_RANKS + RANK_SIZE * i;
        t->rank[i].name = a5_pstr(&g, a, 1);
        t->rank[i].image = a5_pstr(&g, a + 4, 1);
        t->rank[i].min = a5_u32(&g, a + 8);
        t->rank[i].max = a5_u32(&g, a + 12);
        if (!g.bad && t->rank[i].min > t->rank[i].max) g.bad = 1;
    }
    if (g.bad) { why = "its rank table is unreadable"; goto fail; }
    for (int i = 0; i < 9; i++) t->picture_areas[i] = a5_pstr(&g, A5_PICTURE_AREAS + 4 * i, 1);
    for (int i = 0; i < 5; i++) t->music[i] = a5_pstr(&g, A5_MUSIC + 4 * i, 1);
    for (int i = 0; i < 35; i++) t->sounds[i] = a5_pstr(&g, A5_SOUNDS + 4 * i, 1);
    if (g.bad) { why = "its picture, music or sound name lists are unreadable"; goto fail; }

    /* speech: 23 lists of texts */
    for (int k = 0; k < 23; k++) {
        uint32_t c = a5_u32(&g, A5_SPEECH_COUNTS + 4 * k);
        int64_t lst = a5_ptr(&g, A5_SPEECH + 4 * k);
        if (g.bad || c > MAX_SPEECH || (c && (lst >= 0 || !a5_at(&g, lst, (size_t)c * 4)))) {
            why = "its speech table is not where this reader expects it (an unknown build?)";
            goto fail;
        }
        if (!c) continue;
        t->speech[k] = calloc(c, sizeof(char *));
        if (!t->speech[k]) { why = "out of memory"; goto fail; }
        t->nspeech[k] = (int)c;
        for (uint32_t j = 0; j < c; j++) t->speech[k][j] = a5_pstr(&g, lst + 4 * (int64_t)j, 0);
        if (g.bad) { why = "a speech text is unreadable"; goto fail; }
    }

    /* UI strings: STR# blocks holding ids MXT_FIRST_STRING..MXT_LAST_STRING; missing ones stay NULL */
    for (int block = (MXT_FIRST_STRING >> 4) + 1; block <= (MXT_LAST_STRING >> 4) + 1; block++) {
        size_t n = 0;
        const uint8_t *d = fork_get(&f, "STR#", block, &n);
        if (!d) continue;
        if (n < 2) { why = "a text resource (STR#) is damaged"; goto fail; }
        size_t p = 2;
        for (unsigned k = 0, cnt = be16(d); k < cnt; k++) {
            if (p >= n || p + 1 + d[p] > n) { why = "a text resource (STR#) is damaged"; goto fail; }
            int sid = (block - 1) * 16 + (int)k;
            if (d[p] && sid >= MXT_FIRST_STRING && sid <= MXT_LAST_STRING) {
                char **dst = &t->str[sid - MXT_FIRST_STRING];
                free(*dst);
                *dst = mx_strdup_macroman(d + p + 1, d[p]);
                if (!*dst) { why = "out of memory"; goto fail; }
            }
            p += 1 + (size_t)d[p];
        }
    }

    /* panel bitmaps: WBMP with a big-endian BITMAPINFOHEADER -> little-endian DIB -> .bmp */
    static const int FIELD[11] = {4, 4, 4, 2, 2, 4, 4, 4, 4, 4, 4};
    for (int i = 0; i < MXT_NBITMAPS; i++) {
        size_t n = 0;
        const uint8_t *w = fork_get(&f, "WBMP", mx_table_bitmaps[i], &n);
        if (!w || n < 40 || be32(w) != 40 || be16(w + 12) != 1) { why = "a panel bitmap (WBMP) is missing or damaged"; goto fail; }
        uint8_t *dib = malloc(n);
        if (!dib) { why = "out of memory"; goto fail; }
        memcpy(dib, w, n);
        for (int k = 0, o = 0; k < 11; o += FIELD[k++])
            for (int b = 0; b < FIELD[k]; b++) dib[o + b] = w[o + FIELD[k] - 1 - b];
        t->bmp[i] = mx_dib_to_bmp(dib, n, &t->bmplen[i]);
        free(dib);
        if (!t->bmp[i]) { why = "out of memory"; goto fail; }
    }
    free(g.img);
    return 1;

fail:
    if (err && errlen > 0)
        snprintf(err, (size_t)errlen, "Could not read MindMaze from the Encarta 96 (Macintosh) program: %s.", why);
    free(g.img);
    mx_tables_free(t);
    return 0;
}
