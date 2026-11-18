/* Question sources (CHANGE, not in the original): which question banks are loaded, the questions each player has
 * already seen, the online top-up when a category runs low, and the player's own questions.
 *
 * Sources (switched in the settings screen, setting "sources" = comma list of ids):
 *   original   the 8,020 Encarta questions from the ISO (MINDMAZE.IDX/DB)
 *   pack:<id>  question packs in data/packs (index.txt: file, id, name, licence, count; tools/packs/)
 *   custom     the player's own questions, custom_questions.json next to the exe (settings: Add a question)
 *   online     online_questions.json next to the exe: questions fetched from Open Trivia DB / The Trivia API
 *              when fewer than ONLINE_LOW unseen questions are left for an area and level (data/packs/
 *              online_map.json maps areas to the APIs' categories and levels to difficulties)
 *   file       an older "questions=json:<file>" setting
 * Seen questions: seen.txt next to the exe, one line "player<TAB>key" per question shown (setting skip_seen).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#include "mm_tables.h"

#define MAXSRC 24
#define ONLINE_LOW 10                 /* fewer unseen questions than this in an area/level: fetch more */
#define ONLINE_BATCH 30
#define ONLINE_GAP_MS 6000            /* Open Trivia DB allows one request per 5 s */
#define ONLINE_RETRY_MS 600000        /* an area/level whose last fetch added nothing waits 10 minutes */
#define CUSTOM_FILE "custom_questions.json"
#define ONLINE_FILE "online_questions.json"
#define SEEN_FILE "seen.txt"

static const char *AREA_NAMES[9] = {"Physical Science & Technology", "Life Science", "Geography", "History",
                                    "Social Science", "Religion and Philosophy", "Art, Language, and Literature",
                                    "Performing Arts", "Sports, Hobbies, and Pets"};

typedef struct {
    char id[32], name[96], spec[192], licence[96];
    int kind, enabled, staged, hint, loaded_src;   /* loaded_src: entry index in the last load spec, -1 = off */
} Source;
static Source g_src[MAXSRC];
static int g_nsrc;
int mm_skip_seen = 1;

/* strtok_r replacement (not in every Windows C library) */
static char *tok(char *s, const char *delim, char **save)
{
    char *p = s ? s : *save;
    if (!p) return NULL;
    p += strspn(p, delim);
    if (!*p) { *save = NULL; return NULL; }
    char *e = p + strcspn(p, delim);
    if (*e) *e++ = 0;
    *save = e;
    return p;
}

/* ------------------------------------------------------------------------------------------------ */
/* tiny JSON reader (online responses, custom / online files)                                       */
typedef struct JV { int type; char *s; double n; struct JV *v; char **k; int nv; } JV;   /* type: 0 null 1 num 2 str 3 arr 4 obj 5 bool */
typedef struct { const char *p, *e; } JP;

static void jp_ws(JP *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }

