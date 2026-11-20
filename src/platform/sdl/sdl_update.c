/* MindMaze - built-in updater, Linux and macOS part (CHANGE 2026-11-20; portable part: ../common/update.c).
 *
 * At start (unless --no-update-check or update_check=0 in mindmaze.ini) the newest GitHub release is asked for in
 * the background; when it is newer, the player chooses: Update now / Later / Don't ask again. Updating downloads
 * the platform's file and its .sha256, compares the checksum and then:
 *   Linux  MindMaze-linux-x86_64.tar.gz: every file of the tarball's MindMaze/ folder is written next to the program
 *          as <file>.new and renamed over the old one (the running program may be replaced this way); data/,
 *          mindmaze.ini and the player's files are not in the tarball and stay.
 *   macOS  MindMaze-macos.zip: MindMaze.app is unpacked beside the running app as <name>.app.new, the old bundle
 *          renamed to <name>.app.old, the new one put in its place, the old one deleted. The data lives outside the
 *          bundle (Application Support).
 * Then the program starts itself again with the same arguments (execv). A read-only folder (a system install, a
 * macOS app that was not moved out of Downloads and runs "translocated") cannot be updated: the release page opens.
 * --update does the same without a window (no restart). */
#define _DEFAULT_SOURCE                                 /* symlink, lstat under -std=c11 */
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "sdl.h"
#include "mm.h"
#include "update.h"

#ifdef __APPLE__
#define ASSET "MindMaze-macos.zip"
#define TOP "MindMaze.app/"
#else
#define ASSET "MindMaze-linux-x86_64.tar.gz"
#define TOP "MindMaze/"
#endif

char **sdl_argv;

static int update_off(void)
{
    char v[16];
    return plat_get_setting("update_check", v, sizeof v) && atoi(v) == 0;
}

static int api(char *out, size_t n)
{
    char v[512] = "";
    plat_get_setting("update_url", v, sizeof v);
    return upd_api_url(v, out, n);
}

void sdl_update_start(void)
{
    char url[600];
    if (!update_off() && api(url, sizeof url)) plat_http_get(url, SDL_UPD_TAG_START);
}

/* ---- the folders: Linux = the program's folder (sdl_res); macOS = the .app bundle around Contents/Resources ---- */
static char g_target[1100];                       /* Linux: program folder; macOS: the bundle, without the last '/' */

static int target(void)                          /* from SDL itself: --update runs before sdl_res is set */
{
    const char *base = SDL_GetBasePath();                 /* the program's folder, ends in '/'; macOS: .../Resources/ */
    if (!base || base[0] != '/') return 0;
#ifdef __APPLE__
    static const char RES[] = "Contents/Resources/";
    size_t n = strlen(base), r = sizeof RES - 1;
    if (n <= r + 5 || strcmp(base + n - r, RES)) return 0;
    snprintf(g_target, sizeof g_target, "%.*s", (int)(n - r - 1), base);       /* ".../MindMaze.app" */
    return strlen(g_target) > 4 && !strcmp(g_target + strlen(g_target) - 4, ".app");
#else
    size_t n = strlen(base);
    if (n < 2) return 0;                                  /* never the root folder */
    snprintf(g_target, sizeof g_target, "%s", base);
    return 1;
#endif
}

static int writable_dir(const char *dir)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/.mindmaze-update-test", dir);
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fclose(f);
    remove(p);
    return 1;
}

static int safe_rel(const char *rel)                          /* no absolute paths, no "..": stays inside */
{
    if (!*rel || rel[0] == '/') return 0;
    for (const char *p = rel; *p; p++)
        if (p[0] == '.' && p[1] == '.' && (p == rel || p[-1] == '/') && (p[2] == '/' || !p[2])) return 0;
    return 1;
}

/* ---- writing the archive's entries ---- */
typedef struct { const char *dest; int files; char err[300]; } Put;

