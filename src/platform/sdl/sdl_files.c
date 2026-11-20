/* MindMaze - SDL3 files (Linux, macOS): the data folder, the player's files, settings, PNG pictures, the online
 * question GET, links, and "Export data for the phone app".
 *
 * Folders (sdl_setup.c picks them): home holds data/, mindmaze.ini and the player's files, like the folder next to
 * the Windows exe. Data file names are lower case (the extractor writes them so) while the core asks for some in
 * upper case ("X.DIB"): a name is tried as given, then in lower case, since these file systems are case-sensitive.
 * Settings: mindmaze.ini, "key=value" lines under [MindMaze], UTF-8, rewritten as a whole on each change. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdl.h"
#include "mm.h"
#include "music.h"
#include "zip_write.h"
#include "cli.h"
#include "../../third_party/stb_image.h"

char sdl_home[1024], sdl_data[1024], sdl_res[1024];

int sdl_exists(const char *path) { return SDL_GetPathInfo(path, NULL); }

void sdl_join(char *out, size_t n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    snprintf(out, n, l && dir[l - 1] != '/' ? "%s/%s" : "%s%s", dir, name);
}

void *sdl_slurp(const char *path, int *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[n] = 0;
    if (size) *size = (int)n;
    return d;
}

int sdl_write_file(const char *path, const void *data, size_t len)
{
    char dir[1200];
    SDL_strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; SDL_CreateDirectory(dir); }      /* creates the parent folders too */
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

/* data folder file: absolute paths as given; else as named, or with the name in lower case if that one exists */
static void data_path(const char *rel, char *out, size_t n)
{
    if (rel[0] == '/') { SDL_strlcpy(out, rel, n); return; }
    snprintf(out, n, "%s%s", sdl_data, rel);
    if (sdl_exists(out)) return;
    for (char *p = out + strlen(sdl_data); *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
}

void *plat_load(const char *rel, int *size)
{
    char p[1200];
    data_path(rel, p, sizeof p);
    return sdl_slurp(p, size);
}

int plat_save_data(const char *rel, const void *data, int len)
{
    char p[1200];
    snprintf(p, sizeof p, "%s%s", sdl_data, rel);
    return sdl_write_file(p, data, (size_t)len);
}

long plat_data_size(const char *rel)
{
    char p[1200];
    SDL_PathInfo info;
    data_path(rel, p, sizeof p);
    return SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_FILE ? (long)info.size : -1;
}

MMImage *plat_load_png(const char *rel)
{
    int size = 0, w, h, n;
    unsigned char *d = plat_load(rel, &size);
    if (!d) return NULL;
    unsigned char *px = stbi_load_from_memory(d, size, &w, &h, &n, 4);
    free(d);
    if (!px) return NULL;
    MMImage *im = malloc(sizeof *im);
    im->w = w;
    im->h = h;
    im->px = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++)
        im->px[i] = (uint32_t)px[4 * i + 3] << 24 | (uint32_t)px[4 * i] << 16 | (uint32_t)px[4 * i + 1] << 8 | px[4 * i + 2];
    stbi_image_free(px);
    return im;
}

/* ---- the player's files in home ---- */
void *plat_load_user(const char *name, int *size)
{
    char p[1200];
    snprintf(p, sizeof p, "%s%s", sdl_home, name);
    return sdl_slurp(p, size);
}

