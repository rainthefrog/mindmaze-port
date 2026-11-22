/* A complete data folder from one older Encarta disc (95 MPEG, 96 Mac, 97, 98): see mx_oldext.c. */
#pragma once
#include "mx.h"

typedef struct {
    const char *id, *name;
    int year;
    const char *qdir;            /* folder of MINDMAZE.DB / .IDX (Mac: "MindMaze Data" / "MindMaze Index") */
    const char *bag, *exe, *dll, *setup, *midi;
} MXOldEdition;
extern const MXOldEdition mx_old_editions[];      /* ends with id NULL */

/* reads a file of the disc ("ENCYC97/MMBAG.M20"; rsrc 1: the Mac resource fork) -> malloc'd bytes, NULL if missing */
typedef uint8_t *(*MXOldRead)(void *ctx, const char *path, int rsrc, size_t *len);
typedef struct { int dib, wav, mid, strings; } MXOldCounts;

/* the whole disc-specific data folder from an older edition (mx_edition.c): questions (mindmaze.db/.idx in the 2006
 * layout), the extra music, and everything mx_old_build writes. id/name receive the edition; *nq the question count.
 * Returns 1 = done, 0 = error (message in err), -1 = no older edition found (message in err) */
int mx_extract_old(MXIO *io, const char *source, char *id, int idlen, char *name, int namelen, int *nq, MXOldCounts *c,
                   char *err, int errlen);

/* pictures, sounds, the main music, tables.json and res/<id>.bmp into the data folder (questions: mx_edition.c) */
int mx_old_build(MXIO *io, MXOldRead rd, void *ctx, const MXOldEdition *ed, MXOldCounts *c, char *err, int errlen);

/* CHANGE 2026-11-21: QuickTime music movie (the Encarta 96 Mac disc's music) -> Standard MIDI file (mx_qtmusic.c).
 * data / rsrc = the file's data and resource forks (rsrc may be NULL for a flattened movie). Returns the malloc'd
 * SMF (*outlen bytes) or NULL with the reason in err. stats (optional) counts what was converted / left out. */
typedef struct {
    int parts, programs, notes, controllers, restrikes, samples, division, end_ticks;
    int media_end;               /* the movie's length in its timescale (the SMF ends at the last note-off instead) */
    int unmapped;                /* QuickTime-only controllers, knobs, silent notes: not in the SMF */
} MXQtStats;
uint8_t *mx_qtmusic_to_smf(const uint8_t *data, size_t dn, const uint8_t *rsrc, size_t rn, size_t *outlen, MXQtStats *stats,
                           char *err, int errlen);

/* a music file of an older disc as a Standard MIDI file: the file itself if it is one, else a QuickTime music movie
 * (data + resource fork read through rd) converted; NULL if missing or neither (mx_oldext.c) */
uint8_t *mx_old_midi(MXOldRead rd, void *ctx, const char *path, size_t *len);
