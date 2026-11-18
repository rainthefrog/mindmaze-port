/* MindMaze - web platform layer (Emscripten / WebAssembly). The engine of the Android app; not hosted anywhere.
 *
 * Files: the data folder is /data (filled by the page from the app's storage), the player's files are /user
 * (IndexedDB, kept between runs), the fonts /fonts. Settings live in localStorage. Drawing: the core draws into a
 * 32-bit canvas at a whole-number scale; shell.js scales it to the screen (letterboxed). Sound effects: WebAudio.
 * Music: web_music.c (TinySoundFont + a SoundFont, rendered in shell.js's WebAudio callback). */
#include <emscripten.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mm.h"
#include "web_text.h"
#include "../../third_party/stb_image.h"

static MMCanvas g_c;
static uint8_t *g_rgba;
static int g_dev_w = 1280, g_dev_h = 720, g_dirty = 1, g_started;
static double g_next_tick;

/* ---- JS side (shell.js provides the objects these use) ---- */
EM_JS(void, js_present, (const uint8_t *rgba, int w, int h), { MM.present(rgba, w, h); });
EM_JS(void, js_sound, (const char *name), { MM.sound(UTF8ToString(name)); });
EM_JS(void, js_beep, (void), { MM.beep(); });
EM_JS(void, js_cursor, (int hand), { MM.cursor(hand); });
EM_JS(void, js_open_url, (const char *url), { window.open(UTF8ToString(url), "_blank"); });
EM_JS(void, js_http_get, (const char *url, int tag), { MM.httpGet(UTF8ToString(url), tag); });
EM_JS(void, js_persist_user, (void), { MM.persistUser(); });
EM_JS(void, js_music, (int ch, const char *name), { MM.music(ch, name ? UTF8ToString(name) : null); });
EM_JS(void, js_music_pause, (int ch, int pause), { MM.musicPause(ch, pause); });
EM_JS(void, js_volume, (int music, int sound), { MM.volume(music, sound); });
EM_JS(char *, js_get_setting, (const char *key), {
    var v = MM.getSetting(UTF8ToString(key));
    return v === null ? 0 : stringToNewUTF8(v);
});
EM_JS(void, js_set_setting, (const char *key, const char *value), {
    MM.setSetting(UTF8ToString(key), value ? UTF8ToString(value) : null);
});
EM_JS(void, js_error, (const char *msg), { MM.error(UTF8ToString(msg)); });

/* ---- time, files ---- */
uint32_t plat_ticks(void) { return (uint32_t)emscripten_get_now(); }

static void *slurp(const char *path, int *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc((size_t)n + 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[n] = 0;
    if (size) *size = (int)n;
    return d;
}

static void data_path(const char *rel, char *out, size_t n)
{
    if (rel[0] == '/') { snprintf(out, n, "%s", rel); return; }
    snprintf(out, n, "/data/%s", rel);
    for (char *p = out + 6; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;     /* stored with lower-case names */
}

void *plat_load(const char *rel, int *size)
{
    char p[512];
    data_path(rel, p, sizeof p);
    return slurp(p, size);
}

static int write_file(const char *path, const void *data, int len)
{
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; (s = strchr(s, '/')); s++) { *s = 0; mkdir(p, 0755); *s = '/'; }
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    return fclose(f) == 0 && ok;
}

int plat_save_data(const char *rel, const void *data, int len)
{
    char p[512];
    data_path(rel, p, sizeof p);
    return write_file(p, data, len);
}

long plat_data_size(const char *rel)
{
    char p[512];
    struct stat st;
    data_path(rel, p, sizeof p);
    return stat(p, &st) || !S_ISREG(st.st_mode) ? -1 : (long)st.st_size;
}

MMImage *plat_load_png(const char *rel)
{
    int size = 0, w, h, n;
    unsigned char *d = plat_load(rel, &size);
    if (!d) return NULL;
    unsigned char *px = stbi_load_from_memory(d, size, &w, &h, &n, 4);
    free(d);
    if (!px) return NULL;
    MMImage *im = malloc(sizeof *im);
    im->w = w;
    im->h = h;
    im->px = malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++)
        im->px[i] = (uint32_t)px[4 * i + 3] << 24 | (uint32_t)px[4 * i] << 16 | (uint32_t)px[4 * i + 1] << 8 | px[4 * i + 2];
    stbi_image_free(px);
    return im;
}

