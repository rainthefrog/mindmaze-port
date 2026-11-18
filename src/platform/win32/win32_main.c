/* MindMaze - Windows platform layer (window, drawing, text, input, sound mixer, MIDI music, settings).
 * Data files live in <exe folder>\data, settings in <exe folder>\mindmaze.ini.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <wininet.h>
#include <commdlg.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"
#include "mm_art.h"
int win32_setup(HINSTANCE inst, const wchar_t *exedir, wchar_t *home, int homelen);   /* win32_setup.c */
int win32_export_zip(const wchar_t *datadir, const wchar_t *zippath, char *err, int errlen);   /* win32_export.c */
static HWND busy_window(HINSTANCE inst, const wchar_t *text);
static void export_for_phone(void);
int plat_can_export(void) { return 1; }
int win32_add_disc(HINSTANCE inst, const wchar_t *datadir, char *id, char *name, int *count, char *err, int errlen);
static wchar_t g_datadir[MAX_PATH];
void plat_add_disc(void)                                    /* Settings > Add questions from a disc (win32_setup.c) */
{
    char id[16] = "", name[64] = "", err[1024] = "";
    int count = 0, r = win32_add_disc(GetModuleHandleW(NULL), g_datadir, id, name, &count, err, sizeof err);
    mm_disc_added(r > 0, id, name, count, r == 0 ? err : NULL);
}
void plat_export_data(void) { export_for_phone(); }

#define TIMER_ID 1
#define IDM_NEW 101
#define IDM_QUIT 102
#define IDM_MUSIC 103
#define IDM_SOUND 104
#define IDM_HD 105
#define IDM_WIDE 106
#define IDM_FULL 107
#define IDM_EXPORT 108

static HWND g_hwnd;
static HMENU g_menu;
static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH];
static HDC g_memdc;
static HBITMAP g_dib;
static MMCanvas g_canvas;
static int g_scale = 1, g_offx, g_offy, g_dirty = 1, g_hand;
static HFONT g_fonts[FONT_COUNT];
static int g_font_scale;

/* ---- UTF-8 <-> UTF-16 ---- */
static wchar_t *wide(const char *s, wchar_t *buf, int n)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, n)) buf[0] = 0;
    return buf;
}
static char *narrow(const wchar_t *s, char *buf, int n)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, n, NULL, NULL)) buf[0] = 0;
    return buf;
}

/* ---- files ---- */
void *plat_load(const char *relpath, int *size)
{
    wchar_t w[MAX_PATH], path[MAX_PATH * 2];
    wide(relpath, w, MAX_PATH);
    for (wchar_t *p = w; *p; p++) if (*p == '/') *p = '\\';
    if (w[0] == '\\' || (w[0] && w[1] == ':')) wcscpy(path, w);
    else swprintf(path, MAX_PATH * 2, L"%ls%ls", g_datadir, w);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(n + 1);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[n] = 0;
    if (size) *size = (int)n;
    return d;
}

static void data_path(const char *rel, wchar_t *path)
{
    wchar_t w[MAX_PATH];
    wide(rel, w, MAX_PATH);
    for (wchar_t *p = w; *p; p++) if (*p == '/') *p = '\\';
    swprintf(path, MAX_PATH * 2, L"%ls%ls", g_datadir, w);
}

/* files of the built-in art tools (src/art): sub-folders are created as needed */
int plat_save_data(const char *rel, const void *data, int len)
{
    wchar_t path[MAX_PATH * 2];
    data_path(rel, path);
    for (wchar_t *s = path + wcslen(g_datadir); *s; s++)
        if (*s == L'\\') { *s = 0; CreateDirectoryW(path, NULL); *s = L'\\'; }
    FILE *f = _wfopen(path, L"wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    return fclose(f) == 0 && ok;
}

long plat_data_size(const char *rel)
{
    wchar_t path[MAX_PATH * 2];
    WIN32_FILE_ATTRIBUTE_DATA a;
    data_path(rel, path);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return (long)a.nFileSizeLow;
}

/* ---- user files next to the exe (custom questions, online cache, seen list) ---- */
static void user_path(const char *name, wchar_t *path)
{
    wchar_t w[MAX_PATH];
    wide(name, w, MAX_PATH);
    swprintf(path, MAX_PATH * 2, L"%ls%ls", g_dir, w);
}

void *plat_load_user(const char *name, int *size)
{
    wchar_t path[MAX_PATH * 2];
    user_path(name, path);
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(n + 1);
    if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[n] = 0;
    if (size) *size = (int)n;
    return d;
}

int plat_save_user(const char *name, const void *data, int len)   /* write a temp file, then replace */
{
    wchar_t path[MAX_PATH * 2], tmp[MAX_PATH * 2 + 8];
    user_path(name, path);
    swprintf(tmp, MAX_PATH * 2 + 8, L"%ls.tmp", path);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == (size_t)len;
    ok &= fclose(f) == 0;
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp);
    return ok;
}

void plat_append_user(const char *name, const char *text)
{
    wchar_t path[MAX_PATH * 2];
    user_path(name, path);
    FILE *f = _wfopen(path, L"ab");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* ---- online questions: one GET at a time on a thread (WinINet), result posted to the window ---- */
#define WM_HTTP_DONE (WM_APP + 2)
typedef struct { char url[512]; int tag; char *data; int len; } HttpJob;

static DWORD WINAPI http_thread(LPVOID arg)
{
    HttpJob *j = arg;
    HINTERNET net = InternetOpenW(L"MindMaze/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (net) {
        DWORD to = 15000;
        InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof to);
        InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof to);
        wchar_t w[512];
        wide(j->url, w, 512);
        HINTERNET h = InternetOpenUrlW(net, w, NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
        if (h) {
            DWORD status = 0, sl = sizeof status;
            HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sl, NULL);
            int cap = 65536;
            j->data = malloc(cap + 1);
            DWORD got;
            while (InternetReadFile(h, j->data + j->len, cap - j->len, &got) && got) {
                j->len += got;
                if (j->len == cap) { cap *= 2; j->data = realloc(j->data, cap + 1); }
                if (j->len > (4 << 20)) break;
            }
            j->data[j->len] = 0;
            if (status != 200) { free(j->data); j->data = NULL; j->len = 0; }
            InternetCloseHandle(h);
        }
        InternetCloseHandle(net);
    }
    PostMessageW(g_hwnd, WM_HTTP_DONE, 0, (LPARAM)j);
    return 0;
}

