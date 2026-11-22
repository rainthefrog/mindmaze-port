/* Other Encarta editions (CHANGE, 2026-11-18): MindMaze shipped from Encarta 95 to 2006 with the same pictures and
 * sounds but its own question set per edition, and the older discs also carry music the 2006 disc lacks (the
 * transitions between the background loops, "<from>-<to>.mid", which the 2006 code still plays when present, and
 * ATRIUM / FINALET / G-FINAL). mx_add_edition reads an older disc's question files and music:
 *
 *   Encarta 95 (MPEG ed.)  ISO 9660  ENCYC95/MM/INTERACT/MINDMAZE.DB|IDX, ENCYC95/MM/MIDI/(name).MID
 *   Encarta 96 (Macintosh) HFS       Encarta Files/MindMaze Data|Index, Encarta Files/MIDI/(name).mid (QuickTime
 *                                    music movies, converted to SMF by mx_qtmusic.c; CHANGE 2026-11-21)
 *   Encarta 97             ISO 9660  ENCYC97/MINDMAZE.DB|IDX, ENCYC97/MM/MIDI/(name).MID
 *   Encarta 98             ISO 9660  ENCYC98/MINDMAZE.DB|IDX, ENCYC98/MM/MIDI/(name).MID
 *   Encarta 2006           ISO 9660  EE/ENCARTA/MINDMAZE.DB|IDX (added as an extra source to a data folder made
 *                                    from an older disc)
 *   or a folder holding MINDMAZE.DB + MINDMAZE.IDX (and MIDI files)
 *
 * The questions are rewritten in the 2006 layout (the game reads that): record variants
 *   2006: u32 0,          u32 len, text, 4 x {u8 len, text, u32 article, u16 value}
 *   97/98: u32 0xFFFFFFFF, u32 len, text, 4 x {u8 len, text, u32 ref}
 *   95/96: u32 subcategory (0..14), u32 len, text, 4 x {u8 len, text, u32 ref}
 * answers may be NUL-padded (cut at the first NUL); the Mac edition's text is code page 437.
 * Output (relative to the data folder): editions/<id>/mindmaze.db + .idx, editions/<id>/(name).mid (extra music) and the
 * loop transitions 1-2.mid ... 3-2.mid in the data folder itself. Surveys: build/versions (2026-11-18). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"
#include "mx_oldext.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static unsigned be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static const char *const MUSIC[] = {"1-2", "1-3", "2-1", "2-3", "3-1", "3-2", "ATRIUM", "FINALET", "G-FINAL", NULL};

typedef struct {
    MXIO *io;
    void *f;               /* the image (ISO / HFS), or NULL for a folder */
    int64_t size;
    char dir[1024];
    char *err;
    int errlen;
} Src;

static int rd(Src *s, int64_t off, void *buf, size_t n) { return off >= 0 && off + (int64_t)n <= s->size && s->io->read(s->io, s->f, off, buf, n); }

/* ---------------------------------------------------------------------------------------------- ISO 9660 */
static int iso_find(Src *s, uint32_t lba, uint32_t len, const char *name, int want_dir, uint32_t *ol, uint32_t *on)
{
    uint8_t sec[2048];
    for (uint32_t done = 0; done < len && done < (8u << 20); done += 2048) {
        if (!rd(s, (int64_t)(lba + done / 2048) * 2048, sec, 2048)) return 0;
        for (int p = 0; p < 2048;) {
            int rl = sec[p];
            if (rl < 34 || p + rl > 2048) break;
            int nl = sec[p + 32], k = 0;
            char nm[64];
            for (int i = 0; i < nl && i < 63 && sec[p + 33 + i] != ';'; i++) nm[k++] = (char)sec[p + 33 + i];
            if (k && nm[k - 1] == '.') k--;
            nm[k] = 0;
            int same = (int)strlen(name) == k;
            for (int i = 0; same && i < k; i++) {
                char a = nm[i], b = name[i];
                if (a >= 'a' && a <= 'z') a -= 32;
                if (b >= 'a' && b <= 'z') b -= 32;
                same = a == b;
            }
            if (same && !!(sec[p + 25] & 2) == want_dir) { *ol = le32(sec + p + 2); *on = le32(sec + p + 10); return 1; }
            p += rl;
        }
    }
    return 0;
}

