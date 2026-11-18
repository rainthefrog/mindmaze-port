/* Music on the web / Android: the game's MIDI files played with TinySoundFont (MIT, src/third_party) and a General
 * MIDI SoundFont from /soundfonts (several can be installed; Settings > Volume > "Music instruments" switches).
 * shell.js pulls the sound from web_music_render() in its WebAudio callback. Two channels like the Windows build:
 * 0 = background loops, 1 = question music; a channel that reaches the end of its file reports it to the core
 * (mm_music_ended) from the next frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>
#include "mm.h"

#define TSF_IMPLEMENTATION
#include "../../third_party/tsf.h"
#define TML_IMPLEMENTATION
#include "../../third_party/tml.h"

#define MAXFONTS 8
static char g_font_file[MAXFONTS][64], g_font_name[MAXFONTS][64];
static int g_nfonts, g_font = -1, g_rate = 44100;
static tsf *g_base;
/* TinySoundFont at gain 1 renders the intro with peaks of 1.5 (TimGM6mb) to 2.5 (GeneralUser GS) times full scale
 * (measured 2026-11-18): 0.35 keeps every track below full scale, like the quieter Windows synthesizer */
#define MUSIC_GAIN 0.35f
static float g_volume = MUSIC_GAIN;
static struct {
    tsf *synth;
    tml_message *song, *msg;
    double ms;
    int playing, paused, ended;
    float gain, gain_step;                                /* crossfade: per-frame gain and its change */
    char name[32];
} g_ch[3];                                                /* 0 background, 1 question, 2 background fading out */
enum { FADE = 2 };
static int g_fade_ms;                                     /* plat_music_fade: the next background switch fades */

static void channel_reset(int c)
{
    if (!g_ch[c].synth) return;
    tsf_reset(g_ch[c].synth);
    tsf_channel_set_bank_preset(g_ch[c].synth, 9, 128, 0);       /* MIDI channel 10: drums */
}

static void start(int c)
{
    tml_free(g_ch[c].song);
    g_ch[c].song = g_ch[c].msg = NULL;
    g_ch[c].playing = g_ch[c].paused = 0;
    if (!g_ch[c].name[0]) return;
    char path[96];
    snprintf(path, sizeof path, "/data/%s", g_ch[c].name);
    for (char *p = path + 6; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    g_ch[c].song = g_ch[c].msg = tml_load_filename(path);
    g_ch[c].ms = 0;
    g_ch[c].playing = g_ch[c].song != NULL;
    g_ch[c].gain = 1.0f;
    g_ch[c].gain_step = 0.0f;
    channel_reset(c);
}

/* SoundFonts: /soundfonts/list.txt, one "file<TAB>name" per line (written by shell.js) */
static void fonts_scan(void)
{
    if (g_nfonts) return;
    FILE *f = fopen("/soundfonts/list.txt", "r");
    if (!f) return;
    char line[160];
    while (g_nfonts < MAXFONTS && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!*line) continue;
        if (tab) *tab = 0;
        snprintf(g_font_file[g_nfonts], sizeof g_font_file[0], "%s", line);
        snprintf(g_font_name[g_nfonts], sizeof g_font_name[0], "%s", tab ? tab + 1 : line);
        g_nfonts++;
    }
    fclose(f);
}

static void load_font(int i)
{
    if (i == g_font) return;
    for (int c = 0; c < 3; c++) { if (g_ch[c].synth) tsf_close(g_ch[c].synth); g_ch[c].synth = NULL; }
    g_ch[FADE].playing = 0;
    if (g_base) tsf_close(g_base);
    g_base = NULL;
    g_font = i;
    if (i < 0 || i >= g_nfonts) return;
    char path[128];
    snprintf(path, sizeof path, "/soundfonts/%s", g_font_file[i]);
    g_base = tsf_load_filename(path);
    if (!g_base) return;
    tsf_set_output(g_base, TSF_STEREO_INTERLEAVED, g_rate, 0.0f);

    for (int c = 0; c < 3; c++) {
        g_ch[c].synth = tsf_copy(g_base);
        if (g_ch[c].synth) tsf_set_max_voices(g_ch[c].synth, 96);   /* per copy: a copy gets no voices of its own */
        tsf_set_volume(g_ch[c].synth, g_volume);
        channel_reset(c);
    }
}

/* ---- the platform's instrument choice (Settings) ---- */
int plat_instrument_count(void) { fonts_scan(); return g_nfonts; }
const char *plat_instrument_name(int i) { fonts_scan(); return i >= 0 && i < g_nfonts ? g_font_name[i] : ""; }
int plat_instrument_get(void) { return g_font; }
void plat_instrument_set(int i)
{
    fonts_scan();
    if (i < 0 || i >= g_nfonts) return;
    load_font(i);
    for (int c = 0; c < 2; c++) {                               /* the playing tracks continue with the new sound */
        int was = g_ch[c].playing, paused = g_ch[c].paused;
        double ms = g_ch[c].ms;
        if (!was) continue;
        start(c);
        g_ch[c].paused = paused;
        while (g_ch[c].msg && g_ch[c].msg->time < ms) {          /* skip ahead, keeping program changes */
            tml_message *m = g_ch[c].msg;
            if (m->type == TML_PROGRAM_CHANGE)
                tsf_channel_set_presetnumber(g_ch[c].synth, m->channel, m->program, m->channel == 9);
            else if (m->type == TML_CONTROL_CHANGE) tsf_channel_midi_control(g_ch[c].synth, m->channel, m->control, m->control_value);
            g_ch[c].msg = m->next;
        }
        g_ch[c].ms = ms;
    }
    char v[8];
    snprintf(v, sizeof v, "%d", i);
    plat_set_setting("soundfont", v);
}

