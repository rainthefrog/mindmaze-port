/* First start on Windows: find the data folder, or build it from the player's Encarta 2006 disc with the built-in
 * extractor (src/extract). Nothing else needs to be installed: the exe is the whole download.
 *
 * Where the data lives ("home" = the folder holding data\, mindmaze.ini and the player's files):
 *   0. --home <dir> on the command line, if given;
 *   1. next to MindMaze.exe, if data\tables.json is there, or if that folder is writable (portable use);
 *   2. otherwise %LOCALAPPDATA%\MindMaze\ (e.g. when the exe sits in Program Files).
 * The disc: --disc <path> (or the bare argument) skips the questions. quiet = an action flag runs: no windows,
 * messages on stderr (../common/cli.h).
 */
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "mx.h"
#include "cli.h"

int win32_setup(HINSTANCE inst, const wchar_t *exedir, const char *home_arg, const char *disc_arg, int quiet,
                wchar_t *home, int homelen);
int win32_add_disc_quiet(HINSTANCE inst, const wchar_t *datadir, const char *path);

static char *utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc((size_t)n + 1);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static wchar_t *wide16(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * ((size_t)n + 1));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static int exists(const wchar_t *dir, const wchar_t *name)
{
    wchar_t p[MAX_PATH * 2];
    swprintf(p, MAX_PATH * 2, L"%ls%ls", dir, name);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
static int writable(const wchar_t *dir)
{
    wchar_t p[MAX_PATH * 2];
    CreateDirectoryW(dir, NULL);
    swprintf(p, MAX_PATH * 2, L"%ls~mindmaze-write-test.tmp", dir);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

/* ---- MXIO on Win32 files ---- */
static wchar_t g_out[MAX_PATH * 2];
static HWND g_text;
static const wchar_t *g_title = L"Preparing MindMaze from your Encarta disc...";

static void *io_open(MXIO *io, const char *path, int64_t *size)
{
    (void)io;
    wchar_t *w = wide16(path);
    if (!w) return NULL;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return NULL; }
    *size = sz.QuadPart;
    return h;
}
static int io_read(MXIO *io, void *f, int64_t off, void *buf, size_t n)
{
    (void)io;
    LARGE_INTEGER o;
    o.QuadPart = off;
    if (!SetFilePointerEx((HANDLE)f, o, NULL, FILE_BEGIN)) return 0;
    char *b = buf;
    while (n) {
        DWORD chunk = n > (1u << 24) ? 1u << 24 : (DWORD)n, got = 0;
        if (!ReadFile((HANDLE)f, b, chunk, &got, NULL) || got != chunk) return 0;
        b += got;
        n -= got;
    }
    return 1;
}
static void io_close(MXIO *io, void *f) { (void)io; CloseHandle((HANDLE)f); }
static int io_write(MXIO *io, const char *rel, const void *d, size_t n)
{
    (void)io;
    wchar_t *w = wide16(rel), p[MAX_PATH * 2];
    if (!w) return 0;
    swprintf(p, MAX_PATH * 2, L"%ls%ls", g_out, w);
    free(w);
    for (wchar_t *s = p + wcslen(g_out); *s; s++)                    /* sub-folders (res\, packs\, art\) */
        if (*s == L'/' || *s == L'\\') { *s = 0; CreateDirectoryW(p, NULL); *s = L'\\'; }
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, d, (DWORD)n, &put, NULL) && put == n;
    return CloseHandle(h) && ok;
}
static int g_quiet;                                           /* an action flag runs: no windows, progress on stderr */
static void io_progress(MXIO *io, int pct, const char *step)
{
    (void)io;
    if (g_quiet) {
        static int last = -100;
        if (pct >= last && pct < last + 10 && pct != 100) return;  /* every 10 % (a new step may start at 0 again) */
        last = pct;
        fprintf(stderr, "progress %.2f %s\n", pct / 100.0, step ? step : "");
        return;
    }
    if (!g_text) return;
    wchar_t buf[200], *w = wide16(step);
    swprintf(buf, 200, L"%ls\n\n%ls (%d%%)", g_title, w ? w : L"", pct);
    free(w);
    SetWindowTextW(g_text, buf);
    UpdateWindow(g_text);
    MSG m;                                                    /* keep the window responsive */
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

/* an inserted or mounted Encarta disc on any drive: 2006 (EE\ENCARTA\MINDMAZE.EIT) or 95 / 97 / 98 (CHANGE 2026-11-18) */
static int find_disc(wchar_t *root)
{
    DWORD drives = GetLogicalDrives();
    for (int d = 2; d < 26; d++) {
        if (!(drives & (1u << d))) continue;
        wchar_t r[8];
        swprintf(r, 8, L"%lc:\\", L'A' + d);
        UINT type = GetDriveTypeW(r);
        if (type != DRIVE_CDROM && type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;
        UINT old = SetErrorMode(SEM_FAILCRITICALERRORS);               /* empty drives: no "insert a disk" box */
        int ok = (exists(r, L"EE\\ENCARTA\\MINDMAZE.EIT") && exists(r, L"EE\\ENCARTA\\ENCARTA.EXE")) ||
                 exists(r, L"ENCYC95\\MM\\INTERACT\\MINDMAZE.DB") || exists(r, L"ENCYC97\\MINDMAZE.DB") ||
                 exists(r, L"ENCYC98\\MINDMAZE.DB");
        SetErrorMode(old);
        if (ok) { wcscpy(root, r); return 1; }
    }
    return 0;
}

/* file dialog: a disc image, or any of the five files on a disc / in a copied folder */
static int pick_source(wchar_t *src, int n)
{
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW of = {0};
    of.lStructSize = sizeof of;
    of.lpstrFilter = L"Encarta disc image or files (*.iso, ENCARTA.EXE, MINDMAZE.EIT, MINDMAZE.DB)\0"
                     L"*.iso;ENCARTA.EXE;MINDMAZE.EIT;MINDMAZE.DB\0"
                     L"All files\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrTitle = L"MindMaze: choose your Encarta disc image (.iso), or ENCARTA.EXE / MINDMAZE.DB on the disc";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return 0;
    size_t len = wcslen(file);
    if (len > 4 && !_wcsicmp(file + len - 4, L".iso")) { wcsncpy(src, file, (size_t)n); return 1; }
    wchar_t *slash = wcsrchr(file, L'\\');                         /* one of the files: use its folder */
    if (slash) *slash = 0;
    wcsncpy(src, file, (size_t)n);
    return 1;
}

/* add: only the disc's questions (mx_add_edition) into an existing data folder; id/name/count receive the edition */
static int run_extract2(HINSTANCE inst, const wchar_t *src, const wchar_t *datadir, int add, char *id, char *name,
                        int *count, char *err, int errlen);
static int run_extract(HINSTANCE inst, const wchar_t *src, const wchar_t *datadir, char *err, int errlen)
{
    return run_extract2(inst, src, datadir, 0, NULL, NULL, NULL, err, errlen);
}
static int run_extract2(HINSTANCE inst, const wchar_t *src, const wchar_t *datadir, int add, char *id, char *name,
                        int *count, char *err, int errlen)
{
    wcscpy(g_out, datadir);
    CreateDirectoryW(datadir, NULL);
    /* small progress window (none for an action flag) */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = 460, h = 130;
    HWND win = g_quiet ? NULL : CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER,
                               (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, w, h, NULL, NULL, inst, NULL);
    g_text = win;
    if (win) SendMessageW(win, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    MXIO io = {io_open, io_read, io_close, io_write, io_progress, NULL};
    mx_files = mx_embedded;
    wchar_t s[MAX_PATH * 2];
    wcscpy(s, src);
    size_t len = wcslen(s);
    while (len > 2 && (s[len - 1] == L'\\' || s[len - 1] == L'/')) s[--len] = 0;     /* "E:\" -> "E:" */
    if (len == 3 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')) s[2] = 0;
    char *u = utf8(s);
    g_title = add ? L"Reading the questions from the disc..." : L"Preparing MindMaze from your Encarta disc...";
    int ok = u && (add ? mx_add_edition(&io, u, id, 16, name, 64, count, err, errlen) : mx_extract(&io, u, err, errlen));
    free(u);
    SetCursor(old);
    if (win) DestroyWindow(win);
    g_text = NULL;
    return ok;
}

/* a path from the command line -> the extractor's source (like pick_source): a disc image or a folder as it is, a
 * file on the disc -> its folder; relative paths are made absolute */
static void source_of(const char *path, wchar_t *src, int n)
{
    wchar_t *w = wide16(path);
    src[0] = 0;
    if (!w) return;
    if (!GetFullPathNameW(w, (DWORD)n, src, NULL)) wcsncpy(src, w, (size_t)n);
    free(w);
    DWORD a = GetFileAttributesW(src);
    size_t len = wcslen(src);
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && !(len > 4 && !_wcsicmp(src + len - 4, L".iso"))) {
        wchar_t *slash = wcsrchr(src, L'\\');
        if (slash) *slash = 0;
    }
}

static void say(int quiet, const wchar_t *msg)                /* an error: stderr for an action flag, else a box */
{
    if (!quiet) { MessageBoxW(NULL, msg, L"MindMaze", MB_ICONERROR); return; }
    char *u = utf8(msg);
    fprintf(stderr, "MindMaze: %s\n", u ? u : "error");
    free(u);
}

int win32_setup(HINSTANCE inst, const wchar_t *exedir, const char *home_arg, const char *disc_arg, int quiet,
                wchar_t *home, int homelen)
{
    wchar_t local[MAX_PATH * 2] = L"", tmp[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", tmp, MAX_PATH);
    if (n && n < MAX_PATH) swprintf(local, MAX_PATH * 2, L"%ls\\MindMaze\\", tmp);

    if (home_arg) {                                               /* --home <dir> */
        wchar_t *w = wide16(home_arg);
        if (!w || !GetFullPathNameW(w, (DWORD)homelen - 1, home, NULL)) { free(w); say(quiet, L"The --home folder is not valid."); return 0; }
        free(w);
        size_t len = wcslen(home);
        if (len && home[len - 1] != L'\\' && home[len - 1] != L'/') wcscat(home, L"\\");
        SHCreateDirectoryExW(NULL, home, NULL);                   /* (with its parents) */
        if (!writable(home)) {
            wchar_t m[MAX_PATH * 2 + 80];
            swprintf(m, MAX_PATH * 2 + 80, L"MindMaze cannot write to the folder %ls.", home);
            say(quiet, m);
            return 0;
        }
    } else if (exists(exedir, L"data\\tables.json")) wcsncpy(home, exedir, (size_t)homelen);
    else if (*local && exists(local, L"data\\tables.json")) wcsncpy(home, local, (size_t)homelen);
    /* first start: where to put the data */
    else if (writable(exedir)) wcsncpy(home, exedir, (size_t)homelen);
    else if (*local) { CreateDirectoryW(local, NULL); wcsncpy(home, local, (size_t)homelen); }
    else { say(quiet, L"MindMaze cannot find a folder to write its data to."); return 0; }
    if (exists(home, L"data\\tables.json")) return 1;
    wchar_t datadir[MAX_PATH * 2];
    swprintf(datadir, MAX_PATH * 2, L"%lsdata\\", home);

    wchar_t src[MAX_PATH * 2] = L"";
    char err[1024];
    if (disc_arg) {                                               /* --disc <path>: no questions */
        source_of(disc_arg, src, MAX_PATH * 2);
        g_quiet = quiet;
        if (quiet) {
            char *u = utf8(src);
            fprintf(stderr, "MindMaze: preparing the data folder from %s\n", u ? u : "");
            free(u);
        }
        int ok = run_extract(inst, src, datadir, err, sizeof err);
        g_quiet = 0;
        if (ok) return 1;
        if (quiet) { fprintf(stderr, "MindMaze: %s\n", err[0] ? err : "the extraction failed"); return 0; }
        wchar_t *w = wide16(err), msg[1400];
        swprintf(msg, 1400, L"%ls\n\nChoose another disc, image or folder?", w ? w : L"The extraction failed.");
        free(w);
        if (MessageBoxW(NULL, msg, L"MindMaze", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 0;
    } else if (quiet) {
        char *u = utf8(home);
        fprintf(stderr, "MindMaze: no data folder in %s yet: give the Encarta disc with --disc <path>\n", u ? u : "");
        free(u);
        return 0;
    }
    int ask_disc = disc_arg ? 0 : find_disc(src);
    if (disc_arg && !pick_source(src, MAX_PATH * 2)) return 0;
    for (int first = 1;; first = 0) {
        if (disc_arg && first) {                                  /* the given disc failed: chosen just above */
        } else if (ask_disc) {
            wchar_t msg[600];
            swprintf(msg, 600, L"MindMaze uses the pictures, sounds and questions from your own Microsoft Encarta "
                               L"disc. They are copied once into:\n%ls\n\nFound an Encarta disc in %ls. Use it?\n\n"
                               L"(No: choose a disc image (.iso) or another folder.)", datadir, src);
            int r = MessageBoxW(NULL, msg, L"MindMaze - first start", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDNO && !pick_source(src, MAX_PATH * 2)) return 0;
        } else {
            wchar_t msg[600];
            swprintf(msg, 600, L"MindMaze uses the pictures, sounds and questions from your own Microsoft Encarta disc "
                               L"(2006 Premium, or Encarta 95 MPEG, 96 Macintosh, 97 or 98). They are copied once into:\n%ls\n\n"
                               L"Next, choose the disc image (.iso), or on the disc ENCARTA.EXE (2006, folder EE\\ENCARTA) "
                               L"or MINDMAZE.DB (95 to 98).",
                     datadir);
            if (MessageBoxW(NULL, msg, L"MindMaze - first start", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return 0;
            if (!pick_source(src, MAX_PATH * 2)) return 0;
        }
        if (run_extract(inst, src, datadir, err, sizeof err)) return 1;
        wchar_t *w = wide16(err), msg[1400];
        swprintf(msg, 1400, L"%ls\n\nTry another disc, image or folder?", w ? w : L"The extraction failed.");
        free(w);
        if (MessageBoxW(NULL, msg, L"MindMaze", MB_RETRYCANCEL | MB_ICONWARNING) != IDRETRY) return 0;
        ask_disc = 0;
    }
}

/* Settings > "Add questions from a disc" (CHANGE 2026-11-18): an inserted disc, or a chosen image / MINDMAZE.DB /
 * ENCARTA.EXE -> data\editions\<id>\. 1 = added (id[16], name[64], count), 0 = error (err), -1 = cancelled */
int win32_add_disc(HINSTANCE inst, const wchar_t *datadir, char *id, char *name, int *count, char *err, int errlen)
{
    wchar_t src[MAX_PATH * 2] = L"";
    if (find_disc(src)) {
        wchar_t msg[400];
        swprintf(msg, 400, L"Found an Encarta disc in %ls. Add its questions?\n\n(No: choose a disc image (.iso), or "
                           L"MINDMAZE.DB / ENCARTA.EXE on another disc.)", src);
        int r = MessageBoxW(NULL, msg, L"MindMaze - add questions", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return -1;
        if (r == IDNO && !pick_source(src, MAX_PATH * 2)) return -1;
    } else if (!pick_source(src, MAX_PATH * 2)) return -1;
    return run_extract2(inst, src, datadir, 1, id, name, count, err, errlen);
}

/* --add-disc <path> (no window): the disc's questions -> data\editions\<id>\, then registered as a source */
int win32_add_disc_quiet(HINSTANCE inst, const wchar_t *datadir, const char *path)
{
    wchar_t src[MAX_PATH * 2];
    char id[16] = "", name[64] = "", err[1024] = "", msg[1024];
    int count = 0;
    source_of(path, src, MAX_PATH * 2);
    char *u = utf8(src);
    fprintf(stderr, "MindMaze: reading the questions from %s\n", u ? u : "");
    free(u);
    g_quiet = 1;
    int ok = run_extract2(inst, src, datadir, 1, id, name, &count, err, sizeof err);
    g_quiet = 0;
    if (!ok) fprintf(stderr, "MindMaze: %s\n", err[0] ? err : "the disc could not be read");
    else if (!(ok = cli_add_edition(id, name, count, msg, sizeof msg))) fprintf(stderr, "MindMaze: %s\n", msg);
    else printf("%s\n", msg);
    return ok;
}
