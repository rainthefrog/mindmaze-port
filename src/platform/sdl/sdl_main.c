/* MindMaze - SDL3 platform layer for Linux and macOS: window, drawing, input, main loop (sound: sdl_audio.c, files
 * and settings: sdl_files.c, first start: sdl_setup.c). Behaves like the Windows build (win32_main.c):
 *   - the window opens at the biggest whole scale of the 636x437 view that fits the screen; the canvas is drawn at
 *     a whole scale and shown at the largest size that keeps its proportions (when the window is not a whole
 *     multiple it is drawn at the next whole scale up and scaled down smoothly; within 3% of the game's shape it
 *     fills the window);
 *   - F11 or Alt+Enter: full screen; F2 HD art, F3 widescreen (the core's keys);
 *   - the Windows menu's commands as shortcuts (Ctrl, or Cmd on macOS): N new game, Q quit, M music on/off,
 *     E sound effects on/off; V pastes into a text field.
 * The canvas (0x00RRGGBB) goes to a streaming XRGB8888 texture; the renderer scales it on the GPU (or SDL's
 * software renderer). On high-density screens (Retina) everything is in real pixels. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdl.h"
#include "mm.h"
#include "mm_art.h"
#include "text.h"
#include "cli.h"
#include "../../third_party/stb_image.h"

SDL_Window *sdl_window;
Uint32 sdl_event_base;
int sdl_pick_failed;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static MMCanvas g_canvas;
static SDL_FRect g_dst;                        /* where the canvas is shown, in window pixels */
static int g_dirty = 1, g_running = 1, g_quit_pending;
static SDL_Cursor *g_cursor[2];
static int g_hand;
#define FILL_TOLERANCE 0.03

static int view_w(void) { return MM_W + 2 * mm_view_ext(); }   /* logical width: 636, or 764 in widescreen */

/* ---- display ---- */
static void layout(void)
{
    int cw = 1, ch = 1, vw = view_w();
    SDL_GetWindowSizeInPixels(sdl_window, &cw, &ch);
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    double f = (double)cw / vw < (double)ch / MM_H ? (double)cw / vw : (double)ch / MM_H;
    int s = (int)f;
    if (f - s > 0.001) s++;                                /* next whole scale up, scaled down when shown */
    if (s < 1) s = 1;
    if (s > 8) s = 8;
    int dw = (int)(vw * f + 0.5), dh = (int)(MM_H * f + 0.5);
    double aspect = ((double)cw / ch) / ((double)vw / MM_H);
    if (aspect > 1 - FILL_TOLERANCE && aspect < 1 + FILL_TOLERANCE) { dw = cw; dh = ch; }
    g_dst = (SDL_FRect){(float)((cw - dw) / 2), (float)((ch - dh) / 2), (float)dw, (float)dh};
    if (!g_canvas.px || g_canvas.scale != s || g_canvas.w != vw * s) {
        free(g_canvas.px);
        g_canvas.scale = s;
        g_canvas.w = g_canvas.stride = vw * s;
        g_canvas.h = MM_H * s;
        g_canvas.px = calloc((size_t)g_canvas.w * g_canvas.h, 4);
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, g_canvas.w, g_canvas.h);
        if (!g_canvas.px || !g_tex) { SDL_Log("out of memory for the canvas: %s", SDL_GetError()); exit(1); }
    }
    /* exact size: sharp pixels; scaled down: smooth */
    SDL_SetTextureScaleMode(g_tex, dw == g_canvas.w && dh == g_canvas.h ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    g_canvas.ox = mm_view_ext();
    g_dirty = 1;
}

static void present(void)
{
    SDL_UpdateTexture(g_tex, NULL, g_canvas.px, g_canvas.stride * 4);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, NULL, &g_dst);
    SDL_RenderPresent(g_ren);
}

static int is_full(void) { return (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_FULLSCREEN) != 0; }

