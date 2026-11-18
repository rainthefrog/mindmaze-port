/* MindMaze's room tables, read from any edition's program (CHANGE, 2026-11-18).
 *
 * Every MindMaze program from Encarta 95 to 2006 holds the same tables (room objects, ranks, speech, the name
 * lists), the UI texts and 16 panel bitmaps, in different program formats:
 *   mx_tables_pe   32-bit Windows (Encarta 97 ENC97.EXE + ENCRES97.DLL, 98 ENC98.EXE + ENCRES98.DLL, 2006
 *                  ENCARTA.EXE + ENCARTAR.DLL): found by patterns (strings and the pointers to them), no fixed
 *                  addresses
 *   mx_tables_ne   16-bit Windows (Encarta 95 MM95.EXE, NE): far pointers through segment fixups
 *   mx_tables_mac  Macintosh (Encarta 96 application's resource fork): DATA 0 globals (A5 offsets), STR#, WBMP
 * A reader fills MXTables with the program's own values and spellings (e.g. "NDOOR.DI$", "intro.MID", real line
 * breaks); mx_tables_json then writes the 2006 tables.json layout the game reads, normalising the spellings
 * and filling UI texts a program lacks. Reference scripts: build/versions/<edition>/tables/extract_tables*.py.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { MXT_MAX_OBJECTS = 200, MXT_FIRST_STRING = 29001, MXT_LAST_STRING = 29069,
       MXT_NSTRINGS = MXT_LAST_STRING - MXT_FIRST_STRING + 1, MXT_NBITMAPS = 16 };

typedef struct {
    char *name;                  /* picture file name as the program spells it */
    int x, y, rect[4], half, slots[7], flags, id;
} MXObject;

typedef struct {
    int counts[6];               /* doors, floors, ceilings, walls, characters, furniture (more groups: ignored) */
    int nobjects;                /* = the sum of counts */
    MXObject obj[MXT_MAX_OBJECTS];
    struct { char *name, *image; unsigned min, max; } rank[6];
    char **speech[23];           /* 23 lists of texts */
    int nspeech[23];
    char *picture_areas[9], *music[5], *sounds[35];
    char *str[MXT_NSTRINGS];     /* UI texts by id - MXT_FIRST_STRING (UTF-8); NULL = not in this program */
    uint8_t *bmp[MXT_NBITMAPS];  /* panel bitmaps 29150..29164, 29166 as complete .bmp files */
    size_t bmplen[MXT_NBITMAPS];
} MXTables;

extern const int mx_table_bitmaps[MXT_NBITMAPS];   /* 29150..29164, 29166 */

/* All strings in MXTables are malloc'd UTF-8; texts from a program are converted from its code page (Windows
 * 1252, Mac Roman). Each reader returns 1, or 0 with a message in err. */
int mx_tables_pe(const uint8_t *exe, size_t exelen, const uint8_t *dll, size_t dlllen, MXTables *t, char *err, int errlen);
int mx_tables_ne(const uint8_t *exe, size_t exelen, MXTables *t, char *err, int errlen);
int mx_tables_mac(const uint8_t *rsrc, size_t rsrclen, MXTables *t, char *err, int errlen);

/* Encarta 98 Setup.EXE (Wise installer) -> the MindMaze program and its resource DLL (malloc'd); 1 = both found */
int mx_wise_programs(const uint8_t *setup, size_t n, uint8_t **exe, size_t *exelen, uint8_t **dll, size_t *dlllen);

/* tables.json (format "mindmaze-tables-1") as malloc'd text; source = edition name for the "source" field */
char *mx_tables_json(const MXTables *t, const char *source, size_t *len);
void mx_tables_free(MXTables *t);

/* helpers shared by the readers */
char *mx_strdup_1252(const uint8_t *s, size_t n);      /* Windows 1252 bytes -> UTF-8 (n = (size_t)-1: up to NUL) */
char *mx_strdup_macroman(const uint8_t *s, size_t n);  /* Mac Roman bytes -> UTF-8 */
uint8_t *mx_dib_to_bmp(const uint8_t *dib, size_t n, size_t *outlen);   /* BITMAP resource (DIB) -> .bmp file */
