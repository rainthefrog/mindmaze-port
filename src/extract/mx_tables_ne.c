/* MindMaze's tables from Encarta 95 (MM95.EXE, 16-bit Windows NE, MFC). CHANGE 2026-11-18.
 *
 * Reads what build/versions/enc95mpeg/tables/extract_tables95.py reads, all by pattern (strings and the far
 * pointers to them), no fixed offsets. Format facts (surveyed file: 73 segments, alignment shift 9):
 * - a far pointer is u16 offset + u16 segment; the segment word is filled by an NE internal-reference segment
 *   fixup (address type 2, relocation type 0, not 0xFF = movable), whose offsets form a chain through the
 *   segment words (0xFFFF ends it). So a far pointer = a fixup at pos+2 plus the offset word at pos.
 * - room objects (auto data, 73:0F46 in the surveyed file): 0x22-byte records starting at the far pointer to
 *   "NDOOR.DI$": +0 far name, +4 i16 x, +6 i16 y, +8/+A i16 (0), +C i16 rect l,t,r,b, +14 u16 half,
 *   +16 u8 slots[7], +1E u16 flags, +20 u16 id. The group table (73:18D6) is the far pointer to record 0
 *   followed by more far pointers (7: the 7th group FLATPIC/FLATPIC2 is not in 2006, not read), then one u16
 *   count per group. Only the first 6 groups go to MXTables.
 * - ranks (73:0EFE): 6 x {far name, far picture, u16 min, u16 max}, starting at the far pointer to "No Award".
 * - speech (segment 28): 23 far pointers to lists in the same segment, 23 u16 counts (1..10) somewhere in that
 *   segment such that every list entry is a far pointer to a string of more than 20 characters.
 * - picture areas (15:0000): 9 far pointers at "Life Science" (not the run that follows "All Areas");
 *   area chooser (18:0202): 10 far pointers at "All Areas" -> 29014..29023; panel labels (18:0146): 7 far
 *   pointers at "Choose Area of Interest" -> 29028..29034; sounds (29:0000): 35 far pointers at "AMB1.WAV".
 * - music names are pushed inline (no table): found as strings (any case; kept as spelled, e.g. "intro.MID").
 * - other UI strings are inline C strings (no STRINGTABLE); 29040/29041 are wsprintf pieces in 95, composed
 *   into the 2006 printf form; "%c." -> 29035..29038 "A.".."D.". A string not found stays NULL.
 * - panel bitmaps: named BITMAP resources IDB_BOOKS .. COUNTDN, padded to the resource alignment (trimmed to
 *   header + palette + rows) -> 29150..29164, 29166.
 * Texts keep the program's own spellings ("NDOOR.DI$", real line breaks, "<squawk!>"); mx_tables_json
 * normalises them. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_tables.h"

enum { REC = 0x22, NGROUPS = 6, MAX_GROUPS = 64, NSPEECH = 23, NE_RT_BITMAP = 2 };

typedef struct {
    size_t fo, ln, flen;         /* file offset, length (as the header says), length present in the file */
    unsigned flags;
    uint16_t *fxv;               /* fixup target segment by offset of the segment word (lazy) */
    uint8_t *fxh;                /* 1 = a fixup at that offset */
    int fxdone;
} NESeg;

typedef struct {
    const uint8_t *d;
    size_t n;
    unsigned ne, nseg;
    NESeg *seg;
    int oom;
} NE;

static unsigned rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void say(char *err, int errlen, const char *fmt, ...)
{
    if (!err || errlen <= 0) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(err, (size_t)errlen, fmt, a);
    va_end(a);
}