/* Size the window to the biggest whole scale that fits the screen's usable area (keep_scale: stay at the current
 * scale when it still fits; used when widescreen is toggled). Full screen / maximized windows keep their size. */
static void size_window(int keep_scale, int center)
{
    if (is_full() || (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_MAXIMIZED)) { layout(); return; }
    SDL_Rect ub = {0, 0, 1280, 720};
    SDL_DisplayID d = SDL_GetDisplayForWindow(sdl_window);
    if (!d) d = SDL_GetPrimaryDisplay();
    SDL_GetDisplayUsableBounds(d, &ub);
    float dens = SDL_GetWindowPixelDensity(sdl_window);  /* window sizes are in points; the canvas is in pixels */
    if (dens <= 0) dens = 1;
    int vw = view_w(), s = 1;
    for (int t = 2; t <= 8; t++)                          /* room for the title bar */
        if (vw * t / dens <= ub.w && MM_H * t / dens <= ub.h - 48) s = t;
    if (keep_scale && g_canvas.scale && g_canvas.scale < s) s = g_canvas.scale;
    SDL_SetWindowSize(sdl_window, (int)(vw * s / dens), (int)(MM_H * s / dens));
    if (center) SDL_SetWindowPosition(sdl_window, SDL_WINDOWPOS_CENTERED_DISPLAY(d), SDL_WINDOWPOS_CENTERED_DISPLAY(d));
    layout();
}

void plat_view_changed(void) { size_window(1, 0); }       /* widescreen toggled (also from the settings screen) */
void plat_invalidate(void) { g_dirty = 1; }
void plat_quit(void) { g_running = 0; }
void plat_set_cursor(int hand)
{
    if (hand == g_hand) return;
    g_hand = hand;
    if (g_cursor[hand ? 1 : 0]) SDL_SetCursor(g_cursor[hand ? 1 : 0]);
}

/* ---- text (../common/text.c) ---- */
void plat_text(MMCanvas *c, int x, int y, int w, int h, const char *s, int font, uint32_t rgb, int flags)
{
    text_box(c, x, y, w, h, s, font, rgb, flags);
}
int plat_text_width(const char *s, int font) { return text_width(s, font, g_canvas.scale ? g_canvas.scale : 1); }
int plat_text_px_width(MMCanvas *c, const char *s, int font) { return text_px_width(c, s, font); }
int plat_text_px_height(MMCanvas *c, int font) { return text_px_height(c, font); }
int plat_text_px_ascent(MMCanvas *c, int font) { return text_px_ascent(c, font); }
void plat_text_px(MMCanvas *c, int X, int Y, const char *s, int font, uint32_t rgb, int cx0, int cy0, int cx1, int cy1)
{
    text_px(c, X, Y, s, font, rgb, cx0, cy0, cx1, cy1);
}
uint32_t plat_ticks(void) { return (uint32_t)SDL_GetTicks(); }

/* ---- a full-window note: first start, art tools, export ---- */
static char g_st_title[300], g_st_step[300];       /* the last note, drawn again when the window changes size */
static int g_st_pct, g_game_on, g_job_on;          /* g_game_on: mm_init done, the game draws the window */

void sdl_status(const char *title, const char *step, int pct)
{
    MMCanvas *c = &g_canvas;
    if (title != g_st_title) SDL_strlcpy(g_st_title, title, sizeof g_st_title);
    if (step != g_st_step) SDL_strlcpy(g_st_step, step ? step : "", sizeof g_st_step);
    g_st_pct = pct;
    step = g_st_step;
    int ox = c->ox, vw = c->w / c->scale;
    c->ox = 0;
    mm_fill(c, 0, 0, vw, MM_H, 0x1C2430);
    text_box(c, 20, 150, vw - 40, 40, title, FONT_LIST, 0xFFFFFF, TX_CENTER | TX_WRAP);
    if (step && *step) text_box(c, 20, 196, vw - 40, 32, step, FONT_TEXT, 0xC8D0DC, TX_CENTER | TX_WRAP);
    if (pct >= 0) {
        int bw = 300, bx = (vw - bw) / 2;
        mm_frame(c, bx, 236, bw, 12, 1, 0x8090A0);
        mm_fill(c, bx + 2, 238, (bw - 4) * (pct > 100 ? 100 : pct) / 100, 8, 0xE0B040);
    }
    c->ox = ox;
    present();
    g_dirty = 1;                                           /* the game draws itself again afterwards */
}

