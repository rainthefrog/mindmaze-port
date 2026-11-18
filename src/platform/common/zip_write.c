/* A plain ZIP writer for "Export data for the phone app" (Windows: win32_export.c, Linux / macOS: sdl_files.c):
 * the data folder as one .zip that the Android / web app imports. Entries stored, UTF-8 names (flag 0x0800),
 * no ZIP64: the data folder is far below 4 GB. Portable C on a FILE the platform opened. */
#include <stdlib.h>
#include <string.h>
#include "zip_write.h"

typedef struct { char name[260]; unsigned long crc, size, offset; } Entry;
struct ZipW { FILE *f; Entry *e; int n, cap; unsigned long pos; int failed; };

static unsigned long g_crc[256];
static unsigned long crc32_of(const unsigned char *d, size_t n)
{
    if (!g_crc[1])
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
            g_crc[i] = c;
        }
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t i = 0; i < n; i++) c = g_crc[(c ^ d[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { put16(p, (unsigned)(v & 0xFFFF)); put16(p + 2, (unsigned)(v >> 16)); }

ZipW *zipw_open(FILE *f)
{
    ZipW *z = calloc(1, sizeof *z);
    if (!z) { fclose(f); return NULL; }
    z->f = f;
    return z;
}

void zipw_fail(ZipW *z) { z->failed = 1; }

void zipw_add(ZipW *z, const char *name, const void *data, size_t n)
{
    if (z->failed) return;
    if (z->n == z->cap) {
        z->cap = z->cap ? z->cap * 2 : 256;
        Entry *ne = realloc(z->e, sizeof *ne * (size_t)z->cap);
        if (!ne) { z->failed = 1; return; }
        z->e = ne;
    }
    Entry *e = &z->e[z->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->crc = crc32_of(data, n);
    e->size = (unsigned long)n;
    e->offset = z->pos;
    unsigned char h[30] = {0};
    size_t nl = strlen(e->name);
    put32(h, 0x04034b50);
    put16(h + 4, 20);
    put16(h + 6, 0x0800);                                   /* UTF-8 names */
    put32(h + 14, e->crc);
    put32(h + 18, e->size);
    put32(h + 22, e->size);
    put16(h + 26, (unsigned)nl);
    if (fwrite(h, 1, 30, z->f) != 30 || fwrite(e->name, 1, nl, z->f) != nl || fwrite(data, 1, n, z->f) != n) z->failed = 1;
    z->pos += 30 + (unsigned long)nl + (unsigned long)n;
}

int zipw_close(ZipW *z, int *files, double *bytes)
{
    unsigned long cd = z->pos, cdsize = 0;
    for (int i = 0; i < z->n && !z->failed; i++) {           /* central directory */
        Entry *e = &z->e[i];
        unsigned char h[46] = {0};
        size_t nl = strlen(e->name);
        put32(h, 0x02014b50);
        put16(h + 4, 20);
        put16(h + 6, 20);
        put16(h + 8, 0x0800);
        put32(h + 16, e->crc);
        put32(h + 20, e->size);
        put32(h + 24, e->size);
        put16(h + 28, (unsigned)nl);
        put32(h + 42, e->offset);
        if (fwrite(h, 1, 46, z->f) != 46 || fwrite(e->name, 1, nl, z->f) != nl) z->failed = 1;
        cdsize += 46 + (unsigned long)nl;
    }
    unsigned char end[22] = {0};
    put32(end, 0x06054b50);
    put16(end + 8, (unsigned)z->n);
    put16(end + 10, (unsigned)z->n);
    put32(end + 12, cdsize);
    put32(end + 16, cd);
    if (fwrite(end, 1, 22, z->f) != 22) z->failed = 1;
    if (fclose(z->f)) z->failed = 1;
    int ok = !z->failed;
    if (files) *files = z->n;
    if (bytes) *bytes = (double)z->pos + cdsize + 22;
    free(z->e);
    free(z);
    return ok;
}
