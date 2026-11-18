/* The built-in extractor in a Web Worker (web / Android app): the page hands over the player's disc image or files
 * (mounted read-only with WORKERFS at /src, so a 3 GB image is read piece by piece), the extractor writes the data
 * folder to /out, and extract-worker.js sends the files back to the page, which stores them in IndexedDB. */
#include <emscripten.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mx.h"

static void *op(MXIO *io, const char *path, int64_t *size)
{
    (void)io;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return NULL;
    FILE *f = fopen(path, "rb");
    if (f) *size = (int64_t)st.st_size;
    return f;
}
static int rd(MXIO *io, void *f, int64_t off, void *buf, size_t n)
{
    (void)io;
    return fseeko(f, (off_t)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}
static void cl(MXIO *io, void *f) { (void)io; fclose(f); }
static int wr(MXIO *io, const char *rel, const void *d, size_t n)
{
    (void)io;
    char p[1024];
    snprintf(p, sizeof p, "/out/%s", rel);
    for (char *s = p + 5; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static void pr(MXIO *io, int pct, const char *step)
{
    (void)io;
    EM_ASM({ postMessage({ progress: $0, step: UTF8ToString($1) }); }, pct, step);
}

static char g_err[1024];

/* source: "/src/<image>.iso" or "/src" (the five files); returns 1 or 0 (message: web_extract_error) */
EMSCRIPTEN_KEEPALIVE int web_extract(const char *source)
{
    mkdir("/out", 0755);
    MXIO io = {op, rd, cl, wr, pr, NULL};
    mx_files = mx_embedded;
    return mx_extract(&io, source, g_err, sizeof g_err);
}
EMSCRIPTEN_KEEPALIVE const char *web_extract_error(void) { return g_err; }

/* Settings > Add questions from a disc (CHANGE 2026-11-18): another disc's questions -> /out/editions/<id>/ (+ music);
 * web_add_info: "id<TAB>name<TAB>count" */
static char g_id[16], g_name[64];
static int g_count;
EMSCRIPTEN_KEEPALIVE int web_add_disc(const char *source)
{
    mkdir("/out", 0755);
    MXIO io = {op, rd, cl, wr, pr, NULL};
    return mx_add_edition(&io, source, g_id, sizeof g_id, g_name, sizeof g_name, &g_count, g_err, sizeof g_err);
}
EMSCRIPTEN_KEEPALIVE const char *web_add_info(void)
{
    static char b[128];
    snprintf(b, sizeof b, "%s\t%s\t%d", g_id, g_name, g_count);
    return b;
}

int main(void) { return 0; }