int plat_save_user(const char *name, const void *data, int len)   /* write a temp file, then replace */
{
    char p[1200], tmp[1210];
    snprintf(p, sizeof p, "%s%s", sdl_home, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    if (!sdl_write_file(tmp, data, (size_t)len)) { remove(tmp); return 0; }
    if (rename(tmp, p)) { remove(tmp); return 0; }
    return 1;
}

void plat_append_user(const char *name, const char *text)
{
    char p[1200];
    snprintf(p, sizeof p, "%s%s", sdl_home, name);
    FILE *f = fopen(p, "ab");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* ---- settings: mindmaze.ini ---- */
typedef struct { char *key, *value; } Setting;
static Setting *g_set;
static int g_nset;

static void settings_save(void)
{
    size_t cap = 64;
    for (int i = 0; i < g_nset; i++) cap += strlen(g_set[i].key) + strlen(g_set[i].value) + 2;
    char *buf = malloc(cap), *p = buf;
    if (!buf) return;
    p += snprintf(p, cap, "[MindMaze]\n");
    for (int i = 0; i < g_nset; i++) p += snprintf(p, cap - (size_t)(p - buf), "%s=%s\n", g_set[i].key, g_set[i].value);
    plat_save_user("mindmaze.ini", buf, (int)(p - buf));
    free(buf);
}

static void settings_put(const char *key, const char *value)
{
    for (int i = 0; i < g_nset; i++)
        if (!SDL_strcasecmp(g_set[i].key, key)) {             /* keys ignore case, like GetPrivateProfileString */
            free(g_set[i].value);
            if (value) { g_set[i].value = mm_strdup(value); return; }
            free(g_set[i].key);
            g_set[i] = g_set[--g_nset];
            return;
        }
    if (!value) return;
    Setting *ns = realloc(g_set, sizeof *ns * (size_t)(g_nset + 1));
    if (!ns) return;
    g_set = ns;
    g_set[g_nset].key = mm_strdup(key);
    g_set[g_nset].value = mm_strdup(value);
    g_nset++;
}

static void settings_load(void)
{
    char *d = plat_load_user("mindmaze.ini", NULL);
    for (char *line = d, *next; line && *line; line = next) {
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '[' || line[0] == ';' || !eq) continue;
        *eq = 0;
        settings_put(line, eq + 1);
    }
    free(d);
}

int plat_get_setting(const char *key, char *buf, int n)
{
    if (cli_setting(key, buf, n)) return 1;                   /* a command-line flag, this run only (../common/cli.h) */
    for (int i = 0; i < g_nset; i++)
        if (!SDL_strcasecmp(g_set[i].key, key)) { SDL_strlcpy(buf, g_set[i].value, (size_t)n); return 1; }
    return 0;
}

void plat_set_setting(const char *key, const char *value)
{
    char v[1024];
    if (value) {                                              /* one line per setting */
        SDL_strlcpy(v, value, sizeof v);
        for (char *p = v; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    }
    cli_setting_forget(key);                                 /* changed in the game: the saved value counts from now on */
    settings_put(key, value ? v : NULL);
    settings_save();
}

/* SoundFonts (*.sf2) in a folder -> the music's instrument list; "GeneralUser-GS.sf2" shows as "GeneralUser GS" */
static SDL_EnumerationResult SDLCALL add_soundfont(void *ud, const char *dir, const char *name)
{
    (void)ud;
    size_t n = strlen(name);
    if (n < 5 || SDL_strcasecmp(name + n - 4, ".sf2")) return SDL_ENUM_CONTINUE;
    char path[1200], label[64];
    sdl_join(path, sizeof path, dir, name);
    snprintf(label, sizeof label, "%.*s", (int)(n - 4), name);
    for (char *p = label; *p; p++) if (*p == '-' || *p == '_') *p = ' ';
    music_add_soundfont(path, label);
    return SDL_ENUM_CONTINUE;
}

void sdl_files_init(void)
{
    settings_load();
    music_set_data_dir(sdl_data);
    char dir[1100];
    snprintf(dir, sizeof dir, "%ssoundfonts/", sdl_res);         /* shipped with the game */
    SDL_EnumerateDirectory(dir, add_soundfont, NULL);
    snprintf(dir, sizeof dir, "%ssoundfonts/", sdl_home);        /* the player's own */
    if (strcmp(sdl_home, sdl_res)) SDL_EnumerateDirectory(dir, add_soundfont, NULL);
}

/* ---- links and folders ---- */
void plat_open_url(const char *url)
{
    if (!SDL_OpenURL(url)) SDL_Log("open %s: %s", url, SDL_GetError());
}

int plat_art_import(int go) { (void)go; return 0; }   /* the art is made on this computer: Settings opens the folder */

void plat_open_data_folder(const char *rel)
{
    char path[1200], url[3700], *u = url;
    snprintf(path, sizeof path, "%s%s", sdl_data, rel);
    SDL_CreateDirectory(path);
    u += snprintf(u, sizeof url, "file://");
    for (const unsigned char *p = (const unsigned char *)path; *p && u < url + sizeof url - 4; p++)   /* percent-encode */
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p)) *u++ = (char)*p;
        else u += snprintf(u, 4, "%%%02X", *p);
    *u = 0;
    plat_open_url(url);
}

