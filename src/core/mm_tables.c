/* Loader for data/tables.json (see mm_tables.h). A small JSON reader builds a tree, the tables are copied out
 * of it and checked (counts, ranges), then the tree is freed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm_tables.h"

int mm_group_count[MM_GROUPS], mm_group_start[MM_GROUPS];
MMObject *mm_objects;
MMRank mm_ranks[MM_N_RANKS];
const char **mm_speech[MM_N_SPEECH];
int mm_speech_count[MM_N_SPEECH];
const char *mm_picture_area[MM_N_PICTURE_AREAS];
const char *mm_music_files[MM_N_MUSIC];
const char *mm_sound_files[MM_N_SOUNDS];
char mm_tables_edition[64] = "Encarta 2006";

static struct { int id; char *text; } *g_strings;
static int g_nstrings, g_loaded;

const char *mm_str(int id)
{
    for (int i = 0; i < g_nstrings; i++)
        if (g_strings[i].id == id) return g_strings[i].text;
    return "";
}

/* ---- JSON tree ---- */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct JV {
    int type, n;                 /* n: items of an array / members of an object */
    double num;
    char *str;
    char **keys;                 /* object member names */
    struct JV **items;
} JV;
typedef struct { const char *p, *end; int depth, bad; } JP;

static void jfree(JV *v)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        if (v->keys) free(v->keys[i]);
        jfree(v->items[i]);
    }
    free(v->keys);
    free(v->items);
    free(v->str);
    free(v);
}

static void jws(JP *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }

static int hex4(JP *j, unsigned *u)
{
    if (j->end - j->p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)j->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    j->p += 4;
    *u = v;
    return 1;
}

static char *jstr(JP *j)                 /* at the opening quote; returns malloc'd UTF-8 or NULL */
{
    const char *q = j->p + 1;
    while (q < j->end && *q != '"') q += (*q == '\\' && q + 1 < j->end) ? 2 : 1;
    if (q >= j->end) return NULL;
    char *out = malloc((size_t)(q - j->p) + 1), *o = out;      /* the decoded text is never longer than the source */
    if (!out) return NULL;
    j->p++;
    while (j->p < q) {
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
            unsigned u, lo;
            if (!hex4(j, &u)) { free(out); return NULL; }
            if (u >= 0xD800 && u < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                j->p += 2;
                if (!hex4(j, &lo)) { free(out); return NULL; }
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
            if (u < 0x80) *o++ = (char)u;
            else if (u < 0x800) { *o++ = (char)(0xC0 | u >> 6); *o++ = (char)(0x80 | (u & 63)); }
            else if (u < 0x10000) { *o++ = (char)(0xE0 | u >> 12); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
            else { *o++ = (char)(0xF0 | u >> 18); *o++ = (char)(0x80 | ((u >> 12) & 63)); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
            break;
        }
        default: *o++ = c;               /* \" \\ \/ */
        }
    }
    *o = 0;
    j->p = q + 1;
    return out;
}

static JV *jvalue(JP *j);

static int jpush(JV *v, char *key, JV *item)
{
    if ((v->n & (v->n + 1)) == 0) {      /* grow at 0, 1, 3, 7, ... items */
        int cap = v->n * 2 + 1;
        JV **ni = realloc(v->items, sizeof *ni * (size_t)cap);
        if (!ni) return 0;
        v->items = ni;
        if (v->type == J_OBJ) {
            char **nk = realloc(v->keys, sizeof *nk * (size_t)cap);
            if (!nk) return 0;
            v->keys = nk;
        }
    }
    if (v->type == J_OBJ) v->keys[v->n] = key;
    v->items[v->n++] = item;
    return 1;
}

static JV *jcontainer(JP *j, int type)
{
    char close = type == J_OBJ ? '}' : ']';
    JV *v = calloc(1, sizeof *v);
    if (!v) { j->bad = 1; return NULL; }
    v->type = type;
    j->p++;
    if (++j->depth > 32) j->bad = 1;
    jws(j);
    if (j->p < j->end && *j->p == close) { j->p++; j->depth--; return v; }
    while (!j->bad) {
        char *key = NULL;
        jws(j);
        if (type == J_OBJ) {
            if (j->p >= j->end || *j->p != '"' || !(key = jstr(j))) { j->bad = 1; break; }
            jws(j);
            if (j->p >= j->end || *j->p != ':') { free(key); j->bad = 1; break; }
            j->p++;
        }
        JV *item = jvalue(j);
        if (!item || !jpush(v, key, item)) { free(key); jfree(item); j->bad = 1; break; }
        jws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == close) { j->p++; break; }
        j->bad = 1;
    }
    j->depth--;
    if (j->bad) { jfree(v); return NULL; }
    return v;
}