static int put_entry(void *ud, const char *path, char type, unsigned mode, const uint8_t *data, size_t n)
{
    Put *p = ud;
    size_t t = strlen(TOP);
    if (strncmp(path, TOP, t)) return 1;                      /* only the program's own folder / bundle */
    const char *rel = path + t;
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    if (!*rel || !strncmp(base, "._", 2)) return 1;           /* the folder itself; macOS AppleDouble files */
    if (!safe_rel(rel)) { snprintf(p->err, sizeof p->err, "unsafe path in the download: %s", path); return 0; }
    char full[1400], tmp[1500];
    snprintf(full, sizeof full, "%s/%s", p->dest, rel);
    size_t fl = strlen(full);
    if (full[fl - 1] == '/') full[fl - 1] = 0;
    if (type == 'd') { SDL_CreateDirectory(full); return 1; }
    char dir[1400];
    snprintf(dir, sizeof dir, "%s", full);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; SDL_CreateDirectory(dir); }
    snprintf(tmp, sizeof tmp, "%s.new", full);
    remove(tmp);
    if (type == 'l') {
        char target[1100];
        snprintf(target, sizeof target, "%.*s", (int)(n < sizeof target ? n : sizeof target - 1), (const char *)data);
        if (symlink(target, tmp)) { snprintf(p->err, sizeof p->err, "could not write %s", rel); return 0; }
    } else {
        FILE *f = fopen(tmp, "wb");
        int ok = f && fwrite(data, 1, n, f) == n;
        if (f && fclose(f)) ok = 0;
        if (!ok) { remove(tmp); snprintf(p->err, sizeof p->err, "could not write %s (disk full?)", rel); return 0; }
        if (!mode) mode = !strncmp(rel, "Contents/MacOS/", 15) ? 0755 : 0644;   /* a zip without unix permissions */
        chmod(tmp, mode & 0777);
    }
    if (rename(tmp, full)) { remove(tmp); snprintf(p->err, sizeof p->err, "could not replace %s", rel); return 0; }
    p->files++;
    return 1;
}

#ifdef __APPLE__
static SDL_EnumerationResult SDLCALL rm_entry(void *ud, const char *dir, const char *name)
{
    (void)ud;
    char p[1400];
    SDL_PathInfo info;
    snprintf(p, sizeof p, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", name);
    if (SDL_GetPathInfo(p, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
        struct stat st;
        if (!lstat(p, &st) && !S_ISLNK(st.st_mode)) SDL_EnumerateDirectory(p, rm_entry, NULL);
    }
    SDL_RemovePath(p);
    return SDL_ENUM_CONTINUE;
}
static void rm_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st)) return;
    if (S_ISDIR(st.st_mode)) SDL_EnumerateDirectory(path, rm_entry, NULL);
    SDL_RemovePath(path);
}
#endif

/* ---- download, check, put in place ---- */
typedef struct {
    UpdRelease r;
    int show;                                                 /* progress on the status screen */
    char err[300];
} Job;

static void progress(void *ud, long long now, long long total)
{
    Job *j = ud;
    if (!j->show) return;
    if (total > 0) sdl_progress((int)(now * 100 / total), "Downloading the new version");
    else sdl_progress(-1, "Downloading the new version");
}

static int install(Job *j)
{
    const char *url = upd_asset(&j->r, ASSET), *sumurl = upd_asset(&j->r, ASSET ".sha256");
    if (!url || !sumurl) { snprintf(j->err, sizeof j->err, "the release %s has no %s with its checksum", j->r.tag, ASSET); return 0; }
    if (!target()) { snprintf(j->err, sizeof j->err, "MindMaze is not running from its app bundle"); return 0; }
#ifdef __APPLE__
    char parent[1100];
    snprintf(parent, sizeof parent, "%s", g_target);
    char *cut = strrchr(parent, '/');
    if (cut) *cut = 0;
    if (!writable_dir(parent) || !writable_dir(g_target)) { snprintf(j->err, sizeof j->err, "the app's folder is read-only"); return 0; }
#else
    if (!writable_dir(g_target)) { snprintf(j->err, sizeof j->err, "the program's folder is read-only"); return 0; }
#endif
    size_t sl = 0, n = 0;
    char *sum = sdl_download(sumurl, &sl, NULL, NULL);
    char *arc = sum ? sdl_download(url, &n, progress, j) : NULL;
    int ok = 0;
    if (!sum || !arc) snprintf(j->err, sizeof j->err, "the download failed (no connection?)");
    else if (!upd_sha_ok(arc, n, sum, sl)) snprintf(j->err, sizeof j->err, "the download is damaged (checksum differs); nothing was changed");
    else {
        if (j->show) sdl_progress(-1, "Installing the new version");
#ifdef __APPLE__
        char fresh[1200], old[1200];
        snprintf(fresh, sizeof fresh, "%s.new", g_target);
        snprintf(old, sizeof old, "%s.old", g_target);
        rm_tree(fresh);
        rm_tree(old);
        SDL_CreateDirectory(fresh);
        Put p = {fresh, 0, ""};
        if (!upd_unzip((const uint8_t *)arc, n, put_entry, &p) || !p.files)
            snprintf(j->err, sizeof j->err, "%s", p.err[0] ? p.err : "the download is not a MindMaze app");
        else if (rename(g_target, old)) snprintf(j->err, sizeof j->err, "could not move the running app aside");
        else if (rename(fresh, g_target)) {
            rename(old, g_target);                            /* put the old one back */
            snprintf(j->err, sizeof j->err, "could not put the new app in place");
        } else { rm_tree(old); ok = 1; }
        if (!ok) rm_tree(fresh);
#else
        Put p = {g_target, 0, ""};
        if (!upd_untar_gz((const uint8_t *)arc, n, put_entry, &p) || !p.files)
            snprintf(j->err, sizeof j->err, "%s", p.err[0] ? p.err : "the download is not a MindMaze tarball");
        else ok = 1;                                          /* (a failure midway leaves whole files: each rename is atomic) */
#endif
    }
    free(sum);
    free(arc);
    return ok;
}