/* path like "ENCYC97/MM/MIDI/1-2.MID" -> malloc'd contents */
static uint8_t *iso_read(Src *s, const char *path, size_t *len)
{
    uint8_t pvd[2048];
    if (!rd(s, 16 * 2048, pvd, 2048)) return NULL;
    uint32_t l = le32(pvd + 156 + 2), n = le32(pvd + 156 + 10);
    char part[64];
    const char *p = path;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k >= sizeof part) return NULL;
        memcpy(part, p, k);
        part[k] = 0;
        if (!iso_find(s, l, n, part, e != NULL, &l, &n)) return NULL;
        p = e ? e + 1 : p + k;
    }
    if (n > (64u << 20)) return NULL;
    uint8_t *d = malloc(n + 1);
    if (!d || !rd(s, (int64_t)l * 2048, d, n)) { free(d); return NULL; }
    *len = n;
    return d;
}

/* ---------------------------------------------------------------------------------------------- classic HFS */
/* MDB (partition + 1024, "BD"): +20 drAlBlkSiz, +28 drAlBlSt, +134 drXTExtRec (extents file), +150 drCTExtRec
 * (catalog), each 3 x {u16 start, u16 count}. B-tree header node 0: +32 bthNodeSize, +24 bthFNode (first leaf). */
typedef struct { int64_t base; uint32_t blksz; unsigned alst; uint8_t ct[12]; unsigned nodesz; uint32_t first; uint8_t xt[12]; } Hfs;

static int hfs_open(Src *s, Hfs *h)
{
    uint8_t b[512];
    int64_t cand[8];
    int nc = 0;
    cand[nc++] = 0;
    for (int i = 1; i < 64 && nc < 8; i++) {                    /* Apple partition map: "PM" entries in blocks 1.. */
        if (!rd(s, (int64_t)i * 512, b, 512) || b[0] != 'P' || b[1] != 'M') break;
        if (!strncmp((const char *)b + 48, "Apple_HFS", 9)) cand[nc++] = (int64_t)be32(b + 8) * 512;
    }
    for (int c = 0; c < nc; c++) {
        if (!rd(s, cand[c] + 1024, b, 512) || be16(b) != 0x4244) continue;    /* "BD" */
        h->base = cand[c];
        h->blksz = be32(b + 20);
        h->alst = be16(b + 28);
        memcpy(h->ct, b + 150, 12);
        memcpy(h->xt, b + 134, 12);
        uint8_t node[512];
        if (!rd(s, h->base + (int64_t)h->alst * 512 + (int64_t)be16(h->ct) * h->blksz, node, 512)) return 0;
        h->nodesz = be16(node + 14 + 18);
        h->first = be32(node + 14 + 10);
        return h->blksz && h->nodesz >= 512 && h->nodesz <= 4096;
    }
    return 0;
}

/* byte offset of `pos` within a fork described by 3 extents */
static int64_t hfs_fork_off(const Hfs *h, const uint8_t *ext, uint32_t pos)
{
    uint32_t blk = pos / h->blksz;
    for (int i = 0; i < 3; i++) {
        unsigned st = be16(ext + 4 * i), cnt = be16(ext + 4 * i + 2);
        if (blk < cnt) return h->base + (int64_t)h->alst * 512 + ((int64_t)st + blk) * h->blksz + pos % h->blksz;
        blk -= cnt;
    }
    return -1;
}

