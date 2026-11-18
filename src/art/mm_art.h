/* Built-in HD / widescreen art tools (the C port of the Python scripts in tools/art, so the game itself prepares the sheets for the
 * AI agent and installs its results; nothing else needs to be installed).
 *
 * Data folder layout:
 *   art/work/hd/sheets/<sheet>.png + .txt     sheets for the agent (mm_art_prepare)
 *   art/work/hd/out_gen/<sheet>.png           the agent's 2x results
 *   hd/<key>.png + hd/bounds.txt              installed HD pictures (mm_art_install)
 *   art/work/wide/sheets/, masks/, out_gen/   the same for widescreen
 *   wide/<key>.png, <key>_1x.png, wide.txt    installed widescreen pictures
 *   art/work/REPORT.txt                       what the last install did, sheets to generate again
 */
#pragma once
#include <stddef.h>

/* write the sheets (and notes) unless they exist; force = write again. 1 = ok, report = short summary */
int mm_art_prepare(int force, char *report, int n);
/* cut every agent result found into hd/ and wide/, write art/work/REPORT.txt and reload the pictures.
 * Returns the number of pictures installed (0 = none found), -1 on an error; report = short summary. */
int mm_art_install(char *report, int n);
/* agent results waiting (out_gen has a result newer than the last install) */
int mm_art_results_waiting(void);

/* ---- image helpers (mm_art_img.c) ---- */
typedef struct { int w, h, c; unsigned char *p; } AImg;          /* 8-bit, c = 1, 3 or 4 channels */
enum { A_LANCZOS, A_BICUBIC };
AImg *aimg_new(int w, int h, int c);
void aimg_free(AImg *im);
AImg *aimg_load_original(const char *name, int *keyed);           /* RGBA; keyed: palette 254 is cyan */
AImg *aimg_load_png_rgb(const char *rel);                         /* NULL if missing */
int aimg_save_png(const char *rel, const AImg *im);
AImg *aimg_resize(const AImg *in, int w, int h, int filter);      /* Pillow's resampling */
void aimg_premultiply(AImg *im);
void aimg_unpremultiply(AImg *im);
void aimg_box_blur(unsigned char *ch, int w, int h, float radius);
