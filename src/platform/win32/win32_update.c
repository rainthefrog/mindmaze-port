/* MindMaze - built-in updater, Windows part (CHANGE 2026-11-20; portable part: ../common/update.c).
 *
 * At start (unless --no-update-check or update_check=0 in mindmaze.ini) win32_main.c asks GitHub for the newest
 * release in the background; when it is newer, win32_update_offer asks the player: Yes = update now, No = later
 * (the menu has "Check for Updates..." and the switch "Look for Updates at Start" = update_check).
 * Updating: the versioned Windows zip and checksum are downloaded, MindMaze.exe is extracted to <exe>.new, the running exe renamed to
 * <exe>.old (Windows allows renaming a running exe), <exe>.new renamed to <exe>, and the new exe started with the
 * same command line. The next start deletes <exe>.old. A read-only folder (Program Files): the release page opens.
 * --update does the same without a window (no restart). */
#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#include "cli.h"
#include "update.h"

static void asset_name(const UpdRelease *r, char *out, size_t n)
{
    const char *v = r->tag[0] == 'v' ? r->tag + 1 : r->tag;
    snprintf(out, n, "MindMaze-%s-windows-x86.zip", v);
}

void win32_update_cleanup(void);
int win32_update_cli(void);
void win32_update_offer(HWND owner, const char *json, int manual);
static wchar_t g_exe[MAX_PATH * 2];

static void exe_path(void)
{
    if (!*g_exe) GetModuleFileNameW(NULL, g_exe, MAX_PATH * 2);
}

void win32_update_cleanup(void)                                /* at start: the exe an update replaced */
{
    exe_path();
    wchar_t old[MAX_PATH * 2 + 8];
    swprintf(old, MAX_PATH * 2 + 8, L"%ls.old", g_exe);
    for (int i = 0; i < 10 && GetFileAttributesW(old) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(old); i++)
        Sleep(200);                                            /* the old process may still be ending */
}

/* ---- a small progress window (none without a window) ---- */
static HWND g_note;
static void note(const wchar_t *text)
{
    if (!g_note) {
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        int w = 420, h = 90;
        g_note = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                 (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, w, h, NULL, NULL,
                                 GetModuleHandleW(NULL), NULL);
        if (g_note) SendMessageW(g_note, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    }
    if (!g_note) return;
    SetWindowTextW(g_note, text);
    UpdateWindow(g_note);
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}
static void note_close(void) { if (g_note) DestroyWindow(g_note); g_note = NULL; }

/* GET url into memory (malloc'd, zero-terminated). show: progress in the note window. NULL on failure */
static char *download(const char *url, size_t *len, int show, const char *what)
{
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 1024);
    char *data = NULL;
    *len = 0;
    HINTERNET net = InternetOpenW(L"MindMaze-updater", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return NULL;
    DWORD to = 20000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | (strncmp(url, "https:", 6) ? 0 : INTERNET_FLAG_SECURE);
    HINTERNET h = InternetOpenUrlW(net, w, L"Accept: */*\r\n", (DWORD)-1, flags, 0);
    if (h) {
        DWORD status = 0, total = 0, sl = sizeof status;
        HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sl, NULL);
        sl = sizeof total;
        HttpQueryInfoW(h, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &sl, NULL);
        size_t cap = 1 << 16, n = 0;
        data = malloc(cap + 1);
        DWORD got, last = GetTickCount() - 1000;
        while (data && InternetReadFile(h, data + n, (DWORD)(cap - n), &got) && got) {
            n += got;
            if (n == cap) {
                char *nd = cap < (256u << 20) ? realloc(data, cap * 2 + 1) : NULL;
                if (!nd) { free(data); data = NULL; break; }
                data = nd;
                cap *= 2;
            }
            if (show && GetTickCount() - last > 100) {
                wchar_t t[200], wwhat[100];
                MultiByteToWideChar(CP_UTF8, 0, what, -1, wwhat, 100);
                if (total) swprintf(t, 200, L"%ls (%d%%)", wwhat, (int)(n * 100 / total));
                else swprintf(t, 200, L"%ls (%.1f MB)", wwhat, n / 1048576.0);
                note(t);
                last = GetTickCount();
            }
        }
        if (data) data[n] = 0;
        if (status != 200 || !data || (total && n != total)) { free(data); data = NULL; }
        else *len = n;
        InternetCloseHandle(h);
    }
    InternetCloseHandle(net);
    return data;
}

static int write_all(const wchar_t *path, const void *d, size_t n)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(f, d, (DWORD)n, &put, NULL) && put == n;
    return CloseHandle(f) && ok;
}

typedef struct { const uint8_t *exe; size_t n; } ZipExe;
static int zip_exe(void *ud, const char *path, char type, unsigned mode, const uint8_t *data, size_t n)
{
    (void)mode;
    ZipExe *z = ud;
    if (type == 'f' && !_stricmp(path, "MindMaze.exe")) { z->exe = data; z->n = n; }
    return 1;
}