static int install_job(void *arg) { return install(arg); }

static void restart(void)
{
    char exe[1300];
#ifdef __APPLE__
    snprintf(exe, sizeof exe, "%s/Contents/MacOS/MindMaze", g_target);
#else
    snprintf(exe, sizeof exe, "%sMindMaze", g_target);
#endif
    mm_shutdown();
    SDL_Quit();
    if (sdl_argv) execv(exe, sdl_argv);
    fprintf(stderr, "MindMaze was updated; start it again.\n");
    exit(0);
}

void sdl_update_offer(const char *json, int manual)
{
    Job *j = calloc(1, sizeof *j);
    if (!j) return;
    char msg[900];
    if (!upd_parse(json, &j->r)) {
        if (manual) sdl_ask(1, "MindMaze", "MindMaze could not reach GitHub to look for a new version.", "OK", NULL, NULL);
        free(j);
        return;
    }
    if (!upd_newer(j->r.tag, MM_VERSION) || !upd_asset(&j->r, ASSET)) {
        if (manual) {
            snprintf(msg, sizeof msg, "MindMaze %s is the newest version.", MM_VERSION);
            sdl_ask(0, "MindMaze", msg, "OK", NULL, NULL);
        }
        free(j);
        return;
    }
    const char *v = j->r.tag[0] == 'v' ? j->r.tag + 1 : j->r.tag;
    snprintf(msg, sizeof msg, "MindMaze %s is available (you have %s).\n\nUpdate now? MindMaze downloads it (about 35 MB), "
                              "checks it and starts again. Your data and settings stay.", v, MM_VERSION);
    /* Escape presses the last button: "Later" */
    int a = manual ? sdl_ask(0, "MindMaze - new version", msg, "Update now", "Later", NULL)
                   : sdl_ask(0, "MindMaze - new version", msg, "Update now", "Don't ask again", "Later");
    if (!manual && a == 1) plat_set_setting("update_check", "0");
    if (a != 0) { free(j); return; }
    j->show = 1;
    if (sdl_run_job(install_job, j, "Updating MindMaze...")) { free(j); restart(); }
    snprintf(msg, sizeof msg, "The update did not work: %s.\n\nOpen the download page instead?", j->err);
    if (sdl_ask(1, "MindMaze", msg, "Open the page", "Cancel", NULL) == 0)
        plat_open_url(*j->r.page ? j->r.page : MM_REPO_URL "/releases/latest");
    free(j);
    plat_invalidate();
}

int sdl_update_cli(void)
{
    char url[600];
    if (!api(url, sizeof url)) { fprintf(stderr, "MindMaze: this build has no release address\n"); return 1; }
    size_t n = 0;
    char *json = sdl_download(url, &n, NULL, NULL);
    Job *j = calloc(1, sizeof *j);
    int ok = j && json && upd_parse(json, &j->r);
    free(json);
    if (!ok) { fprintf(stderr, "MindMaze: could not read the newest release from %s\n", url); free(j); return 1; }
    if (!upd_newer(j->r.tag, MM_VERSION) || !upd_asset(&j->r, ASSET)) {
        printf("MindMaze %s is the newest version.\n", MM_VERSION);
        free(j);
        return 0;
    }
    printf("Updating MindMaze %s to %s...\n", MM_VERSION, j->r.tag);
    fflush(stdout);
    ok = install(j);
    if (ok) printf("Updated to %s.\n", j->r.tag);
    else fprintf(stderr, "MindMaze: the update did not work: %s\n", j->err);
    free(j);
    return ok ? 0 : 1;
}