/* player files: /user (IndexedDB) */
void *plat_load_user(const char *name, int *size)
{
    char p[512];
    snprintf(p, sizeof p, "/user/%s", name);
    return slurp(p, size);
}
int plat_save_user(const char *name, const void *data, int len)
{
    char p[512];
    snprintf(p, sizeof p, "/user/%s", name);
    int ok = write_file(p, data, len);
    js_persist_user();
    return ok;
}
void plat_append_user(const char *name, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "/user/%s", name);
    FILE *f = fopen(p, "ab");
    if (f) { fputs(text, f); fclose(f); }
    js_persist_user();
}

/* ---- settings (localStorage) ---- */
int plat_get_setting(const char *key, char *buf, int n)
{
    char *v = js_get_setting(key);
    if (!v) return 0;
    snprintf(buf, (size_t)n, "%s", v);
    free(v);
    return 1;
}
void plat_set_setting(const char *key, const char *value) { js_set_setting(key, value); }

/* ---- text (web_text.c) ---- */
void plat_text(MMCanvas *c, int x, int y, int w, int h, const char *s, int font, uint32_t rgb, int flags)
{
    text_box(c, x, y, w, h, s, font, rgb, flags);
}
int plat_text_width(const char *s, int font) { return text_width(s, font, g_c.scale ? g_c.scale : 1); }
int plat_text_px_width(MMCanvas *c, const char *s, int font) { return text_px_width(c, s, font); }
int plat_text_px_height(MMCanvas *c, int font) { return text_px_height(c, font); }
int plat_text_px_ascent(MMCanvas *c, int font) { return text_px_ascent(c, font); }
void plat_text_px(MMCanvas *c, int X, int Y, const char *s, int font, uint32_t rgb, int cx0, int cy0, int cx1, int cy1)
{
    text_px(c, X, Y, s, font, rgb, cx0, cy0, cx1, cy1);
}

/* ---- sound, music, misc ---- */
void plat_sound(const char *name)
{
    char lower[64];
    snprintf(lower, sizeof lower, "%s", name);
    for (char *p = lower; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    js_sound(lower);
}
void web_music_play(int c, const char *name);                  /* web_music.c */
void web_music_pause(int c, int pause);
void web_music_volume(int pct);
void web_music_fade(int ms);
int web_music_take_ended(void);
void plat_music(int ch, const char *name) { web_music_play(ch, name); js_music(ch, name); }
void plat_music_pause(int ch, int pause) { web_music_pause(ch, pause); js_music_pause(ch, pause); }
void plat_music_fade(int ms) { web_music_fade(ms); }
void plat_volume(int music, int sound) { web_music_volume(music); js_volume(music, sound); }
void plat_beep(void) { js_beep(); }
void plat_open_url(const char *url) { js_open_url(url); }
void plat_open_data_folder(const char *rel) { (void)rel; }
EM_JS(void, js_art_import, (void), { MM.artImport(); });
int plat_art_import(int go) { if (go) js_art_import(); return 1; }
int plat_can_export(void) { return 0; }   /* the phone app imports; it does not export */
/* Settings > Add questions from a disc (CHANGE 2026-11-18): shell.js opens a file chooser on the tap's release, runs the
 * extractor worker (mx_add_edition), stores the files in /data and calls web_disc_reading / web_disc_added */
EM_JS(void, js_add_disc, (void), { MM.addDisc(); });
void plat_add_disc(void) { js_add_disc(); }
EMSCRIPTEN_KEEPALIVE void web_disc_reading(void) { mm_disc_reading(); }
/* the HD art pack was installed into /data/hd, /data/wide (shell.js installArt): use the new pictures */
EMSCRIPTEN_KEEPALIVE void web_art_installed(void) { mm_pictures_reload(); plat_invalidate(); }
EMSCRIPTEN_KEEPALIVE void web_disc_added(int ok, const char *id, const char *name, int count, const char *msg)
{
    mm_disc_added(ok, id, name, count, msg && *msg ? msg : NULL);
    plat_invalidate();
}
void plat_export_data(void) {}      /* no file manager on the web */
void plat_set_cursor(int hand) { js_cursor(hand); }
void plat_invalidate(void) { g_dirty = 1; }
void plat_quit(void) {}
void plat_http_get(const char *url, int tag) { js_http_get(url, tag); }


/* ---- canvas size: the biggest whole scale that fits the device pixels (at least 1, at most 4) ---- */
static void layout(void)
{
    int vw = MM_W + 2 * mm_view_ext(), vh = MM_H;
    int s = g_dev_w / vw < g_dev_h / vh ? g_dev_w / vw : g_dev_h / vh;
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    if (g_c.px && g_c.scale == s && g_c.w == vw * s) return;
    free(g_c.px);
    free(g_rgba);
    g_c.scale = s;
    g_c.w = vw * s;
    g_c.h = vh * s;
    g_c.stride = g_c.w;
    g_c.ox = mm_view_ext();
    g_c.px = calloc((size_t)g_c.w * g_c.h, 4);
    g_rgba = malloc((size_t)g_c.w * g_c.h * 4);
    g_dirty = 1;
}
void plat_view_changed(void) { layout(); }

static void present(void)
{
    size_t n = (size_t)g_c.w * g_c.h;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = g_c.px[i];
        g_rgba[4 * i] = (uint8_t)(p >> 16);
        g_rgba[4 * i + 1] = (uint8_t)(p >> 8);
        g_rgba[4 * i + 2] = (uint8_t)p;
        g_rgba[4 * i + 3] = 255;
    }
    js_present(g_rgba, g_c.w, g_c.h);
}