/* events while the game waits (a job or a dialog): keep the window drawn, remember a quit, drop the input */
static void wait_events(int ms)
{
    SDL_Event e;
    if (!SDL_WaitEventTimeout(&e, ms)) return;
    do {
        if (e.type == SDL_EVENT_QUIT) g_quit_pending = 1;
        else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || e.type == SDL_EVENT_WINDOW_EXPOSED) {
            layout();                                  /* a new canvas is blank: draw it again */
            if (g_game_on && !g_job_on) { mm_draw(&g_canvas); present(); }
            else sdl_status(g_st_title, g_st_step, g_st_pct);
        }
    } while (SDL_PollEvent(&e));
}

typedef struct { int (*fn)(void *); void *arg; int result; SDL_AtomicInt done; } Job;
static SDL_Mutex *g_prog_lock;
static int g_prog_pct;
static char g_prog_step[256];

static int SDLCALL job_thread(void *p)
{
    Job *j = p;
    j->result = j->fn(j->arg);
    SDL_SetAtomicInt(&j->done, 1);
    return 0;
}

void sdl_progress(int pct, const char *step)
{
    SDL_LockMutex(g_prog_lock);
    g_prog_pct = pct;
    SDL_strlcpy(g_prog_step, step ? step : "", sizeof g_prog_step);
    SDL_UnlockMutex(g_prog_lock);
}

int sdl_run_job(int (*fn)(void *), void *arg, const char *title)
{
    Job j = {fn, arg, 0, {0}};
    sdl_progress(-1, "");
    g_job_on = 1;
    SDL_Thread *t = SDL_CreateThread(job_thread, "job", &j);
    if (!t) {                                              /* no thread: run it here (the window waits) */
        sdl_status(title, NULL, -1);
        j.result = fn(arg);
        g_job_on = 0;
        return j.result;
    }
    while (!SDL_GetAtomicInt(&j.done)) {
        char step[256];
        SDL_LockMutex(g_prog_lock);
        int pct = g_prog_pct;
        SDL_strlcpy(step, g_prog_step, sizeof step);
        SDL_UnlockMutex(g_prog_lock);
        if (pct >= 0) {
            char line[300];
            snprintf(line, sizeof line, "%s (%d%%)", step, pct);
            sdl_status(title, line, pct);
        } else sdl_status(title, step, -1);
        wait_events(50);
    }
    SDL_WaitThread(t, NULL);
    g_job_on = 0;
    return j.result;
}

/* ---- dialogs ---- */
typedef struct { SDL_AtomicInt done; char *path; int failed; } Pick;

static void SDLCALL pick_done(void *ud, const char *const *list, int filter)
{
    Pick *p = ud;
    (void)filter;
    if (list && list[0]) p->path = mm_strdup(list[0]);
    else if (!list) { p->failed = 1; SDL_Log("file dialog: %s", SDL_GetError()); }
    SDL_SetAtomicInt(&p->done, 1);
}

char *sdl_pick_file(int save, const SDL_DialogFileFilter *filters, int nfilters, const char *def)
{
    Pick p = {{0}, NULL, 0};
    if (save) SDL_ShowSaveFileDialog(pick_done, &p, sdl_window, filters, nfilters, def);
    else SDL_ShowOpenFileDialog(pick_done, &p, sdl_window, filters, nfilters, def, false);
    while (!SDL_GetAtomicInt(&p.done)) {                  /* the answer comes through the event loop */
        wait_events(50);
        present();
    }
    sdl_pick_failed = p.failed;
    g_dirty = 1;
    return p.path;
}

