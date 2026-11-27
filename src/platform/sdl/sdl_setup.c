/* MindMaze - SDL3 first start (Linux, macOS): find the data folder, or build it from the player's Encarta disc with
 * the built-in extractor (src/extract), like win32_setup.c; plus Settings > "Add questions from a disc".
 *
 * Where the data lives ("home" = the folder holding data/, mindmaze.ini and the player's files):
 *   0. --home <dir> on the command line, if given;
 *   1. next to the program, if data/tables.json is there (on macOS that is the app's Resources);
 *   2. the user's application data folder, if an earlier start put the data there: Linux $XDG_DATA_HOME/MindMaze
 *      (~/.local/share/MindMaze), macOS ~/Library/Application Support/MindMaze (SDL_GetPrefPath);
 *   3. Linux: next to the program if that folder is writable (portable by default, CHANGE 2026-11-19);
 *   4. otherwise the application data folder (macOS always: the signed app bundle must stay unchanged).
 * The disc: a path given on the command line (disc image, folder, or a file on the disc), a mounted Encarta disc
 * (Linux /media, /run/media, /mnt; macOS /Volumes), or one chosen in the file dialog. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdl.h"
#include "mm.h"
#include "mx.h"
#include "cli.h"

/* ---- MXIO on SDL file streams (64-bit offsets: a disc image is 3 GB) ---- */
static char g_out[1024];

static void *io_open(MXIO *io, const char *path, int64_t *size)
{
    (void)io;
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) return NULL;
    SDL_IOStream *f = SDL_IOFromFile(path, "rb");
    if (f) *size = (int64_t)info.size;
    return f;
}
static int io_read(MXIO *io, void *f, int64_t off, void *buf, size_t n)
{
    (void)io;
    if (SDL_SeekIO(f, off, SDL_IO_SEEK_SET) != off) return 0;
    for (char *b = buf; n;) {
        size_t got = SDL_ReadIO(f, b, n);
        if (!got) return 0;
        b += got;
        n -= got;
    }
    return 1;
}
static void io_close(MXIO *io, void *f) { (void)io; SDL_CloseIO(f); }
static int io_write(MXIO *io, const char *rel, const void *d, size_t n)
{
    (void)io;
    char p[1200];
    snprintf(p, sizeof p, "%s%s", g_out, rel);
    return sdl_write_file(p, d, n);
}
static int g_headless;                                    /* an action flag runs: progress to stderr, no window */
static void io_progress(MXIO *io, int pct, const char *step)
{
    (void)io;
    static int last = -100;
    if (!g_headless) { sdl_progress(pct, step); return; }
    if (pct >= last && pct < last + 10 && pct != 100) return;    /* every 10 % (a new step may start at 0 again) */
    last = pct;
    if (pct >= 0) fprintf(stderr, "progress %.2f %s\n", pct / 100.0, step ? step : "");
    else if (step && *step) fprintf(stderr, "  %s\n", step);
}

/* the extractor on a background thread (sdl_run_job): the whole data folder, or one more disc's questions */
typedef struct { char src[1100]; int add; char id[16], name[64]; int count; char err[1024]; } Extract;

static int extract_job(void *arg)
{
    Extract *x = arg;
    MXIO io = {io_open, io_read, io_close, io_write, io_progress, NULL};
    mx_files = mx_embedded;
    return x->add ? mx_add_edition(&io, x->src, x->id, sizeof x->id, x->name, sizeof x->name, &x->count, x->err, sizeof x->err)
                  : mx_extract(&io, x->src, x->err, sizeof x->err);
}

/* ---- finding the disc ---- */
static int is_disc(const char *root)
{
    static const char *const MARK[] = {"EE/ENCARTA/MINDMAZE.EIT", "ee/encarta/mindmaze.eit", "ENCYC95/MM/INTERACT/MINDMAZE.DB",
                                       "encyc95/mm/interact/mindmaze.db", "ENCYC97/MINDMAZE.DB", "encyc97/mindmaze.db",
                                       "ENCYC98/MINDMAZE.DB", "encyc98/mindmaze.db", NULL};
    char p[1200];
    for (int i = 0; MARK[i]; i++) {
        sdl_join(p, sizeof p, root, MARK[i]);
        if (sdl_exists(p)) return 1;
    }
    return 0;
}