static char *jp_str(JP *j)
{
    if (j->p >= j->e || *j->p != '"') return NULL;
    j->p++;
    const char *s = j->p;
    size_t n = 0;
    while (j->p < j->e && *j->p != '"') { if (*j->p == '\\') j->p++; j->p++; n++; }
    char *out = malloc(n * 4 + 1), *o = out;
    for (const char *p = s; p < j->p; p++) {
        if (*p != '\\') { *o++ = *p; continue; }
        p++;
        if (*p == 'n') *o++ = '\n';
        else if (*p == 't') *o++ = '\t';
        else if (*p == 'u' && p + 4 < j->e) {
            unsigned u = (unsigned)strtoul((char[5]){p[1], p[2], p[3], p[4], 0}, NULL, 16);
            p += 4;
            if (u >= 0xD800 && u < 0xDC00 && p + 6 < j->e && p[1] == '\\' && p[2] == 'u') {
                unsigned lo = (unsigned)strtoul((char[5]){p[3], p[4], p[5], p[6], 0}, NULL, 16);
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            }
            if (u < 0x80) *o++ = (char)u;
            else if (u < 0x800) { *o++ = (char)(0xC0 | u >> 6); *o++ = (char)(0x80 | (u & 63)); }
            else if (u < 0x10000) { *o++ = (char)(0xE0 | u >> 12); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
            else { *o++ = (char)(0xF0 | u >> 18); *o++ = (char)(0x80 | ((u >> 12) & 63)); *o++ = (char)(0x80 | ((u >> 6) & 63)); *o++ = (char)(0x80 | (u & 63)); }
        } else *o++ = *p;
    }
    *o = 0;
    if (j->p < j->e) j->p++;
    return out;
}

static int jp_val(JP *j, JV *v, int depth)
{
    memset(v, 0, sizeof *v);
    jp_ws(j);
    if (j->p >= j->e || depth > 32) return 0;
    char c = *j->p;
    if (c == '"') { v->type = 2; v->s = jp_str(j); return v->s != NULL; }
    if (c == '{' || c == '[') {
        v->type = c == '{' ? 4 : 3;
        j->p++;
        for (;;) {
            jp_ws(j);
            if (j->p < j->e && (*j->p == '}' || *j->p == ']')) { j->p++; return 1; }
            char *key = NULL;
            if (v->type == 4) {
                key = jp_str(j);
                if (!key) return 0;
                jp_ws(j);
                if (j->p < j->e && *j->p == ':') j->p++;
            }
            v->v = realloc(v->v, sizeof(JV) * (v->nv + 1));
            v->k = realloc(v->k, sizeof(char *) * (v->nv + 1));
            v->k[v->nv] = key;
            if (!jp_val(j, &v->v[v->nv], depth + 1)) { v->nv++; return 0; }
            v->nv++;
            jp_ws(j);
            if (j->p < j->e && *j->p == ',') j->p++;
        }
    }
    if (c == 't' || c == 'f') { v->type = 5; v->n = c == 't'; while (j->p < j->e && *j->p >= 'a' && *j->p <= 'z') j->p++; return 1; }
    if (c == 'n') { j->p += 4; return 1; }
    v->type = 1;
    v->n = strtod(j->p, (char **)&j->p);
    return 1;
}

static void jv_free(JV *v)
{
    free(v->s);
    for (int i = 0; i < v->nv; i++) { jv_free(&v->v[i]); free(v->k[i]); }
    free(v->v);
    free(v->k);
}

static JV *jv_get(JV *o, const char *key)
{
    if (!o || o->type != 4) return NULL;
    for (int i = 0; i < o->nv; i++) if (o->k[i] && !strcmp(o->k[i], key)) return &o->v[i];
    return NULL;
}

static const char *jv_str(JV *o, const char *key) { JV *v = jv_get(o, key); return v && v->type == 2 ? v->s : NULL; }

static int jv_parse(const char *data, int len, JV *out) { JP j = {data, data + len}; return jp_val(&j, out, 0); }

/* ------------------------------------------------------------------------------------------------ */
/* question lists kept as files (custom, online)                                                    */
typedef struct { char *text, *ans[4]; int area, level; } QItem;
typedef struct { QItem *it; int n, cap; const char *file, *id, *name; } QFile;
static QFile g_custom = {NULL, 0, 0, CUSTOM_FILE, "custom", "My questions"};
static QFile g_online = {NULL, 0, 0, ONLINE_FILE, "online", "Online questions"};

static void qfile_push(QFile *f, const char *text, const char *const ans[4], int area, int level)
{
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 64; f->it = realloc(f->it, sizeof(QItem) * f->cap); }
    QItem *q = &f->it[f->n++];
    q->text = mm_strdup(text);
    for (int k = 0; k < 4; k++) q->ans[k] = mm_strdup(ans[k]);
    q->area = area;
    q->level = level;
}

static void qfile_load(QFile *f)
{
    int size = 0;
    char *d = plat_load_user(f->file, &size);
    if (!d) return;
    JV root;
    if (jv_parse(d, size, &root)) {
        JV *qs = jv_get(&root, "questions");
        for (int i = 0; qs && qs->type == 3 && i < qs->nv; i++) {
            JV *q = &qs->v[i], *ans = jv_get(q, "answers"), *a = jv_get(q, "area"), *l = jv_get(q, "level");
            const char *t = jv_str(q, "question");
            if (!t || !ans || ans->type != 3 || ans->nv != 4 || !a || !l) continue;
            const char *as[4];
            int ok = 1;
            for (int k = 0; k < 4; k++) if (!(as[k] = jv_str(&ans->v[k], "text"))) ok = 0;
            if (ok) qfile_push(f, t, as, (int)a->n, (int)l->n);
        }
    }
    jv_free(&root);
    free(d);
}

static void put_json_str(char **o, size_t *n, size_t *cap, const char *s)
{
    size_t need = strlen(s) * 6 + 8;
    if (*n + need >= *cap) { *cap = (*cap + need) * 2; *o = realloc(*o, *cap); }
    char *w = *o + *n;
    *w++ = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { *w++ = '\\'; *w++ = (char)*p; }
        else if (*p == '\n') { *w++ = '\\'; *w++ = 'n'; }
        else if (*p < 32) w += sprintf(w, "\\u%04x", *p);
        else *w++ = (char)*p;
    }
    *w++ = '"';
    *n = (size_t)(w - *o);
}