/* ---- online questions: one GET per thread through the system's libcurl, loaded when first needed (so the game
 * starts without it; Linux: libcurl.so.4 or Debian's libcurl-gnutls, macOS: /usr/lib/libcurl.4.dylib). The option
 * numbers are libcurl's stable ABI values (curl.h), so no curl headers are needed to build. ---- */
typedef void CURL;
enum { CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_USERAGENT = 10018, CURLOPT_WRITEFUNCTION = 20011,
       CURLOPT_TIMEOUT = 13, CURLOPT_FOLLOWLOCATION = 52, CURLOPT_CONNECTTIMEOUT = 78, CURLOPT_NOSIGNAL = 99,
       CURLOPT_NOPROGRESS = 43, CURLOPT_XFERINFODATA = 10057, CURLOPT_XFERINFOFUNCTION = 20219,
       CURLINFO_RESPONSE_CODE = 0x200002 };
static struct {
    CURL *(*init)(void);
    int (*setopt)(CURL *, int, ...);
    int (*perform)(CURL *);
    int (*getinfo)(CURL *, int, ...);
    void (*cleanup)(CURL *);
} g_curl;
static int g_curl_state;                                         /* 0 not tried, 1 loaded, -1 missing */

static int curl_load(void)
{
    static const char *const LIBS[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", "/usr/lib/libcurl.4.dylib",
                                       "libcurl.4.dylib", NULL};
    if (g_curl_state) return g_curl_state > 0;
    g_curl_state = -1;
    for (int i = 0; LIBS[i]; i++) {
        SDL_SharedObject *so = SDL_LoadObject(LIBS[i]);
        if (!so) continue;
        g_curl.init = (CURL * (*)(void)) SDL_LoadFunction(so, "curl_easy_init");
        g_curl.setopt = (int (*)(CURL *, int, ...))SDL_LoadFunction(so, "curl_easy_setopt");
        g_curl.perform = (int (*)(CURL *))SDL_LoadFunction(so, "curl_easy_perform");
        g_curl.getinfo = (int (*)(CURL *, int, ...))SDL_LoadFunction(so, "curl_easy_getinfo");
        g_curl.cleanup = (void (*)(CURL *))SDL_LoadFunction(so, "curl_easy_cleanup");
        if (g_curl.init && g_curl.setopt && g_curl.perform && g_curl.getinfo && g_curl.cleanup) { g_curl_state = 1; break; }
        SDL_UnloadObject(so);
    }
    if (g_curl_state < 0) SDL_Log("online questions: no libcurl found");
    return g_curl_state > 0;
}

typedef struct { char url[512]; int tag; char *data; size_t len, cap; } HttpJob;

static size_t http_write(char *p, size_t size, size_t n, void *ud)
{
    HttpJob *j = ud;
    size_t k = size * n;
    if (j->len + k > (4u << 20)) return 0;                      /* 4 MB is plenty: stop */
    if (j->len + k + 1 > j->cap) {
        size_t cap = (j->len + k + 1) * 2;
        char *nd = realloc(j->data, cap);
        if (!nd) return 0;
        j->data = nd;
        j->cap = cap;
    }
    memcpy(j->data + j->len, p, k);
    j->len += k;
    j->data[j->len] = 0;
    return k;
}