typedef struct { char *found; int depth; } Search;

static SDL_EnumerationResult SDLCALL search_dir(void *ud, const char *dir, const char *name)
{
    Search *s = ud;
    char p[1100];
    SDL_PathInfo info;
    sdl_join(p, sizeof p, dir, name);
    if (!SDL_GetPathInfo(p, &info) || info.type != SDL_PATHTYPE_DIRECTORY) return SDL_ENUM_CONTINUE;
    if (is_disc(p)) { SDL_strlcpy(s->found, p, 1100); return SDL_ENUM_SUCCESS; }
    if (s->depth > 1) {                                           /* /media/<user>/<disc>, /run/media/<user>/<disc> */
        Search sub = {s->found, s->depth - 1};
        SDL_EnumerateDirectory(p, search_dir, &sub);
        if (*s->found) return SDL_ENUM_SUCCESS;
    }
    return SDL_ENUM_CONTINUE;
}

/* a mounted Encarta disc: 2006 (EE/ENCARTA/MINDMAZE.EIT) or 95 / 97 / 98; root receives its folder (1100 bytes) */
static int find_disc(char *root)
{
    static const struct { const char *dir; int depth; } WHERE[] = {
        {"/Volumes", 1}, {"/media", 2}, {"/run/media", 2}, {"/mnt", 1}};
    *root = 0;
    for (size_t i = 0; i < sizeof WHERE / sizeof WHERE[0] && !*root; i++) {
        Search s = {root, WHERE[i].depth};
        if (sdl_exists(WHERE[i].dir)) SDL_EnumerateDirectory(WHERE[i].dir, search_dir, &s);
    }
    return *root != 0;
}

/* a chosen path -> the extractor's source: a disc image as it is, a file on the disc -> its folder */
static void source_of(const char *path, char *src)
{
    SDL_PathInfo info;
    size_t n = strlen(path);
    SDL_strlcpy(src, path, 1100);
    if (SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE && !(n > 4 && !SDL_strcasecmp(path + n - 4, ".iso"))) {
        char *slash = strrchr(src, '/');
        if (slash) *(slash == src ? slash + 1 : slash) = 0;
    }
    for (n = strlen(src); n > 1 && src[n - 1] == '/';) src[--n] = 0;   /* "/media/x/" -> "/media/x" */
}

/* file dialog: a disc image, or any of the files on a disc / in a copied folder. 0 = cancelled */
static int pick_source(char *src)
{
    static const SDL_DialogFileFilter F[] = {
        {"Encarta disc image or files (*.iso, ENCARTA.EXE, MINDMAZE.EIT, MINDMAZE.DB)", "iso;ISO;exe;EXE;eit;EIT;db;DB"},
        {"All files", "*"}};
    char *p = sdl_pick_file(0, F, 2, NULL);
    if (!p) return 0;
    source_of(p, src);
    free(p);
    return 1;
}

static const char NO_DIALOG[] = "No file dialog is available on this system (it needs xdg-desktop-portal or zenity). "
                                "Start MindMaze with the disc image or the disc's folder as its argument instead, e.g.\n"
                                "  ./MindMaze ~/Downloads/Encarta.iso";

/* a folder MindMaze may write to (a test file is made and removed) */
static int writable(const char *dir)
{
    char p[1200];
    sdl_join(p, sizeof p, dir, ".mindmaze-write-test");
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fclose(f);
    remove(p);
    return 1;
}

static int has_data(const char *home)
{
    char p[1200];
    sdl_join(p, sizeof p, home, "data/tables.json");
    return sdl_exists(p);
}