void plat_http_get(const char *url, int tag)
{
    HttpJob *j = calloc(1, sizeof *j);
    snprintf(j->url, sizeof j->url, "%s", url);
    j->tag = tag;
    HANDLE t = CreateThread(NULL, 0, http_thread, j, 0, NULL);
    if (t) CloseHandle(t);
    else { free(j); mm_http_done(tag, NULL, 0); }
}

uint32_t plat_ticks(void) { return GetTickCount(); }

/* ---- PNG through WIC (HD pictures) ---- */
static IWICImagingFactory *g_wic;

MMImage *plat_load_png(const char *relpath)
{
    wchar_t w[MAX_PATH], path[MAX_PATH * 2];
    wide(relpath, w, MAX_PATH);
    for (wchar_t *p = w; *p; p++) if (*p == '/') *p = '\\';
    swprintf(path, MAX_PATH * 2, L"%ls%ls", g_datadir, w);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    if (!g_wic) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&g_wic)))
            return NULL;
    }
    MMImage *im = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT iw = 0, ih = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(g_wic, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_wic, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, &iw, &ih)) && iw && ih) {
        im = malloc(sizeof *im);
        im->w = (int)iw;
        im->h = (int)ih;
        im->px = malloc((size_t)iw * ih * 4);                 /* BGRA bytes = 0xAARRGGBB words */
        if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, iw * 4, iw * ih * 4, (BYTE *)im->px))) {
            free(im->px);
            free(im);
            im = NULL;
        }
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    return im;
}

/* ---- settings (UTF-16 ini so player names keep any characters) ---- */
int plat_get_setting(const char *key, char *buf, int n)
{
    wchar_t k[128], v[1024];
    GetPrivateProfileStringW(L"MindMaze", wide(key, k, 128), L"\x01", v, 1024, g_ini);
    if (v[0] == 1 && !v[1]) return 0;
    narrow(v, buf, n);
    return 1;
}

void plat_set_setting(const char *key, const char *value)
{
    wchar_t k[128], v[1024];
    WritePrivateProfileStringW(L"MindMaze", wide(key, k, 128), value ? wide(value, v, 1024) : NULL, g_ini);
}

void plat_open_url(const char *url)
{
    wchar_t w[2048];
    ShellExecuteW(g_hwnd, L"open", wide(url, w, 2048), NULL, NULL, SW_SHOWNORMAL);
}

int plat_art_import(int go) { (void)go; return 0; }   /* the art is made on this computer */
void plat_open_data_folder(const char *rel)
{
    wchar_t w[256], path[MAX_PATH * 2];
    swprintf(path, MAX_PATH * 2, L"%ls%ls", g_datadir, wide(rel, w, 256));
    for (wchar_t *p = path; *p; p++) if (*p == L'/') *p = L'\\';
    ShellExecuteW(g_hwnd, L"explore", path, NULL, NULL, SW_SHOWNORMAL);
}

void plat_set_cursor(int hand)
{
    if (hand == g_hand) return;
    g_hand = hand;
    POINT pt;
    RECT rc;
    if (g_hwnd && GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) && GetClientRect(g_hwnd, &rc) && PtInRect(&rc, pt))
        SetCursor(LoadCursor(NULL, hand ? IDC_HAND : IDC_ARROW));   /* now, not at the next mouse move */
}
void plat_invalidate(void) { g_dirty = 1; if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE); }
void plat_quit(void) { DestroyWindow(g_hwnd); }

/* ---- font faces. CHANGE 2026-11-18: Wine / Proton without the Microsoft fonts substituted a much wider face for
 * Tahoma (DejaVu Sans), which cut off panel labels ("Physical Scienc"). A face missing from the system is replaced
 * by the open font built into the exe (mindmaze.rc, the same fonts the web / Android build uses), loaded for this
 * process only. On Windows every face exists and nothing changes. ---- */
static const wchar_t *g_tahoma = L"Tahoma", *g_arial = L"Arial", *g_times = L"Times New Roman",
                     *g_palatino = L"Palatino Linotype";

static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found)
{
    (void)lf; (void)tm; (void)type;
    *(int *)found = 1;
    return 0;
}