static int SDLCALL http_thread(void *arg)
{
    HttpJob *j = arg;
    CURL *c = g_curl.init();
    long status = 0;
    if (c) {
        g_curl.setopt(c, CURLOPT_URL, j->url);
        g_curl.setopt(c, CURLOPT_USERAGENT, "MindMaze/1.0");
        g_curl.setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        g_curl.setopt(c, CURLOPT_NOSIGNAL, 1L);
        g_curl.setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        g_curl.setopt(c, CURLOPT_TIMEOUT, 30L);
        g_curl.setopt(c, CURLOPT_WRITEFUNCTION, http_write);
        g_curl.setopt(c, CURLOPT_WRITEDATA, j);
        if (g_curl.perform(c) == 0) g_curl.getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        g_curl.cleanup(c);
    }
    if (status != 200) { free(j->data); j->data = NULL; j->len = 0; }
    SDL_Event e = {0};
    e.type = sdl_event_base + SDL_EV_HTTP;
    e.user.data1 = j;
    SDL_PushEvent(&e);                                           /* -> sdl_http_done_event on the main thread */
    return 0;
}

void plat_http_get(const char *url, int tag)
{
    HttpJob *j = calloc(1, sizeof *j);
    if (!j || !curl_load()) { free(j); mm_http_done(tag, NULL, 0); return; }
    SDL_strlcpy(j->url, url, sizeof j->url);
    j->tag = tag;
    SDL_Thread *t = SDL_CreateThread(http_thread, "http", j);
    if (t) SDL_DetachThread(t);
    else { free(j); mm_http_done(tag, NULL, 0); }
}

void sdl_http_done_event(void *job)
{
    HttpJob *j = job;
    if (j->tag < 0) sdl_update_offer(j->data, j->tag == SDL_UPD_TAG_MANUAL);   /* the update check (sdl_update.c) */
    else mm_http_done(j->tag, j->data, (int)j->len);
    free(j->data);
    free(j);
}

/* ---- the updater's downloads (sdl_update.c): one GET into memory on the calling thread, up to 256 MB ---- */
typedef struct { char *data; size_t len, cap; SdlProgress fn; void *ud; } Dl;

static size_t dl_write(char *p, size_t size, size_t n, void *ud)
{
    Dl *d = ud;
    size_t k = size * n;
    if (d->len + k > (256u << 20)) return 0;
    if (d->len + k + 1 > d->cap) {
        size_t cap = (d->len + k + 1) * 2;
        char *nd = realloc(d->data, cap);
        if (!nd) return 0;
        d->data = nd;
        d->cap = cap;
    }
    memcpy(d->data + d->len, p, k);
    d->len += k;
    d->data[d->len] = 0;
    return k;
}
static int dl_progress(void *ud, long long total, long long now, long long ut, long long un)
{
    (void)ut, (void)un;
    Dl *d = ud;
    if (d->fn) d->fn(d->ud, now, total);
    return 0;
}

char *sdl_download(const char *url, size_t *len, SdlProgress fn, void *ud)
{
    *len = 0;
    if (!curl_load()) return NULL;
    Dl d = {NULL, 0, 0, fn, ud};
    CURL *c = g_curl.init();
    long status = 0;
    if (!c) return NULL;
    g_curl.setopt(c, CURLOPT_URL, url);
    g_curl.setopt(c, CURLOPT_USERAGENT, "MindMaze-updater");
    g_curl.setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    g_curl.setopt(c, CURLOPT_NOSIGNAL, 1L);
    g_curl.setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    g_curl.setopt(c, CURLOPT_TIMEOUT, 600L);
    g_curl.setopt(c, CURLOPT_WRITEFUNCTION, dl_write);
    g_curl.setopt(c, CURLOPT_WRITEDATA, &d);
    g_curl.setopt(c, CURLOPT_NOPROGRESS, 0L);
    g_curl.setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress);
    g_curl.setopt(c, CURLOPT_XFERINFODATA, &d);
    if (g_curl.perform(c) == 0) g_curl.getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    g_curl.cleanup(c);
    if (status != 200 || !d.data) { free(d.data); return NULL; }
    *len = d.len;
    return d.data;
}

