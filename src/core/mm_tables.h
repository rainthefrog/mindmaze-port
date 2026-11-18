/* The original game's data tables, loaded at start from data/tables.json.
 *
 * tables.json is written by tools/extract/extract.py from the player's own Encarta 2006 files; this repository
 * holds no table data. Format ("mindmaze-tables-1"): docs/FORMATS.md.
 */
#pragma once
#include "mm.h"

enum { MM_GROUPS = 6, MM_N_RANKS = 6, MM_N_SPEECH = 23, MM_N_PICTURE_AREAS = 9, MM_N_MUSIC = 5, MM_N_SOUNDS = 35 };
enum { MM_MAX_FURNITURE = 64 };          /* rooms keep furniture as bits of a 64-bit word */

/* room objects in six groups, in this order: doors, floors, ceilings, walls, characters, furniture */
extern int mm_group_count[MM_GROUPS], mm_group_start[MM_GROUPS];
extern MMObject *mm_objects;
#define MM_N_DOORS      (mm_group_count[0])
#define MM_N_FLOORS     (mm_group_count[1])
#define MM_N_CEILINGS   (mm_group_count[2])
#define MM_N_WALLS      (mm_group_count[3])
#define MM_N_CHARACTERS (mm_group_count[4])
#define MM_N_FURNITURE  (mm_group_count[5])
#define MM_N_OBJECTS    (mm_group_start[5] + mm_group_count[5])
#define MM_G_DOORS      (mm_group_start[0])
#define MM_G_FLOORS     (mm_group_start[1])
#define MM_G_CEILINGS   (mm_group_start[2])
#define MM_G_WALLS      (mm_group_start[3])
#define MM_G_CHARACTERS (mm_group_start[4])
#define MM_G_FURNITURE  (mm_group_start[5])

extern MMRank mm_ranks[MM_N_RANKS];
extern const char **mm_speech[MM_N_SPEECH];          /* character speech texts, index = object id - 7 */
extern int mm_speech_count[MM_N_SPEECH];
extern const char *mm_picture_area[MM_N_PICTURE_AREAS];   /* picture ids 31..39 */
extern const char *mm_music_files[MM_N_MUSIC];
extern const char *mm_sound_files[MM_N_SOUNDS];
extern char mm_tables_edition[64];                   /* the disc the data folder came from, e.g. "Encarta 97" */

/* Load data/tables.json (once; later calls return 1 at once). 0 + message on failure. */
int mm_tables_load(char *err, int errlen);