static int font_exists(const wchar_t *face)
{
    LOGFONTW lf = {0};
    int found = 0;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static void font_load(const wchar_t *res)                /* an RCDATA font from mindmaze.rc, private to this process */
{
    HRSRC r = FindResourceW(NULL, res, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(NULL, r) : NULL;
    DWORD n = 0;
    if (g) AddFontMemResourceEx(LockResource(g), SizeofResource(NULL, r), NULL, &n);
}

static void fonts_init(void)
{
    if (!font_exists(L"Tahoma") || !font_exists(L"Arial")) {
        font_load(L"FONT_SANS");
        font_load(L"FONT_SANS_B");
        font_load(L"FONT_SANS_I");
        if (!font_exists(L"Tahoma")) g_tahoma = L"Liberation Sans";
        if (!font_exists(L"Arial")) g_arial = L"Liberation Sans";
    }
    if (!font_exists(L"Times New Roman")) {
        font_load(L"FONT_SERIF");
        font_load(L"FONT_SERIF_B");
        g_times = L"Liberation Serif";
    }
    if (!font_exists(L"Palatino Linotype")) {
        font_load(L"FONT_PAGELLA_I");
        g_palatino = L"TeX Gyre Pagella";
    }
}

/* ---- text: Tahoma 8pt (Encarta font resource 32001), bold, Arial for the player list (4921) ---- */
static void make_fonts(int s)
{
    if (g_font_scale == s) return;
    for (int i = 0; i < FONT_COUNT; i++) if (g_fonts[i]) DeleteObject(g_fonts[i]);
    g_fonts[FONT_TEXT] = CreateFontW(-11 * s, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_tahoma);
    g_fonts[FONT_BOLD] = CreateFontW(-11 * s, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_tahoma);
    g_fonts[FONT_LIST] = CreateFontW(-13 * s, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_arial);
    g_fonts[FONT_SMALL] = CreateFontW(-9 * s, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_tahoma);
    /* Panel fonts from the original's FONT resources (ENCARTAR.DLL RCDATA, "FONT" records: 0x65 height, 0x69 weight,
     * 0x72 face string 20010 Arial / 20011 Times New Roman / 20015 Tahoma): 32001 Tahoma -11 (buttons, radios),
     * 32002 Tahoma -11 bold (headings), 4916 Arial -12 (Progress line), 4931 Times New Roman -18 (name, answer
     * letters, points), 4929 Times New Roman -14 (answer buttons). Assigned in FUN_0051edb0 / FUN_0051e630. */
    g_fonts[FONT_NAME] = CreateFontW(-18 * s, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_times);
    g_fonts[FONT_SCORE] = CreateFontW(-12 * s, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_arial);
    /* answers: Tahoma 11, centred, as in a screenshot of the original (user, 2026-11-18; was Times New Roman 14
     * from RCDATA 4929) */
    g_fonts[FONT_ANSWER] = CreateFontW(-11 * s, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_tahoma);
    g_fonts[FONT_ANSWER_I] = CreateFontW(-11 * s, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_tahoma);
    /* level numerals: bitmaps 29154-29157 have 17 px tall italic serif digits; Palatino Linotype figures are ~0.69 em,
     * so -25 px gives the same height (was -22, 15 px digits) */
    g_fonts[FONT_LEVEL] = CreateFontW(-25 * s, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_palatino);
    g_fonts[FONT_ITALIC] = CreateFontW(-11 * s, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_tahoma);
    g_fonts[FONT_POINTS] = CreateFontW(-24 * s, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_times);
    g_font_scale = s;
}

void plat_text(MMCanvas *c, int x, int y, int w, int h, const char *utf8, int font, uint32_t rgb, int flags)
{
    wchar_t buf[4096];
    wide(utf8, buf, 4096);
    int s = c->scale;
    x += c->ox;                                           /* widescreen margin */
    RECT r = {x * s, y * s, (x + w) * s, (y + h) * s};
    UINT f = DT_NOPREFIX;
    if (flags & TX_CENTER) f |= DT_CENTER;
    if (flags & TX_RIGHT) f |= DT_RIGHT;
    if (flags & TX_WRAP) f |= DT_WORDBREAK;
    else f |= DT_SINGLELINE;
    if (flags & TX_VCENTER) f |= DT_VCENTER;
    if (flags & TX_ELLIPSIS) f |= DT_END_ELLIPSIS;
    SelectObject(g_memdc, g_fonts[font]);
    SetBkMode(g_memdc, TRANSPARENT);
    SetTextColor(g_memdc, RGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
    DrawTextW(g_memdc, buf, -1, &r, f);
    GdiFlush();
}

int plat_text_px_width(MMCanvas *c, const char *utf8, int font)
{
    wchar_t buf[1024];
    SIZE sz = {0};
    (void)c;
    wide(utf8, buf, 1024);
    SelectObject(g_memdc, g_fonts[font]);
    GetTextExtentPoint32W(g_memdc, buf, (int)wcslen(buf), &sz);
    return sz.cx;
}

int plat_text_px_height(MMCanvas *c, int font)
{
    TEXTMETRICW tm;
    (void)c;
    SelectObject(g_memdc, g_fonts[font]);
    GetTextMetricsW(g_memdc, &tm);
    return tm.tmHeight;
}

int plat_text_px_ascent(MMCanvas *c, int font)
{
    TEXTMETRICW tm;
    (void)c;
    SelectObject(g_memdc, g_fonts[font]);
    GetTextMetricsW(g_memdc, &tm);
    return tm.tmAscent;
}

void plat_text_px(MMCanvas *c, int X, int Y, const char *utf8, int font, uint32_t rgb, int cx0, int cy0, int cx1, int cy1)
{
    wchar_t buf[1024];
    int m = c->ox * c->scale;                             /* widescreen margin (callers use view pixels) */
    X += m;
    cx0 += m;
    cx1 += m;
    wide(utf8, buf, 1024);
    RECT clip = {cx0, cy0, cx1, cy1};
    SelectObject(g_memdc, g_fonts[font]);
    SetBkMode(g_memdc, TRANSPARENT);
    SetTextAlign(g_memdc, TA_LEFT | TA_TOP | TA_NOUPDATECP);
    SetTextColor(g_memdc, RGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
    ExtTextOutW(g_memdc, X, Y, ETO_CLIPPED, &clip, buf, (UINT)wcslen(buf), NULL);
    GdiFlush();
}

int plat_text_width(const char *utf8, int font)
{
    wchar_t buf[1024];
    SIZE sz = {0};
    wide(utf8, buf, 1024);
    SelectObject(g_memdc, g_fonts[font]);
    GetTextExtentPoint32W(g_memdc, buf, (int)wcslen(buf), &sz);
    return sz.cx / (g_scale ? g_scale : 1);
}

/* ---- sound effects: small mixer on waveOut, 22050 Hz mono 16-bit ---- */
#define RATE 22050
#define NBUF 4
#define BUFLEN 1024
#define NVOICE 8
typedef struct { char name[24]; short *pcm; int n; } Sample;
static Sample g_samples[48];
static int g_nsamples;
static struct { const Sample *s; int pos; } g_voice[NVOICE];
static CRITICAL_SECTION g_mixlock;
static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NBUF];
static short g_buf[NBUF][BUFLEN];
static HANDLE g_wo_event;

static const Sample *load_sample(const char *name)
{
    for (int i = 0; i < g_nsamples; i++) if (!_stricmp(g_samples[i].name, name)) return &g_samples[i];
    if (g_nsamples >= 48) return NULL;
    char path[64];
    snprintf(path, sizeof path, "%s", name);
    for (char *p = path; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    int size = 0;
    unsigned char *d = plat_load(path, &size);
    Sample *s = &g_samples[g_nsamples];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    if (d && size > 12 && !memcmp(d, "RIFF", 4)) {
        int ch = 1, bits = 16, rate = RATE, fmt = 1;
        for (int p = 12; p + 8 <= size;) {
            int len = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | d[p + 7] << 24;
            if (!memcmp(d + p, "fmt ", 4)) {
                fmt = d[p + 8] | d[p + 9] << 8;
                ch = d[p + 10];
                rate = d[p + 12] | d[p + 13] << 8 | d[p + 14] << 16;
                bits = d[p + 22];
            } else if (!memcmp(d + p, "data", 4) && fmt == 1) {
                int frames = len / (ch * bits / 8);
                if (p + 8 + len > size) frames = (size - p - 8) / (ch * bits / 8);
                long long out = (long long)frames * RATE / rate;
                s->pcm = malloc(sizeof(short) * (out + 1));
                for (long long i = 0; i < out; i++) {
                    int f = (int)(i * rate / RATE);
                    const unsigned char *q = d + p + 8 + f * ch * bits / 8;
                    s->pcm[i] = bits == 8 ? (short)((q[0] - 128) << 8) : (short)(q[0] | q[1] << 8);
                }
                s->n = (int)out;
            }
            p += 8 + len + (len & 1);
        }
    }
    free(d);
    g_nsamples++;
    return s;
}

static volatile LONG g_sfx_vol = 256;     /* sound effects volume, 0..256 (Settings) */

static void mix(short *out)
{
    EnterCriticalSection(&g_mixlock);
    int vol = (int)g_sfx_vol;
    for (int i = 0; i < BUFLEN; i++) {
        int acc = 0;
        for (int v = 0; v < NVOICE; v++)
            if (g_voice[v].s) {
                acc += g_voice[v].s->pcm[g_voice[v].pos++];
                if (g_voice[v].pos >= g_voice[v].s->n) g_voice[v].s = NULL;
            }
        acc = acc * vol / 256;
        out[i] =(short)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
    }
    LeaveCriticalSection(&g_mixlock);
}

static DWORD WINAPI audio_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_wo_event, INFINITE);
        for (int i = 0; i < NBUF; i++)
            if (g_hdr[i].dwFlags & WHDR_DONE) {
                mix(g_buf[i]);
                g_hdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(g_wo, &g_hdr[i], sizeof g_hdr[i]);
            }
    }
    return 0;
}

static void audio_init(void)
{
    InitializeCriticalSection(&g_mixlock);
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0};
    g_wo_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, (DWORD_PTR)g_wo_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) { g_wo = NULL; return; }
    for (int i = 0; i < NBUF; i++) {
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = sizeof g_buf[i];
        waveOutPrepareHeader(g_wo, &g_hdr[i], sizeof g_hdr[i]);
        g_hdr[i].dwFlags |= WHDR_DONE;
    }
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    SetEvent(g_wo_event);
}