static void put_raw(char **o, size_t *n, size_t *cap, const char *s)
{
    size_t l = strlen(s);
    if (*n + l + 1 >= *cap) { *cap = (*cap + l + 1) * 2; *o = realloc(*o, *cap); }
    memcpy(*o + *n, s, l);
    *n += l;
}

static int qfile_save(QFile *f)
{
    char *o = NULL, num[64];
    size_t n = 0, cap = 0;
    put_raw(&o, &n, &cap, "{\n \"format\": \"mindmaze-questions-1\",\n \"pack\": {\"id\": ");
    put_json_str(&o, &n, &cap, f->id);
    put_raw(&o, &n, &cap, ", \"name\": ");
    put_json_str(&o, &n, &cap, f->name);
    put_raw(&o, &n, &cap, "},\n \"note\": \"answers[0] is the correct answer; area 0-8 (see areas), level 0-3\",\n \"areas\": [");
    for (int a = 0; a < 9; a++) { if (a) put_raw(&o, &n, &cap, ", "); put_json_str(&o, &n, &cap, AREA_NAMES[a]); }
    put_raw(&o, &n, &cap, "],\n \"questions\": [");
    for (int i = 0; i < f->n; i++) {
        QItem *q = &f->it[i];
        snprintf(num, sizeof num, "%s\n  {\"id\": %d, \"area\": %d, \"level\": %d, \"question\": ", i ? "," : "", i + 1, q->area, q->level);
        put_raw(&o, &n, &cap, num);
        put_json_str(&o, &n, &cap, q->text);
        put_raw(&o, &n, &cap, ", \"answers\": [");
        for (int k = 0; k < 4; k++) {
            put_raw(&o, &n, &cap, k ? ", {\"text\": " : "{\"text\": ");
            put_json_str(&o, &n, &cap, q->ans[k]);
            put_raw(&o, &n, &cap, "}");
        }
        put_raw(&o, &n, &cap, "]}");
    }
    put_raw(&o, &n, &cap, "\n ]\n}\n");
    int ok = plat_save_user(f->file, o, (int)n);
    free(o);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ */
/* registry and loading                                                                             */
static Source *add_source(const char *id, const char *name, const char *spec, const char *licence, int kind, int hint)
{
    if (g_nsrc >= MAXSRC) return NULL;
    Source *s = &g_src[g_nsrc++];
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", id);
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->spec, sizeof s->spec, "%s", spec);
    snprintf(s->licence, sizeof s->licence, "%s", licence);
    s->kind = kind;
    s->hint = hint;
    s->loaded_src = -1;
    return s;
}

/* questions announced by a MINDMAZE.IDX (36 counts at the start); 0 if missing */
static int idx_count(const char *rel)
{
    int size = 0, n = 0;
    unsigned char *d = plat_load(rel, &size);
    for (int i = 0; d && i < 36 && 4 * i + 4 <= size; i++) n += d[4 * i] | d[4 * i + 1] << 8 | d[4 * i + 2] << 16 | d[4 * i + 3] << 24;
    free(d);
    return n;
}

static int find_src(int kind)
{
    for (int i = 0; i < g_nsrc; i++) if (g_src[i].kind == kind) return i;
    return -1;
}

static int load_enabled(char *err, int errlen)
{
    char spec[2048] = "";
    int n = 0;
    for (int i = 0; i < g_nsrc; i++) {
        g_src[i].loaded_src = -1;
        if (!g_src[i].enabled) continue;
        if (spec[0]) strncat(spec, "|", sizeof spec - strlen(spec) - 1);
        strncat(spec, g_src[i].spec, sizeof spec - strlen(spec) - 1);
        g_src[i].loaded_src = n++;
    }
    return mm_questions_load(spec, err, errlen);
}

static void save_enabled(void)
{
    char v[512] = "";
    for (int i = 0; i < g_nsrc; i++)
        if (g_src[i].enabled) {
            if (v[0]) strncat(v, ",", sizeof v - strlen(v) - 1);
            strncat(v, g_src[i].id, sizeof v - strlen(v) - 1);
        }
    plat_set_setting("sources", v[0] ? v : "none");
}

static void seen_load(void);
static void online_map_load(void);

