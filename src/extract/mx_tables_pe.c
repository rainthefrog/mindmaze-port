/* MindMaze's tables from a 32-bit Windows program: Encarta 97 (ENC97.EXE + ENCRES97.DLL), 98 (ENC98.EXE +
 * ENCRES98.DLL, inside the disc's Wise Setup.EXE) and 2006 (ENCARTA.EXE + ENCARTAR.DLL). CHANGE 2026-11-18.
 *
 * Everything is found by patterns, so other builds of the same programs work too (reference:
 * build/versions/enc97/tables/extract_tables97.py, enc98-95/tables/extract_tables98.py; addresses in docs/FORMATS.md):
 *   room objects   the string "NDOOR.DI$" / "NDOOR.DIB"; the one pointer to it is record 0 (0x38-byte records:
 *                  +0 name*, +4 x, +8 y, +0x14 rect[4], +0x24 half, +0x28 slots[7], +0x30 flags (u8), +0x32 id (u16))
 *   groups         a pointer array starting with record 0, followed by as many counts; ptr[g] = ptr[g-1] +
 *                  count[g-1] * 0x38 (97/98 have a 7th group, FLATPIC: ignored)
 *   ranks          {name*, picture*, min, max} x 6, the first name "No Award" with an AWARD.DI? picture
 *   speech         23 list pointers (0-terminated) with 23 counts at +0x60; the first list's first text starts
 *                  "Ugh! Be quiet"; of several such tables (2006 and 98 have two) the first one is used
 *   picture areas  9 char* from "Life Science" to "History"; sounds: 35 char* from "AMB1.WAV"
 *   music          the non-NULL *.MID char* from "intro.mid" on (98 has a NULL gap in the array)
 *   DLL            STRINGTABLE ids 29001-29069, BITMAP 29150-29164 and 29166
 * Encarta 98 Setup.EXE (Wise installer): every installed file is a raw deflate stream followed by the CRC-32 of
 * its contents; mx_wise_programs scans for those and keeps the program holding "NDOOR.DI$" and the DLL holding
 * string 29044 and bitmap 29166. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"
#include "mx_tables.h"

enum { OBJECT_SIZE = 0x38, MAX_SEC = 96 };

typedef struct {
    const uint8_t *d;
    size_t n;
    uint32_t base, rsrc;
    int nsec;
    struct { uint32_t rva, vsize, off, raw, flags; char name[9]; } sec[MAX_SEC];
} PE;

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned u16(const uint8_t *p) { return p[0] | p[1] << 8; }

static int pe_open(PE *pe, const uint8_t *d, size_t n)
{
    memset(pe, 0, sizeof *pe);
    pe->d = d;
    pe->n = n;
    if (!d || n < 0x400 || d[0] != 'M' || d[1] != 'Z') return 0;
    uint32_t h = u32(d + 0x3c);
    if (h > n - 0x100 || memcmp(d + h, "PE\0\0", 4)) return 0;
    unsigned nsec = u16(d + h + 6), optsz = u16(d + h + 20);
    const uint8_t *opt = d + h + 24;
    if (u16(opt) != 0x10b || nsec > MAX_SEC || (size_t)h + 24 + optsz + 40 * nsec > n) return 0;
    pe->base = u32(opt + 28);
    pe->rsrc = u32(opt + 92) > 2 ? u32(opt + 96 + 16) : 0;
    pe->nsec = (int)nsec;
    for (unsigned i = 0; i < nsec; i++) {
        const uint8_t *s = opt + optsz + 40 * i;
        memcpy(pe->sec[i].name, s, 8);
        pe->sec[i].vsize = u32(s + 8);
        pe->sec[i].rva = u32(s + 12);
        pe->sec[i].raw = u32(s + 16);
        pe->sec[i].off = u32(s + 20);
        pe->sec[i].flags = u32(s + 36);
        if (pe->sec[i].off > n) pe->sec[i].raw = 0;
        else if (pe->sec[i].raw > n - pe->sec[i].off) pe->sec[i].raw = (uint32_t)(n - pe->sec[i].off);
    }
    return 1;
}

/* file pointer for va with len bytes available, or NULL */
static const uint8_t *at(const PE *pe, uint32_t va, size_t len)
{
    uint32_t rva = va - pe->base;
    for (int i = 0; i < pe->nsec; i++) {
        uint32_t r = pe->sec[i].rva, raw = pe->sec[i].raw;
        if (rva >= r && rva - r < raw && len <= raw - (rva - r)) return pe->d + pe->sec[i].off + (rva - r);
    }
    return NULL;
}
static int rd32(const PE *pe, uint32_t va, uint32_t *v)
{
    const uint8_t *p = at(pe, va, 4);
    if (p) *v = u32(p);
    return p != NULL;
}
static uint32_t va_of(const PE *pe, size_t off)
{
    for (int i = 0; i < pe->nsec; i++)
        if (off >= pe->sec[i].off && off - pe->sec[i].off < pe->sec[i].raw)
            return pe->base + pe->sec[i].rva + (uint32_t)(off - pe->sec[i].off);
    return 0;
}
/* the NUL-terminated text at va (raw bytes), its length in *len; NULL if out of range or unterminated */
static const uint8_t *cstr(const PE *pe, uint32_t va, size_t *len)
{
    const uint8_t *p = at(pe, va, 1);
    if (!p) return NULL;
    size_t max = (size_t)(pe->d + pe->n - p), n = 0;
    while (n < max && n < 20000 && p[n]) n++;
    if (n == max || n == 20000) return NULL;
    *len = n;
    return p;
}
static char *text(const PE *pe, uint32_t va)
{
    size_t n;
    const uint8_t *p = cstr(pe, va, &n);
    return p ? mx_strdup_1252(p, n) : NULL;
}
static int is_data(const PE *pe, int i)            /* initialised data, not code / resources / relocations */
{
    return (pe->sec[i].flags & 0x40) && !(pe->sec[i].flags & 0x20000000) && strcmp(pe->sec[i].name, ".rsrc") &&
           strcmp(pe->sec[i].name, ".reloc");
}