/* download, check and put the new exe from the release zip in place. */
static int install(const UpdRelease *r, int show, char *err, size_t errlen)
{
    exe_path();
    char asset[160], sumname[168];
    asset_name(r, asset, sizeof asset);
    snprintf(sumname, sizeof sumname, "%s.sha256", asset);
    const char *url = upd_asset(r, asset), *sumurl = upd_asset(r, sumname);
    if (!url || !sumurl) { snprintf(err, errlen, "The release %s has no %s with its checksum.", r->tag, asset); return 0; }
    wchar_t newp[MAX_PATH * 2 + 8], oldp[MAX_PATH * 2 + 8];
    swprintf(newp, MAX_PATH * 2 + 8, L"%ls.new", g_exe);
    swprintf(oldp, MAX_PATH * 2 + 8, L"%ls.old", g_exe);
    if (!write_all(newp, "", 0)) { snprintf(err, errlen, "The program's folder is read-only."); return 0; }
    size_t sl = 0, n = 0;
    char *sum = download(sumurl, &sl, 0, ""), *arc = sum ? download(url, &n, show, "Downloading the new version") : NULL;
    int ok = 0;
    ZipExe ze = {0};
    if (!sum || !arc) snprintf(err, errlen, "The download failed (no connection?).");
    else if (!upd_sha_ok(arc, n, sum, sl)) snprintf(err, errlen, "The download is damaged (checksum differs); nothing was changed.");
    else if (!upd_unzip((const uint8_t *)arc, n, zip_exe, &ze) || !ze.exe) snprintf(err, errlen, "The download is not a MindMaze package.");
    else if (!write_all(newp, ze.exe, ze.n)) snprintf(err, errlen, "Could not write the new program.");
    else {
        DeleteFileW(oldp);
        if (!MoveFileExW(g_exe, oldp, MOVEFILE_REPLACE_EXISTING)) snprintf(err, errlen, "Could not move the running program aside.");
        else if (!MoveFileExW(newp, g_exe, MOVEFILE_REPLACE_EXISTING)) {
            MoveFileExW(oldp, g_exe, MOVEFILE_REPLACE_EXISTING);      /* put the old one back */
            snprintf(err, errlen, "Could not put the new program in place.");
        } else ok = 1;
    }
    if (!ok) DeleteFileW(newp);
    free(sum);
    free(arc);
    return ok;
}

static void open_page(const UpdRelease *r)
{
    wchar_t w[600];
    MultiByteToWideChar(CP_UTF8, 0, *r->page ? r->page : MM_REPO_URL "/releases/latest", -1, w, 600);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

/* the release JSON arrived (async check at start, or the menu's check: manual = 1 also says "up to date") */
void win32_update_offer(HWND owner, const char *json, int manual)
{
    UpdRelease r;
    wchar_t msg[800];
    if (!upd_parse(json, &r)) {
        if (manual) MessageBoxW(owner, L"MindMaze could not reach GitHub to look for a new version.", L"MindMaze", MB_ICONWARNING);
        return;
    }
    char asset[160]; asset_name(&r, asset, sizeof asset);
    if (!upd_newer(r.tag, MM_VERSION) || !upd_asset(&r, asset)) {
        if (manual) {
            swprintf(msg, 800, L"MindMaze %hs is the newest version.", MM_VERSION);
            MessageBoxW(owner, msg, L"MindMaze", MB_ICONINFORMATION);
        }
        return;
    }
    swprintf(msg, 800, L"MindMaze %hs is available (you have %hs).\n\nUpdate now? MindMaze downloads it (about 6 MB), "
                       L"checks it and starts again. Your data and settings stay.\n\n(No: later. To stop this question: "
                       L"MindMaze menu > Look for Updates at Start.)",
             r.tag[0] == 'v' ? r.tag + 1 : r.tag, MM_VERSION);
    int a = MessageBoxW(owner, msg, L"MindMaze - new version", MB_YESNO | MB_ICONINFORMATION);
    if (a != IDYES) return;
    char err[300] = "";
    note(L"Downloading the new version...");
    int ok = install(&r, 1, err, sizeof err);
    note_close();
    if (!ok) {
        wchar_t w[300];
        MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 300);
        swprintf(msg, 800, L"The update did not work: %ls\n\nOpen the download page instead?", w);
        if (MessageBoxW(owner, msg, L"MindMaze", MB_YESNO | MB_ICONWARNING) == IDYES) open_page(&r);
        return;
    }
    /* start the new exe with the same command line, then end this one */
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    mm_shutdown();
    if (CreateProcessW(g_exe, GetCommandLineW(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else MessageBoxW(owner, L"MindMaze was updated. Start it again.", L"MindMaze", MB_ICONINFORMATION);
    ExitProcess(0);
}

/* --update: no window. 0 = updated or already the newest, 1 = failed */
int win32_update_cli(void)
{
    char api[600], v[512] = "", err[300] = "";
    plat_get_setting("update_url", v, sizeof v);
    if (!upd_api_url(v, api, sizeof api)) { fprintf(stderr, "MindMaze: this build has no release address\n"); return 1; }
    size_t n = 0;
    char *json = download(api, &n, 0, "");
    UpdRelease r;
    int ok = json && upd_parse(json, &r);
    free(json);
    if (!ok) { fprintf(stderr, "MindMaze: could not read the newest release from %s\n", api); return 1; }
    char asset[160]; asset_name(&r, asset, sizeof asset);
    if (!upd_newer(r.tag, MM_VERSION) || !upd_asset(&r, asset)) { printf("MindMaze %s is the newest version.\n", MM_VERSION); return 0; }
    printf("Updating MindMaze %s to %s...\n", MM_VERSION, r.tag);
    fflush(stdout);
    if (!install(&r, 0, err, sizeof err)) { fprintf(stderr, "MindMaze: the update did not work: %s\n", err); return 1; }
    printf("Updated to %s.\n", r.tag);
    return 0;
}