int mm_sources_init(char *err, int errlen)
{
    char v[512];
    g_nsrc = 0;
    /* the questions of the disc the data folder came from (CHANGE 2026-11-18: any edition, see mm_tables_edition) */
    char oname[128], olic[96];
    snprintf(oname, sizeof oname, "Original MindMaze questions (%s)", mm_tables_edition);
    snprintf(olic, sizeof olic, "Microsoft %s", mm_tables_edition);
    add_source("original", oname, "original", olic, SRC_ORIGINAL, idx_count("mindmaze.idx"));
    int size = 0;
    /* other Encarta editions' questions (CHANGE, 2026-11-18): data/editions/index.txt "id<TAB>name<TAB>count", written
     * when the player adds an older disc (Settings); each edition's files are in data/editions/<id>/ */
    char *eds = plat_load("editions/index.txt", &size);
    if (eds) {
        char *save = NULL;
        for (char *line = tok(eds, "\r\n", &save); line; line = tok(NULL, "\r\n", &save)) {
            char *f[3] = {line, NULL, NULL};
            for (int k = 1; k < 3 && f[k - 1]; k++) { char *t = strchr(f[k - 1], '\t'); if (t) { *t = 0; f[k] = t + 1; } }
            if (!f[1] || !f[2]) continue;
            char id[64], spec[160], name[128];
            snprintf(id, sizeof id, "edition:%s", f[0]);
            snprintf(spec, sizeof spec, "original:editions/%s/", f[0]);
            snprintf(name, sizeof name, "%s questions", f[1]);
            char lic[96];
            snprintf(lic, sizeof lic, "Microsoft %s", f[1]);
            add_source(id, name, spec, lic, SRC_ORIGINAL, atoi(f[2]));
        }
        free(eds);
    }
    size = 0;                                                /* packs: data/packs/index.txt, written by extract.py */
    char *idx = plat_load("packs/index.txt", &size);
    if (idx) {
        char *save = NULL;
        for (char *line = tok(idx, "\r\n", &save); line; line = tok(NULL, "\r\n", &save)) {
            char *f[5] = {0};
            int nf = 0;
            for (char *p = line; nf < 5; nf++) { f[nf] = p; p = strchr(p, '\t'); if (!p) { nf++; break; } *p++ = 0; }
            if (nf < 3) continue;
            char id[40], spec[192];
            snprintf(id, sizeof id, "pack:%s", f[1]);
            snprintf(spec, sizeof spec, "json:packs/%s", f[0]);
            add_source(id, f[2], spec, nf > 3 ? f[3] : "", SRC_PACK, nf > 4 ? atoi(f[4]) : 0);
        }
        free(idx);
    }
    add_source("custom", "My questions", "user:" CUSTOM_FILE, "custom_questions.json next to MindMaze.exe", SRC_CUSTOM, 0);
    add_source("online", "Online top-up", "user:" ONLINE_FILE,
               "Open Trivia DB / The Trivia API: new questions when a category runs low", SRC_ONLINE, 0);
    /* enabled sources; an older "questions=json:<file>" setting becomes its own source */
    if (plat_get_setting("questions", v, sizeof v) && !strncmp(v, "json:", 5))
        add_source("file", v + 5, v, "set by questions= in mindmaze.ini", SRC_FILE, 0);
    if (plat_get_setting("sources", v, sizeof v)) {
        char *save = NULL;
        for (char *t = tok(v, ",", &save); t; t = tok(NULL, ",", &save))
            for (int i = 0; i < g_nsrc; i++) if (!strcmp(g_src[i].id, t)) g_src[i].enabled = 1;
    } else {
        int f = find_src(SRC_FILE);                          /* default: as before (original or the json file) */
        g_src[f >= 0 ? f : 0].enabled = 1;
        if (find_src(SRC_CUSTOM) >= 0) g_src[find_src(SRC_CUSTOM)].enabled = 1;
    }
    if (plat_get_setting("skip_seen", v, sizeof v)) mm_skip_seen = atoi(v) != 0;
    for (int i = 0; i < g_nsrc; i++) g_src[i].staged = g_src[i].enabled;
    qfile_load(&g_custom);
    qfile_load(&g_online);
    seen_load();
    online_map_load();
    if (load_enabled(err, errlen)) return 1;
    /* the saved selection loads nothing (a pack removed, everything off): fall back to the original questions */
    for (int i = 0; i < g_nsrc; i++) g_src[i].enabled = g_src[i].staged = i == 0;   /* the 2006 questions */
    if (!load_enabled(err, errlen)) return 0;
    save_enabled();
    return 1;
}