static void frame(void)
{
    double now = emscripten_get_now();
    if (now >= g_next_tick) {                                   /* the core's 50 ms tick */
        mm_tick();
        g_next_tick = now + 50 > g_next_tick + 50 ? now + 50 : g_next_tick + 50;
    }
    for (int ch; (ch = web_music_take_ended()) >= 0;) mm_music_ended(ch);   /* a track reached its end */
    if (g_dirty) {
        g_dirty = 0;
        mm_draw(&g_c);
        present();
    }
}

/* ---- called from shell.js ---- */
EMSCRIPTEN_KEEPALIVE int web_start(int dev_w, int dev_h)
{
    g_dev_w = dev_w;
    g_dev_h = dev_h;
    text_init("/fonts");
    char err[512] = "";
    if (!mm_init(err, sizeof err)) { js_error(err); return 0; }
    layout();
    g_started = 1;
    g_next_tick = emscripten_get_now();
    emscripten_set_main_loop(frame, 0, 0);
    return 1;
}

EMSCRIPTEN_KEEPALIVE void web_resize(int dev_w, int dev_h)
{
    g_dev_w = dev_w;
    g_dev_h = dev_h;
    if (g_started) layout();
}

/* pointer in canvas pixels -> logical coordinates of the view (minus the widescreen margin) */
EMSCRIPTEN_KEEPALIVE void web_mouse(int X, int Y, int button, int action, int mods)
{
    if (!g_started || !g_c.scale) return;
    mm_mouse(X / g_c.scale - g_c.ox, Y / g_c.scale, button, action, mods);
}
EMSCRIPTEN_KEEPALIVE void web_key(int key, int mods) { if (g_started) mm_key(key, mods); }
EMSCRIPTEN_KEEPALIVE void web_char(unsigned ch) { if (g_started) mm_char(ch); }
EMSCRIPTEN_KEEPALIVE void web_cmd(int cmd) { if (g_started) mm_cmd(cmd); }
EMSCRIPTEN_KEEPALIVE void web_music_ended(int ch) { if (g_started) mm_music_ended(ch); }
EMSCRIPTEN_KEEPALIVE void web_http_done(int tag, const char *data, int len) { mm_http_done(tag, data, len); }
EMSCRIPTEN_KEEPALIVE int web_scale(void) { return g_c.scale; }
EMSCRIPTEN_KEEPALIVE int web_view_ext(void) { return g_c.ox; }

int main(void) { return 0; }                                    /* shell.js calls web_start when the files are ready */