/* ---- called by web_main.c (plat_music*, plat_volume) ---- */
void web_music_play(int c, const char *name)
{
    if (c < 0 || c > 1) return;
    if (g_font < 0) {
        char v[8];
        fonts_scan();
        load_font(plat_get_setting("soundfont", v, sizeof v) && atoi(v) < g_nfonts ? atoi(v) : 0);
    }
    if (c == 0 && g_fade_ms > 0 && g_ch[0].playing && name) {
        /* crossfade: the old track moves to the fading slot (with its synth and position) and fades out while
         * the new one fades in */
        tsf *spare = g_ch[FADE].synth;
        tml_free(g_ch[FADE].song);
        g_ch[FADE] = g_ch[0];
        g_ch[FADE].ended = 0;
        g_ch[FADE].gain_step = -1.0f / (g_fade_ms * g_rate / 1000.0f);
        memset(&g_ch[0], 0, sizeof g_ch[0]);
        g_ch[0].synth = spare;
        snprintf(g_ch[0].name, sizeof g_ch[0].name, "%s", name);
        start(0);
        g_ch[0].gain = 0.0f;
        g_ch[0].gain_step = 1.0f / (g_fade_ms * g_rate / 1000.0f);
        g_fade_ms = 0;
        return;
    }
    g_fade_ms = 0;
    snprintf(g_ch[c].name, sizeof g_ch[c].name, "%s", name ? name : "");
    start(c);
}
void web_music_fade(int ms) { g_fade_ms = ms; }
void web_music_pause(int c, int pause)
{
    if (c < 0 || c > 1) return;
    g_ch[c].paused = pause;
    if (pause && g_ch[c].synth) tsf_note_off_all(g_ch[c].synth);
}
void web_music_volume(int pct)
{
    g_volume = MUSIC_GAIN * pct / 100.0f;
    for (int c = 0; c < 3; c++) if (g_ch[c].synth) tsf_set_volume(g_ch[c].synth, g_volume);
}
/* returns a channel whose track ended since the last call (-1 = none) */
int web_music_take_ended(void)
{
    for (int c = 0; c < 2; c++) if (g_ch[c].ended) { g_ch[c].ended = 0; return c; }
    return -1;
}

/* ---- the WebAudio callback ---- */
EMSCRIPTEN_KEEPALIVE void web_music_rate(int rate)
{
    if (rate == g_rate || rate < 8000) return;
    g_rate = rate;
    int f = g_font;
    g_font = -1;
    if (f >= 0) load_font(f);
}

EMSCRIPTEN_KEEPALIVE void web_music_render(float *out, int frames)
{
    memset(out, 0, sizeof(float) * 2 * (size_t)frames);
    static float tmp[2 * 64];
    for (int c = 0; c < 3; c++) {
        if (!g_ch[c].playing || g_ch[c].paused || !g_ch[c].synth) continue;
        tsf *s = g_ch[c].synth;
        for (int done = 0; done < frames;) {
            int n = frames - done < 64 ? frames - done : 64;
            g_ch[c].ms += n * 1000.0 / g_rate;
            for (tml_message *m = g_ch[c].msg; m && m->time <= g_ch[c].ms; m = g_ch[c].msg = m->next)
                switch (m->type) {
                case TML_PROGRAM_CHANGE: tsf_channel_set_presetnumber(s, m->channel, m->program, m->channel == 9); break;
                case TML_NOTE_ON: tsf_channel_note_on(s, m->channel, m->key, m->velocity / 127.0f); break;
                case TML_NOTE_OFF: tsf_channel_note_off(s, m->channel, m->key); break;
                case TML_PITCH_BEND: tsf_channel_set_pitchwheel(s, m->channel, m->pitch_bend); break;
                case TML_CONTROL_CHANGE: tsf_channel_midi_control(s, m->channel, m->control, m->control_value); break;
                }
            tsf_render_float(s, tmp, n, 0);
            float g = g_ch[c].gain, st = g_ch[c].gain_step;
            for (int i = 0; i < n; i++) {
                out[2 * (done + i)] += tmp[2 * i] * g;
                out[2 * (done + i) + 1] += tmp[2 * i + 1] * g;
                g += st;
                if (g > 1.0f) { g = 1.0f; st = 0.0f; }
                if (g < 0.0f) g = 0.0f;
            }
            g_ch[c].gain = g;
            g_ch[c].gain_step = st;
            done += n;
            if (c == FADE && g <= 0.0f) break;                      /* faded out */
        }
        if (c == FADE) {
            if (g_ch[c].gain <= 0.0f || !g_ch[c].msg) g_ch[c].playing = 0;   /* the faded track never reports an end */
            continue;
        }
        if (!g_ch[c].msg) {     /* the file's last event: the track ends there, as on Windows (some SoundFont
                                    * instruments sustain for 40 s after it, so waiting for silence delayed the next loop) */
            g_ch[c].playing = 0;
            g_ch[c].ended = 1;
        }
    }
}

/* diagnostics for the browser test: font index, SoundFont loaded, channel 0/1 playing / track loaded */
EMSCRIPTEN_KEEPALIVE int web_music_state(void)
{
    return (g_font + 1) | (g_base != NULL) << 4 | g_ch[0].playing << 5 | (g_ch[0].song != NULL) << 6 |
           g_ch[1].playing << 7 | (g_ch[1].song != NULL) << 8 | g_nfonts << 9;
}