int mm_source_count(void) { return g_nsrc; }
const char *mm_source_name(int i) { return i >= 0 && i < g_nsrc ? g_src[i].name : ""; }
int mm_source_kind(int i) { return i >= 0 && i < g_nsrc ? g_src[i].kind : -1; }
int mm_source_enabled(int i) { return i >= 0 && i < g_nsrc && g_src[i].staged; }
void mm_source_set(int i, int on) { if (i >= 0 && i < g_nsrc) g_src[i].staged = on; }

const char *mm_source_info(int i)
{
    static char buf[256];
    if (i < 0 || i >= g_nsrc) return "";
    Source *s = &g_src[i];
    int n = s->loaded_src >= 0 ? mm_questions_count_src(s->loaded_src) : s->hint;
    if (s->kind == SRC_CUSTOM) n = g_custom.n;
    if (s->kind == SRC_ONLINE) n = g_online.n;
    char num[32], all[32];
    if (n >= 1000) snprintf(num, sizeof num, "%d,%03d", n / 1000, n % 1000);
    else snprintf(num, sizeof num, "%d", n);
    if (s->kind == SRC_ORIGINAL && s->loaded_src > 0 && s->hint > n) {   /* duplicates of a source above left out */
        if (s->hint >= 1000) snprintf(all, sizeof all, "%d,%03d", s->hint / 1000, s->hint % 1000);
        else snprintf(all, sizeof all, "%d", s->hint);
        snprintf(buf, sizeof buf, "%s new of %s (the rest repeat a source above)", num, all);
        return buf;
    }
    snprintf(buf, sizeof buf, "%s question%s%s%s", num, n == 1 ? "" : "s", s->licence[0] ? "  -  " : "", s->licence);
    return buf;
}

/* a disc's questions were added to data/editions/<id>/ (CHANGE 2026-11-18): note them in editions/index.txt, list
 * them right after the other Encarta sources and switch them on. Questions already in a source above are left out
 * when loading (finish_source in mm_questions.c). Returns the source's index, -1 + message on failure. */
int mm_sources_add_edition(const char *id, const char *name, int count, char *err, int errlen)
{
    int size = 0;
    char *old = plat_load("editions/index.txt", &size), line[256];
    size_t cap = (size_t)size + 300, n = 0;
    char *out = malloc(cap);
    if (!out) { free(old); snprintf(err, errlen, "out of memory"); return -1; }
    char *save = NULL;
    for (char *l = old ? tok(old, "\r\n", &save) : NULL; l; l = tok(NULL, "\r\n", &save)) {
        size_t k = strlen(id);
        if (!strncmp(l, id, k) && l[k] == '\t') continue;             /* replaced below */
        n += (size_t)snprintf(out + n, cap - n, "%s\n", l);
    }
    free(old);
    snprintf(line, sizeof line, "%s\t%s\t%d\n", id, name, count);
    n += (size_t)snprintf(out + n, cap - n, "%s", line);
    int ok = plat_save_data("editions/index.txt", out, (int)n);
    free(out);
    if (!ok) { snprintf(err, errlen, "Could not write editions/index.txt in the data folder."); return -1; }

    char sid[64], spec[160], sname[128];
    snprintf(sid, sizeof sid, "edition:%s", id);
    snprintf(spec, sizeof spec, "original:editions/%s/", id);
    snprintf(sname, sizeof sname, "%s questions", name);
    int at = -1;
    for (int i = 0; i < g_nsrc; i++) if (!strcmp(g_src[i].id, sid)) at = i;
    if (at < 0) {
        if (g_nsrc >= MAXSRC) { snprintf(err, errlen, "Too many question sources."); return -1; }
        at = 0;
        for (int i = 0; i < g_nsrc; i++) if (g_src[i].kind == SRC_ORIGINAL) at = i + 1;   /* after the Encarta sources */
        memmove(&g_src[at + 1], &g_src[at], (size_t)(g_nsrc - at) * sizeof g_src[0]);
        g_nsrc++;
        memset(&g_src[at], 0, sizeof g_src[at]);
        snprintf(g_src[at].id, sizeof g_src[at].id, "%s", sid);
        g_src[at].kind = SRC_ORIGINAL;
        g_src[at].loaded_src = -1;
    }
    snprintf(g_src[at].name, sizeof g_src[at].name, "%s", sname);
    snprintf(g_src[at].spec, sizeof g_src[at].spec, "%s", spec);
    snprintf(g_src[at].licence, sizeof g_src[at].licence, "Microsoft %s", name);
    g_src[at].hint = count;
    g_src[at].staged = 1;
    return mm_sources_apply(err, errlen) ? at : -1;
}