static JV *jvalue(JP *j)
{
    jws(j);
    if (j->p >= j->end) { j->bad = 1; return NULL; }
    char c = *j->p;
    if (c == '{') return jcontainer(j, J_OBJ);
    if (c == '[') return jcontainer(j, J_ARR);
    JV *v = calloc(1, sizeof *v);
    if (!v) { j->bad = 1; return NULL; }
    if (c == '"') {
        v->type = J_STR;
        if (!(v->str = jstr(j))) { free(v); j->bad = 1; return NULL; }
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[40];
        int n = 0;
        while (j->p < j->end && n < 39 && (*j->p == '-' || *j->p == '+' || *j->p == '.' || *j->p == 'e' || *j->p == 'E' ||
                                             (*j->p >= '0' && *j->p <= '9'))) buf[n++] = *j->p++;
        buf[n] = 0;
        v->type = J_NUM;
        v->num = atof(buf);
    } else if (j->end - j->p >= 4 && !strncmp(j->p, "true", 4)) { v->type = J_BOOL; v->num = 1; j->p += 4; }
    else if (j->end - j->p >= 5 && !strncmp(j->p, "false", 5)) { v->type = J_BOOL; j->p += 5; }
    else if (j->end - j->p >= 4 && !strncmp(j->p, "null", 4)) { j->p += 4; }
    else { free(v); j->bad = 1; return NULL; }
    return v;
}

