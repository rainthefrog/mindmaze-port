/* Question bank: the pluggable question source.
 *
 * Sources (a load spec of entries separated by "|", built by mm_sources.c from the settings screen):
 *   original          MINDMAZE.IDX + MINDMAZE.DB in the data folder (format: tools/mmdb.py)
 *   original:<dir>/   the same files of another Encarta edition, data/<dir>/ (editions/<id>/, written by the
 *                     extractor in the 2006 layout from that edition's own question files)
 *   user:<file>       the same JSON format, next to the exe (custom / online questions; missing = empty)
 *   json:<file>       a questions.json style file ({"questions": [{id, area, level, question,
 *                     answers: [{text}, x4]}]}, answers[0] correct). Relative paths are under the data folder.
 * Both fill the same per (area, level) lists, in the original IDX order, which the picker (mm_game.c,
 * ported from FUN_00522a30 / FUN_00522c10 / FUN_005226a0) works on.
 */
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "mm.h"

#define AREAS 9
#define LEVELS 4

static MMQuestion *g_q;
static int g_nq, g_capq;
static int *g_list[AREAS][LEVELS];   /* indices into g_q */
static int g_count[AREAS][LEVELS];

static MMQuestion *new_question(void)
{
    if (g_nq == g_capq) {
        g_capq = g_capq ? g_capq * 2 : 1024;
        g_q = realloc(g_q, sizeof *g_q * g_capq);
    }
    MMQuestion *q = &g_q[g_nq++];
    memset(q, 0, sizeof *q);
    return q;
}

static void build_lists(void)
{
    for (int a = 0; a < AREAS; a++)
        for (int l = 0; l < LEVELS; l++) {
            free(g_list[a][l]);
            g_list[a][l] = malloc(sizeof(int) * (g_nq + 1));
            g_count[a][l] = 0;
        }
    for (int i = 0; i < g_nq; i++) {
        MMQuestion *q = &g_q[i];
        if (q->area < 0 || q->area >= AREAS || q->level < 0 || q->level >= LEVELS) continue;
        g_list[q->area][q->level][g_count[q->area][q->level]++] = i;
    }
}

static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* ---- original MINDMAZE.IDX / MINDMAZE.DB (FUN_00522760 reads the IDX the same way) ---- */
static int load_original(const char *dir, char *err, int errlen)
{
    int isz = 0, dsz = 0;
    char pi[300], pd[300];
    snprintf(pi, sizeof pi, "%smindmaze.idx", dir);
    snprintf(pd, sizeof pd, "%smindmaze.db", dir);
    unsigned char *idx = plat_load(pi, &isz);
    unsigned char *db = plat_load(pd, &dsz);
    if (!idx || !db || isz < AREAS * LEVELS * 4) {
        snprintf(err, errlen, "%s", mm_str(29048));
        free(idx);
        free(db);
        return 0;
    }
    int pos = AREAS * LEVELS * 4;
    for (int a = 0; a < AREAS; a++)
        for (int l = 0; l < LEVELS; l++) {
            unsigned n = rd32(idx + (a * LEVELS + l) * 4);
            for (unsigned k = 0; k < n && pos + 8 <= isz; k++, pos += 8) {
                unsigned number = rd32(idx + pos), off = rd32(idx + pos + 4);
                if (off + 8 > (unsigned)dsz) continue;
                const unsigned char *p = db + off + 4;          /* u32 0, then the question */
                unsigned qlen = rd32(p);
                p += 4;
                MMQuestion *q = new_question();
                q->id = (int)number;
                q->area = a;
                q->level = l;
                q->text = mm_cp1252_to_utf8(p, (int)qlen);
                p += qlen;
                for (int i = 0; i < 4; i++) {
                    int alen = *p++;
                    q->answer[i] = mm_cp1252_to_utf8(p, alen);
                    p += alen;
                    q->article[i] = rd32(p);
                    p += 6;                                     /* u32 article id, u16 value */
                }
            }
        }
    free(idx);
    free(db);
    return 1;
}

/* ---- JSON (small reader for the questions format) ---- */
typedef struct { const char *p, *end; } J;

static void jws(J *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }

static char *jstring(J *j)
{
    jws(j);
    if (j->p >= j->end || *j->p != '"') return NULL;
    j->p++;
    const char *e = j->p;                                  /* find the closing quote first: size the buffer */
    while (e < j->end && *e != '"') e += (*e == '\\' && e + 1 < j->end) ? 2 : 1;
    char *out = malloc((size_t)(e - j->p) + 4), *o = out;   /* escapes never make text longer */
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') { *o++ = c; continue; }
        c = *j->p++;
        switch (c) {
        case 'n': *o++ = '\n'; break;
        case 't': *o++ = '\t'; break;
        case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'u': {
            unsigned u = (unsigned)strtoul((char[]){j->p[0], j->p[1], j->p[2], j->p[3], 0}, NULL, 16);
            j->p += 4;
            if (u >= 0xD800 && u < 0xDC00 && j->p[0] == '\\' && j->p[1] == 'u') {
                unsigned lo = (unsigned)strtoul((char[]){j->p[2], j->p[3], j->p[4], j->p[5], 0}, NULL, 16);
                j->p += 6;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
            if (u < 0x80) *o++ = (char)u;
            else if (u < 0x800) { *o++ = (char)(0xC0 | u >> 6); *o++ = (char)(0x80 | (u & 63)); }
            else if (u < 0x10000) { *o++ = (char)(0xE0 | u >> 12); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
            else { *o++ = (char)(0xF0 | u >> 18); *o++ = (char)(0x80 | ((u >> 12) & 63)); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
            break;
        }
        default: *o++ = c;
        }
    }
    j->p++;
    *o = 0;
    return out;
}

static void jskip(J *j);
static void jskip_container(J *j, char close)
{
    j->p++;
    for (;;) {
        jws(j);
        if (j->p >= j->end) return;
        if (*j->p == close) { j->p++; return; }
        if (*j->p == ',' || *j->p == ':') { j->p++; continue; }
        jskip(j);
    }
}
static void jskip(J *j)
{
    jws(j);
    if (j->p >= j->end) return;
    if (*j->p == '"') free(jstring(j));
    else if (*j->p == '{') jskip_container(j, '}');
    else if (*j->p == '[') jskip_container(j, ']');
    else while (j->p < j->end && !strchr(",}] \n\r\t", *j->p)) j->p++;
}

static int jexpect(J *j, char c) { jws(j); if (j->p < j->end && *j->p == c) { j->p++; return 1; } return 0; }

/* parse one answer: {"text": ...} or a plain string */
static char *janswer(J *j, unsigned *article)
{
    jws(j);
    if (*j->p == '"') return jstring(j);
    char *text = NULL;
    if (!jexpect(j, '{')) return NULL;
    while (!jexpect(j, '}')) {
        char *k = jstring(j);
        if (!k) return text;
        jexpect(j, ':');
        if (!strcmp(k, "text")) { free(text); text = jstring(j); }
        else if (!strcmp(k, "article")) { jws(j); *article = (unsigned)strtoul(j->p, (char **)&j->p, 10); }
        else jskip(j);
        free(k);
        jexpect(j, ',');
    }
    return text;
}

static int jquestion(J *j)
{
    MMQuestion q = {0};
    int na = 0;
    if (!jexpect(j, '{')) return 0;
    while (!jexpect(j, '}')) {
        char *k = jstring(j);
        if (!k) return 0;
        jexpect(j, ':');
        jws(j);
        if (!strcmp(k, "id")) q.id = (int)strtol(j->p, (char **)&j->p, 10);
        else if (!strcmp(k, "area")) q.area = (int)strtol(j->p, (char **)&j->p, 10);
        else if (!strcmp(k, "level")) q.level = (int)strtol(j->p, (char **)&j->p, 10);
        else if (!strcmp(k, "question")) q.text = jstring(j);
        else if (!strcmp(k, "answers")) {
            jexpect(j, '[');
            while (!jexpect(j, ']')) {
                unsigned art = 0;
                char *a = janswer(j, &art);
                if (a && na < 4) { q.answer[na] = a; q.article[na] = art; na++; }
                else free(a);
                jexpect(j, ',');
            }
        } else jskip(j);
        free(k);
        jexpect(j, ',');
    }
    if (q.text && na == 4) {
        if (!q.id) q.id = g_nq + 1;
        *new_question() = q;
    }
    return 1;
}

static int load_json(const char *file, int user, char *err, int errlen)
{
    int size = 0;
    char *data = user ? plat_load_user(file, &size) : plat_load(file, &size);
    if (!data) {
        if (user) return 1;                                  /* custom / online file not created yet */
        snprintf(err, errlen, "Cannot read the question file %s.", file);
        return 0;
    }
    J j = {data, data + size};
    /* find "questions": [ ... ] at the top level */
    if (!jexpect(&j, '{')) { free(data); snprintf(err, errlen, "%s: not a questions file.", file); return 0; }
    while (!jexpect(&j, '}')) {
        char *k = jstring(&j);
        if (!k) break;
        jexpect(&j, ':');
        if (!strcmp(k, "questions")) {
            jexpect(&j, '[');
            while (!jexpect(&j, ']')) {
                if (!jquestion(&j)) break;
                jexpect(&j, ',');
            }
        } else jskip(&j);
        free(k);
        jexpect(&j, ',');
    }
    free(data);
    return 1;
}

/* ---- Encarta text markup: HTML entities (&eacute; &rsquo; ...) are decoded to UTF-8 at load time; the
 *      tags (<it> italic, <scp>, <sup>, ...) stay in the text for the renderer (mm_rich.c). ---- */
static const struct { const char *name; unsigned cp, cp2; } ENT[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"emsp", 0x2003},
    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
    {"deg", 0xB0}, {"middot", 0xB7}, {"frac12", 0xBD}, {"frac14", 0xBC}, {"frac34", 0xBE}, {"uml", 0xA8},
    {"Aring", 0xC5}, {"aring", 0xE5}, {"Eacute", 0xC9}, {"eacute", 0xE9}, {"Icirc", 0xCE}, {"icirc", 0xEE},
    {"Uuml", 0xDC}, {"uuml", 0xFC}, {"aacute", 0xE1}, {"acirc", 0xE2}, {"agrave", 0xE0}, {"atilde", 0xE3},
    {"auml", 0xE4}, {"ccedil", 0xE7}, {"ecirc", 0xEA}, {"egrave", 0xE8}, {"euml", 0xEB}, {"iacute", 0xED},
    {"iuml", 0xEF}, {"ntilde", 0xF1}, {"oacute", 0xF3}, {"ocirc", 0xF4}, {"ograve", 0xF2}, {"oslash", 0xF8},
    {"ouml", 0xF6}, {"uacute", 0xFA}, {"Amacr", 0x100}, {"amacr", 0x101}, {"abreve", 0x103}, {"cacute", 0x107},
    {"ccaron", 0x10D}, {"eogon", 0x119}, {"Idot", 0x130}, {"imacr", 0x12B}, {"lstrok", 0x142}, {"nacute", 0x144},
    {"Omacr", 0x14C}, {"omacr", 0x14D}, {"obreve", 0x14F}, {"rcaron", 0x159}, {"sacute", 0x15B}, {"scedil", 0x15F},
    {"scaron", 0x161}, {"tcedil", 0x163}, {"umacr", 0x16B}, {"Hcedil", 0x1E28},
    {"cbreve", 'c', 0x306}, {"sbreve", 's', 0x306}, {"zbreve", 'z', 0x306},   /* letter + combining breve */
    /* used by the Encarta 95-98 question sets (CHANGE 2026-11-18) */
    {"Acirc", 0xC2}, {"yacute", 0xFD}, {"times", 0xD7}, {"emacr", 0x113}, {"gbreve", 0x11F}, {"uring", 0x16F},
    {"alpha", 0x3B1}, {"pi", 0x3C0}, {"uranus2", 0x2645},
};

static char *put_utf8(char *o, unsigned u)
{
    if (u < 0x80) *o++ = (char)u;
    else if (u < 0x800) { *o++ = (char)(0xC0 | u >> 6); *o++ = (char)(0x80 | (u & 63)); }
    else if (u < 0x10000) { *o++ = (char)(0xE0 | u >> 12); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
    else { *o++ = (char)(0xF0 | u >> 18); *o++ = (char)(0x80 | ((u >> 12) & 63)); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
    return o;
}

static void decode_entities(char *s)
{
    /* Encarta 97/98 lost the "<date " of living people's dates: "(1947-type=alive>)" -> "(1947-)" */
    for (char *a; s && (a = strstr(s, "type=alive>"));) memmove(a, a + 11, strlen(a + 11) + 1);
    /* Encarta's own codes (2026-11-21: shown raw, and the formula ran past the box): chemical bonds and a time signature */
    static const char *const ZE[4][2] = {{"&z.1horbar;", "\xE2\x80\x93"}, {"&z.2horbar;", "="}, {"&z.3horbar;", "\xE2\x89\xA1"},
                                         {"&z.notation3.4;", "\xC2\xBE"}};   /* en dash, U+2261 triple bar, 3/4 */
    for (int i = 0; s && i < 4; i++)
        for (char *a; (a = strstr(s, ZE[i][0]));) {
            size_t n = strlen(ZE[i][0]), m = strlen(ZE[i][1]);
            memcpy(a, ZE[i][1], m);
            memmove(a + m, a + n, strlen(a + n) + 1);
        }
    if (!s || !strchr(s, '&')) return;
    char *o = s;
    for (char *p = s; *p;) {
        if (*p == '&') {
            char *semi = p + 1;                               /* the name, then ';' - or a typo of the data: */
            while (*semi && (isalnum((unsigned char)*semi) || *semi == '#')) semi++;   /* "Moli&egrave:re", "&mdash." */
            if (*semi != ';' && *semi != ':' && *semi != '.') semi = NULL;
            if (semi && semi - p < 12) {
                unsigned cp = 0, cp2 = 0;
                if (p[1] == '#') cp = (unsigned)(p[2] == 'x' || p[2] == 'X' ? strtoul(p + 3, NULL, 16) : strtoul(p + 2, NULL, 10));
                else
                    for (size_t i = 0; i < sizeof ENT / sizeof ENT[0]; i++)
                        if ((size_t)(semi - p - 1) == strlen(ENT[i].name) && !strncmp(p + 1, ENT[i].name, semi - p - 1)) {
                            cp = ENT[i].cp;
                            cp2 = ENT[i].cp2;
                            break;
                        }
                if (cp) {                                     /* UTF-8 is never longer than the entity */
                    o = put_utf8(o, cp);
                    if (cp2) o = put_utf8(o, cp2);
                    p = semi + (*semi != '.');                   /* (a typo's '.' is a full stop: kept) */
                    continue;
                }
            }
        }
        *o++ = *p++;
    }
    *o = 0;
}

/* ---- keys, duplicates, several sources ---- */
unsigned mm_question_key(const char *text)                  /* FNV-1a over the lower-cased letters and digits */
{
    unsigned h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
        unsigned c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) { h ^= c; h *= 16777619u; }
    }
    return h ? h : 1;
}

static unsigned *g_keys;                                    /* open addressing set of loaded keys */
static int g_keycap, g_nkeys;
static unsigned g_gen = 1;
static int g_srccount[32];

static int key_has(unsigned k)
{
    if (!g_keycap) return 0;
    for (unsigned i = k & (g_keycap - 1);; i = (i + 1) & (g_keycap - 1)) {
        if (!g_keys[i]) return 0;
        if (g_keys[i] == k) return 1;
    }
}

static void key_put(unsigned k)
{
    if ((g_nkeys + 1) * 2 > g_keycap) {
        int old = g_keycap;
        unsigned *ok = g_keys;
        g_keycap = g_keycap ? g_keycap * 2 : 4096;
        g_keys = calloc(g_keycap, sizeof *g_keys);
        g_nkeys = 0;
        for (int i = 0; i < old; i++) if (ok[i]) key_put(ok[i]);
        free(ok);
    }
    for (unsigned i = k & (g_keycap - 1);; i = (i + 1) & (g_keycap - 1)) {
        if (g_keys[i] == k) return;
        if (!g_keys[i]) { g_keys[i] = k; g_nkeys++; return; }
    }
}

static void free_question(MMQuestion *q)
{
    free(q->text);
    for (int k = 0; k < 4; k++) free(q->answer[k]);
}

/* decode, key and de-duplicate the questions loaded from source `src` (from index `from` on). Only questions
 * already loaded from an earlier source are dropped: a source keeps its own repeats (the original bank has 40
 * questions whose text appears twice, with different answers or areas). */
static void finish_source(int from, int src)
{
    int o = from;
    for (int i = from; i < g_nq; i++) {
        MMQuestion *q = &g_q[i];
        decode_entities(q->text);
        for (int k = 0; k < 4; k++) decode_entities(q->answer[k]);
        q->key = mm_question_key(q->text);
        q->src = src;
        if (key_has(q->key)) { free_question(q); continue; }
        g_q[o++] = *q;
    }
    g_nq = o;
    for (int i = from; i < g_nq; i++) key_put(g_q[i].key);
    if (src >= 0 && src < 32) g_srccount[src] = g_nq - from;
}

int mm_questions_load(const char *spec, char *err, int errlen)
{
    for (int i = 0; i < g_nq; i++) free_question(&g_q[i]);   /* replace any previously loaded sources */
    g_nq = 0;
    free(g_keys);
    g_keys = NULL;
    g_keycap = g_nkeys = 0;
    memset(g_srccount, 0, sizeof g_srccount);
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", spec);
    int src = 0, ok = 1;
    err[0] = 0;
    for (char *e = strtok(buf, "|"); e; e = strtok(NULL, "|"), src++) {
        int from = g_nq, r;
        char sub[256] = "";
        if (!strncmp(e, "json:", 5)) r = load_json(e + 5, 0, sub, sizeof sub);
        else if (!strncmp(e, "user:", 5)) r = load_json(e + 5, 1, sub, sizeof sub);
        else r = load_original(!strncmp(e, "original:", 9) ? e + 9 : "", sub, sizeof sub);
        if (!r) { ok = 0; if (!err[0]) snprintf(err, errlen, "%s", sub); }
        finish_source(from, src);
    }
    g_gen++;
    if (!g_nq) {
        if (!err[0] || ok) snprintf(err, errlen, "No questions: every question source is empty or switched off.");
        return 0;
    }
    err[0] = 0;                                              /* a missing pack is not fatal when others loaded */
    build_lists();
    return 1;
}

int mm_questions_count_src(int src) { return src >= 0 && src < 32 ? g_srccount[src] : 0; }
unsigned mm_questions_generation(void) { return g_gen; }

int mm_questions_add(const char *text, const char *const answers[4], int area, int level, int src)
{
    if (!text || area < 0 || area >= AREAS || level < 0 || level >= LEVELS) return 0;
    int from = g_nq;
    MMQuestion *q = new_question();
    q->id = 900000 + g_nq;
    q->area = area;
    q->level = level;
    q->text = mm_strdup(text);
    for (int k = 0; k < 4; k++) q->answer[k] = mm_strdup(answers[k]);
    int before = src >= 0 && src < 32 ? g_srccount[src] : 0;
    finish_source(from, src);
    if (src >= 0 && src < 32) g_srccount[src] = before + (g_nq - from);
    if (g_nq == from) return 0;                              /* duplicate */
    /* appended at the end: the per (area, level) lists only grow, so indices held by the picker stay valid */
    int *l = realloc(g_list[area][level], sizeof(int) * (g_count[area][level] + 1));
    g_list[area][level] = l;
    l[g_count[area][level]++] = g_nq - 1;
    g_gen++;
    return 1;
}

int mm_question_count(int area, int level)
{
    if (area < 0 || area >= AREAS || level < 0 || level >= LEVELS) return 0;
    return g_count[area][level];
}

const MMQuestion *mm_question_at(int area, int level, int index)
{
    if (index < 0 || index >= mm_question_count(area, level)) return NULL;
    return &g_q[g_list[area][level][index]];
}

const MMQuestion *mm_question_random_in_area(int area)
{
    int total = 0;
    for (int l = 0; l < LEVELS; l++) total += mm_question_count(area, l);
    if (!total) return NULL;
    int r = mm_rand() % total;
    for (int l = 0; l < LEVELS; l++) {
        if (r < g_count[area][l]) return mm_question_at(area, l, r);
        r -= g_count[area][l];
    }
    return NULL;
}