/* VAs of the NUL-terminated strings equal to s (ci: ignoring ASCII case; prefix: s only needs to start there) */
static int find_strings(const PE *pe, const char *s, int ci, int prefix, uint32_t *out, int max)
{
    size_t k = strlen(s) + !prefix;
    int n = 0;
    for (size_t i = 1; i + k <= pe->n && n < max; i++) {
        size_t j = 0;
        for (; j < k; j++) {
            unsigned a = pe->d[i + j], b = (unsigned char)s[j];
            if (ci && a >= 'A' && a <= 'Z') a += 32;
            if (ci && b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (j == k && pe->d[i - 1] == 0) {
            uint32_t v = va_of(pe, i);
            if (v) out[n++] = v;
        }
    }
    return n;
}
/* VAs of 4-aligned u32 values equal to v in the data sections, ascending */
static int refs(const PE *pe, uint32_t v, uint32_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < pe->nsec && n < max; i++) {
        if (!is_data(pe, i)) continue;
        size_t o = pe->sec[i].off, e = o + pe->sec[i].raw;
        for (size_t p = (o + 3) & ~(size_t)3; p + 4 <= e && n < max; p += 4)
            if (u32(pe->d + p) == v) out[n++] = pe->base + pe->sec[i].rva + (uint32_t)(p - o);
    }
    return n;
}

static int upper_eq_prefix(const uint8_t *p, size_t n, const char *s)
{
    size_t k = strlen(s);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        unsigned c = p[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != (unsigned char)s[i]) return 0;
    }
    return 1;
}
static int ends_ci(const uint8_t *p, size_t n, const char *s)
{
    size_t k = strlen(s);
    return n >= k && upper_eq_prefix(p + n - k, k, s);
}