/* the home folder (see the top of this file); --home wins. 0 = none found (message shown / printed) */
static int pick_home(const char *home, int headless)
{
    if (home) {                                                    /* --home <dir> */
        SDL_CreateDirectory(home);                                 /* (with its parents) */
        if (!sdl_exists(home)) {
            char m[1200];
            snprintf(m, sizeof m, "MindMaze cannot create the folder %s.", home);
            if (headless) fprintf(stderr, "MindMaze: %s\n", m); else sdl_ask(1, "MindMaze", m, "OK", NULL, NULL);
            return 0;
        }
        sdl_join(sdl_home, sizeof sdl_home, home, "");
        return 1;
    }
    if (has_data(sdl_res)) { SDL_strlcpy(sdl_home, sdl_res, sizeof sdl_home); return 1; }
#ifndef __APPLE__
    /* portable by default (CHANGE 2026-11-19, like Windows): everything stays in the folder the tarball was unpacked
     * to, unless an older start already put the data in the application data folder (1.0.0/1.0.1 did) or this folder
     * is read-only. SDL_GetPrefPath creates its folder, so it is only asked when that folder is used. */
    {
        const char *xdg = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
        char old[1100] = "";
        if (xdg && *xdg) snprintf(old, sizeof old, "%s/MindMaze/", xdg);
        else if (h && *h) snprintf(old, sizeof old, "%s/.local/share/MindMaze/", h);
        if (!(*old && has_data(old)) && writable(sdl_res)) { SDL_strlcpy(sdl_home, sdl_res, sizeof sdl_home); return 1; }
    }
#endif
    char *pref = SDL_GetPrefPath(NULL, "MindMaze");                 /* (creates the folder) */
    int got = pref != NULL;
    if (got) sdl_join(sdl_home, sizeof sdl_home, pref, "");
    SDL_free(pref);
    if (!got) {
        if (headless) fprintf(stderr, "MindMaze: cannot find a folder to write its data to\n");
        else sdl_ask(1, "MindMaze", "MindMaze cannot find a folder to write its data to.", "OK", NULL, NULL);
        return 0;
    }
    return 1;
}

int sdl_setup(const char *source, const char *home, int headless)
{
    const char *base = SDL_GetBasePath();
    char p[1200];
    sdl_join(sdl_res, sizeof sdl_res, base ? base : ".", "");
    if (!pick_home(home, headless)) return 0;
    snprintf(sdl_data, sizeof sdl_data, "%sdata/", sdl_home);
    snprintf(p, sizeof p, "%stables.json", sdl_data);
    if (sdl_exists(p)) return 1;

    /* first start */
    Extract *x = calloc(1, sizeof *x);
    if (!x) return 0;
    if (headless) {                                                /* an action flag: no window, no questions */
        int ok = 0;
        if (!source) fprintf(stderr, "MindMaze: no data folder in %s yet: give the Encarta disc with --disc <path>\n", sdl_home);
        else {
            source_of(source, x->src);
            SDL_strlcpy(g_out, sdl_data, sizeof g_out);
            SDL_CreateDirectory(sdl_data);
            g_headless = 1;
            fprintf(stderr, "MindMaze: preparing %s from %s\n", sdl_data, x->src);
            ok = extract_job(x);
            g_headless = 0;
            if (!ok) fprintf(stderr, "MindMaze: %s\n", x->err[0] ? x->err : "the extraction failed");
        }
        free(x);
        return ok;
    }
    char msg[2600];
    int ask = 0, ok = 0;
    if (source) source_of(source, x->src);                           /* given on the command line: no questions */
    else ask = find_disc(x->src) ? 1 : 2;
    for (;;) {
        if (ask == 1) {
            snprintf(msg, sizeof msg, "MindMaze uses the pictures, sounds and questions from your own Microsoft Encarta disc. "
                                      "They are copied once into:\n%s\n\nFound an Encarta disc in %s. Use it?", sdl_data, x->src);
            int r = sdl_ask(0, "MindMaze - first start", msg, "Use it", "Choose another...", "Cancel");
            if (r < 0 || r == 2) break;
            if (r == 1 && !pick_source(x->src)) { if (sdl_pick_failed) sdl_ask(1, "MindMaze", NO_DIALOG, "OK", NULL, NULL); break; }
        } else if (ask == 2) {
            snprintf(msg, sizeof msg, "MindMaze uses the pictures, sounds and questions from your own Microsoft Encarta disc "
                                      "(2006 Premium, or Encarta 95 MPEG, 96 Macintosh, 97 or 98). They are copied once into:\n%s\n\n"
                                      "Next, choose the disc image (.iso), or on the disc ENCARTA.EXE (2006, folder EE/ENCARTA) "
                                      "or MINDMAZE.DB (95 to 98).", sdl_data);
            if (sdl_ask(0, "MindMaze - first start", msg, "Choose...", "Cancel", NULL) != 0) break;
            if (!pick_source(x->src)) { if (sdl_pick_failed) sdl_ask(1, "MindMaze", NO_DIALOG, "OK", NULL, NULL); break; }
        }
        SDL_strlcpy(g_out, sdl_data, sizeof g_out);
        SDL_CreateDirectory(sdl_data);
        x->add = 0;
        if ((ok = sdl_run_job(extract_job, x, "Preparing MindMaze from your Encarta disc..."))) break;
        snprintf(msg, sizeof msg, "%s\n\nTry another disc, image or folder?", x->err[0] ? x->err : "The extraction failed.");
        if (sdl_ask(1, "MindMaze", msg, "Retry", "Cancel", NULL) != 0) break;
        ask = 2;
    }
    free(x);
    return ok;
}