/* ---- Settings > "Export data for the phone app": the data folder as one .zip (../common/zip_write.c) ---- */
int plat_can_export(void) { return 1; }

typedef struct { ZipW *z; char rel[600]; } Walk;

static SDL_EnumerationResult SDLCALL export_entry(void *ud, const char *dir, const char *name)
{
    Walk *w = ud;
    char path[1200], rel[900];
    SDL_PathInfo info;
    sdl_join(path, sizeof path, dir, name);
    snprintf(rel, sizeof rel, "%s%s", w->rel, name);
    if (!SDL_GetPathInfo(path, &info)) return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (!strcmp(rel, "art/work")) return SDL_ENUM_CONTINUE;  /* the AI agent's work files: not needed to play */
        Walk sub = {w->z, ""};
        snprintf(sub.rel, sizeof sub.rel, "%s/", rel);
        SDL_EnumerateDirectory(path, export_entry, &sub);
    } else if (info.type == SDL_PATHTYPE_FILE) {
        int size = 0;
        void *d = sdl_slurp(path, &size);
        if (d) zipw_add(w->z, rel, d, (size_t)size);
        else zipw_fail(w->z);
        free(d);
    }
    return SDL_ENUM_CONTINUE;
}

typedef struct { const char *zip; char msg[256]; } ExportJob;

static int export_job(void *arg)
{
    ExportJob *x = arg;
    FILE *f = fopen(x->zip, "wb");
    ZipW *z = f ? zipw_open(f) : NULL;
    if (!z) { snprintf(x->msg, sizeof x->msg, "Could not create the file."); return 0; }
    Walk w = {z, ""};
    SDL_EnumerateDirectory(sdl_data, export_entry, &w);
    int files = 0;
    double bytes = 0;
    if (!zipw_close(z, &files, &bytes)) {
        snprintf(x->msg, sizeof x->msg, "Writing the file failed (disk full?).");
        remove(x->zip);
        return 0;
    }
    snprintf(x->msg, sizeof x->msg, "%d files, %.1f MB", files, bytes / 1048576.0);
    return 1;
}

void plat_export_data(void)
{
    static const SDL_DialogFileFilter ZIP[] = {{"MindMaze data file (*.zip)", "zip"}};
    char def[1200];
    sdl_join(def, sizeof def, sdl_home, "MindMaze-data.zip");   /* the game's own folder (portable) */
    char *zip = sdl_pick_file(1, ZIP, 1, def);
    if (!zip) {
        if (sdl_pick_failed)
            sdl_ask(1, "MindMaze", "No file dialog is available on this system (it needs xdg-desktop-portal or zenity).", "OK", NULL, NULL);
        return;
    }
    size_t n = strlen(zip);
    if (n < 4 || SDL_strcasecmp(zip + n - 4, ".zip")) {          /* the dialog may leave the extension off */
        char *z2 = malloc(n + 5);
        if (!z2) { free(zip); return; }
        snprintf(z2, n + 5, "%s.zip", zip);
        free(zip);
        zip = z2;
    }
    ExportJob x = {zip, ""};
    int ok = sdl_run_job(export_job, &x, "Writing the data file...");
    char msg[1600];
    if (ok)
        snprintf(msg, sizeof msg, "Saved %s (%s).\n\nCopy it to your phone and choose it in the MindMaze app: \"Import a MindMaze "
                                  "data file (.zip)\". It holds pictures from your Encarta disc: keep it for yourself.", zip, x.msg);
    else snprintf(msg, sizeof msg, "The data could not be exported: %s", x.msg);
    sdl_ask(!ok, "MindMaze", msg, "OK", NULL, NULL);
    free(zip);
    plat_invalidate();
}

/* --export <file.zip> (no window) */
int sdl_export_headless(const char *zip)
{
    ExportJob x = {zip, ""};
    int ok = export_job(&x);
    if (ok) printf("Saved %s (%s). It holds pictures from your Encarta disc: keep it for yourself.\n", zip, x.msg);
    else fprintf(stderr, "MindMaze: the data could not be exported: %s\n", x.msg);
    return ok;
}