/* a char* array of n entries starting with the string `first` (and ending with `last`); its VA or 0 */
static uint32_t array_of(const PE *pe, const char *first, int n, const char *last)
{
    uint32_t s[16], r[16];
    int ns = find_strings(pe, first, 0, 0, s, 16);
    for (int i = 0; i < ns; i++) {
        int nr = refs(pe, s[i], r, 16);
        for (int j = 0; j < nr; j++) {
            int k = 0;
            size_t len = 0;
            const uint8_t *p = NULL;
            for (; k < n; k++) {
                uint32_t v;
                if (!rd32(pe, r[j] + 4u * (uint32_t)k, &v) || !(p = cstr(pe, v, &len))) break;
            }
            if (k == n && (!last || (len == strlen(last) && !memcmp(p, last, len)))) return r[j];
        }
    }
    return 0;
}

static int fail(char *err, int errlen, const char *what, const char *msg)
{
    snprintf(err, (size_t)errlen, "%s: %s", what, msg);
    return 0;
}

static int read_names(const PE *pe, uint32_t va, char **out, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t v;
        if (!rd32(pe, va + 4u * (uint32_t)i, &v) || !(out[i] = text(pe, v))) return 0;
    }
    return 1;
}

static int exe_tables(const PE *pe, MXTables *t, char *err, int errlen)
{
    const char *E = "Encarta program";
    uint32_t c[16], tab[4], g[16];
    int nc = find_strings(pe, "NDOOR.DI$", 0, 0, c, 8);
    nc += find_strings(pe, "NDOOR.DIB", 0, 0, c + nc, 8);
    int nt = 0;
    for (int i = 0; i < nc && nt < 2; i++) nt += refs(pe, c[i], tab + nt, 2 - nt);
    if (nt != 1) return fail(err, errlen, E, "MindMaze's room object table was not found.");
    /* groups */
    uint32_t ptr[16] = {0}, cnt[16] = {0};
    int ng = refs(pe, tab[0], g, 16), found = 0;
    for (int i = 0; i < ng && !found; i++)
        for (int n = 6; n < 10 && !found; n++) {
            int ok = 1;
            for (int k = 0; k < n && ok; k++)
                ok = rd32(pe, g[i] + 4u * k, &ptr[k]) && rd32(pe, g[i] + 4u * (n + k), &cnt[k]) && cnt[k] > 0 && cnt[k] <= 200 &&
                     (!k || ptr[k] == ptr[k - 1] + cnt[k - 1] * OBJECT_SIZE);
            found = ok;
        }
    if (!found) return fail(err, errlen, E, "MindMaze's room object groups were not found.");
    t->nobjects = 0;
    for (int k = 0; k < 6; k++) { t->counts[k] = (int)cnt[k]; t->nobjects += (int)cnt[k]; }
    if (t->nobjects > MXT_MAX_OBJECTS) return fail(err, errlen, E, "too many room objects.");
    for (int i = 0; i < t->nobjects; i++) {
        const uint8_t *r = at(pe, ptr[0] + (uint32_t)i * OBJECT_SIZE, OBJECT_SIZE);
        MXObject *o = &t->obj[i];
        if (!r || !(o->name = text(pe, u32(r)))) { t->nobjects = i; return fail(err, errlen, E, "damaged room object table."); }
        o->x = (int32_t)u32(r + 4);
        o->y = (int32_t)u32(r + 8);
        for (int k = 0; k < 4; k++) o->rect[k] = (int32_t)u32(r + 0x14 + 4 * k);
        o->half = (int32_t)u32(r + 0x24);
        for (int k = 0; k < 7; k++) o->slots[k] = r[0x28 + k];
        o->flags = r[0x30];
        o->id = (int)u16(r + 0x32);
    }
    /* ranks */
    uint32_t s[8], r[16], rk = 0;
    int ns = find_strings(pe, "No Award", 0, 0, s, 8);
    for (int i = 0; i < ns && !rk; i++) {
        int nr = refs(pe, s[i], r, 16);
        for (int j = 0; j < nr && !rk; j++) {
            uint32_t pic, lo;
            size_t len;
            const uint8_t *p;
            if (rd32(pe, r[j] + 4, &pic) && rd32(pe, r[j] + 8, &lo) && lo == 0 && (p = cstr(pe, pic, &len)) &&
                upper_eq_prefix(p, len, "AWARD.DI"))
                rk = r[j];
        }
    }
    if (!rk) return fail(err, errlen, E, "MindMaze's rank table was not found.");
    for (int i = 0; i < 6; i++) {
        uint32_t va = rk + 16u * (uint32_t)i, pn, pi;
        if (!rd32(pe, va, &pn) || !rd32(pe, va + 4, &pi) || !rd32(pe, va + 8, &t->rank[i].min) ||
            !rd32(pe, va + 12, &t->rank[i].max) || !(t->rank[i].name = text(pe, pn)) || !(t->rank[i].image = text(pe, pi)))
            return fail(err, errlen, E, "damaged rank table.");
    }
    /* speech: the first table (lowest address) */
    uint32_t sp = 0, u[16], l[16], q[16];
    int nu = find_strings(pe, "Ugh! Be quiet", 0, 1, u, 16);
    for (int i = 0; i < nu; i++) {
        int nl = refs(pe, u[i], l, 16);
        for (int j = 0; j < nl; j++) {
            int nq = refs(pe, l[j], q, 16);
            for (int k = 0; k < nq; k++) {
                uint32_t end = 1, cn;
                int ok = rd32(pe, q[k] + 23 * 4, &end) && end == 0;
                for (int m = 0; m < 23 && ok; m++) ok = rd32(pe, q[k] + 0x60 + 4u * m, &cn) && cn <= 50;
                if (ok && (!sp || q[k] < sp)) sp = q[k];
            }
        }
    }
    if (!sp) return fail(err, errlen, E, "MindMaze's speech table was not found.");
    for (int k = 0; k < 23; k++) {
        uint32_t p = 0, n = 0;
        if (!rd32(pe, sp + 4u * k, &p) || !rd32(pe, sp + 0x60 + 4u * k, &n)) return fail(err, errlen, E, "damaged speech table.");
        if (!p) n = 0;
        if (n && !(t->speech[k] = calloc(n, sizeof(char *)))) return fail(err, errlen, E, "out of memory");
        for (uint32_t j = 0; j < n; j++) {
            uint32_t tv;
            if (!rd32(pe, p + 4 * j, &tv) || !(t->speech[k][j] = text(pe, tv))) return fail(err, errlen, E, "damaged speech table.");
            t->nspeech[k] = (int)j + 1;
        }
    }
    /* name lists */
    uint32_t pa = array_of(pe, "Life Science", 9, "History"), so = array_of(pe, "AMB1.WAV", 35, NULL);
    if (!pa || !read_names(pe, pa, t->picture_areas, 9)) return fail(err, errlen, E, "MindMaze's picture areas were not found.");
    if (!so || !read_names(pe, so, t->sounds, 35)) return fail(err, errlen, E, "MindMaze's sound list was not found.");
    int nm = 0;
    ns = find_strings(pe, "intro.mid", 1, 0, s, 8);
    for (int i = 0; i < ns && nm < 5; i++) {
        int nr = refs(pe, s[i], r, 16);
        for (int j = 0; j < nr && nm < 5; j++) {
            nm = 0;
            for (int k = 0; k < 16 && nm < 5; k++) {
                uint32_t v;
                size_t len;
                const uint8_t *p;
                if (!rd32(pe, r[j] + 4u * k, &v)) break;
                if (!v) continue;
                if (!(p = cstr(pe, v, &len)) || !ends_ci(p, len, ".MID")) break;
                t->music[nm++] = mx_strdup_1252(p, len);
            }
            if (nm < 5) for (int k = 0; k < nm; k++) { free(t->music[k]); t->music[k] = NULL; }
        }
    }
    if (nm < 5) return fail(err, errlen, E, "MindMaze's music list was not found.");
    return 1;
}