/* Settings > "Add questions from a disc" (CHANGE 2026-11-18): a mounted disc, or a chosen image / MINDMAZE.DB /
 * ENCARTA.EXE -> data/editions/<id>/ (mx_add_edition), then the core's mm_disc_added */
void plat_add_disc(void)
{
    Extract *x = calloc(1, sizeof *x);
    if (!x) return;
    int go = 1;
    sdl_pick_failed = 0;
    if (find_disc(x->src)) {
        char msg[1400];
        snprintf(msg, sizeof msg, "Found an Encarta disc in %s. Add its questions?", x->src);
        int r = sdl_ask(0, "MindMaze - add questions", msg, "Add", "Choose another...", "Cancel");
        if (r < 0 || r == 2) go = 0;
        else if (r == 1) go = pick_source(x->src);
    } else go = pick_source(x->src);
    if (!go) {
        mm_disc_added(0, "", "", 0, sdl_pick_failed ? NO_DIALOG : NULL);   /* NULL: the player cancelled */
        free(x);
        return;
    }
    SDL_strlcpy(g_out, sdl_data, sizeof g_out);
    x->add = 1;
    int ok = sdl_run_job(extract_job, x, "Reading the questions from the disc...");
    mm_disc_added(ok, x->id, x->name, x->count, ok ? NULL : (x->err[0] ? x->err : "The disc could not be read."));
    free(x);
    plat_invalidate();
}

/* --add-disc <path> (no window): the disc's questions -> data/editions/<id>/, then registered as a source */
int sdl_add_disc_headless(const char *path)
{
    Extract *x = calloc(1, sizeof *x);
    if (!x) return 0;
    source_of(path, x->src);
    SDL_strlcpy(g_out, sdl_data, sizeof g_out);
    x->add = 1;
    g_headless = 1;
    fprintf(stderr, "MindMaze: reading the questions from %s\n", x->src);
    char msg[1024];
    int ok = extract_job(x);
    g_headless = 0;
    if (!ok) fprintf(stderr, "MindMaze: %s\n", x->err[0] ? x->err : "the disc could not be read");
    else if (!(ok = cli_add_edition(x->id, x->name, x->count, msg, sizeof msg))) fprintf(stderr, "MindMaze: %s\n", msg);
    else printf("%s\n", msg);
    free(x);
    return ok;
}