int mm_sources_apply(char *err, int errlen)
{
    int any = 0;
    for (int i = 0; i < g_nsrc; i++) any |= g_src[i].staged;
    if (!any) { snprintf(err, errlen, "Switch on at least one question source."); return 0; }
    int was[MAXSRC];
    for (int i = 0; i < g_nsrc; i++) { was[i] = g_src[i].enabled; g_src[i].enabled = g_src[i].staged; }
    if (!load_enabled(err, errlen)) {                        /* keep the previous selection */
        char e2[256];
        for (int i = 0; i < g_nsrc; i++) g_src[i].enabled = g_src[i].staged = was[i];
        load_enabled(e2, sizeof e2);
        return 0;
    }
    save_enabled();
    return 1;
}

void mm_set_skip_seen(int on) { mm_skip_seen = on; plat_set_setting("skip_seen", on ? "1" : "0"); }

/* ------------------------------------------------------------------------------------------------ */
/* seen questions: set of (player, key)                                                             */
typedef unsigned long long u64;
static u64 *g_seen;
static int g_seencap, g_nseen;

static u64 seen_id(const char *player, unsigned key) { return (u64)mm_question_key(player) << 32 | key; }

static int seen_has(u64 id)
{
    if (!g_seencap) return 0;
    for (u64 i = (id * 0x9E3779B97F4A7C15ull) >> 40 & (g_seencap - 1);; i = (i + 1) & (g_seencap - 1)) {
        if (!g_seen[i]) return 0;
        if (g_seen[i] == id) return 1;
    }
}

static int seen_put(u64 id)
{
    if ((g_nseen + 1) * 2 > g_seencap) {
        int old = g_seencap;
        u64 *o = g_seen;
        g_seencap = g_seencap ? g_seencap * 2 : 1024;
        g_seen = calloc(g_seencap, sizeof *g_seen);
        g_nseen = 0;
        for (int i = 0; i < old; i++) if (o[i]) seen_put(o[i]);
        free(o);
    }
    for (u64 i = (id * 0x9E3779B97F4A7C15ull) >> 40 & (g_seencap - 1);; i = (i + 1) & (g_seencap - 1)) {
        if (g_seen[i] == id) return 0;
        if (!g_seen[i]) { g_seen[i] = id; g_nseen++; return 1; }
    }
}

static void seen_load(void)
{
    int size = 0;
    char *d = plat_load_user(SEEN_FILE, &size);
    if (!d) return;
    char *save = NULL;
    for (char *line = tok(d, "\r\n", &save); line; line = tok(NULL, "\r\n", &save)) {
        char *tab = strrchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        seen_put(seen_id(line, (unsigned)strtoul(tab + 1, NULL, 16)));
    }
    free(d);
}

int mm_seen(const char *player, unsigned key) { return player && seen_has(seen_id(player, key)); }

void mm_mark_seen(const char *player, unsigned key)
{
    if (!player || !seen_put(seen_id(player, key))) return;
    char line[160];
    snprintf(line, sizeof line, "%s\t%08x\n", player, key);
    plat_append_user(SEEN_FILE, line);
}

void mm_seen_forget_all(void)
{
    free(g_seen);
    g_seen = NULL;
    g_seencap = g_nseen = 0;
    plat_save_user(SEEN_FILE, "", 0);
}

int mm_seen_total(void) { return g_nseen; }

/* forget one player's seen marks for the given questions (a list ran out: start a fresh round of it), except the
 * newest quarter of them: those were seen last and come back only at the end of the new round. The seen file is
 * in the order the questions were seen; it keeps the other lines and the set is rebuilt from it. */