/* ---------------------------------------------------------------------------------------------- resources */
static const uint8_t *rva_ptr(const PE *pe, uint32_t rva, size_t len) { return at(pe, pe->base + rva, len); }
static int rsrc_find(const PE *pe, uint32_t off, uint32_t id, int any, uint32_t *target, int *isdir)
{
    const uint8_t *dir = rva_ptr(pe, pe->rsrc + off, 16);
    if (!dir) return 0;
    unsigned cnt = u16(dir + 12) + u16(dir + 14);
    const uint8_t *e = rva_ptr(pe, pe->rsrc + off + 16, 8u * cnt);
    if (!e) return 0;
    for (unsigned i = 0; i < cnt; i++)
        if (any || (!(u32(e + 8 * i) & 0x80000000u) && u32(e + 8 * i) == id)) {
            uint32_t v = u32(e + 8 * i + 4);
            *isdir = (v & 0x80000000u) != 0;
            *target = v & 0x7fffffffu;
            return 1;
        }
    return 0;
}
static const uint8_t *resource(const PE *pe, uint32_t type, uint32_t id, size_t *len)
{
    uint32_t a, b, c;
    int dir;
    if (!pe->rsrc || !rsrc_find(pe, 0, type, 0, &a, &dir) || !dir || !rsrc_find(pe, a, id, 0, &b, &dir) || !dir) return NULL;
    if (!rsrc_find(pe, b, 0x409, 0, &c, &dir) && !rsrc_find(pe, b, 0, 1, &c, &dir)) return NULL;
    if (dir) return NULL;
    const uint8_t *de = rva_ptr(pe, pe->rsrc + c, 16);
    if (!de) return NULL;
    *len = u32(de + 4);
    return rva_ptr(pe, u32(de), *len);
}