static int name_eq(const uint8_t *pas, const char *s)            /* Pascal string vs C string, ASCII case-insensitive */
{
    if ((size_t)pas[0] != strlen(s)) return 0;
    for (int i = 0; i < pas[0]; i++) {
        char a = (char)pas[1 + i], b = s[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

/* catalog leaf walk: the record (parent, name); returns 1 and copies up to 120 bytes of its data */
static int hfs_lookup(Src *s, const Hfs *h, uint32_t parent, const char *name, uint8_t *rec)
{
    uint8_t node[4096];
    uint32_t n = h->first;
    for (int guard = 0; n && guard < 200000; guard++) {
        int64_t off = hfs_fork_off(h, h->ct, n * h->nodesz);
        if (off < 0 || !rd(s, off, node, h->nodesz)) return 0;
        unsigned nrec = be16(node + 10);
        for (unsigned r = 0; r < nrec; r++) {
            unsigned ro = be16(node + h->nodesz - 2 * (r + 1));
            if (ro + 8 > h->nodesz) break;
            const uint8_t *k = node + ro;
            unsigned kl = k[0];
            if (be32(k + 2) != parent || !name_eq(k + 6, name)) continue;
            unsigned dofs = (1 + kl + 1) & ~1u;
            if (ro + dofs + 102 > h->nodesz) return 0;
            memcpy(rec, k + dofs, 102);
            return 1;
        }
        n = be32(node);                                          /* fLink */
    }
    return 0;
}

/* "dir/sub" (folders from the root, id 2; "" = the root) + file name -> the file's catalog record */
static int hfs_find(Src *s, const Hfs *h, const char *dir, const char *file, uint8_t *rec)
{
    uint32_t id = 2;
    char part[32];
    for (const char *p = dir ? dir : ""; *p;) {
        if (*p == '/') { p++; continue; }
        const char *e = strchr(p, '/');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k >= sizeof part) return 0;                          /* HFS names are at most 31 characters */
        memcpy(part, p, k);
        part[k] = 0;
        if (!hfs_lookup(s, h, id, part, rec) || rec[0] != 1) return 0;          /* cdrType 1: folder */
        id = be32(rec + 6);                                      /* dirDirID */
        p += k;
    }
    return hfs_lookup(s, h, id, file, rec) && rec[0] == 2;      /* cdrType 2: file */
}

/* A fork's extents (CHANGE 2026-11-18): the 3 in the file record, then records of the extents-overflow B-tree, leaf
 * key {u8 keyLen 7, u8 forkType (0 data, 0xFF resource), u32 filFlNum, u16 startBlk (fork-relative)} + 3 extents,
 * taken in startBlk order until the fork's `need` allocation blocks are covered. Returns the extent count, 0 = error. */
enum { HFS_MAX_EXT = 99 };
typedef struct { unsigned st, cnt; } HfsExt;

static int hfs_extents(Src *s, const Hfs *h, const uint8_t *first3, uint32_t fnum, int rsrc, uint32_t need, HfsExt *e)
{
    int n = 0;
    uint32_t have = 0;
    for (int i = 0; i < 3; i++, n++) { e[n].st = be16(first3 + 4 * i); e[n].cnt = be16(first3 + 4 * i + 2); have += e[n].cnt; }
    if (have >= need) return n;
    uint8_t node[4096];
    int64_t off = hfs_fork_off(h, h->xt, 0);
    if (off < 0 || !rd(s, off, node, 512)) return 0;
    unsigned nsz = be16(node + 32);
    uint32_t leaf = be32(node + 24);
    if (nsz < 512 || nsz > 4096) return 0;
    while (have < need) {                                        /* find the record that starts at block `have` */
        uint32_t added = 0;
        uint32_t nd = leaf;
        for (int guard = 0; nd && !added && guard < 100000; guard++) {
            off = hfs_fork_off(h, h->xt, nd * nsz);
            if (off < 0 || !rd(s, off, node, nsz) || node[8] != 0xFF) return 0;    /* ndType -1: leaf */
            unsigned nrec = be16(node + 10);
            for (unsigned r = 0; r < nrec && !added; r++) {
                unsigned ro = be16(node + nsz - 2 * (r + 1));
                if (ro + 8 > nsz) break;
                const uint8_t *k = node + ro;
                unsigned dofs = (1 + k[0] + 1) & ~1u;
                if (k[0] < 7 || ro + dofs + 12 > nsz) continue;
                if (k[1] != (rsrc ? 0xFF : 0) || be32(k + 2) != fnum || be16(k + 6) != have) continue;
                if (n + 3 > HFS_MAX_EXT) return 0;
                for (int i = 0; i < 3; i++, n++) {
                    e[n].st = be16(k + dofs + 4 * i);
                    e[n].cnt = be16(k + dofs + 4 * i + 2);
                    added += e[n].cnt;
                }
            }
            nd = be32(node);                                     /* fLink */
        }
        if (!added) return 0;                                    /* a fork extent is missing */
        have += added;
    }
    return n;
}

/* the data (rsrc 0) or resource fork (rsrc 1) of a file record: +20 filFlNum, +26 filLgLen, +74 filExtRec,
 * +36 filRLgLen, +86 filRExtRec */
static uint8_t *hfs_fork_read(Src *s, const Hfs *h, const uint8_t *rec, int rsrc, size_t *len)
{
    uint32_t n = be32(rec + (rsrc ? 36 : 26));
    if (n > (64u << 20)) return NULL;
    HfsExt e[HFS_MAX_EXT];
    int ne = hfs_extents(s, h, rec + (rsrc ? 86 : 74), be32(rec + 20), rsrc, (uint32_t)(((uint64_t)n + h->blksz - 1) / h->blksz), e);
    if (!ne) return NULL;
    uint8_t *d = malloc(n + 1);
    if (!d) return NULL;
    for (uint32_t pos = 0; pos < n;) {                           /* block by block (extents may be split) */
        uint32_t chunk = h->blksz - pos % h->blksz;
        if (chunk > n - pos) chunk = n - pos;
        uint32_t blk = pos / h->blksz;
        int64_t off = -1;
        for (int i = 0; i < ne; i++) {
            if (blk < e[i].cnt) { off = h->base + (int64_t)h->alst * 512 + ((int64_t)e[i].st + blk) * h->blksz + pos % h->blksz; break; }
            blk -= e[i].cnt;
        }
        if (off < 0 || !rd(s, off, d + pos, chunk)) { free(d); return NULL; }
        pos += chunk;
    }
    *len = n;
    return d;
}


/* a file's fork from an HFS image that is already open (see mx.h) */
uint8_t *mx_hfs_file(MXIO *io, void *f, int64_t size, const char *dir, const char *file, int rsrc, size_t *len)
{
    Src s;
    memset(&s, 0, sizeof s);
    s.io = io;
    s.f = f;
    s.size = size;
    Hfs h;
    uint8_t rec[120];
    if (!f || !file || !hfs_open(&s, &h) || !hfs_find(&s, &h, dir, file, rec)) return NULL;
    return hfs_fork_read(&s, &h, rec, rsrc != 0, len);
}

/* ---------------------------------------------------------------------------------------------- folder */
static uint8_t *folder_read(Src *s, const char *rel, size_t *len)
{
    const char *variants[4];
    char up[256], lo[256];
    snprintf(up, sizeof up, "%s", rel);
    snprintf(lo, sizeof lo, "%s", rel);
    for (char *p = up; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    for (char *p = lo; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    variants[0] = rel; variants[1] = up; variants[2] = lo; variants[3] = NULL;
    for (int i = 0; variants[i]; i++) {
        char path[1400];
        snprintf(path, sizeof path, "%s%s%s", s->dir, *s->dir ? "/" : "", variants[i]);
        int64_t size = 0;
        void *f = s->io->open(s->io, path, &size);
        if (!f) continue;
        uint8_t *d = size >= 0 && size < (64 << 20) ? malloc((size_t)size + 1) : NULL;
        int ok = d && s->io->read(s->io, f, 0, d, (size_t)size);
        s->io->close(s->io, f);
        if (!ok) { free(d); return NULL; }
        *len = (size_t)size;
        return d;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- questions */
static const unsigned short CP437_HI[128] = {
    0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5, 0xC9, 0xE6, 0xC6, 0xF4,
    0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, 0x20A7, 0x192, 0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA,
    0xBF, 0x2310, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555,
    0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553,
    0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x3B1, 0xDF, 0x393, 0x3C0, 0x3A3, 0x3C3, 0xB5,
    0x3C4, 0x3A6, 0x398, 0x3A9, 0x3B4, 0x221E, 0x3C6, 0x3B5, 0x2229, 0x2261, 0xB1, 0x2265, 0x2264, 0x2320, 0x2321, 0xF7,
    0x2248, 0xB0, 0x2219, 0xB7, 0x221A, 0x207F, 0xB2, 0x25A0, 0xA0};

static uint8_t cp437_to_1252(uint8_t c)
{
    if (c < 0x80) return c;
    unsigned u = CP437_HI[c - 0x80];
    if (u < 0x100 && (u >= 0xA0 || u < 0x80)) return (uint8_t)u;
    if (u == 0x192) return 0x83;
    return '?';
}

typedef struct { uint8_t *p; size_t n, cap; } Out;
static int out_put(Out *o, const void *d, size_t n)
{
    if (o->n + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 1 << 20;
        while (c < o->n + n) c *= 2;
        uint8_t *np = realloc(o->p, c);
        if (!np) return 0;
        o->p = np;
        o->cap = c;
    }
    memcpy(o->p + o->n, d, n);
    o->n += n;
    return 1;
}
static int out_u32(Out *o, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; return out_put(o, b, 4); }

/* one record at off; layout: 0 = 2006, 1 = old (no u16 per answer). Text bytes are converted with cp437 when asked. */
static int convert_record(const uint8_t *db, size_t dn, uint32_t off, int layout, int cp437, Out *o)
{
    if (off + 8 > dn) return 0;
    uint32_t ql = le32(db + off + 4);
    size_t p = off + 8;
    if (ql > 4000 || p + ql > dn) return 0;
    uint8_t t[4096];
    size_t tl = 0;
    for (size_t i = 0; i < ql && db[p + i]; i++) t[tl++] = cp437 ? cp437_to_1252(db[p + i]) : db[p + i];
    p += ql;
    if (!out_u32(o, 0) || !out_u32(o, (uint32_t)tl) || !out_put(o, t, tl)) return 0;
    for (int a = 0; a < 4; a++) {
        if (p >= dn) return 0;
        unsigned al = db[p++];
        if (p + al + 4 + (layout == 0 ? 2 : 0) > dn) return 0;
        uint8_t at[256];
        unsigned k = 0;
        for (unsigned i = 0; i < al && db[p + i]; i++) at[k++] = cp437 ? cp437_to_1252(db[p + i]) : db[p + i];
        p += al;
        uint32_t ref = le32(db + p);
        p += 4 + (layout == 0 ? 2 : 0);
        uint8_t z[2] = {0, 0}, len8 = (uint8_t)k;
        if (!out_put(o, &len8, 1) || !out_put(o, at, k) || !out_u32(o, ref) || !out_put(o, z, 2)) return 0;
    }
    return 1;
}

/* rewrite DB/IDX in the 2006 layout; returns the number of questions, 0 + err on damage */
static int convert_questions(Src *s, const uint8_t *idx, size_t in, const uint8_t *db, size_t dn, int cp437, Out *odb, Out *oidx)
{
    if (in < 36 * 4) { snprintf(s->err, (size_t)s->errlen, "The question index is too short (damaged disc?)."); return 0; }
    uint32_t total = 0;
    for (int i = 0; i < 36; i++) total += le32(idx + 4 * i);
    if (total == 0 || total > 50000 || 36 * 4 + (size_t)total * 8 > in) {
        snprintf(s->err, (size_t)s->errlen, "The question index is damaged (%u questions announced).", total);
        return 0;
    }
    uint32_t first = le32(idx + 36 * 4 + 4);
    int layout = first + 4 <= dn && le32(db + first) == 0 ? 0 : 1;   /* 2006 starts every record with 0 */
    for (int i = 0; i < 36; i++) if (!out_u32(oidx, le32(idx + 4 * i))) return 0;
    int bad = 0;
    for (uint32_t k = 0; k < total; k++) {
        uint32_t num = le32(idx + 36 * 4 + 8 * k), off = le32(idx + 36 * 4 + 8 * k + 4);
        uint32_t newoff = (uint32_t)odb->n;
        if (!convert_record(db, dn, off, layout, cp437, odb)) {    /* a damaged record: keep a placeholder */
            bad++;
            static const char q[] = "(This question could not be read from the disc.)";
            out_u32(odb, 0); out_u32(odb, sizeof q - 1); out_put(odb, q, sizeof q - 1);
            for (int a = 0; a < 4; a++) { uint8_t l = 1, z[6] = {0}; out_put(odb, &l, 1); out_put(odb, "-", 1); out_put(odb, z, 6); }
        }
        if (!out_u32(oidx, num) || !out_u32(oidx, newoff)) return 0;
    }
    if (bad > (int)total / 20) {
        snprintf(s->err, (size_t)s->errlen, "%d of %u questions could not be read: the question file is damaged.", bad, total);
        return 0;
    }
    return (int)total - bad;
}

/* ---------------------------------------------------------------------------------------------- finding the questions */
/* CHANGE 2026-11-18: one search for both uses - adding a disc's questions (mx_add_edition: any edition, 2006 too,
 * or a folder holding MINDMAZE.DB + .IDX) and a whole data folder from an older disc (mx_extract_old). A folder may
 * be a file's folder on the disc (ENCYC95/MM/INTERACT, ENCYC97, EE/ENCARTA): the search walks up to the disc's top. */
typedef struct { Src *s; Hfs *h; int kind; } OldRd;       /* kind 0 ISO, 1 HFS, 2 folder */

static uint8_t *old_read(void *ctx, const char *path, int rsrc, size_t *len)
{
    OldRd *r = ctx;
    if (r->kind == 0) return iso_read(r->s, path, len);
    if (r->kind == 2) return folder_read(r->s, path, len);
    char dir[256];
    const char *slash = strrchr(path, '/');
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 0, path);
    uint8_t rec[120];
    return hfs_find(r->s, r->h, dir, slash ? slash + 1 : path, rec) ? hfs_fork_read(r->s, r->h, rec, rsrc, len) : NULL;
}

/* the 2006 disc's questions (its pictures come from MINDMAZE.EIT: mx_extract.c) */
static const MXOldEdition ENC2006 = {"enc2006", "Encarta 2006", 2006, "EE/ENCARTA", NULL, NULL, NULL, NULL, NULL};

typedef struct {
    Src s;
    Hfs h;
    OldRd r;
    const MXOldEdition *ed;
    MXOldEdition own;            /* a folder holding the two files: named after the folder */
    char own_id[16], own_name[64];
    uint8_t *db, *idx;
    size_t dn, in;
} Found;

static int try_edition(Found *F, const MXOldEdition *e)
{
    char p[160];
    if ((e->year == 96) != (F->r.kind == 1)) return 0;
    snprintf(p, sizeof p, "%s/%s", e->qdir, e->year == 96 ? "MindMaze Index" : "MINDMAZE.IDX");
    if (!(F->idx = old_read(&F->r, p, 0, &F->in))) return 0;
    snprintf(p, sizeof p, "%s/%s", e->qdir, e->year == 96 ? "MindMaze Data" : "MINDMAZE.DB");
    F->db = old_read(&F->r, p, 0, &F->dn);
    F->ed = e;
    return 1;
}

/* open the source and find an edition's question files. any = also the 2006 disc and a plain folder of the two files.
 * 1 = found (F->ed; F->db may be NULL if unreadable), -1 = none (message in err) */
static int find_questions(MXIO *io, const char *source, int any, Found *F, char *err, int errlen)
{
    F->s.io = io;
    F->s.err = err;
    F->s.errlen = errlen;
    F->r.s = &F->s;
    F->r.h = &F->h;
    F->r.kind = 2;
    F->s.f = io->open(io, source, &F->s.size);
    uint8_t m[8];
    if (F->s.f && F->s.size > 40000 && rd(&F->s, 16 * 2048, m, 8) && !memcmp(m + 1, "CD001", 5)) F->r.kind = 0;
    else if (F->s.f && hfs_open(&F->s, &F->h)) F->r.kind = 1;
    else {
        if (F->s.f) { io->close(io, F->s.f); F->s.f = NULL; }
        snprintf(F->s.dir, sizeof F->s.dir, "%s", source);
    }
    char top[1024];
    snprintf(top, sizeof top, "%s", F->s.dir);
    for (int up = 0; up < 4 && !F->idx; up++) {
        if (up) {
            if (F->r.kind != 2) break;
            char *a = strrchr(F->s.dir, '/'), *b = strrchr(F->s.dir, '\\');
            if (b > a) a = b;
            if (!a || a == F->s.dir) break;                       /* "E:\ENCYC97" -> "E:" -> stop */
            *a = 0;
        }
        for (int i = 0; mx_old_editions[i].id && !F->idx; i++) try_edition(F, &mx_old_editions[i]);
        if (any && !F->idx) try_edition(F, &ENC2006);
    }
    if (!F->idx && any && F->r.kind == 2) {                       /* a folder holding MINDMAZE.IDX + .DB itself */
        snprintf(F->s.dir, sizeof F->s.dir, "%s", top);
        if ((F->idx = folder_read(&F->s, "MINDMAZE.IDX", &F->in))) {
            F->db = folder_read(&F->s, "MINDMAZE.DB", &F->dn);
            const char *b = strrchr(source, '/'), *b2 = strrchr(source, '\\');
            if (b2 > b) b = b2;
            b = b ? b + 1 : source;
            snprintf(F->own_id, sizeof F->own_id, "folder");
            snprintf(F->own_name, sizeof F->own_name, "Encarta (%.40s)", b);
            for (const char *p = b; *p; p++)                          /* ENCYC97 etc. */
                if (p[0] == '9' && p[1] >= '5' && p[1] <= '8') {
                    snprintf(F->own_id, sizeof F->own_id, "enc9%c", p[1]);
                    snprintf(F->own_name, sizeof F->own_name, "Encarta 9%c", p[1]);
                    break;
                }
            F->own.id = F->own_id;
            F->own.name = F->own_name;
            F->ed = &F->own;
        }
    }
    if (F->idx) return 1;
    snprintf(err, (size_t)errlen, F->r.kind == 2 ? "No MindMaze questions found here. Choose an Encarta disc image (.iso: Encarta "
                                                   "95 to 98 or 2006), the disc drive, or MINDMAZE.DB on the disc."
                                                 : "No MindMaze questions found on this disc image. If it is an Encarta 95 to 98 "
                                                   "disc, the image may be incomplete (e.g. cut off while copying).");
    return -1;
}

static void found_free(Found *F)
{
    if (F->s.f) F->s.io->close(F->s.io, F->s.f);
    free(F->db);
    free(F->idx);
    free(F);
}

/* the edition's questions in the 2006 layout -> dbpath / idxpath; the question count, 0 = error (message in err) */
static int write_questions(MXIO *io, Found *F, const char *dbpath, const char *idxpath, char *err, int errlen)
{
    if (!F->db) { snprintf(err, (size_t)errlen, "%s: the questions file is missing or unreadable (damaged disc?).", F->ed->name); return 0; }
    Out odb = {0}, oidx = {0};
    int n = convert_questions(&F->s, F->idx, F->in, F->db, F->dn, F->ed->year == 96, &odb, &oidx);
    if (n > 0 && (!io->write(io, dbpath, odb.p, odb.n) || !io->write(io, idxpath, oidx.p, oidx.n))) {
        snprintf(err, (size_t)errlen, "Could not write the questions into the data folder.");
        n = 0;
    }
    free(odb.p);
    free(oidx.p);
    return n > 0 ? n : 0;
}

/* the extra music: loop transitions into the data folder (every edition plays them), the others to editions/<id>/ */
static void copy_music(MXIO *io, Found *F)
{
    for (int i = 0; MUSIC[i]; i++) {
        size_t ml = 0;
        char p[160];
        uint8_t *mid = NULL;
        if (F->ed->midi) {                         /* CHANGE 2026-11-21: the Mac's QuickTime music is converted */
            snprintf(p, sizeof p, "%s/%s.MID", F->ed->midi, MUSIC[i]);
            mid = mx_old_midi(old_read, &F->r, p, &ml);
        }
        else if (F->ed == &F->own) {
            snprintf(p, sizeof p, "MM/MIDI/%s.MID", MUSIC[i]);
            if (!(mid = folder_read(&F->s, p, &ml))) { snprintf(p, sizeof p, "%s.MID", MUSIC[i]); mid = folder_read(&F->s, p, &ml); }
        }
        if (mid && ml >= 4 && !memcmp(mid, "MThd", 4)) {
            char lower[32], out[96];
            snprintf(lower, sizeof lower, "%s.mid", MUSIC[i]);
            for (char *q = lower; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
            if (i < 6) snprintf(out, sizeof out, "%s", lower);
            else snprintf(out, sizeof out, "editions/%s/%s", F->ed->id, lower);
            io->write(io, out, mid, ml);
        }
        free(mid);
    }
}

/* ---------------------------------------------------------------------------------------------- adding a disc */
int mx_add_edition(MXIO *io, const char *source, char *id, int idlen, char *name, int namelen, int *count, char *err, int errlen)
{
    Found *F = calloc(1, sizeof *F);
    int ok = 0;
    *count = 0;
    err[0] = 0;
    if (!F) { snprintf(err, (size_t)errlen, "out of memory"); return 0; }
    if (find_questions(io, source, 1, F, err, errlen) > 0) {
        snprintf(id, (size_t)idlen, "%s", F->ed->id);
        snprintf(name, (size_t)namelen, "%s", F->ed->name);
        if (io->progress) io->progress(io, 30, "Reading the questions");
        char dbp[96], idxp[96];
        snprintf(dbp, sizeof dbp, "editions/%s/mindmaze.db", id);
        snprintf(idxp, sizeof idxp, "editions/%s/mindmaze.idx", id);
        if ((*count = write_questions(io, F, dbp, idxp, err, errlen)) > 0) {
            if (io->progress) io->progress(io, 70, "Copying the music");
            copy_music(io, F);
            if (io->progress) io->progress(io, 100, "Done");
            ok = 1;
        }
    }
    found_free(F);
    return ok;
}

/* ---------------------------------------------------------------------------------------------- whole data folder */
/* An older disc as the only source (CHANGE 2026-11-18): questions into the data folder itself, the extra music, and
 * pictures / sounds / main music / tables through mx_old_build (mx_oldext.c). */
int mx_extract_old(MXIO *io, const char *source, char *id, int idlen, char *name, int namelen, int *nq, MXOldCounts *c,
                   char *err, int errlen)
{
    Found *F = calloc(1, sizeof *F);
    int ok = 0;
    *nq = 0;
    err[0] = 0;
    memset(c, 0, sizeof *c);
    if (!F) { snprintf(err, (size_t)errlen, "out of memory"); return 0; }
    int r = find_questions(io, source, 0, F, err, errlen);
    if (r < 0) ok = -1;
    else {
        snprintf(id, (size_t)idlen, "%s", F->ed->id);
        snprintf(name, (size_t)namelen, "%s", F->ed->name);
        if (io->progress) io->progress(io, 5, "Reading the questions");
        if ((*nq = write_questions(io, F, "mindmaze.db", "mindmaze.idx", err, errlen)) > 0) {
            free(F->db);
            free(F->idx);
            F->db = F->idx = NULL;
            if (mx_old_build(io, old_read, &F->r, F->ed, c, err, errlen)) {
                if (io->progress) io->progress(io, 70, "Copying the extra music");
                copy_music(io, F);
                ok = 1;
            }
        }
    }
    found_free(F);
    return ok;
}
