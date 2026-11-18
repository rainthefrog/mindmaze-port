/* MindMaze - SDL3 sound (Linux, macOS): one audio stream at 44.1 kHz stereo float. Its callback renders the music
 * (../common/music.c: TinySoundFont, the SoundFonts found in soundfonts/) and mixes the WAV sound effects on top,
 * like the Windows build's waveOut mixer (8 voices). Without an audio device the game runs silently.
 * The callback runs on SDL's audio thread: one mutex guards the music state (music_set_lock) and the voices. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "sdl.h"
#include "mm.h"
#include "music.h"

#define RATE 44100
#define NVOICE 8
#define NSAMPLE 64
#define CHUNK 512                                    /* frames rendered per step of the callback */

typedef struct { char name[32]; float *pcm; int n; } Sample;   /* mono float at RATE */
static Sample g_samples[NSAMPLE];
static int g_nsamples;
static struct { const Sample *s; int pos; } g_voice[NVOICE];
static SDL_AudioStream *g_stream;
static SDL_Mutex *g_lock;
static float g_sfx_vol = 1.0f;

static void lock(void) { SDL_LockMutex(g_lock); }
static void unlock(void) { SDL_UnlockMutex(g_lock); }

static void SDLCALL feed(void *ud, SDL_AudioStream *stream, int additional, int total)
{
    (void)ud;
    (void)total;
    static float buf[2 * CHUNK];
    for (int frames = additional / (int)(2 * sizeof(float)); frames > 0;) {
        int n = frames < CHUNK ? frames : CHUNK;
        music_render(buf, n);                         /* locks by itself */
        lock();
        for (int v = 0; v < NVOICE; v++) {
            if (!g_voice[v].s) continue;
            const Sample *s = g_voice[v].s;
            int pos = g_voice[v].pos;
            for (int i = 0; i < n && pos < s->n; i++, pos++) {
                float x = s->pcm[pos] * g_sfx_vol;
                buf[2 * i] += x;
                buf[2 * i + 1] += x;
            }
            g_voice[v].pos = pos;
            if (pos >= s->n) g_voice[v].s = NULL;
        }
        unlock();
        for (int i = 0; i < 2 * n; i++) buf[i] = buf[i] > 1.0f ? 1.0f : buf[i] < -1.0f ? -1.0f : buf[i];
        SDL_PutAudioStreamData(stream, buf, n * (int)(2 * sizeof(float)));
        frames -= n;
    }
}

void sdl_audio_init(void)
{
    g_lock = SDL_CreateMutex();
    music_set_lock(lock, unlock);
    music_rate(RATE);
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) { SDL_Log("no sound: %s", SDL_GetError()); return; }
    SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, RATE};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, NULL);
    if (!g_stream) { SDL_Log("no sound: %s", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(g_stream);
}

/* tracks that reached their end -> the core (from the main loop) */
void sdl_audio_poll(void)
{
    for (int ch; (ch = music_take_ended()) >= 0;) mm_music_ended(ch);
}

/* ---- sound effects: the data folder's WAV files, converted once to mono float at RATE ---- */
static const Sample *load_sample(const char *name)
{
    for (int i = 0; i < g_nsamples; i++) if (!SDL_strcasecmp(g_samples[i].name, name)) return &g_samples[i];
    if (g_nsamples >= NSAMPLE) return NULL;
    Sample *s = &g_samples[g_nsamples++];
    SDL_strlcpy(s->name, name, sizeof s->name);
    int size = 0;
    void *d = plat_load(name, &size);                 /* plat_load also finds the lower-case file name */
    SDL_AudioSpec src, dst = {SDL_AUDIO_F32, 1, RATE};
    Uint8 *wav = NULL, *out = NULL;
    Uint32 len = 0;
    int outlen = 0;
    if (d && SDL_LoadWAV_IO(SDL_IOFromConstMem(d, (size_t)size), true, &src, &wav, &len) &&
        SDL_ConvertAudioSamples(&src, wav, (int)len, &dst, &out, &outlen)) {
        s->pcm = (float *)out;                        /* kept for the whole run (SDL's allocation) */
        s->n = outlen / (int)sizeof(float);
    } else if (d) SDL_Log("sound %s: %s", name, SDL_GetError());
    SDL_free(wav);
    free(d);
    return s;
}

void plat_sound(const char *name)
{
    if (!g_stream) return;
    const Sample *s = load_sample(name);
    if (!s || !s->n) return;
    lock();
    int v = 0;
    for (int i = 0; i < NVOICE; i++) if (!g_voice[i].s) { v = i; break; }   /* all busy: the first one is replaced */
    g_voice[v].s = s;
    g_voice[v].pos = 0;
    unlock();
}

/* MessageBeep: a short soft tone (made once) */
void plat_beep(void)
{
    static Sample beep = {"(beep)", NULL, 0};
    if (!g_stream) return;
    if (!beep.pcm) {
        int n = RATE / 12;
        beep.pcm = malloc(sizeof(float) * (size_t)n);
        if (!beep.pcm) return;
        for (int i = 0; i < n; i++)
            beep.pcm[i] = 0.25f * sinf(2.0f * 3.14159265f * 880.0f * i / RATE) * (1.0f - (float)i / n);
        beep.n = n;
    }
    lock();
    g_voice[0].s = &beep;
    g_voice[0].pos = 0;
    unlock();
}

/* ---- music: ../common/music.c ---- */
void plat_music(int ch, const char *name) { music_play(ch, name); }
void plat_music_pause(int ch, int pause) { music_pause(ch, pause); }
void plat_music_fade(int ms) { music_fade(ms); }
void plat_volume(int music, int sound)
{
    music_volume(music);
    if (g_lock) lock();
    g_sfx_vol = sound / 100.0f;
    if (g_lock) unlock();
}