void mm_seen_forget(const char *player, const unsigned *keys, int n)
{
    if (!player || n <= 0) return;
    u64 *drop = malloc(sizeof *drop * (size_t)n);
    if (!drop) return;
    for (int i = 0; i < n; i++) drop[i] = seen_id(player, keys[i]);
    int size = 0;
    char *d = plat_load_user(SEEN_FILE, &size);
    int matches = 0;                                         /* first pass: how many marks of these questions */
    if (d) {
        const char *p = d;
        while (*p) {
            const char *e = strchr(p, '\n'), *tab = NULL;
            size_t len = e ? (size_t)(e - p) : strlen(p);
            for (const char *q = p; q < p + len; q++) if (*q == '\t') tab = q;
            if (tab && (size_t)(tab - p) < 128) {
                char name[128];
                memcpy(name, p, (size_t)(tab - p));
                name[tab - p] = 0;
                u64 id = seen_id(name, (unsigned)strtoul(tab + 1, NULL, 16));
                for (int i = 0; i < n; i++) if (drop[i] == id) { matches++; break; }
            }
            p += len + (e ? 1 : 0);
        }
    }
    int forget = matches - matches / 4;                      /* the oldest three quarters */
    free(g_seen);
    g_seen = NULL;
    g_seencap = g_nseen = 0;
    char *out = d ? malloc((size_t)size + 1) : NULL;
    size_t o = 0;
    if (d && out) {
        char *save = NULL;
        for (char *line = tok(d, "\r\n", &save); line; line = tok(NULL, "\r\n", &save)) {
            char *tab = strrchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            unsigned key = (unsigned)strtoul(tab + 1, NULL, 16);
            u64 id = seen_id(line, key);
            int gone = 0;
            for (int i = 0; i < n && !gone; i++) gone = drop[i] == id;
            if (gone && forget > 0) { forget--; continue; }
            seen_put(id);
            o += (size_t)snprintf(out + o, (size_t)size + 1 - o, "%s\t%08x\n", line, key);
        }
        plat_save_user(SEEN_FILE, out, (int)o);
    }
    free(out);
    free(d);
    free(drop);
}

/* ------------------------------------------------------------------------------------------------ */
/* custom questions                                                                                 */
int mm_custom_count(void) { return g_custom.n; }

static int count_chars(const char *s) { int n = 0; for (; *s; s++) n += (*s & 0xC0) != 0x80; return n; }

int mm_custom_add(const char *text, const char *const answers[4], int area, int level, char *err, int errlen)
{
    if (!text[0]) { snprintf(err, errlen, "Type the question."); return 0; }
    if (count_chars(text) > 300) { snprintf(err, errlen, "The question is too long (300 characters at most)."); return 0; }
    for (int k = 0; k < 4; k++) {
        if (!answers[k][0]) { snprintf(err, errlen, k ? "Type all three wrong answers." : "Type the correct answer."); return 0; }
        if (count_chars(answers[k]) > 40) { snprintf(err, errlen, "Answer %d is too long (40 characters at most).", k + 1); return 0; }
        for (int m = 0; m < k; m++)
            if (!strcmp(answers[k], answers[m])) { snprintf(err, errlen, "The four answers must all be different."); return 0; }
    }
    unsigned key = mm_question_key(text);
    for (int i = 0; i < g_custom.n; i++)
        if (mm_question_key(g_custom.it[i].text) == key) { snprintf(err, errlen, "You already have this question."); return 0; }
    qfile_push(&g_custom, text, answers, area, level);
    if (!qfile_save(&g_custom)) {
        g_custom.n--;
        snprintf(err, errlen, "Could not save " CUSTOM_FILE " next to MindMaze.exe.");
        return 0;
    }
    int s = find_src(SRC_CUSTOM);
    if (s >= 0 && g_src[s].enabled && g_src[s].loaded_src >= 0) mm_questions_add(text, answers, area, level, g_src[s].loaded_src);
    return 1;
}

/* ------------------------------------------------------------------------------------------------ */
/* online top-up                                                                                    */
typedef struct { char cat[9][12][40]; int ncat[9]; char diff[4][12]; int ok; } ApiMap;
static ApiMap g_api[2];               /* 0 = Open Trivia DB, 1 = The Trivia API */
static struct { int pending; uint32_t next_try; int tries; } g_cell[9][4];
static uint32_t g_last_req;
static int g_pending, g_rng = 12345;
static int rnd(int n) { g_rng = g_rng * 1103515245 + 12345; return (int)((unsigned)g_rng >> 16) % (n > 0 ? n : 1); }

static void online_map_load(void)
{
    int size = 0;
    char *d = plat_load("packs/online_map.json", &size);
    if (!d) return;
    JV root;
    if (jv_parse(d, size, &root)) {
        const char *names[2] = {"opentdb", "triviaapi"};
        for (int a = 0; a < 2; a++) {
            JV *m = jv_get(&root, names[a]), *cats = jv_get(m, "categories_by_area"), *dif = jv_get(m, "difficulty_by_level");
            if (!cats || !dif) continue;
            for (int ar = 0; ar < 9; ar++) {
                char k[4];
                snprintf(k, sizeof k, "%d", ar);
                JV *l = jv_get(cats, k);
                for (int i = 0; l && l->type == 3 && i < l->nv && g_api[a].ncat[ar] < 12; i++) {
                    char *dst = g_api[a].cat[ar][g_api[a].ncat[ar]++];
                    if (l->v[i].type == 1) snprintf(dst, 40, "%d", (int)l->v[i].n);
                    else snprintf(dst, 40, "%s", l->v[i].s ? l->v[i].s : "");
                }
            }
            for (int lv = 0; lv < 4; lv++) {
                char k[4];
                snprintf(k, sizeof k, "%d", lv);
                const char *s = jv_str(dif, k);
                snprintf(g_api[a].diff[lv], sizeof g_api[a].diff[lv], "%s", s ? s : (lv == 0 ? "easy" : lv == 1 ? "medium" : "hard"));
            }
            g_api[a].ok = 1;
        }
    }
    jv_free(&root);
    free(d);
}

