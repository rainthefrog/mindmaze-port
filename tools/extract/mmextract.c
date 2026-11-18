/* Command-line front end of the built-in extractor (the same code the game runs on first start).
 *   mmextract <disc image or folder> <output folder>
 * Build: make mmextract (Linux) - used by the tests to compare with tools/extract/extract.py. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mx.h"

static const char *g_out;

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
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", g_out, rel);
    for (char *s = p + strlen(g_out) + 1; (s = strchr(s, '/')); s++) {      /* create sub-folders */
        *s = 0;
        if (mkdir(p, 0755) && errno != EEXIST) return 0;
        *s = '/';
    }
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static void pr(MXIO *io, int pct, const char *step) { (void)io; printf("%3d%% %s\n", pct, step); }

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: mmextract <disc image or folder> <output folder>\n"); return 2; }
    g_out = argv[2];
    if (mkdir(g_out, 0755) && errno != EEXIST) { perror(g_out); return 1; }
    MXIO io = {op, rd, cl, wr, pr, NULL};
    mx_files = mx_embedded;
    char err[1024];
    if (!mx_extract(&io, argv[1], err, sizeof err)) { fprintf(stderr, "ERROR: %s\n", err); return 1; }
    return 0;
}