static int dll_tables(const PE *pe, MXTables *t, char *err, int errlen)
{
    const char *E = "Encarta resource file";
    for (int i = 0; i < MXT_NBITMAPS; i++) {
        size_t len = 0;
        const uint8_t *dib = resource(pe, 2, (uint32_t)mx_table_bitmaps[i], &len);
        if (!dib || !(t->bmp[i] = mx_dib_to_bmp(dib, len, &t->bmplen[i]))) {
            char m[64];
            snprintf(m, sizeof m, "MindMaze's panel picture %d is missing.", mx_table_bitmaps[i]);
            return fail(err, errlen, E, m);
        }
    }
    int found = 0;
    for (uint32_t block = (MXT_FIRST_STRING >> 4) + 1; block <= (MXT_LAST_STRING >> 4) + 1; block++) {
        size_t len = 0, pos = 0;
        const uint8_t *d = resource(pe, 6, block, &len);
        for (int k = 0; d && k < 16 && pos + 2 <= len; k++) {
            unsigned n = u16(d + pos);
            int id = (int)(block - 1) * 16 + k;
            if (pos + 2 + 2 * (size_t)n > len) break;
            if (n && id >= MXT_FIRST_STRING && id <= MXT_LAST_STRING) {
                char *s = malloc(n * 4 + 1), *o = s;
                if (!s) return fail(err, errlen, E, "out of memory");
                for (unsigned i = 0; i < n; i++) {
                    unsigned ch = u16(d + pos + 2 + 2 * i);
                    if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < n) {
                        ch = 0x10000 + ((ch - 0xD800) << 10) + (u16(d + pos + 4 + 2 * i) - 0xDC00);
                        i++;
                    }
                    if (ch < 0x80) *o++ = (char)ch;
                    else if (ch < 0x800) { *o++ = (char)(0xC0 | ch >> 6); *o++ = (char)(0x80 | (ch & 63)); }
                    else if (ch < 0x10000) { *o++ = (char)(0xE0 | ch >> 12); *o++ = (char)(0x80 | (ch >> 6 & 63)); *o++ = (char)(0x80 | (ch & 63)); }
                    else { *o++ = (char)(0xF0 | ch >> 18); *o++ = (char)(0x80 | (ch >> 12 & 63)); *o++ = (char)(0x80 | (ch >> 6 & 63)); *o++ = (char)(0x80 | (ch & 63)); }
                }
                *o = 0;
                free(t->str[id - MXT_FIRST_STRING]);
                t->str[id - MXT_FIRST_STRING] = s;
                found++;
            }
            pos += 2 + 2 * (size_t)n;
        }
    }
    if (found < 40) return fail(err, errlen, E, "MindMaze's texts (ids 29002-29069) are missing.");
    return 1;
}