int sdl_ask(int warn, const char *title, const char *msg, const char *b0, const char *b1, const char *b2)
{
    const char *b[3] = {b0, b1, b2};
    SDL_MessageBoxButtonData btn[3];
    int n = 0;
    for (int i = 0; i < 3; i++)
        if (b[i]) btn[n++] = (SDL_MessageBoxButtonData){i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0, i, b[i]};
    btn[n - 1].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;   /* Escape: the last button (Cancel) */
    SDL_MessageBoxData d = {(warn ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION) | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT,
                            sdl_window, title, msg, n, btn, NULL};
    int id = -1;
    wait_events(0);                                       /* a pending resize: draw the window first (the box is modal) */
    if (!SDL_ShowMessageBox(&d, &id)) { SDL_Log("message box: %s\n%s", SDL_GetError(), msg); return -1; }
    g_dirty = 1;
    return id;
}

/* ---- input ---- */
static int mods(void)
{
    SDL_Keymod m = SDL_GetModState();
    return (m & SDL_KMOD_SHIFT ? MMOD_SHIFT : 0) | (m & (SDL_KMOD_CTRL | SDL_KMOD_GUI) ? MMOD_CTRL : 0);
}

static void mouse(float wx, float wy, int button, int action)
{
    float dens = SDL_GetWindowPixelDensity(sdl_window);   /* event positions are in points */
    if (dens <= 0) dens = 1;
    double px = wx * dens - g_dst.x, py = wy * dens - g_dst.y;
    int x = (int)floor(px * view_w() / (g_dst.w > 0 ? g_dst.w : 1)) - g_canvas.ox;
    int y = (int)floor(py * MM_H / (g_dst.h > 0 ? g_dst.h : 1));
    mm_mouse(x, y, button, action, mods());
}

/* SDL key -> the core's VK-like codes (mm.h): letters 'A'..'Z', digits, KEY_* */
static int map_key(SDL_Keycode k)
{
    if (k >= SDLK_A && k <= SDLK_Z) return 'A' + (int)(k - SDLK_A);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (int)(k - SDLK_0);
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return '1' + (int)(k - SDLK_KP_1);
    switch (k) {
    case SDLK_KP_0: return '0';
    case SDLK_RETURN: case SDLK_KP_ENTER: return KEY_ENTER;
    case SDLK_ESCAPE: return KEY_ESC;
    case SDLK_BACKSPACE: return KEY_BACK;
    case SDLK_TAB: return KEY_TAB;
    case SDLK_DELETE: return KEY_DELETE;
    case SDLK_SPACE: return ' ';
    case SDLK_F2: return KEY_F2;
    case SDLK_F3: return KEY_F3;
    }
    return 0;
}

static void utf8_chars(const char *s)                    /* typed or pasted text -> mm_char per code point */
{
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63u) << 12 | (p[1] & 63u) << 6 | (p[2] & 63u); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63u) << 6 | (p[1] & 63u); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63u); p += 1; }
        if (c >= 32) mm_char(c);
    }
}

