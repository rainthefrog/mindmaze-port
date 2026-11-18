/* Built-in extractor (see mx.h): Encarta 2006 disc -> data folder. Mirrors tools/extract/extract.py.
 * CHANGE 2026-11-18: the tables are read by patterns (mx_tables_pe.c, no fixed addresses), and a disc without the
 * 2006 files is tried as an older edition (95 MPEG, 96 Mac, 97, 98: mx_edition.c / mx_oldext.c). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"
#include "mx_tables.h"
#include "mx_oldext.h"

const MXFile *mx_files;

/* ---- reference files (Encarta 2006 Premium, US English) ---- */
enum { F_EXE, F_DLL, F_DB, F_IDX, F_EIT, NFILES };
static const struct { const char *name; long size; const char *sha; } REF[NFILES] = {
    {"ENCARTA.EXE", 2992848, "089adc5db30653e4af2d8a37072052c0bddc80049f9943e25b8d41d95905045f"},
    {"ENCARTAR.DLL", 7723728, "5d1705d02996d94b973c634503ce63135aa77ad595e685a29dfd4416857708ef"},
    {"MINDMAZE.DB", 1468278, "a28de3741a8cd16ed3959556470437d64d94b190fd79508146f0d60b87913a7e"},
    {"MINDMAZE.IDX", 64304, "0f16c38bae3ae23d6ca6a3d10eb4b110e1a39da1765c49f9dce34a4606aa04ba"},
    {"MINDMAZE.EIT", 2924179, "b7e3ccec82048f57fc0a7f887569fcc1dce737cd58c113726394ea5287f8fc0e"},
};
static const char *const DISC_DIRS[] = {"EE/ENCARTA/", "EE/KIDS/", ""};

typedef struct {
    MXIO *io;
    char *err;
    int errlen;
    uint8_t *data[NFILES];
    size_t size[NFILES];
    char sha[NFILES][65];
    int ref[NFILES];
    char names[200][64];         /* files written from MINDMAZE.EIT (lower case) */
    int nnames, ndib, nwav, nmid, failed;
} X;

static int fail(X *x, const char *fmt, ...)
{
    if (!x->failed) {
        va_list a;
        va_start(a, fmt);
        vsnprintf(x->err, (size_t)x->errlen, fmt, a);
        va_end(a);
    }
    x->failed = 1;
    return 0;
}
static void progress(X *x, int pct, const char *step) { if (x->io->progress) x->io->progress(x->io, pct, step); }
static int wr(X *x, const char *path, const void *d, size_t n)
{
    if (!x->io->write(x->io, path, d, n)) return fail(x, "Could not write %s into the data folder (disk full or no permission?).", path);
    return 1;
}

/* ---- growable text buffer (JSON) ---- */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        char *np = realloc(b->p, c);
        if (!np) return;
        b->p = np;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bfmt(Buf *b, const char *fmt, ...)
{
    char t[256];
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    bput(b, t, (size_t)(n < 0 ? 0 : n >= (int)sizeof t ? (int)sizeof t - 1 : n));
}
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- source: disc image or folder ---- */
typedef struct { void *f; int64_t size; int iso; char dir[1024]; } Source;

static int iso_read(X *x, Source *s, uint64_t off, void *buf, size_t n)
{
    return x->io->read(x->io, s->f, (int64_t)off, buf, n);
}
/* look up `name` in the ISO 9660 directory at (lba, len); returns 1 and its extent */
static int iso_lookup(X *x, Source *s, uint32_t lba, uint32_t len, const char *name, int want_dir, uint32_t *olba, uint32_t *olen)
{
    uint8_t sec[2048];
    for (uint32_t done = 0; done < len && done < (64u << 20); done += 2048) {
        if (!iso_read(x, s, (uint64_t)(lba + done / 2048) * 2048, sec, 2048)) return 0;
        for (int p = 0; p < 2048;) {
            int rl = sec[p];
            if (rl == 0 || p + rl > 2048 || rl < 34) break;
            int nl = sec[p + 32];
            if (33 + nl <= rl) {
                char nm[64];
                int k = 0;
                for (int i = 0; i < nl && i < 63 && sec[p + 33 + i] != ';'; i++) nm[k++] = (char)sec[p + 33 + i];
                if (k && nm[k - 1] == '.') k--;
                nm[k] = 0;
                int isdir = (sec[p + 25] & 2) != 0;
                int same = (int)strlen(name) == k;
                for (int i = 0; same && i < k; i++) {
                    char a = nm[i], b = name[i];
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                    same = a == b;
                }
                if (same && isdir == want_dir) { *olba = u32(sec + p + 2); *olen = u32(sec + p + 10); return 1; }
            }
            p += rl;
        }
    }
    return 0;
}

