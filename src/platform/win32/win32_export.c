/* "Export data for the phone app": the data folder (as extracted from the player's disc, with the HD / widescreen
 * pictures if made) written to one .zip file that the Android / web app imports, so the disc does not have to be
 * extracted again on the phone. Names relative to the data folder; art/work (the AI agent's sheets and results) is
 * left out. The ZIP format itself is in ../common/zip_write.c (shared with the SDL build); this file walks the folder. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zip_write.h"

int win32_export_zip(const wchar_t *datadir, const wchar_t *zippath, char *err, int errlen);

static void add_file(ZipW *z, const wchar_t *path, const char *name)
{
    FILE *in = _wfopen(path, L"rb");
    if (!in) { zipw_fail(z); return; }
    fseek(in, 0, SEEK_END);
    long n = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *d = malloc((size_t)n + 1);
    if (!d || fread(d, 1, (size_t)n, in) != (size_t)n) zipw_fail(z);
    else zipw_add(z, name, d, (size_t)n);
    fclose(in);
    free(d);
}

static void add_dir(ZipW *z, const wchar_t *dir, const char *rel)
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
    } while (FindNextFileW(h, &fd));
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
    FILE *f = _wfopen(zippath, L"wb");
    ZipW *z = f ? zipw_open(f) : NULL;
    if (!z) { snprintf(err, (size_t)errlen, "Could not create the file."); return 0; }
    add_dir(z, datadir, "");
    int files;
    double bytes;
    if (!zipw_close(z, &files, &bytes)) {
        snprintf(err, (size_t)errlen, "Writing the file failed (disk full?).");
        DeleteFileW(zippath);
        return 0;
    }
    snprintf(err, (size_t)errlen, "%d files, %.1f MB", files, bytes / 1048576.0);
    return 1;
}