/* ---------------------------------------------------------------------------------------------- NE file */
static int ne_open(NE *ne, const uint8_t *d, size_t n)
{
    memset(ne, 0, sizeof *ne);
    ne->d = d;
    ne->n = n;
    if (!d || n < 0x40 || d[0] != 'M' || d[1] != 'Z') return 0;
    ne->ne = rd16(d + 0x3C);
    if ((size_t)ne->ne + 0x40 > n || d[ne->ne] != 'N' || d[ne->ne + 1] != 'E') return 0;
    const uint8_t *h = d + ne->ne;
    unsigned nseg = rd16(h + 0x1C), segtab = rd16(h + 0x22), align = rd16(h + 0x32);
    if (align > 16 || (size_t)ne->ne + segtab + 8 * (size_t)nseg > n) return 0;
    ne->seg = calloc(nseg ? nseg : 1, sizeof *ne->seg);
    if (!ne->seg) { ne->oom = 1; return 0; }
    ne->nseg = nseg;
    for (unsigned i = 0; i < nseg; i++) {
        const uint8_t *e = d + ne->ne + segtab + 8 * i;
        NESeg *s = &ne->seg[i];
        unsigned off = rd16(e), ln = rd16(e + 2);
        s->flags = rd16(e + 4);
        if (!off) continue;
        s->fo = (size_t)off << align;
        s->ln = ln ? ln : 0x10000;
        s->flen = s->fo >= n ? 0 : n - s->fo < s->ln ? n - s->fo : s->ln;
    }
    return 1;
}

static void ne_close(NE *ne)
{
    for (unsigned i = 0; i < ne->nseg; i++) { free(ne->seg[i].fxv); free(ne->seg[i].fxh); }
    free(ne->seg);
}

/* data of segment sn (1-based); NULL + 0 for none */
static const uint8_t *seg(const NE *ne, unsigned sn, size_t *len)
{
    *len = 0;
    if (sn < 1 || sn > ne->nseg || !ne->seg[sn - 1].flen) return NULL;
    *len = ne->seg[sn - 1].flen;
    return ne->d + ne->seg[sn - 1].fo;
}

/* internal segment fixups of segment sn, chained; the first record to claim an offset keeps it */
static NESeg *fixups(NE *ne, unsigned sn)
{
    if (sn < 1 || sn > ne->nseg) return NULL;
    NESeg *g = &ne->seg[sn - 1];
    if (g->fxdone) return g->fxv ? g : NULL;
    g->fxdone = 1;
    size_t len;
    const uint8_t *s = seg(ne, sn, &len);
    if (!s || !(g->flags & 0x100)) return NULL;
    size_t p = g->fo + g->ln;
    if (p + 2 > ne->n) return NULL;
    unsigned nrel = rd16(ne->d + p);
    g->fxv = calloc(len + 2, sizeof *g->fxv);
    g->fxh = calloc(len + 2, 1);
    if (!g->fxv || !g->fxh) { free(g->fxv); free(g->fxh); g->fxv = NULL; g->fxh = NULL; ne->oom = 1; return NULL; }
    for (unsigned i = 0; i < nrel && p + 2 + 8 * (size_t)i + 8 <= ne->n; i++) {
        const uint8_t *r = ne->d + p + 2 + 8 * i;
        unsigned at = r[0], rt = r[1], off = rd16(r + 2), a = rd16(r + 4);
        if (at != 2 || rt != 0 || a == 0xFF) continue;
        while (off != 0xFFFF && off + 2 <= len && !g->fxh[off]) {
            g->fxh[off] = 1;
            g->fxv[off] = (uint16_t)a;
            off = rd16(s + off);
        }
    }
    return g;
}

/* the far pointer stored at sn:pos -> *ts:*to; 0 if there is none */
static int far_ptr(NE *ne, unsigned sn, long pos, unsigned *ts, unsigned *to)
{
    NESeg *g = fixups(ne, sn);
    size_t len;
    const uint8_t *s = seg(ne, sn, &len);
    if (!g || pos < 0 || (size_t)pos + 4 > len || !g->fxh[pos + 2] || !g->fxv[pos + 2]) return 0;
    *ts = g->fxv[pos + 2];
    *to = rd16(s + pos);
    return 1;
}

/* the NUL-terminated string at ts:to (raw bytes, length in *n); NULL if it runs off the segment */
static const uint8_t *cstr_raw(const NE *ne, unsigned ts, unsigned to, size_t *n)
{
    size_t len;
    const uint8_t *s = seg(ne, ts, &len);
    if (!s || to >= len) return NULL;
    const uint8_t *z = memchr(s + to, 0, len - to);
    if (!z) return NULL;
    *n = (size_t)(z - (s + to));
    return s + to;
}

static char *cstr(NE *ne, unsigned ts, unsigned to)
{
    size_t n;
    const uint8_t *s = cstr_raw(ne, ts, to, &n);
    char *t = s ? mx_strdup_1252(s, n) : NULL;
    if (s && !t) ne->oom = 1;
    return t;
}