static int load_file(X *x, Source *s, int k)
{
    const char *name = REF[k].name;
    if (s->iso) {
        uint8_t pvd[2048];
        if (!iso_read(x, s, 16 * 2048, pvd, 2048)) return 0;
        uint32_t rl = u32(pvd + 156 + 2), rlen = u32(pvd + 156 + 10);
        for (int d = 0; d < 2; d++) {
            const char *sub = d ? "KIDS" : "ENCARTA";
            uint32_t el, elen, sl, slen, fl, flen;
            if (!iso_lookup(x, s, rl, rlen, "EE", 1, &el, &elen) || !iso_lookup(x, s, el, elen, sub, 1, &sl, &slen) ||
                !iso_lookup(x, s, sl, slen, name, 0, &fl, &flen)) continue;
            if (flen > (64u << 20)) return 0;
            x->data[k] = malloc(flen + 1);
            if (!x->data[k] || !iso_read(x, s, (uint64_t)fl * 2048, x->data[k], flen)) return 0;
            x->size[k] = flen;
            return 1;
        }
        return 0;
    }
    /* folder names and file names each in upper case, lower case, or capitalised (case-sensitive file systems) */
    for (int d = 0; d < 3; d++)
        for (int combo = 0; combo < 9; combo++) {
            char path[1200], nm[24], sub[24];
            snprintf(nm, sizeof nm, "%s", name);
            snprintf(sub, sizeof sub, "%s", DISC_DIRS[d]);
            for (int part = 0; part < 2; part++) {
                int style = part ? combo / 3 : combo % 3;              /* 0 upper, 1 lower, 2 capitalised */
                int first = 1;
                for (char *p = part ? sub : nm; *p; p++) {
                    int keep_upper = style == 0 || (style == 2 && first);
                    if (!keep_upper && *p >= 'A' && *p <= 'Z') *p += 32;
                    first = *p == '/';
                }
            }
            if (!*DISC_DIRS[d] && combo >= 3) break;
            snprintf(path, sizeof path, "%s%s%s%s", s->dir, *s->dir ? "/" : "", sub, nm);
            int64_t size = 0;
            void *f = x->io->open(x->io, path, &size);
            if (!f) continue;
            int ok = size >= 0 && size < (64 << 20) && (x->data[k] = malloc((size_t)size + 1)) &&
                     x->io->read(x->io, f, 0, x->data[k], (size_t)size);
            x->io->close(x->io, f);
            if (!ok) return 0;
            x->size[k] = (size_t)size;
            return 1;
        }
    return 0;
}