static const JV *jget(const JV *o, const char *key)
{
    if (!o || o->type != J_OBJ) return NULL;
    for (int i = 0; i < o->n; i++) if (!strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}
static const JV *jarr(const JV *o, const char *key, int n)       /* array member; n >= 0 demands that length */
{
    const JV *a = jget(o, key);
    return a && a->type == J_ARR && (n < 0 || a->n == n) ? a : NULL;
}
static int jint(const JV *o, const char *key, int *out)
{
    const JV *v = jget(o, key);
    if (!v || v->type != J_NUM) return 0;
    *out = (int)v->num;
    return 1;
}
static char *jtake(const JV *v)                                   /* copy of a string value, NULL if not a string */
{
    return v && v->type == J_STR ? mm_strdup(v->str) : NULL;
}

/* ---- tables ---- */
static int names(const JV *root, const char *key, const char **out, int n)
{
    const JV *a = jarr(root, key, n);
    if (!a) return 0;
    for (int i = 0; i < n; i++) if (!(out[i] = jtake(a->items[i]))) return 0;
    return 1;
}

static const char *build(const JV *root)
{
    static const char *const group[MM_GROUPS] = {"doors", "floors", "ceilings", "walls", "characters", "furniture"};
    const JV *f = jget(root, "format");
    if (!f || f->type != J_STR || strcmp(f->str, "mindmaze-tables-1")) return "unknown format (expected mindmaze-tables-1)";
    /* "source": "Encarta 2006 Premium (US English)" / "Encarta 97" ... -> the short edition name (CHANGE 2026-11-18) */
    const JV *src = jget(root, "source");
    snprintf(mm_tables_edition, sizeof mm_tables_edition, "%s", src && src->type == J_STR ? src->str : "Encarta 2006");
    char *cut = strstr(mm_tables_edition, " Premium");
    if (!cut) cut = strstr(mm_tables_edition, " (US");
    if (cut) *cut = 0;

    const JV *g = jget(root, "groups");
    int total = 0;
    for (int i = 0; i < MM_GROUPS; i++) {
        if (!jint(g, group[i], &mm_group_count[i]) || mm_group_count[i] < 1 || mm_group_count[i] > 1000) return "bad groups";
        mm_group_start[i] = total;
        total += mm_group_count[i];
    }
    if (MM_N_DOORS != 5) return "there must be 5 doors (north, south, east, west, stairs)";
    if (MM_N_FURNITURE > MM_MAX_FURNITURE) return "too many furniture objects (64 at most)";
    if (MM_N_CHARACTERS >= 0x28) return "too many characters (39 at most)";

    const JV *objs = jarr(root, "objects", total);
    if (!objs) return "objects: missing or not as many as the groups say";
    if (total <= 0 || total > 6000) return "bad groups";
    mm_objects = calloc((size_t)total, sizeof *mm_objects);
    if (!mm_objects) return "out of memory";
    for (int i = 0; i < total; i++) {
        const JV *o = objs->items[i];
        MMObject *m = &mm_objects[i];
        const JV *rect = jarr(o, "rect", 4), *slots = jarr(o, "slots", 7);
        int flags, id;
        if (!(m->name = jtake(jget(o, "name"))) || !jint(o, "x", &m->x) || !jint(o, "y", &m->y) || !rect || !slots ||
            !jint(o, "half", &m->half) || !jint(o, "flags", &flags) || !jint(o, "id", &id)) return "objects: bad record";
        for (int k = 0; k < 4; k++) m->rect[k] = (int)rect->items[k]->num;
        for (int k = 0; k < 7; k++) m->slots[k] = (unsigned char)slots->items[k]->num;
        m->flags = (unsigned char)flags;
        m->id = (unsigned short)id;
    }

    const JV *ranks = jarr(root, "ranks", MM_N_RANKS);
    if (!ranks) return "ranks: 6 expected";
    for (int i = 0; i < MM_N_RANKS; i++) {
        const JV *r = ranks->items[i];
        if (!(mm_ranks[i].name = jtake(jget(r, "name"))) || !(mm_ranks[i].image = jtake(jget(r, "image"))) ||
            !jint(r, "min", &mm_ranks[i].min) || !jint(r, "max", &mm_ranks[i].max)) return "ranks: bad record";
    }

    const JV *speech = jarr(root, "speech", MM_N_SPEECH);
    if (!speech) return "speech: 23 lists expected";
    for (int k = 0; k < MM_N_SPEECH; k++) {
        const JV *list = speech->items[k];
        if (list->type != J_ARR) return "speech: bad list";
        mm_speech[k] = calloc((size_t)list->n + 1, sizeof *mm_speech[k]);
        if (!mm_speech[k]) return "out of memory";
        for (int i = 0; i < list->n; i++) if (!(mm_speech[k][i] = jtake(list->items[i]))) return "speech: bad text";
        mm_speech_count[k] = list->n;
    }

    if (!names(root, "picture_areas", mm_picture_area, MM_N_PICTURE_AREAS)) return "picture_areas: 9 names expected";
    if (!names(root, "music", mm_music_files, MM_N_MUSIC)) return "music: 5 file names expected";
    if (!names(root, "sounds", mm_sound_files, MM_N_SOUNDS)) return "sounds: 35 file names expected";

    const JV *s = jget(root, "strings");
    if (!s || s->type != J_OBJ || !s->n) return "strings: missing";
    g_strings = calloc((size_t)s->n, sizeof *g_strings);
    if (!g_strings) return "out of memory";
    for (int i = 0; i < s->n; i++) {
        g_strings[i].id = atoi(s->keys[i]);
        if (!(g_strings[i].text = jtake(s->items[i]))) return "strings: bad text";
        g_nstrings = i + 1;
    }
    return NULL;
}

int mm_tables_load(char *err, int errlen)
{
    if (g_loaded) return 1;
    int size = 0;
    char *data = plat_load("tables.json", &size);
    if (!data) {
        snprintf(err, errlen, "tables.json is missing from the data folder. Create the data folder from your own "
                              "Encarta 2006 disc with tools/extract/extract.py (see the README).");
        return 0;
    }
    JP j = {data, data + size, 0, 0};
    if (size >= 3 && !memcmp(data, "\xEF\xBB\xBF", 3)) j.p += 3;
    JV *root = jvalue(&j);
    const char *problem = root ? build(root) : "not valid JSON";
    jfree(root);
    free(data);
    if (problem) { snprintf(err, errlen, "tables.json: %s.", problem); return 0; }
    g_loaded = 1;
    return 1;
}