/* string at the far pointer sn:pos (malloc'd), NULL if none */
static char *far_str(NE *ne, unsigned sn, long pos)
{
    unsigned ts, to;
    return far_ptr(ne, sn, pos, &ts, &to) ? cstr(ne, ts, to) : NULL;
}

/* first NUL-preceded string text (exact: also NUL-terminated; icase: ASCII case-insensitive) in any segment */
static int find_string(const NE *ne, const char *text, int exact, int icase, unsigned *sn, unsigned *so)
{
    size_t tl = strlen(text);
    for (unsigned n = 1; n <= ne->nseg; n++) {
        size_t len;
        const uint8_t *s = seg(ne, n, &len);
        if (!s || len < tl + 1 + (exact ? 1 : 0)) continue;
        for (size_t i = 0; i + 1 + tl + (exact ? 1 : 0) <= len; i++) {
            if (s[i] != 0) continue;
            size_t k = 0;
            for (; k < tl; k++) {
                unsigned a = s[i + 1 + k], b = (unsigned char)text[k];
                if (icase) { if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; }
                if (a != b) break;
            }
            if (k == tl && (!exact || s[i + 1 + tl] == 0)) { *sn = n; *so = (unsigned)(i + 1); return 1; }
        }
    }
    return 0;
}

typedef int (*Pick)(NE *ne, unsigned sn, unsigned pos, const void *ctx);

/* first far pointer (by segment, then position) to ts:to that pick accepts */
static int far_ref(NE *ne, unsigned ts, unsigned to, Pick pick, const void *ctx, unsigned *rn, unsigned *rp)
{
    for (unsigned n = 1; n <= ne->nseg; n++) {
        NESeg *g = fixups(ne, n);
        size_t len;
        const uint8_t *s = seg(ne, n, &len);
        if (!g) continue;
        for (size_t loc = 2; loc + 2 <= len; loc++)
            if (g->fxh[loc] && g->fxv[loc] == ts && rd16(s + loc - 2) == to && (!pick || pick(ne, n, (unsigned)loc - 2, ctx))) {
                *rn = n;
                *rp = (unsigned)loc - 2;
                return 1;
            }
    }
    return 0;
}

/* the first far pointer to the string text */
static int ref_of(NE *ne, const char *text, int exact, Pick pick, const void *ctx, unsigned *rn, unsigned *rp)
{
    unsigned sn, so;
    return find_string(ne, text, exact, 0, &sn, &so) && far_ref(ne, sn, so, pick, ctx, rn, rp);
}

