/* A complete data folder from one older Encarta disc (CHANGE 2026-11-18: "allow the extraction for any one of those
 * encarta sources"). The pictures, sounds and the five main MIDIs of every edition are the same as 2006 (95's
 * pictures after removing their mask half); the tables come from the edition's own program (mx_tables_*.c), the
 * questions from its MINDMAZE.DB / .IDX (mx_edition.c converts them). Where the files are:
 *
 *   edition  baggage (pictures, sounds)            program (tables)                          MIDI
 *   95 MPEG  ENCYC95/MM/INTERACT/MM95BAG.M12        ENCYC95/MM/INTERACT/MM95.EXE (16-bit)     ENCYC95/MM/MIDI
 *   96 Mac   Encarta Files/MindMaze Baggage (HFS)   Installer Files/Encarta 96, resource fork  - (QuickTime music)
 *   97       ENCYC97/MMBAG.M20                      AAMSSTP/ENCARTA/ENC97.EXE + ENCRES97.DLL  ENCYC97/MM/MIDI
 *   98       ENCYC98/MMBAG.M20                      SETUP.EXE (Wise installer: ENC98.EXE + DLL) ENCYC98/MM/MIDI
 *
 * The Mac edition has no Standard MIDI files, so a data folder made from it alone plays without music. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"
#include "mx_oldext.h"
#include "mx_tables.h"

const MXOldEdition mx_old_editions[] = {
    {"enc95", "Encarta 95", 95, "ENCYC95/MM/INTERACT", "ENCYC95/MM/INTERACT/MM95BAG.M12", "ENCYC95/MM/INTERACT/MM95.EXE", NULL,
     NULL, "ENCYC95/MM/MIDI"},
    {"enc96", "Encarta 96 (Macintosh)", 96, "Encarta Files", "Encarta Files/MindMaze Baggage", "Installer Files/Encarta 96",
     NULL, NULL, NULL},
    {"enc97", "Encarta 97", 97, "ENCYC97", "ENCYC97/MMBAG.M20", "AAMSSTP/ENCARTA/ENC97.EXE", "AAMSSTP/ENCARTA/ENCRES97.DLL",
     NULL, "ENCYC97/MM/MIDI"},
    {"enc98", "Encarta 98", 98, "ENCYC98", "ENCYC98/MMBAG.M20", NULL, NULL, "SETUP.EXE", "ENCYC98/MM/MIDI"},
    {NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL}};

typedef struct {
    MXIO *io;
    int masked;                  /* Encarta 95: pictures carry a mask half */
    int ndib, nwav, failed;
    char names[200][24];         /* written files (lower case) */
    int nnames;
    char *err;
    int errlen;
} Bag;

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }

static void bag_file(void *ctx, const char *name, const uint8_t *d, size_t n)
{
    Bag *b = ctx;
    char nm[24];
    size_t k = strlen(name);
    if (b->failed || k < 5 || k >= sizeof nm || b->nnames >= 200) return;
    for (size_t i = 0; i < k; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '$'))
            return;                                                /* internal files: |SYSTEM, >00003593 ... */
    }
    memcpy(nm, name, k + 1);
    lower(nm);
    uint8_t *out = NULL;
    size_t on = 0;
    if (!strcmp(nm + k - 4, ".di$")) {
        uint8_t *bmp = mx_kwaj(d, n, &on);
        if (!bmp) { snprintf(b->err, (size_t)b->errlen, "The picture %s on the disc is damaged.", name); b->failed = 1; return; }
        if (b->masked) {
            size_t un = 0;
            uint8_t *u = mx_unmask95(bmp, on, &un);
            if (u) { free(bmp); bmp = u; on = un; }
        }
        out = bmp;
        nm[k - 1] = 'b';                                           /* .di$ -> .dib */
        b->ndib++;
    } else if (!strcmp(nm + k - 4, ".wav")) {
        if (!(out = mx_wav_to_pcm(d, n, &on))) { snprintf(b->err, (size_t)b->errlen, "The sound %s on the disc is damaged.", name); b->failed = 1; return; }
        b->nwav++;
    } else return;
    if (!b->io->write(b->io, nm, out, on)) {
        snprintf(b->err, (size_t)b->errlen, "Could not write %s into the data folder (disk full or no permission?).", nm);
        b->failed = 1;
    } else snprintf(b->names[b->nnames++], sizeof b->names[0], "%s", nm);
    free(out);
}

/* a name from the tables (".DI$" spellings included) is among the written files */
static int have(const Bag *b, const char *name)
{
    char nm[24];
    if (!name || strlen(name) >= sizeof nm) return 0;
    snprintf(nm, sizeof nm, "%s", name);
    lower(nm);
    size_t k = strlen(nm);
    if (k > 4 && !strcmp(nm + k - 4, ".di$")) nm[k - 1] = 'b';
    for (int i = 0; i < b->nnames; i++) if (!strcmp(b->names[i], nm)) return 1;
    return 0;
}

