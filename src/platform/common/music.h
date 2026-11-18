/* SoundFont music (music.c, shared by the web and SDL layers): TinySoundFont playing the game's MIDI files.
 * The plat_instrument_* functions of mm.h are implemented there too. */
#pragma once

/* set up before mm_init: the data folder ("/data/", with the trailing slash), the installed SoundFonts (a file
 * path and the name Settings shows) and, where music_render runs on another thread, a lock around the state */
void music_set_data_dir(const char *dir);
int music_add_soundfont(const char *path, const char *name);      /* 0: list full */
void music_set_lock(void (*lock)(void), void (*unlock)(void));

/* the platform's plat_music, plat_music_pause, plat_music_fade, plat_volume (music part) */
void music_play(int channel, const char *name);
void music_pause(int channel, int pause);
void music_fade(int ms);
void music_volume(int pct);
int music_take_ended(void);              /* a channel whose track ended since the last call, -1 = none */

/* the audio output: sample rate, and interleaved stereo float frames (the buffer is overwritten, not mixed into) */
void music_rate(int rate);
void music_render(float *out, int frames);
int music_state(void);                   /* diagnostics: see music.c */