int mm_online_pending(void) { return g_pending; }

void mm_online_low(int area, int level)
{
    int s = find_src(SRC_ONLINE);
    if (s < 0 || !g_src[s].enabled || area < 0 || area > 8 || level < 0 || level > 3) return;
    uint32_t now = plat_ticks();
    if (g_cell[area][level].pending || (int32_t)(now - g_cell[area][level].next_try) < 0) return;
    if (g_pending || (g_last_req && now - g_last_req < ONLINE_GAP_MS)) return;
    int api = g_cell[area][level].tries & 1;                 /* alternate the two APIs per area/level */
    if (!g_api[api].ok || !g_api[api].ncat[area]) api ^= 1;
    if (!g_api[api].ok || !g_api[api].ncat[area]) return;
    const char *cat = g_api[api].cat[area][rnd(g_api[api].ncat[area])];
    char url[512];
    if (api == 0)
        snprintf(url, sizeof url, "https://opentdb.com/api.php?amount=%d&category=%s&difficulty=%s&type=multiple",
                 ONLINE_BATCH, cat, g_api[0].diff[level]);
    else
        snprintf(url, sizeof url, "https://the-trivia-api.com/v2/questions?limit=%d&categories=%s&difficulties=%s",
                 ONLINE_BATCH, cat, g_api[1].diff[level]);
    g_cell[area][level].pending = 1;
    g_cell[area][level].tries++;
    g_pending = 1;
    g_last_req = now;
    plat_http_get(url, api << 8 | area << 4 | level);
}

static int add_online(const char *q, const char *correct, JV *wrong, int area, int level)
{
    if (!q || !correct || !wrong || wrong->type != 3 || wrong->nv != 3) return 0;
    const char *a[4] = {correct, wrong->v[0].s, wrong->v[1].s, wrong->v[2].s};
    char dq[1024], da[4][256];                               /* entity-decoded copies for the length checks */
    snprintf(dq, sizeof dq, "%s", q);
    if (count_chars(dq) > 300) return 0;
    for (int k = 0; k < 4; k++) {
        if (!a[k] || !a[k][0] || count_chars(a[k]) > 48) return 0;
        snprintf(da[k], sizeof da[k], "%s", a[k]);
    }
    const char *const as[4] = {da[0], da[1], da[2], da[3]};
    int s = find_src(SRC_ONLINE);
    if (s < 0 || g_src[s].loaded_src < 0) return 0;
    if (!mm_questions_add(dq, as, area, level, g_src[s].loaded_src)) return 0;   /* duplicate of a loaded question */
    qfile_push(&g_online, dq, as, area, level);
    return 1;
}

void mm_http_done(int tag, const char *data, int len)
{
    int api = tag >> 8, area = (tag >> 4) & 15, level = tag & 15;
    g_pending = 0;
    if (area > 8 || level > 3) return;
    g_cell[area][level].pending = 0;
    int added = 0;
    JV root;
    if (data && jv_parse(data, len, &root)) {
        JV *list = api == 0 ? jv_get(&root, "results") : &root;
        for (int i = 0; list && list->type == 3 && i < list->nv; i++) {
            JV *it = &list->v[i];
            if (api == 0) added += add_online(jv_str(it, "question"), jv_str(it, "correct_answer"), jv_get(it, "incorrect_answers"), area, level);
            else added += add_online(jv_str(jv_get(it, "question"), "text"), jv_str(it, "correctAnswer"), jv_get(it, "incorrectAnswers"), area, level);
        }
        jv_free(&root);
    }
    if (added) qfile_save(&g_online);
    /* nothing new (offline, API error, or only duplicates): wait before asking for this area/level again */
    g_cell[area][level].next_try = plat_ticks() + (added ? ONLINE_GAP_MS : ONLINE_RETRY_MS);
    if (added) plat_invalidate();
}