int mx_old_build(MXIO *io, MXOldRead rd, void *ctx, const MXOldEdition *ed, MXOldCounts *c, char *err, int errlen)
{
    Bag *b = calloc(1, sizeof *b);
    MXTables *t = calloc(1, sizeof *t);
    uint8_t *bag = NULL, *exe = NULL, *dll = NULL;
    size_t bn = 0, en = 0, dn = 0;
    int ok = 0;
    memset(c, 0, sizeof *c);
    if (!b || !t) { snprintf(err, (size_t)errlen, "out of memory"); goto done; }
    b->io = io;
    b->masked = ed->year == 95;
    b->err = err;
    b->errlen = errlen;

    if (io->progress) io->progress(io, 10, "Unpacking pictures and sounds");
    if (!(bag = rd(ctx, ed->bag, 0, &bn))) { snprintf(err, (size_t)errlen, "%s: the pictures file (%s) is missing or unreadable.", ed->name, ed->bag); goto done; }
    if (mx_old_files(bag, bn, bag_file, b) < 0) { snprintf(err, (size_t)errlen, "%s: %s is not a MindMaze pictures file (damaged disc?).", ed->name, ed->bag); goto done; }
    if (b->failed) goto done;
    if (b->ndib < 80 || b->nwav < 30) { snprintf(err, (size_t)errlen, "%s: %s holds too few pictures or sounds (damaged disc?).", ed->name, ed->bag); goto done; }
    free(bag);
    bag = NULL;

    if (io->progress) io->progress(io, 40, "Reading the room tables");
    char terr[300] = "";
    int tok = 0;
    if (ed->year == 95) {
        if ((exe = rd(ctx, ed->exe, 0, &en))) tok = mx_tables_ne(exe, en, t, terr, sizeof terr);
    } else if (ed->year == 96) {
        if ((exe = rd(ctx, ed->exe, 1, &en))) tok = mx_tables_mac(exe, en, t, terr, sizeof terr);
    } else if (ed->setup) {
        uint8_t *setup = rd(ctx, ed->setup, 0, &en);
        if (setup && !mx_wise_programs(setup, en, &exe, &en, &dll, &dn)) snprintf(terr, sizeof terr, "the MindMaze program was not found in %s.", ed->setup);
        free(setup);
        if (exe) tok = mx_tables_pe(exe, en, dll, dn, t, terr, sizeof terr);
        else if (!setup) exe = NULL;
    } else {
        exe = rd(ctx, ed->exe, 0, &en);
        dll = exe ? rd(ctx, ed->dll, 0, &dn) : NULL;
        if (exe && dll) tok = mx_tables_pe(exe, en, dll, dn, t, terr, sizeof terr);
        else if (exe) snprintf(terr, sizeof terr, "%s is missing.", ed->dll);
    }
    if (!tok) {
        if (!*terr) snprintf(terr, sizeof terr, "%s is missing or unreadable.", ed->setup ? ed->setup : ed->exe);
        snprintf(err, (size_t)errlen, "%s: %s", ed->name, terr);
        goto done;
    }
    for (int i = 0; i < t->nobjects; i++)
        if (!have(b, t->obj[i].name)) { snprintf(err, (size_t)errlen, "%s: the picture %s is missing on the disc.", ed->name, t->obj[i].name); goto done; }
    for (int i = 0; i < 35; i++)
        if (!have(b, t->sounds[i])) { snprintf(err, (size_t)errlen, "%s: the sound %s is missing on the disc.", ed->name, t->sounds[i]); goto done; }
    size_t jn = 0;
    char *json = mx_tables_json(t, ed->name, &jn);
    int w = json && io->write(io, "tables.json", json, jn);
    free(json);
    for (int i = 0; w && i < MXT_NBITMAPS; i++) {
        char p[32];
        snprintf(p, sizeof p, "res/%d.bmp", mx_table_bitmaps[i]);
        w = io->write(io, p, t->bmp[i], t->bmplen[i]);
    }
    if (!w) { snprintf(err, (size_t)errlen, "Could not write the tables into the data folder."); goto done; }

    if (io->progress) io->progress(io, 55, "Copying the music");
    for (int i = 0; ed->midi && i < 5; i++) {
        char p[128], nm[24];
        size_t ml = 0;
        snprintf(nm, sizeof nm, "%s", t->music[i]);
        lower(nm);
        snprintf(p, sizeof p, "%s/%s", ed->midi, nm);
        uint8_t *mid = rd(ctx, p, 0, &ml);
        if (mid && ml >= 4 && !memcmp(mid, "MThd", 4) && io->write(io, nm, mid, ml)) c->mid++;
        free(mid);
    }
    c->dib = b->ndib;
    c->wav = b->nwav;
    c->strings = 0;
    for (int i = 0; i < MXT_NSTRINGS; i++) c->strings += t->str[i] != NULL;
    ok = 1;
done:
    free(bag);
    free(exe);
    free(dll);
    if (t) mx_tables_free(t);
    free(t);
    free(b);
    return ok;
}
