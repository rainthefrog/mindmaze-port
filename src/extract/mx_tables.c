/* MindMaze's tables from any edition: the shared part (see mx_tables.h). tables.json writer with the 2006
 * spellings, UI text fallbacks, code page helpers. CHANGE 2026-11-18. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_tables.h"

const int mx_table_bitmaps[MXT_NBITMAPS] = {29150, 29151, 29152, 29153, 29154, 29155, 29156, 29157,
                                            29158, 29159, 29160, 29161, 29162, 29163, 29164, 29166};

/* ---------------------------------------------------------------------------------------------- code pages */
static const unsigned short CP1252_80[32] = {
    0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F,
    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D, 0x17E, 0x178};
static const unsigned short MACROMAN_80[128] = {
    0xC4, 0xC5, 0xC7, 0xC9, 0xD1, 0xD6, 0xDC, 0xE1, 0xE0, 0xE2, 0xE4, 0xE3, 0xE5, 0xE7, 0xE9, 0xE8,
    0xEA, 0xEB, 0xED, 0xEC, 0xEE, 0xEF, 0xF1, 0xF3, 0xF2, 0xF4, 0xF6, 0xF5, 0xFA, 0xF9, 0xFB, 0xFC,
    0x2020, 0xB0, 0xA2, 0xA3, 0xA7, 0x2022, 0xB6, 0xDF, 0xAE, 0xA9, 0x2122, 0xB4, 0xA8, 0x2260, 0xC6, 0xD8,
    0x221E, 0xB1, 0x2264, 0x2265, 0xA5, 0xB5, 0x2202, 0x2211, 0x220F, 0x3C0, 0x222B, 0xAA, 0xBA, 0x3A9, 0xE6, 0xF8,
    0xBF, 0xA1, 0xAC, 0x221A, 0x192, 0x2248, 0x2206, 0xAB, 0xBB, 0x2026, 0xA0, 0xC0, 0xC3, 0xD5, 0x152, 0x153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0xF7, 0x25CA, 0xFF, 0x178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0xB7, 0x201A, 0x201E, 0x2030, 0xC2, 0xCA, 0xC1, 0xCB, 0xC8, 0xCD, 0xCE, 0xCF, 0xCC, 0xD3, 0xD4,
    0xF8FF, 0xD2, 0xDA, 0xDB, 0xD9, 0x131, 0x2C6, 0x2DC, 0xAF, 0x2D8, 0x2D9, 0x2DA, 0xB8, 0x2DD, 0x2DB, 0x2C7};

static char *utf8_put(char *o, unsigned c)
{
    if (c < 0x80) *o++ = (char)c;
    else if (c < 0x800) { *o++ = (char)(0xC0 | c >> 6); *o++ = (char)(0x80 | (c & 63)); }
    else { *o++ = (char)(0xE0 | c >> 12); *o++ = (char)(0x80 | (c >> 6 & 63)); *o++ = (char)(0x80 | (c & 63)); }
    return o;
}

static char *decode(const uint8_t *s, size_t n, int mac)
{
    if (!s) return NULL;
    if (n == (size_t)-1) n = strlen((const char *)s);
    char *t = malloc(3 * n + 1), *o = t;
    if (!t) return NULL;
    for (size_t i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c >= 0x80) c = mac ? MACROMAN_80[c - 0x80] : c < 0xA0 ? CP1252_80[c - 0x80] : c;
        o = utf8_put(o, c);
    }
    *o = 0;
    return t;
}
char *mx_strdup_1252(const uint8_t *s, size_t n) { return decode(s, n, 0); }
char *mx_strdup_macroman(const uint8_t *s, size_t n) { return decode(s, n, 1); }