/* count strings from consecutive far pointers starting at the one to first */
static int read_list(NE *ne, const char *first, int count, Pick pick, char **out)
{
    unsigned n, p;
    if (!ref_of(ne, first, 1, pick, NULL, &n, &p)) return 0;
    for (int i = 0; i < count; i++)
        if (!(out[i] = far_str(ne, n, (long)p + 4 * i))) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------------------------- tables */
static int is_group_table(NE *ne, unsigned sn, unsigned pos, const void *ctx)
{
    unsigned ts, to;
    return sn == *(const unsigned *)ctx && far_ptr(ne, sn, (long)pos + 4, &ts, &to);
}

static int read_objects(NE *ne, MXTables *t)
{
    unsigned n, base, gn, gpos, ts, to;
    size_t len;
    if (!ref_of(ne, "NDOOR.DI$", 1, NULL, NULL, &n, &base)) return 0;
    if (!far_ref(ne, n, base, is_group_table, &n, &gn, &gpos)) return 0;
    const uint8_t *s = seg(ne, n, &len);
    unsigned starts[MAX_GROUPS], total = 0, sum = 0;
    while (total < MAX_GROUPS && far_ptr(ne, n, (long)gpos + 4 * total, &ts, &to)) starts[total++] = to;
    if (total < NGROUPS || gpos + 6 * (size_t)total > len) return 0;
    for (unsigned i = 0; i < total; i++) {
        unsigned c = rd16(s + gpos + 4 * total + 2 * i);
        if (i && starts[i] != starts[i - 1] + rd16(s + gpos + 4 * total + 2 * (i - 1)) * (unsigned)REC) return 0;
        if (i < NGROUPS) { t->counts[i] = (int)c; sum += c; }
    }
    if (sum > MXT_MAX_OBJECTS || base + (size_t)sum * REC > len) return 0;
    for (unsigned i = 0; i < sum; i++) {
        size_t p = base + (size_t)i * REC;
        MXObject *o = &t->obj[i];
        if (!(o->name = far_str(ne, n, (long)p))) return 0;
        t->nobjects = (int)i + 1;
        const uint8_t *r = s + p;
        o->x = (int16_t)rd16(r + 4);
        o->y = (int16_t)rd16(r + 6);
        for (int k = 0; k < 4; k++) o->rect[k] = (int16_t)rd16(r + 0xC + 2 * k);
        o->half = (int)rd16(r + 0x14);
        for (int k = 0; k < 7; k++) o->slots[k] = r[0x16 + k];
        o->flags = (int)rd16(r + 0x1E);
        o->id = (int)rd16(r + 0x20);
    }
    return 1;
}

static int read_ranks(NE *ne, MXTables *t)
{
    unsigned n, p;
    size_t len;
    if (!ref_of(ne, "No Award", 1, NULL, NULL, &n, &p)) return 0;
    const uint8_t *s = seg(ne, n, &len);
    if (p + 12 * 6 > len) return 0;
    for (int i = 0; i < 6; i++) {
        long q = (long)p + 12 * i;
        if (!(t->rank[i].name = far_str(ne, n, q)) || !(t->rank[i].image = far_str(ne, n, q + 4))) return 0;
        t->rank[i].min = rd16(s + q + 8);
        t->rank[i].max = rd16(s + q + 10);
    }
    return 1;
}

/* every entry of the 23 lists (counts cnt) is a far pointer to a string of more than 20 characters */
static int speech_fits(NE *ne, unsigned sn, const unsigned *lists, const uint8_t *cnt)
{
    for (int k = 0; k < NSPEECH; k++)
        for (unsigned j = 0; j < rd16(cnt + 2 * k); j++) {
            unsigned ts, to;
            size_t sl;
            if (!far_ptr(ne, sn, (long)lists[k] + 4 * j, &ts, &to) || !cstr_raw(ne, ts, to, &sl) || sl <= 20) return 0;
        }
    return 1;
}

static int read_speech(NE *ne, MXTables *t)
{
    for (unsigned n = 1; n <= ne->nseg; n++) {
        NESeg *g = fixups(ne, n);
        size_t len;
        const uint8_t *s = seg(ne, n, &len);
        if (!g) continue;
        for (size_t loc = 2; loc + 2 <= len; loc++) {
            if (!g->fxh[loc] || g->fxv[loc] != n) continue;
            unsigned lists[NSPEECH], ts, to;
            int k = 0;
            for (; k < NSPEECH; k++)
                if (!far_ptr(ne, n, (long)loc - 2 + 4 * k, &ts, &to) || ts != n) break;
                else lists[k] = to;
            if (k < NSPEECH) continue;
            for (size_t c = 0; c + 46 < len; c += 2) {
                for (k = 0; k < NSPEECH; k++) { unsigned v = rd16(s + c + 2 * k); if (v < 1 || v > 10) break; }
                if (k < NSPEECH || !speech_fits(ne, n, lists, s + c)) continue;
                for (k = 0; k < NSPEECH; k++) {
                    unsigned m = rd16(s + c + 2 * k);
                    if (!(t->speech[k] = calloc(m, sizeof *t->speech[k]))) { ne->oom = 1; return 0; }
                    for (unsigned j = 0; j < m; j++) {
                        if (!(t->speech[k][j] = far_str(ne, n, (long)lists[k] + 4 * j))) return 0;
                        t->nspeech[k] = (int)j + 1;
                    }
                }
                return 1;
            }
        }
    }
    return 0;
}

static int not_after_all_areas(NE *ne, unsigned sn, unsigned pos, const void *ctx)
{
    char *prev = far_str(ne, sn, (long)pos - 4);
    int ok = !prev || strcmp(prev, "All Areas") != 0;
    free(prev);
    return ok;
}

static int read_music(NE *ne, MXTables *t)
{
    static const char *const NAMES[5] = {"intro.mid", "bgloop1.mid", "bgloop2.mid", "bgloop3.mid", "question.mid"};
    for (int i = 0; i < 5; i++) {
        unsigned sn, so;
        if (!find_string(ne, NAMES[i], 1, 1, &sn, &so) || !(t->music[i] = cstr(ne, sn, so))) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------------------------- UI strings */
static const struct { int id; const char *text; int exact; } STRINGS[] = {   /* 2006 id: 95 text (or its start) */
    {29002, "GRATING.DI$", 1}, {29003, "LOGONSCR.DI$", 1}, {29005, "mindmaze.db", 1}, {29006, "RMOFDOOR.DI$", 1},
    {29007, "BANNER.DI$", 1}, {29008, "DIALOG.DI$", 1}, {29009, "MindMaze", 1}, {29024, "New Player", 1},
    {29025, "Delete", 1}, {29026, "mindmaze.idx", 1}, {29039, "Sorry, that answer is incorrect. Please try again.", 1},
    {29043, "Tourist", 1}, {29050, "MindMaze cannot play game music.", 1}, {29067, "MindMaze", 1},
    {29042, "Would you like to re", 0}, {29044, "Who dares to enter", 0}, {29045, "Your party is too large", 0},
    {29046, "That player is already", 0}, {29048, "Could not initialize the question database", 0},
    {29049, "Click on the stairs", 0}, {0, NULL, 0}};
static const struct { int id; const char *a, *fmt, *b; } DERIVED[] = {   /* wsprintf/strcat pieces in 95 */
    {29040, "Sorry, that answer is incorrect. The correct answer is ", "%s", "."},
    {29041, "Correct!  ", "%u", " points added to your score."}, {0, NULL, NULL, NULL}};

static char **str_slot(MXTables *t, int id) { return &t->str[id - MXT_FIRST_STRING]; }

static void read_strings(NE *ne, MXTables *t)
{
    unsigned sn, so, n, p;
    for (int i = 0; STRINGS[i].id; i++)
        if (find_string(ne, STRINGS[i].text, STRINGS[i].exact, 0, &sn, &so)) *str_slot(t, STRINGS[i].id) = cstr(ne, sn, so);
    for (int i = 0; DERIVED[i].id; i++) {
        if (!find_string(ne, DERIVED[i].a, 1, 0, &sn, &so) || !find_string(ne, DERIVED[i].b, 1, 0, &sn, &so)) continue;
        size_t m = strlen(DERIVED[i].a) + strlen(DERIVED[i].fmt) + strlen(DERIVED[i].b) + 1;
        char *s = malloc(m);
        if (!s) { ne->oom = 1; continue; }
        snprintf(s, m, "%s%s%s", DERIVED[i].a, DERIVED[i].fmt, DERIVED[i].b);
        *str_slot(t, DERIVED[i].id) = s;
    }
    if (ref_of(ne, "All Areas", 1, NULL, NULL, &n, &p))                 /* area chooser -> 29014..29023 */
        for (int i = 0; i < 10; i++) *str_slot(t, 29014 + i) = far_str(ne, n, (long)p + 4 * i);
    if (ref_of(ne, "Choose Area of Interest", 1, NULL, NULL, &n, &p))   /* info panel labels -> 29028..29034 */
        for (int i = 0; i < 7; i++) *str_slot(t, 29028 + i) = far_str(ne, n, (long)p + 4 * i);
    if (find_string(ne, "%c.", 1, 0, &sn, &so))                         /* answer letters -> 29035..29038 */
        for (int i = 0; i < 4; i++) {
            char l[3] = {(char)('A' + i), '.', 0}, *s = malloc(3);
            if (s) memcpy(s, l, 3); else ne->oom = 1;
            *str_slot(t, 29035 + i) = s;
        }
}

/* ---------------------------------------------------------------------------------------------- bitmaps */
static const struct { const char *name; int count; } BITMAPS[] = {   /* in mx_table_bitmaps order */
    {"IDB_BOOKS", 4}, {"IDB_DIFFONE", 1}, {"IDB_DIFFTWO", 1}, {"IDB_DIFFTHREE", 1}, {"IDB_DIFFFOUR", 1},
    {"IDB_MATCHES", 5}, {"IDB_NEXTPAGE", 1}, {"IDB_RADIOBUTTONS", 1}, {"COUNTDN", 1}, {NULL, 0}};

/* the named BITMAP resource (the last one of that name, as a later entry would replace an earlier one) */
static const uint8_t *bitmap_res(const NE *ne, const char *name, size_t *len)
{
    const uint8_t *d = ne->d, *hit = NULL;
    size_t rt = ne->ne + rd16(d + ne->ne + 0x24), p = rt + 2, nl = strlen(name);
    if (rt + 2 > ne->n) return NULL;
    unsigned shift = rd16(d + rt);
    if (shift > 24) return NULL;
    while (p + 8 <= ne->n) {
        unsigned type = rd16(d + p), cnt = rd16(d + p + 2);
        p += 8;
        if (!type) break;
        for (unsigned i = 0; i < cnt; i++, p += 12) {
            if (p + 12 > ne->n) return hit;
            unsigned rid = rd16(d + p + 6);
            if (type != (0x8000 | NE_RT_BITMAP) || (rid & 0x8000)) continue;
            size_t np = rt + rid;
            if (np >= ne->n || np + 1 + d[np] > ne->n || d[np] != nl || memcmp(d + np + 1, name, nl)) continue;
            size_t o = (size_t)rd16(d + p) << shift, l = (size_t)rd16(d + p + 2) << shift;
            if (o >= ne->n) continue;
            hit = d + o;
            *len = l < ne->n - o ? l : ne->n - o;
        }
    }
    return hit;
}

/* BITMAP resource -> .bmp file, trimmed of the resource alignment padding; NULL if not a plain <= 8-bit DIB */
static uint8_t *bitmap_file(const uint8_t *dib, size_t len, size_t *outlen)
{
    if (len < 40 || rd32(dib) != 40 || rd32(dib + 16) != 0) return NULL;
    long long w = (int32_t)rd32(dib + 4), h = (int32_t)rd32(dib + 8);
    unsigned bpp = rd16(dib + 14);
    if (bpp > 8 || w < 0 || w > 0x10000 || h > 0x10000 || h < -0x10000) return NULL;
    long long nc = rd32(dib + 32) ? (long long)rd32(dib + 32) : 1LL << bpp;
    long long need = 40 + 4 * nc + (w * bpp + 31) / 32 * 4 * (h < 0 ? -h : h);
    if (need > (long long)len) return NULL;
    return mx_dib_to_bmp(dib, (size_t)need, outlen);
}

static const char *read_bitmaps(NE *ne, MXTables *t)
{
    int slot = 0;
    for (int i = 0; BITMAPS[i].name; i++) {
        size_t len = 0;
        const uint8_t *dib = bitmap_res(ne, BITMAPS[i].name, &len);
        if (!dib) return BITMAPS[i].name;
        for (int k = 0; k < BITMAPS[i].count && slot < MXT_NBITMAPS; k++, slot++)
            if (!(t->bmp[slot] = bitmap_file(dib, len, &t->bmplen[slot]))) return BITMAPS[i].name;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- reader */
int mx_tables_ne(const uint8_t *exe, size_t exelen, MXTables *t, char *err, int errlen)
{
    NE ne;
    const char *what = NULL, *bm;
    memset(t, 0, sizeof *t);
    if (!ne_open(&ne, exe, exelen)) what = "this is not a 16-bit Windows program";
    else if (!read_objects(&ne, t)) what = "room object table not found";
    else if (!read_ranks(&ne, t)) what = "rank table not found";
    else if (!read_speech(&ne, t)) what = "speech table not found";
    else if (!read_list(&ne, "Life Science", 9, not_after_all_areas, t->picture_areas)) what = "picture area list not found";
    else if (!read_list(&ne, "AMB1.WAV", 35, NULL, t->sounds)) what = "sound list not found";
    else if (!read_music(&ne, t)) what = "music names not found";
    else {
        read_strings(&ne, t);
        if ((bm = read_bitmaps(&ne, t))) {
            say(err, errlen, "MM95.EXE: panel picture %s not found or damaged.", bm);
            what = "";
        }
    }
    if (ne.oom) what = "out of memory";
    if (what && *what) say(err, errlen, "MM95.EXE: %s.", what);
    ne_close(&ne);
    if (what) { mx_tables_free(t); return 0; }
    return 1;
}