/* ---- MINDMAZE.EIT contents ---- */
static void eit_file(void *ctx, const char *name, const uint8_t *data, size_t len)
{
    X *x = ctx;
    if (x->failed || strncmp(name, "baggage/", 8)) return;
    char nm[64];
    int k = 0;
    for (const char *p = name + 8; *p && k < 23; p++) {
        char c = *p >= 'A' && *p <= 'Z' ? *p + 32 : *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return;
        nm[k++] = c;
    }
    nm[k] = 0;
    const char *ext = strrchr(nm, '.');
    if (!ext || x->nnames >= 200) return;
    if (!strcmp(ext, ".dib")) {
        if (len < 2 || data[0] != 'B' || data[1] != 'M') { fail(x, "MINDMAZE.EIT: %s is not a picture.", nm); return; }
        if (wr(x, nm, data, len)) x->ndib++;
    } else if (!strcmp(ext, ".mid")) {
        if (wr(x, nm, data, len)) x->nmid++;
    } else if (!strcmp(ext, ".wav")) {
        size_t n = 0;
        uint8_t *pcm = mx_wav_to_pcm(data, len, &n);
        if (!pcm) { fail(x, "MINDMAZE.EIT: %s could not be decoded.", nm); return; }
        if (wr(x, nm, pcm, n)) x->nwav++;
        free(pcm);
    } else return;
    snprintf(x->names[x->nnames++], sizeof x->names[0], "%s", nm);
}
static int have_file(X *x, const char *name)
{
    char nm[64];
    snprintf(nm, sizeof nm, "%s", name);
    for (char *p = nm; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    for (int i = 0; i < x->nnames; i++) if (!strcmp(x->names[i], nm)) return 1;
    return 0;
}

/* ---- main ---- */
int mx_extract(MXIO *io, const char *source, char *err, int errlen)
{
    X *x = calloc(1, sizeof *x);
    Buf man = {0};
    char *json = NULL;
    char edition[64] = "Encarta 2006";
    int old = 0, nstrings = 0, differ = 0;
    Source s = {0};
    if (!x) { snprintf(err, (size_t)errlen, "out of memory"); return 0; }
    x->io = io;
    x->err = err;
    x->errlen = errlen;
    err[0] = 0;

    progress(x, 0, "Reading the disc");
    /* a disc image has "CD001" at 32 KB; anything else is treated as a folder */
    s.f = io->open(io, source, &s.size);
    if (s.f) {
        uint8_t pvd[8];
        if (s.size > 40000 && io->read(io, s.f, 16 * 2048, pvd, 8) && !memcmp(pvd + 1, "CD001", 5)) s.iso = 1;
        else { io->close(io, s.f); s.f = NULL; }
    }
    if (!s.iso) snprintf(s.dir, sizeof s.dir, "%s", source);
    int missing = 0;
    char miss[128] = "";
    for (int k = 0; k < NFILES; k++) {
        progress(x, 2 + 4 * k, "Reading the disc");
        if (!load_file(x, &s, k)) {
            snprintf(miss + strlen(miss), sizeof miss - strlen(miss), "%s%s", missing ? ", " : "", REF[k].name);
            missing++;
        }
    }
    if (s.f) io->close(io, s.f);
    if (!x->data[F_EIT]) {
        /* no 2006 picture archive: an older edition (95 MPEG, 96 Mac, 97, 98)? */
        int nq = 0;
        MXOldCounts oc;
        char id[16];
        int r = mx_extract_old(io, source, id, sizeof id, edition, sizeof edition, &nq, &oc, x->err, x->errlen);
        if (r < 0 && missing < NFILES)
            fail(x, "Not found: %s.\n\nChoose your Encarta disc image (.iso), the disc drive, or a folder holding the "
                    "five files (on the Encarta 2006 disc they are in EE\\ENCARTA).", miss);
        if (r <= 0) {
            x->failed = 1;
            goto done;
        }
        old = 1;
        x->ndib = oc.dib;
        x->nwav = oc.wav;
        x->nmid = oc.mid;
        nstrings = oc.strings;
    } else if (missing) {
        fail(x, "Not found: %s.\n\nChoose your Encarta disc image (.iso), the disc drive, or a folder holding the "
                "five files (on the Encarta 2006 disc they are in EE\\ENCARTA).", miss);
        goto done;
    } else {
        progress(x, 25, "Checking the files");
        for (int k = 0; k < NFILES; k++) {
            uint8_t h[32];
            mx_sha256(x->data[k], x->size[k], h);
            for (int i = 0; i < 32; i++) snprintf(x->sha[k] + 2 * i, 3, "%02x", h[i]);
            x->ref[k] = !strcmp(x->sha[k], REF[k].sha);
            differ += !x->ref[k];
        }
        progress(x, 35, "Unpacking pictures, sounds and music");
        char sub[256];
        if (!mx_itol_files(x->data[F_EIT], x->size[F_EIT], eit_file, x, sub, sizeof sub)) { fail(x, "%s", sub); goto done; }
        if (x->failed) goto done;
        if (!x->ndib || !x->nwav) { fail(x, "MINDMAZE.EIT holds no pictures or sounds."); goto done; }

        progress(x, 70, "Reading the room tables, panel pictures and texts");
        MXTables *t = calloc(1, sizeof *t);
        char terr[300];
        if (!t) { fail(x, "out of memory"); goto done; }
        if (!mx_tables_pe(x->data[F_EXE], x->size[F_EXE], x->data[F_DLL], x->size[F_DLL], t, terr, sizeof terr)) {
            fail(x, "%s\nIf this is an unusual Encarta 2006 copy (another language or update), please open an issue on the "
                    "project page with this line:\nENCARTA.EXE %lu bytes, SHA-256 %s", terr, (unsigned long)x->size[F_EXE], x->sha[F_EXE]);
            free(t);
            goto done;
        }
        for (int i = 0; i < t->nobjects && !x->failed; i++)
            if (!have_file(x, t->obj[i].name)) fail(x, "%s is named in ENCARTA.EXE but missing from MINDMAZE.EIT.", t->obj[i].name);
        for (int i = 0; i < 6 && !x->failed; i++)
            if (!have_file(x, t->rank[i].image)) fail(x, "%s is named in ENCARTA.EXE but missing from MINDMAZE.EIT.", t->rank[i].image);
        for (int i = 0; i < 35 && !x->failed; i++)
            if (!have_file(x, t->sounds[i])) fail(x, "%s is named in ENCARTA.EXE but missing from MINDMAZE.EIT.", t->sounds[i]);
        for (int i = 0; i < 5 && !x->failed; i++)
            if (!have_file(x, t->music[i])) fail(x, "%s is named in ENCARTA.EXE but missing from MINDMAZE.EIT.", t->music[i]);
        size_t jn = 0;
        if (!x->failed && !(json = mx_tables_json(t, "Encarta 2006 Premium (US English)", &jn))) fail(x, "out of memory");
        for (int i = 0; i < MXT_NBITMAPS && !x->failed; i++) {
            char path[32];
            snprintf(path, sizeof path, "res/%d.bmp", mx_table_bitmaps[i]);
            wr(x, path, t->bmp[i], t->bmplen[i]);
        }
        for (int i = 0; i < MXT_NSTRINGS; i++) nstrings += t->str[i] != NULL;
        mx_tables_free(t);
        free(t);
        if (x->failed || !wr(x, "tables.json", json, jn)) goto done;
        progress(x, 85, "Copying the questions");
        if (!wr(x, "mindmaze.db", x->data[F_DB], x->size[F_DB]) || !wr(x, "mindmaze.idx", x->data[F_IDX], x->size[F_IDX])) goto done;
    }

    progress(x, 90, "Adding question packs and the art kit");
    int nembedded = 0;
    for (const MXFile *f = mx_files; f && f->name; f++) {
        const uint8_t *d = f->data;
        size_t n = f->size;
        uint8_t *raw = NULL;
        if (f->zlib) {
            if (!(raw = mx_inflate_zlib(f->data, f->size, &n))) { fail(x, "Built-in file %s is damaged.", f->name); goto done; }
            d = raw;
        }
        int ok = wr(x, f->name, d, n);
        free(raw);
        if (!ok) goto done;
        nembedded++;
    }

    bstr(&man, "{\n \"format\": \"mindmaze-data-1\",\n \"tool\": \"mindmaze-port built-in extractor " MX_VERSION "\",\n");
    bfmt(&man, " \"source\": {\"kind\": \"%s\", \"edition\": \"%s\"},\n \"files\": {", s.iso ? "image" : "folder", edition);
    for (int k = 0; k < NFILES && !old; k++)
        bfmt(&man, "%s\n  \"%s\": {\"bytes\": %lu, \"sha256\": \"%s\", \"reference\": %s}", k ? "," : "", REF[k].name,
             (unsigned long)x->size[k], x->sha[k], x->ref[k] ? "true" : "false");
    bfmt(&man, "\n },\n \"counts\": {\"dib\": %d, \"wav\": %d, \"mid\": %d, \"strings\": %d, \"embedded\": %d}\n}\n",
         x->ndib, x->nwav, x->nmid, nstrings, nembedded);
    if (!wr(x, "manifest.json", man.p, man.n)) goto done;
    progress(x, 100, differ ? "Done (some files differ from the reference disc)" : "Done");

done:
    for (int k = 0; k < NFILES; k++) free(x->data[k]);
    free(json);
    free(man.p);
    int ok = !x->failed;
    free(x);
    return ok;
}