uint8_t *mx_dib_to_bmp(const uint8_t *dib, size_t len, size_t *outlen)
{
    if (!dib || len < 40) return NULL;
    uint32_t hs = dib[0] | dib[1] << 8 | dib[2] << 16 | (uint32_t)dib[3] << 24;
    uint32_t colours = dib[32] | dib[33] << 8 | dib[34] << 16 | (uint32_t)dib[35] << 24;
    unsigned bpp = dib[14] | dib[15] << 8;
    uint32_t comp = dib[16] | dib[17] << 8 | dib[18] << 16 | (uint32_t)dib[19] << 24;
    if (!colours && bpp <= 8) colours = 1u << bpp;
    uint32_t off = 14 + hs + 4 * colours + (comp == 3 && hs == 40 ? 12 : 0), total = (uint32_t)len + 14;
    uint8_t *bmp = malloc(len + 14);
    if (!bmp) return NULL;
    memcpy(bmp, "BM", 2);
    for (int b = 0; b < 4; b++) { bmp[2 + b] = (uint8_t)(total >> 8 * b); bmp[10 + b] = (uint8_t)(off >> 8 * b); }
    memset(bmp + 6, 0, 4);
    memcpy(bmp + 14, dib, len);
    *outlen = len + 14;
    return bmp;
}

/* ---------------------------------------------------------------------------------------------- JSON */
typedef struct { char *p; size_t n, cap; int bad; } Buf;
static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 65536;
        while (c < b->n + n + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) { b->bad = 1; return; }
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bfmt(Buf *b, const char *fmt, ...)
{
    char t[512];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    if (n > 0) bput(b, t, (size_t)n < sizeof t ? (size_t)n : sizeof t - 1);
}

/* text kinds: how a program's spelling becomes the 2006 one */
enum { K_TEXT, K_FILE, K_MUSIC, K_SPEECH };

static void bjson(Buf *b, const char *s, int kind)
{
    size_t n = s ? strlen(s) : 0;
    int dib = kind == K_FILE || kind == K_TEXT;
    bstr(b, "\"");
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char e[8];
        if (dib && i + 4 == n && c == '.' && (s[i + 1] | 32) == 'd' && (s[i + 2] | 32) == 'i' && s[i + 3] == '$') {
            bstr(b, ".DIB");                                       /* compressed baggage name NAME.DI$ */
            break;
        }
        if (kind == K_SPEECH && (c == '\r' || c == '\n')) {        /* the 2006 tables have a literal \n */
            if (c == '\r' && s[i + 1] == '\n') i++;
            bstr(b, "\\\\n");
            continue;
        }
        if (kind == K_SPEECH && c == '<') { bstr(b, "&lt;"); continue; }
        if (kind == K_SPEECH && c == '>') { bstr(b, "&gt;"); continue; }
        if (kind == K_MUSIC && c >= 'a' && c <= 'z') c -= 32;
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; bput(b, e, 2); }
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); bstr(b, e); }
        else bput(b, (const char *)&c, 1);
    }
    bstr(b, "\"");
}

static void names(Buf *b, const char *key, char *const *v, int n, int kind)
{
    bfmt(b, ",\n \"%s\": [", key);
    for (int i = 0; i < n; i++) {
        if (i) bstr(b, ", ");
        bjson(b, v[i], kind);
    }
    bstr(b, "]");
}

/* UI texts some programs lack (the older menus differ): the game's English defaults */
static const struct { int id; const char *text; } DEFAULTS[] = {
    {29005, "mindmaze.db"}, {29026, "mindmaze.idx"},
    {29010, "Answer 1"}, {29011, "Answer 2"}, {29012, "Answer 3"}, {29013, "Answer 4"}, {29027, "TITLESCR.DIB"},
    {29035, "A."}, {29036, "B."}, {29037, "C."}, {29038, "D."},
    {29041, "Correct!  %u points added to your score."}, {29057, "Correct!  %u point added to your score."},
    {29061, "Start &New Game"}, {29062, "&Quit Game"}, {29063, "Turn &Music Off"}, {29064, "Turn &Music On"},
    {29065, "Turn &Sound Effects Off"}, {29066, "Turn &Sound Effects On"}, {29067, "MindMaze"},
    {29068, "New Game"}, {29069, "Quit Game"}, {0, NULL}};