static void key_down(const SDL_KeyboardEvent *e)
{
    int primary = (e->mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    if (e->key == SDLK_F11 || ((e->key == SDLK_RETURN || e->key == SDLK_KP_ENTER) && (e->mod & SDL_KMOD_ALT))) {
        if (!e->repeat) SDL_SetWindowFullscreen(sdl_window, !is_full());
        return;
    }
    if (primary && !(e->mod & SDL_KMOD_ALT)) {           /* the Windows menu's commands */
        switch (e->key) {
        case SDLK_V: {                                  /* paste into a text field */
            char *t = SDL_GetClipboardText();
            if (t) utf8_chars(t);
            SDL_free(t);
            return;
        }
        case SDLK_N: if (!e->repeat) mm_cmd(CMD_NEW_GAME); return;
        case SDLK_Q: if (!e->repeat) mm_cmd(CMD_QUIT); return;
        case SDLK_M: if (!e->repeat) mm_cmd(CMD_MUSIC); return;
        case SDLK_E: if (!e->repeat) mm_cmd(CMD_SOUND); return;
        }
    }
    int k = map_key(e->key);
    if (k) mm_key(k, mods());
}

static void handle(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_QUIT: mm_shutdown(); g_running = 0; break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: layout(); break;
    case SDL_EVENT_WINDOW_EXPOSED: g_dirty = 1; break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: mm_activate(1); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: mm_activate(0); break;
    case SDL_EVENT_MOUSE_MOTION: mouse(e->motion.x, e->motion.y, 0, MA_MOVE); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int b = e->button.button == SDL_BUTTON_LEFT ? MMB_LEFT : e->button.button == SDL_BUTTON_RIGHT ? MMB_RIGHT : 0;
        if (!b) break;
        mouse(e->button.x, e->button.y, b, MA_DOWN);
        if (e->button.clicks >= 2 && e->button.clicks % 2 == 0) mouse(e->button.x, e->button.y, b, MA_DOUBLE);   /* as WM_LBUTTONDBLCLK */
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) mouse(e->button.x, e->button.y, MMB_LEFT, MA_UP);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.y) mm_mouse(0, 0, e->wheel.y > 0 ? 1 : -1, MA_WHEEL, 0);
        break;
    case SDL_EVENT_KEY_DOWN: key_down(&e->key); break;
    case SDL_EVENT_TEXT_INPUT:
        if (!(SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) utf8_chars(e->text.text);
        break;
    default:
        if (e->type == sdl_event_base + SDL_EV_HTTP) sdl_http_done_event(e->user.data1);
    }
}

/* ---- start ---- */
static void set_icon(void)                               /* Linux: the window's icon (macOS: the app bundle's) */
{
    char p[1200];
    int w, h, n, size = 0;
    sdl_join(p, sizeof p, sdl_res, "mindmaze.png");
    unsigned char *d = sdl_slurp(p, &size), *px = d ? stbi_load_from_memory(d, size, &w, &h, &n, 4) : NULL;
    free(d);
    if (!px) return;
    SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
    if (s) { SDL_SetWindowIcon(sdl_window, s); SDL_DestroySurface(s); }
    stbi_image_free(px);
}

/* the action flags (--add-disc, --export, --prepare-art, --install-art): no window, messages on stdout / stderr.
 * The data folder must exist, or come from --disc. Exit code 0 = all done, 1 = one failed. */
static int run_actions(void)
{
    if (!SDL_Init(0)) { fprintf(stderr, "MindMaze: %s\n", SDL_GetError()); return 1; }
    if (cli.update) {                                     /* --update needs no data folder: it runs first */
        int r = sdl_update_cli();
        if (r || !(cli.add_disc || cli.export_zip || cli.prepare_art || cli.install_art)) { SDL_Quit(); return r; }
    }
    int ok = sdl_setup(cli.disc, cli.home, 1);
    if (ok) {
        sdl_files_init();
        char rep[4096];
        if (ok && cli.add_disc) ok = sdl_add_disc_headless(cli.add_disc);
        if (ok && cli.prepare_art) {
            ok = mm_art_prepare(1, rep, sizeof rep);
            printf("%s%s", rep, *rep && rep[strlen(rep) - 1] != '\n' ? "\n" : "");
        }
        if (ok && cli.install_art) {
            ok = mm_art_install(rep, sizeof rep) >= 0;         /* 0 = no new pictures: not an error */
            printf("%s%s", rep, *rep && rep[strlen(rep) - 1] != '\n' ? "\n" : "");
        }
        if (ok && cli.export_zip) ok = sdl_export_headless(cli.export_zip);
    }
    SDL_Quit();
    return ok ? 0 : 1;
}

static int art_prepare_job(void *arg) { char rep[4096]; (void)arg; return mm_art_prepare(0, rep, sizeof rep); }
static int art_install_job(void *arg) { char rep[4096]; (void)arg; return mm_art_install(rep, sizeof rep); }

