/* "Export data for the phone app": the data folder (as extracted from the player's disc, with the HD / widescreen
 * pictures if made) written to one .zip file that the Android / web app imports, so the disc does not have to be
 * extracted again on the phone. Plain ZIP, entries stored (the pictures and sounds hardly compress), UTF-8 names
 * relative to the data folder; art/work (the AI agent's sheets and results) is left out. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int win32_export_zip(const wchar_t *datadir, const wchar_t *zippath, char *err, int errlen);

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

typedef struct { char name[260]; unsigned long crc, size, offset; } Entry;
typedef struct { FILE *f; Entry *e; int n, cap; unsigned long pos; int failed; } Zip;

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v) { put16(p, (unsigned)(v & 0xFFFF)); put16(p + 2, (unsigned)(v >> 16)); }

static void add_file(Zip *z, const wchar_t *path, const char *name)
{
    FILE *in = _wfopen(path, L"rb");
    if (!in) { z->failed = 1; return; }
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n + 1);
    if (!d || fread(d, 1, (size_t)n, in) != (size_t)n) { fclose(in); free(d); z->failed = 1; return; }
    fclose(in);
    if (z->n == z->cap) {
        z->cap = z->cap ? z->cap * 2 : 256;
        Entry *ne = realloc(z->e, sizeof *ne * (size_t)z->cap);
        if (!ne) { free(d); z->failed = 1; return; }
        z->e = ne;
    }
    Entry *e = &z->e[z->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->crc = crc32_of(d, (size_t)n);
    e->size = (unsigned long)n;
    e->offset = z->pos;
    unsigned char h[30] = {0};
    size_t nl = strlen(name);
    put32(h, 0x04034b50);
    put16(h + 4, 20);
    put16(h + 6, 0x0800);                                   /* UTF-8 names */
    put32(h + 14, e->crc);
    put32(h + 18, e->size);
    put32(h + 22, e->size);
    put16(h + 26, (unsigned)nl);
    if (fwrite(h, 1, 30, z->f) != 30 || fwrite(name, 1, nl, z->f) != nl || fwrite(d, 1, (size_t)n, z->f) != (size_t)n) z->failed = 1;
    z->pos += 30 + (unsigned long)nl + (unsigned long)n;
    free(d);
}

static void add_dir(Zip *z, const wchar_t *dir, const char *rel)
{
    wchar_t pattern[MAX_PATH * 2];
    swprintf(pattern, MAX_PATH * 2, L"%ls*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char u[260], name[300];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u, sizeof u, NULL, NULL);
        snprintf(name, sizeof name, "%s%s", rel, u);
        wchar_t path[MAX_PATH * 2];
        swprintf(path, MAX_PATH * 2, L"%ls%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!strcmp(name, "art/work")) continue;          /* the AI agent's work files: not needed to play */
            wcscat(path, L"\\");
            strcat(name, "/");
            add_dir(z, path, name);
        } else add_file(z, path, name);
    } while (!z->failed && FindNextFileW(h, &fd));
    FindClose(h);
}

int win32_export_zip(const wchar_t *datadir, const wchar_t *zippath, char *err, int errlen)
{
    wchar_t tables[MAX_PATH * 2];
    swprintf(tables, MAX_PATH * 2, L"%lstables.json", datadir);
    if (GetFileAttributesW(tables) == INVALID_FILE_ATTRIBUTES) {
        snprintf(err, (size_t)errlen, "There is no data folder to export yet.");
        return 0;
    }
    Zip z = {0};
    z.f = _wfopen(zippath, L"wb");
    if (!z.f) { snprintf(err, (size_t)errlen, "Could not create the file."); return 0; }
    add_dir(&z, datadir, "");
    unsigned long cd = z.pos, cdsize = 0;
    for (int i = 0; i < z.n && !z.failed; i++) {               /* central directory */
        Entry *e = &z.e[i];
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
        if (fwrite(h, 1, 46, z.f) != 46 || fwrite(e->name, 1, nl, z.f) != nl) z.failed = 1;
        cdsize += 46 + (unsigned long)nl;
    }
    unsigned char end[22] = {0};
    put32(end, 0x06054b50);
    put16(end + 8, (unsigned)z.n);
    put16(end + 10, (unsigned)z.n);
    put32(end + 12, cdsize);
    put32(end + 16, cd);
    if (fwrite(end, 1, 22, z.f) != 22) z.failed = 1;
    if (fclose(z.f)) z.failed = 1;
    free(z.e);
    if (z.failed) { snprintf(err, (size_t)errlen, "Writing the file failed (disk full?)."); DeleteFileW(zippath); return 0; }
    snprintf(err, (size_t)errlen, "%d files, %.1f MB", z.n, (z.pos + cdsize + 22) / 1048576.0);
    return 1;
}