char *mx_tables_json(const MXTables *t, const char *source, size_t *len)
{
    static const char *const group[6] = {"doors", "floors", "ceilings", "walls", "characters", "furniture"};
    Buf b = {0};
    bstr(&b, "{\n \"format\": \"mindmaze-tables-1\",\n \"source\": ");
    bjson(&b, source, K_TEXT);
    bstr(&b, ",\n \"groups\": {");
    for (int g = 0; g < 6; g++) bfmt(&b, "%s\"%s\": %d", g ? ", " : "", group[g], t->counts[g]);
    bstr(&b, "},\n \"objects\": [");
    for (int i = 0; i < t->nobjects; i++) {
        const MXObject *o = &t->obj[i];
        bstr(&b, i ? ",\n  {\"name\": " : "\n  {\"name\": ");
        bjson(&b, o->name, K_FILE);
        bfmt(&b, ", \"x\": %d, \"y\": %d, \"rect\": [%d, %d, %d, %d], \"half\": %d, \"slots\": [%d, %d, %d, %d, %d, %d, %d], "
                 "\"flags\": %d, \"id\": %d}",
             o->x, o->y, o->rect[0], o->rect[1], o->rect[2], o->rect[3], o->half, o->slots[0], o->slots[1], o->slots[2],
             o->slots[3], o->slots[4], o->slots[5], o->slots[6], o->flags, o->id);
    }
    bstr(&b, "\n ],\n \"ranks\": [");
    for (int i = 0; i < 6; i++) {
        bstr(&b, i ? ",\n  {\"name\": " : "\n  {\"name\": ");
        bjson(&b, t->rank[i].name, K_TEXT);
        bstr(&b, ", \"image\": ");
        bjson(&b, t->rank[i].image, K_FILE);
        bfmt(&b, ", \"min\": %u, \"max\": %u}", t->rank[i].min, t->rank[i].max);
    }
    bstr(&b, "\n ],\n \"speech\": [");
    for (int k = 0; k < 23; k++) {
        bstr(&b, k ? ",\n  [" : "\n  [");
        for (int j = 0; j < t->nspeech[k]; j++) {
            if (j) bstr(&b, ", ");
            bjson(&b, t->speech[k][j], K_SPEECH);
        }
        bstr(&b, "]");
    }
    bstr(&b, "\n ]");
    names(&b, "picture_areas", t->picture_areas, 9, K_TEXT);
    names(&b, "music", t->music, 5, K_MUSIC);
    names(&b, "sounds", t->sounds, 35, K_MUSIC);
    bstr(&b, ",\n \"strings\": {");
    int first = 1;
    for (int id = MXT_FIRST_STRING + 1; id <= MXT_LAST_STRING; id++) {
        const char *s = t->str[id - MXT_FIRST_STRING], *def = NULL;
        for (int d = 0; DEFAULTS[d].id; d++) if (DEFAULTS[d].id == id) def = DEFAULTS[d].text;
        if (id == 29005 || id == 29026) s = NULL;                  /* Mac: "MindMaze Data" -> the port's file names */
        if (!s) s = def;
        if (!s) continue;
        bfmt(&b, "%s\n  \"%d\": ", first ? "" : ",", id);
        bjson(&b, s, K_TEXT);
        first = 0;
    }
    bstr(&b, "\n }\n}\n");
    if (b.bad) { free(b.p); return NULL; }
    *len = b.n;
    return b.p;
}

void mx_tables_free(MXTables *t)
{
    for (int i = 0; i < t->nobjects; i++) free(t->obj[i].name);
    for (int i = 0; i < 6; i++) { free(t->rank[i].name); free(t->rank[i].image); }
    for (int k = 0; k < 23; k++) {
        for (int j = 0; j < t->nspeech[k]; j++) free(t->speech[k][j]);
        free(t->speech[k]);
    }
    for (int i = 0; i < 9; i++) free(t->picture_areas[i]);
    for (int i = 0; i < 5; i++) free(t->music[i]);
    for (int i = 0; i < 35; i++) free(t->sounds[i]);
    for (int i = 0; i < MXT_NSTRINGS; i++) free(t->str[i]);
    for (int i = 0; i < MXT_NBITMAPS; i++) free(t->bmp[i]);
    memset(t, 0, sizeof *t);
}