void plat_sound(const char *name)
{
    if (!g_wo) return;
    const Sample *s = load_sample(name);
    if (!s || !s->n) return;
    EnterCriticalSection(&g_mixlock);
    int v = 0;
    for (int i = 0; i < NVOICE; i++) if (!g_voice[i].s) { v = i; break; }
    g_voice[v].s = s;
    g_voice[v].pos = 0;
    LeaveCriticalSection(&g_mixlock);
}

/* ---- MIDI music through the MCI sequencer (one file at a time; the background is closed while the
 *      question music plays and reopened at its old position afterwards).
 *      Opening/closing MIDI files through MCI can block for hundreds of milliseconds, so every MCI call runs
 *      on a music thread: plat_music / plat_music_pause only queue a command. The thread polls the playing
 *      track and posts WM_MUSIC_ENDED to the window when it has finished. ---- */
#define WM_MUSIC_ENDED (WM_APP + 1)
static char g_mus_name[2][32];
static int g_mus_open[2], g_mus_paused[2];
static DWORD g_mus_pos[2];
static volatile LONG g_mus_gen[2];      /* bumped on the UI thread by every plat_music / plat_music_pause call */
static LONG g_mus_done_gen[2];          /* music thread: generation of the command that set up the channel */
static const wchar_t *ALIAS[2] = {L"mmbg", L"mmq"};

/* music volume (Settings): output volume of the MIDI synthesizer(s) the sequencer plays through; 0..100 */
static void music_volume_apply(int pct)
{
    DWORD v = (DWORD)(pct * 0xFFFF / 100);
    for (UINT d = 0; d < midiOutGetNumDevs(); d++) midiOutSetVolume((HMIDIOUT)(UINT_PTR)d, v | v << 16);
}

int plat_instrument_count(void) { return 0; }                    /* MIDI plays on the Windows synthesizer */
const char *plat_instrument_name(int i) { (void)i; return ""; }
int plat_instrument_get(void) { return -1; }
void plat_instrument_set(int i) { (void)i; }