int main(int argc, char **argv)
{
    /* command-line options (../common/cli.h): MindMaze --help */
    char usage[256];
    if (!cli_parse(argc, argv, usage, sizeof usage)) { fprintf(stderr, "MindMaze: %s\n", usage); return 2; }
    if (cli.help) { fputs(cli_usage(), stdout); return 0; }
    if (cli.version) { puts("MindMaze " MM_VERSION); return 0; }
    const char *source = cli.disc;                        /* the first start's disc */
    SDL_SetAppMetadata("MindMaze", MM_VERSION, "io.github.mindmaze_port");
    sdl_argv = argv;                                      /* the restart after an update */
    if (cli.action) return run_actions();                 /* no window */
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "MindMaze: %s\n", SDL_GetError()); return 1; }
    sdl_event_base = SDL_RegisterEvents(SDL_EV_COUNT);
    g_prog_lock = SDL_CreateMutex();
    if (!SDL_CreateWindowAndRenderer("MindMaze", MM_W, MM_H, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN,
                                     &sdl_window, &g_ren)) {
        fprintf(stderr, "MindMaze: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(sdl_window, MM_W / 2, MM_H / 2);
    g_cursor[0] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    g_cursor[1] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    /* fonts and the program's other files sit next to it (macOS: the app's Resources) */
    const char *base = SDL_GetBasePath();
    char fonts[1200];
    sdl_join(sdl_res, sizeof sdl_res, base ? base : ".", "");
    sdl_join(fonts, sizeof fonts, sdl_res, "fonts");
    if (!text_init(fonts)) SDL_Log("fonts missing in %s: text uses fallback metrics", fonts);
    set_icon();
    size_window(0, 1);
    SDL_ShowWindow(sdl_window);
    SDL_SyncWindow(sdl_window);                           /* the new size is known before the first picture */
    layout();
    sdl_status("MindMaze", NULL, -1);

    /* the data folder; on the first start it is built from the player's disc (sdl_setup.c) */
    if (!sdl_setup(source, cli.home, 0) || g_quit_pending) { SDL_Quit(); return 0; }
    sdl_files_init();
    sdl_audio_init();

    /* built-in art tools (src/art): the sheets are made once, new agent results installed at start */
    if (plat_data_size("art/work/hd/sheets/backgrounds_000.txt") <= 0)
        sdl_run_job(art_prepare_job, NULL, "Preparing the kit for HD and widescreen pictures (first start)...");
    if (mm_art_results_waiting()) sdl_run_job(art_install_job, NULL, "Installing the new HD / widescreen pictures...");

    char err[512] = "";
    if (!mm_init(err, sizeof err)) {
        sdl_ask(1, "MindMaze", err, "OK", NULL, NULL);
        SDL_Quit();
        return 1;
    }
    if (mm_view_ext()) size_window(0, 1);                 /* the window was made before the setting was read */
    if (cli.fullscreen == 1) { SDL_SetWindowFullscreen(sdl_window, true); SDL_SyncWindow(sdl_window); }   /* --fullscreen */
    layout();
    SDL_StartTextInput(sdl_window);
    g_game_on = 1;
    sdl_update_start();                                   /* a newer release? (sdl_update.c asks the player) */
    if (g_quit_pending) { mm_shutdown(); g_running = 0; }

    Uint64 next = SDL_GetTicks();
    while (g_running) {
        Uint64 now = SDL_GetTicks();
        if (now >= next) {                                /* the core's 50 ms tick (catching up at most a little) */
            mm_tick();
            next = now - next > 200 ? now + 50 : next + 50;
        }
        sdl_audio_poll();
        if (g_dirty) {
            g_dirty = 0;
            mm_draw(&g_canvas);
            present();
        }
        now = SDL_GetTicks();
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, next > now ? (Sint32)(next - now) : 0))
            do handle(&e); while (g_running && SDL_PollEvent(&e));
    }
    SDL_Quit();
    return 0;
}