int mx_tables_pe(const uint8_t *exe, size_t exelen, const uint8_t *dll, size_t dlllen, MXTables *t, char *err, int errlen)
{
    PE *pe = malloc(sizeof *pe);
    int ok = 0;
    memset(t, 0, sizeof *t);
    if (!pe) return fail(err, errlen, "Encarta program", "out of memory");
    if (!pe_open(pe, exe, exelen)) fail(err, errlen, "Encarta program", "not a 32-bit Windows program.");
    else if (exe_tables(pe, t, err, errlen)) {
        if (!pe_open(pe, dll, dlllen)) fail(err, errlen, "Encarta resource file", "not a Windows program file.");
        else ok = dll_tables(pe, t, err, errlen);
    }
    free(pe);
    if (!ok) mx_tables_free(t);
    return ok;
}

/* ---------------------------------------------------------------------------------------------- Wise Setup.EXE */
static int pe_has_resources(const uint8_t *d, size_t n, int need_exe)
{
    PE *pe = malloc(sizeof *pe);
    int ok = 0;
    if (pe && pe_open(pe, d, n)) {
        unsigned chars = u16(d + u32(d + 0x3c) + 22);
        int is_dll = (chars & 0x2000) != 0;
        size_t len;
        if (need_exe) {
            uint32_t s[1];
            ok = !is_dll && find_strings(pe, "NDOOR.DI$", 0, 0, s, 1) + find_strings(pe, "NDOOR.DIB", 0, 0, s, 1) > 0;
        } else ok = is_dll && resource(pe, 2, 29166, &len) && resource(pe, 6, (29044 >> 4) + 1, &len);
    }
    free(pe);
    return ok;
}

int mx_wise_programs(const uint8_t *d, size_t n, uint8_t **exe, size_t *exelen, uint8_t **dll, size_t *dlllen)
{
    *exe = *dll = NULL;
    for (size_t i = 0; i + 8 < n && !(*exe && *dll);) {
        /* a deflate block header: 3 bits; type 3 is invalid, skip quickly */
        if ((d[i] >> 1 & 3) == 3) { i++; continue; }
        size_t len = 0, used = 0;
        uint8_t *o = mx_inflate_raw(d + i, n - i, 64u << 20, &len, &used);
        if (o && len >= 0x400 && i + used + 4 <= n && u32(d + i + used) == mx_crc32(o, len)) {
            if (o[0] == 'M' && o[1] == 'Z' && !*exe && pe_has_resources(o, len, 1)) { *exe = o; *exelen = len; o = NULL; }
            else if (o[0] == 'M' && o[1] == 'Z' && !*dll && pe_has_resources(o, len, 0)) { *dll = o; *dlllen = len; o = NULL; }
            free(o);
            i += used + 4;
            continue;
        }
        free(o);
        i++;
    }
    if (*exe && *dll) return 1;
    free(*exe);
    free(*dll);
    *exe = *dll = NULL;
    return 0;
}