static volatile LONG g_music_pct = 100;             /* music volume (Settings), for the fade */
static volatile LONG g_music_fade_ms;               /* the next switch of the background track fades the old one out */
void plat_music_fade(int ms) { InterlockedExchange(&g_music_fade_ms, ms); }

void plat_volume(int music, int sound)
{
    InterlockedExchange(&g_sfx_vol, sound * 256 / 100);
    InterlockedExchange(&g_music_pct, music);
    music_volume_apply(music);
}

static void mci(const wchar_t *fmt, ...)
{
    wchar_t cmd[600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(cmd, 600, fmt, ap);
    va_end(ap);
    mciSendStringW(cmd, NULL, 0, NULL);
}

static void mus_close(int ch)
{
    if (g_mus_open[ch]) { mci(L"close %ls", ALIAS[ch]); g_mus_open[ch] = 0; }
}

static int mus_open_play(int ch, const char *name, DWORD from)
{
    wchar_t w[64], path[MAX_PATH * 2];
    char lower[32];
    snprintf(lower, sizeof lower, "%s", name);
    for (char *p = lower; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    swprintf(path, MAX_PATH * 2, L"%ls%ls", g_datadir, wide(lower, w, 64));
    wchar_t cmd[700];
    swprintf(cmd, 700, L"open \"%ls\" type sequencer alias %ls", path, ALIAS[ch]);
    if (mciSendStringW(cmd, NULL, 0, NULL)) return 0;
    g_mus_open[ch] = 1;
    if (from) { mci(L"set %ls time format milliseconds", ALIAS[ch]); mci(L"seek %ls to %lu", ALIAS[ch], from); }
    mci(L"play %ls", ALIAS[ch]);
    return 1;
}

static void music_volume_apply(int pct);

static void music_do(int ch, const char *name)          /* runs on the music thread */
{
    LONG fade = InterlockedExchange(&g_music_fade_ms, 0);
    if (ch == 0 && fade > 0 && g_mus_open[0] && name) {   /* fade the old background track out (MCI plays one file) */
        int steps = 12, pct = (int)g_music_pct;
        for (int i = 1; i <= steps; i++) { music_volume_apply(pct * (steps - i) / steps); Sleep((DWORD)(fade / 2 / steps)); }
        mus_close(0);
        music_volume_apply(pct);
    }
    g_mus_paused[ch] = 0;
    mus_close(ch);                                       /* a switch always stops the old track first */
    if (!name) { g_mus_name[ch][0] = 0; return; }
    mus_close(!ch);                                      /* the sequencer plays one file at a time */
    snprintf(g_mus_name[ch], sizeof g_mus_name[ch], "%s", name);
    mus_open_play(ch, name, 0);
}

/* MCI cannot keep two MIDI files open, so a pause closes the file and remembers the position; resuming
 * reopens it there (closing the other channel, which the game has stopped or paused by then). */
static void music_pause_do(int ch, int pause)         /* runs on the music thread */
{
    if (pause && g_mus_open[ch]) {
        wchar_t r[32] = L"0", cmd[64];
        mci(L"set %ls time format milliseconds", ALIAS[ch]);
        swprintf(cmd, 64, L"status %ls position", ALIAS[ch]);
        mciSendStringW(cmd, r, 32, NULL);
        g_mus_pos[ch] = wcstoul(r, NULL, 10);
        mus_close(ch);
        g_mus_paused[ch] = 1;
    } else if (!pause && g_mus_paused[ch] && g_mus_name[ch][0]) {
        g_mus_paused[ch] = 0;
        mus_close(!ch);
        mus_open_play(ch, g_mus_name[ch], g_mus_pos[ch]);
    }
}

typedef struct { int op, ch, arg; LONG gen; char name[32]; } MusCmd;   /* op 0 play/stop, 1 pause, 2 quit */
static MusCmd g_mq[32];
static int g_mq_n;
static CRITICAL_SECTION g_mq_lock;
static HANDLE g_mq_event, g_mus_thread;

static void music_queue(int op, int ch, int arg, const char *name)
{
    LONG gen = op < 2 ? InterlockedIncrement(&g_mus_gen[ch]) : 0;
    EnterCriticalSection(&g_mq_lock);
    if (g_mq_n < 32) {
        MusCmd *c = &g_mq[g_mq_n++];
        c->op = op;
        c->ch = ch;
        c->arg = arg;
        c->gen = gen;
        snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    }
    LeaveCriticalSection(&g_mq_lock);
    SetEvent(g_mq_event);
}

static DWORD WINAPI music_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_mq_event, 250);
        MusCmd q[32];
        int n;
        EnterCriticalSection(&g_mq_lock);
        n = g_mq_n;
        memcpy(q, g_mq, sizeof(MusCmd) * n);
        g_mq_n = 0;
        LeaveCriticalSection(&g_mq_lock);
        for (int i = 0; i < n; i++) {
            if (q[i].op == 2) { mus_close(0); mus_close(1); return 0; }
            g_mus_done_gen[q[i].ch] = q[i].gen;
            if (q[i].op == 0) music_do(q[i].ch, q[i].name[0] ? q[i].name : NULL);
            else music_pause_do(q[i].ch, q[i].arg);
        }
        for (int ch = 0; ch < 2; ch++)                     /* finished tracks */
            if (g_mus_open[ch]) {
                wchar_t mode[32] = L"", cmd[64];
                swprintf(cmd, 64, L"status %ls mode", ALIAS[ch]);
                if (!mciSendStringW(cmd, mode, 32, NULL) && !wcscmp(mode, L"stopped")) {
                    mus_close(ch);
                    PostMessageW(g_hwnd, WM_MUSIC_ENDED, ch, g_mus_done_gen[ch]);
                }
            }
    }
}

void plat_music(int ch, const char *name) { music_queue(0, ch, 0, name); }
void plat_music_pause(int ch, int pause) { music_queue(1, ch, pause, NULL); }
void plat_beep(void) { MessageBeep(0); }

static void music_init(void)
{
    InitializeCriticalSection(&g_mq_lock);
    g_mq_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_mus_thread = CreateThread(NULL, 0, music_thread, NULL, 0, NULL);
}

/* ---- window ---- */
static void update_menu(void)
{
    ModifyMenuW(g_menu, IDM_MUSIC, MF_BYCOMMAND | MF_STRING, IDM_MUSIC, mm_music_on ? L"Turn &Music Off" : L"Turn &Music On");
    ModifyMenuW(g_menu, IDM_SOUND, MF_BYCOMMAND | MF_STRING, IDM_SOUND,
                mm_sound_on ? L"Turn &Sound Effects Off" : L"Turn &Sound Effects On");
    CheckMenuItem(g_menu, IDM_HD, MF_BYCOMMAND | (mm_hd_on ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(g_menu, IDM_HD, MF_BYCOMMAND | (mm_hd_available() ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(g_menu, IDM_WIDE, MF_BYCOMMAND | (mm_view_ext() ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(g_menu, IDM_WIDE, MF_BYCOMMAND | (mm_wide_available() ? MF_ENABLED : MF_GRAYED));
}

static int view_w(void) { return MM_W + 2 * mm_view_ext(); }   /* logical width: 636, or 764 in widescreen */

/* Display: the canvas is drawn at a whole scale (g_scale) and shown in the client area at the largest size that
 * keeps its proportions (g_dstw x g_dsth at g_offx, g_offy). CHANGE: no longer only whole scales with black
 * bars - when the window (maximized or full screen) is not a whole multiple, the canvas is drawn at the next
 * whole scale up and scaled down smoothly (HALFTONE) to fill it; when the window's shape is within 3% of the
 * game's, it fills the window completely (the tiny stretch is not visible). */
static int g_dstw, g_dsth;
#define FILL_TOLERANCE 0.03

static void layout(void)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right, chh = rc.bottom, vw = view_w();
    if (cw < 1) cw = 1;
    if (chh < 1) chh = 1;
    double f = (double)cw / vw < (double)chh / MM_H ? (double)cw / vw : (double)chh / MM_H;
    int s = (int)f;
    if (f - s > 0.001) s++;                                /* next whole scale up, scaled down when shown */
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    g_dstw = (int)(vw * f + 0.5);
    g_dsth = (int)(MM_H * f + 0.5);
    double aspect = ((double)cw / chh) / ((double)vw / MM_H);
    if (aspect > 1 - FILL_TOLERANCE && aspect < 1 + FILL_TOLERANCE) { g_dstw = cw; g_dsth = chh; }
    g_offx = (cw - g_dstw) / 2;
    g_offy = (chh - g_dsth) / 2;
    if (s != g_scale || !g_dib || g_canvas.w != vw * s) {
        int new_scale = s != g_scale || !g_dib;
        g_scale = s;
        if (g_dib) DeleteObject(g_dib);
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = vw * s;
        bi.bmiHeader.biHeight = -MM_H * s;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits;
        g_dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        SelectObject(g_memdc, g_dib);
        g_canvas.px = bits;
        g_canvas.w = vw * s;
        g_canvas.h = MM_H * s;
        g_canvas.stride = vw * s;
        g_canvas.scale = s;
        if (new_scale) make_fonts(s);
    }
    g_canvas.ox = mm_view_ext();
    g_dirty = 1;
}

/* Borderless full screen on the window's monitor (F11 / Alt+Enter, CHANGE); the menu is hidden meanwhile. */
static int g_full;
static WINDOWPLACEMENT g_place = {sizeof(WINDOWPLACEMENT)};

static void set_fullscreen(int on)
{
    if (on == g_full) return;
    if (on) {
        MONITORINFO mi = {sizeof mi};
        GetWindowPlacement(g_hwnd, &g_place);
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(g_hwnd, NULL);
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetMenu(g_hwnd, g_menu);
        SetWindowPlacement(g_hwnd, &g_place);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_full = on;
    layout();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* Size the window to the biggest whole scale that fits the work area (keep_scale: stay at the current scale when
 * it still fits; used when widescreen is toggled), centred on its old centre. A maximized window keeps its size. */
static void size_window(int keep_scale)
{
    if (g_full || IsZoomed(g_hwnd)) { layout(); InvalidateRect(g_hwnd, NULL, FALSE); return; }
    RECT wa, wr;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    GetWindowRect(g_hwnd, &wr);
    DWORD style = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    int vw = view_w(), s = 1;
    for (int t = 2; t <= 8; t++) {
        RECT r = {0, 0, vw * t, MM_H * t};
        AdjustWindowRect(&r, style, TRUE);
        if (r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top) s = t;
    }
    if (keep_scale && g_scale < s) s = g_scale;
    RECT r = {0, 0, vw * s, MM_H * s};
    AdjustWindowRect(&r, style, TRUE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = (wr.left + wr.right - ww) / 2, y = (wr.top + wr.bottom - wh) / 2;
    if (x + ww > wa.right) x = wa.right - ww;
    if (y + wh > wa.bottom) y = wa.bottom - wh;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    SetWindowPos(g_hwnd, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    layout();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

void plat_view_changed(void) { size_window(1); update_menu(); }   /* also from the settings screen */

static int mods(void)
{
    return (GetKeyState(VK_SHIFT) < 0 ? MMOD_SHIFT : 0) | (GetKeyState(VK_CONTROL) < 0 ? MMOD_CTRL : 0);
}

static void mouse(LPARAM lp, int button, int action)
{
    int px = GET_X_LPARAM(lp) - g_offx, py = GET_Y_LPARAM(lp) - g_offy;
    int vw = view_w();                                    /* window pixels -> logical (shown size g_dstw x g_dsth) */
    int x = (int)floor((double)px * vw / (g_dstw > 0 ? g_dstw : 1)) - g_canvas.ox;
    int y = (int)floor((double)py * MM_H / (g_dsth > 0 ? g_dsth : 1));
    mm_mouse(x, y, button, action, mods());
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: layout(); InvalidateRect(h, NULL, TRUE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (g_dirty) { mm_draw(&g_canvas); g_dirty = 0; }
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH black = GetStockObject(BLACK_BRUSH);
        RECT bars[4] = {{0, 0, rc.right, g_offy}, {0, g_offy + g_dsth, rc.right, rc.bottom},
                        {0, 0, g_offx, rc.bottom}, {g_offx + g_dstw, 0, rc.right, rc.bottom}};
        for (int i = 0; i < 4; i++) FillRect(dc, &bars[i], black);
        if (g_dstw == g_canvas.w && g_dsth == g_canvas.h) BitBlt(dc, g_offx, g_offy, g_canvas.w, g_canvas.h, g_memdc, 0, 0, SRCCOPY);
        else {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, g_offx, g_offy, g_dstw, g_dsth, g_memdc, 0, 0, g_canvas.w, g_canvas.h, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER: mm_tick(); return 0;
    case WM_MOUSEMOVE: mouse(lp, 0, MA_MOVE); return 0;
    case WM_LBUTTONDOWN: mouse(lp, MMB_LEFT, MA_DOWN); return 0;
    case WM_RBUTTONDOWN: mouse(lp, MMB_RIGHT, MA_DOWN); return 0;
    case WM_LBUTTONDBLCLK: mouse(lp, MMB_LEFT, MA_DOWN); mouse(lp, MMB_LEFT, MA_DOUBLE); return 0;
    case WM_RBUTTONDBLCLK: mouse(lp, MMB_RIGHT, MA_DOWN); mouse(lp, MMB_RIGHT, MA_DOUBLE); return 0;
    case WM_LBUTTONUP: mouse(lp, MMB_LEFT, MA_UP); return 0;
    case WM_MOUSEWHEEL: mm_mouse(0, 0, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1, MA_WHEEL, 0); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(NULL, g_hand ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_KEYDOWN: {
        int k = (int)wp;
        if (k == VK_F11) { set_fullscreen(!g_full); return 0; }   /* full screen on/off */
        if (k == VK_RETURN) k = KEY_ENTER;
        else if (k == VK_ESCAPE) k = KEY_ESC;
        else if (k == VK_BACK) k = KEY_BACK;
        else if (k == VK_TAB) k = KEY_TAB;
        else if (k == 'V' && GetKeyState(VK_CONTROL) < 0) {     /* paste into a text field */
            if (OpenClipboard(h)) {
                HANDLE cd = GetClipboardData(CF_UNICODETEXT);
                const wchar_t *s = cd ? GlobalLock(cd) : NULL;
                for (; s && *s; s++) if (*s >= 32) mm_char(*s);
                if (cd) GlobalUnlock(cd);
                CloseClipboard();
            }
            return 0;
        }
        else if (k >= VK_NUMPAD0 && k <= VK_NUMPAD9) k = '0' + k - VK_NUMPAD0;
        mm_key(k, mods());
        if (k == VK_F2 || k == VK_F3) update_menu();
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { set_fullscreen(!g_full); return 0; }   /* Alt+Enter */
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;                    /* no beep after Alt+Enter */
        break;
    case WM_CHAR: if (wp >= 32) mm_char((unsigned)wp); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_NEW: mm_cmd(CMD_NEW_GAME); break;
        case IDM_QUIT: mm_cmd(CMD_QUIT); break;
        case IDM_MUSIC: mm_cmd(CMD_MUSIC); break;
        case IDM_SOUND: mm_cmd(CMD_SOUND); break;
        case IDM_HD: mm_cmd(CMD_HD); break;
        case IDM_WIDE: mm_cmd(CMD_WIDE); break;
        case IDM_FULL: set_fullscreen(!g_full); break;
        case IDM_EXPORT: export_for_phone(); break;
        }
        update_menu();
        return 0;
    case WM_HTTP_DONE: {                                  /* plat_http_get finished */
        HttpJob *j = (HttpJob *)lp;
        mm_http_done(j->tag, j->data, j->len);
        free(j->data);
        free(j);
        return 0;
    }
    case WM_MUSIC_ENDED:                                  /* drop ends of tracks the game has replaced since */
        if (wp < 2 && (LONG)lp == g_mus_gen[wp]) mm_music_ended((int)wp);
        return 0;
    case WM_ACTIVATE: mm_activate(LOWORD(wp) != WA_INACTIVE); break;
    case WM_CLOSE: mm_shutdown(); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* MindMaze menu > Export Data for the Phone App: the data folder as one .zip (win32_export.c) for the Android app's
 * import, so the disc does not have to be extracted on the phone */
static void export_for_phone(void)
{
    wchar_t file[MAX_PATH * 2] = L"MindMaze-data.zip";
    OPENFILENAMEW of = {0};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"MindMaze data file (*.zip)\0*.zip\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = L"zip";
    of.lpstrTitle = L"Export MindMaze's data for the phone app";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&of)) return;
    HWND note = busy_window(GetModuleHandleW(NULL), L"Writing the data file...");
    char msg[256];
    int ok = win32_export_zip(g_datadir, file, msg, sizeof msg);
    if (note) DestroyWindow(note);
    wchar_t w[600], m[256];
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, m, 256);
    if (ok)
        swprintf(w, 600, L"Saved %ls (%ls).\n\nCopy it to your phone and choose it in the MindMaze app: \"Import a MindMaze data "
                         L"file (.zip)\". It holds pictures from your Encarta disc: keep it for yourself.", file, m);
    else swprintf(w, 600, L"The data could not be exported: %ls", m);
    MessageBoxW(g_hwnd, w, L"MindMaze", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}

/* small borderless note window while the art tools work (a few seconds) */
static HWND busy_window(HINSTANCE inst, const wchar_t *text)
{
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = 420, h = 90;
    HWND win = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", text, WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                               (wa.left + wa.right - w) / 2, (wa.top + wa.bottom - h) / 2, w, h, NULL, NULL, inst, NULL);
    if (win) { SendMessageW(win, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE); UpdateWindow(win); }
    SetCursor(LoadCursor(NULL, IDC_WAIT));
    DWORD until = GetTickCount() + 150;                       /* let it paint before the work blocks the thread */
    while ((LONG)(GetTickCount() - until) < 0) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
    return win;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev;
    typedef BOOL(WINAPI * SetDpiCtx)(HANDLE);
    SetDpiCtx setdpi = (SetDpiCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext");
    if (setdpi) setdpi((HANDLE)-4); /* per-monitor v2 */
    else SetProcessDPIAware();

    wchar_t exedir[MAX_PATH];
    GetModuleFileNameW(NULL, exedir, MAX_PATH);
    wchar_t *slash = wcsrchr(exedir, L'\\');
    if (slash) slash[1] = 0;
    /* g_dir = the folder holding data\, mindmaze.ini and the player's files: next to the exe or in LOCALAPPDATA.
     * On the first start the data folder is built from the player's Encarta disc (win32_setup.c). */
    if (!win32_setup(inst, exedir, g_dir, MAX_PATH)) return 0;
    swprintf(g_datadir, MAX_PATH, L"%lsdata\\", g_dir);
    swprintf(g_ini, MAX_PATH, L"%lsmindmaze.ini", g_dir);
    if (GetFileAttributesW(g_ini) == INVALID_FILE_ATTRIBUTES) {        /* UTF-16 ini: Unicode player names */
        FILE *f = _wfopen(g_ini, L"wb");
        if (f) { fwrite("\xFF\xFE", 1, 2, f); fclose(f); }
    }

    (void)cmdline;
    /* built-in art tools (src/art): the sheets are made once, new agent results installed at start */
    {
        char rep[4096];
        if (plat_data_size("art/work/hd/sheets/backgrounds_000.txt") <= 0) {
            HWND note = busy_window(inst, L"Preparing the kit for HD and widescreen pictures (first start)...");
            mm_art_prepare(0, rep, sizeof rep);
            if (note) DestroyWindow(note);
        }
        if (mm_art_results_waiting()) {
            HWND note = busy_window(inst, L"Installing the new HD / widescreen pictures...");
            mm_art_install(rep, sizeof rep);
            if (note) DestroyWindow(note);
        }
    }

    g_memdc = CreateCompatibleDC(NULL);
    fonts_init();
    WNDCLASSEXW wc = {sizeof wc, CS_DBLCLKS, wndproc, 0, 0, inst,
                      LoadImageW(inst, L"APPICON", IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0),
                      LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"MindMaze",
                      LoadImageW(inst, L"APPICON", IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0)};
    RegisterClassExW(&wc);

    g_menu = CreateMenu();
    HMENU sub = CreatePopupMenu();
    AppendMenuW(sub, MF_STRING, IDM_NEW, L"Start &New Game");
    AppendMenuW(sub, MF_STRING, IDM_QUIT, L"&Quit Game");
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, IDM_MUSIC, L"Turn &Music Off");
    AppendMenuW(sub, MF_STRING, IDM_SOUND, L"Turn &Sound Effects Off");
    AppendMenuW(sub, MF_STRING, IDM_HD, L"Use &HD Art\tF2");
    AppendMenuW(sub, MF_STRING, IDM_WIDE, L"&Widescreen\tF3");
    AppendMenuW(sub, MF_STRING, IDM_FULL, L"&Full Screen\tF11");
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, IDM_EXPORT, L"&Export Data for the Phone App...");
    AppendMenuW(g_menu, MF_POPUP, (UINT_PTR)sub, L"&MindMaze");

    /* biggest whole scale that fits the work area */
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int s = 1;
    DWORD style = WS_OVERLAPPEDWINDOW;
    for (int t = 2; t <= 8; t++) {
        RECT r = {0, 0, MM_W * t, MM_H * t};
        AdjustWindowRect(&r, style, TRUE);
        if (r.right - r.left <= wa.right - wa.left && r.bottom - r.top <= wa.bottom - wa.top) s = t;
    }
    RECT r = {0, 0, MM_W * s, MM_H * s};
    AdjustWindowRect(&r, style, TRUE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    g_hwnd = CreateWindowW(L"MindMaze", L"MindMaze", style, wa.left + (wa.right - wa.left - ww) / 2,
                           wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, NULL, g_menu, inst, NULL);
    layout();
    audio_init();
    music_init();

    char err[512] = "";
    if (!mm_init(err, sizeof err)) {
        wchar_t w[512];
        MessageBoxW(g_hwnd, wide(err, w, 512), L"MindMaze", MB_ICONERROR);
        return 1;
    }
    if (mm_view_ext()) size_window(0);                    /* the window was made before the setting was read */
    update_menu();
    ShowWindow(g_hwnd, show);
    SetTimer(g_hwnd, TIMER_ID, 50, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    music_queue(2, 0, 0, NULL);                         /* stop the music thread (closes MCI) */
    WaitForSingleObject(g_mus_thread, 3000);
    return 0;
}
