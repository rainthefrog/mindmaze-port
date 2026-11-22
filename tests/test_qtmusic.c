/* Tests of the QuickTime music -> SMF converter (src/extract/mx_qtmusic.c, CHANGE 2026-11-21).
 *
 *   build/test_qtmusic                      a synthetic movie built here (make test; no Encarta files needed)
 *   build/test_qtmusic MAC.iso WINMIDI/     plus the Encarta 96 Macintosh disc's 14 music movies, each converted
 *                                           and compared with the Windows edition's MIDI file of the same name
 *                                           (a folder such as ENCYC95/MM/MIDI or ENCYC97/MM/MIDI of a Windows disc)
 * (or the environment variables MM_MAC96_ISO / MM_WIN_MIDI; `make test-qtmusic MAC96=... WINMIDI=...`). The disc
 * part is skipped with a note when the files are absent.
 *
 * The comparison: both files are read into notes (channel, pitch, velocity, start and end in seconds through the
 * tempo map, program at the start). The Windows files are Microsoft "dual mode" MIDI (every part twice: extended
 * channels 1-10 and base-level channels 13-16); the extended channels are compared (the Mac movies hold that copy).
 * Each Mac channel is paired with the Windows channel with the same pitch sequence; the test requires the same note
 * count, pitches, programs (drum channel: kit not compared, Windows selects 6, the Mac "Standard Kit") and velocities
 * (except doubled notes: the movies hold a few notes twice with the same velocity where Windows has a 0-length note
 * of another velocity), start and end times within 2 ms + 0.3 % (the movies' rests are rounded to 1/600 s: up to
 * 0.28 % slower, 87 ms after 40 s in INTRO), at most 2 % of the note lengths off by > 5 ms. Where the Windows file
 * repeats the Mac one (QUESTION.MID: 16 identical rounds) the first round is compared. The Mac disc's FINALET.mid is
 * a copy of its BGLOOP3.mid and is compared with Windows BGLOOP3. MM_QT_VERBOSE=1 lists every note; MM_QT_OUT=<dir>
 * keeps the converted files. */
#define _GNU_SOURCE                             /* memmem */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"
#include "mx_oldext.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------------------------------------- SMF reader */
typedef struct { int ch, pitch, vel, program; double on, off; } Note;
typedef struct { Note *n; int count; double end; int division; } Song;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

typedef struct { uint32_t tick; uint32_t tempo; } Tempo;

static uint32_t var(const uint8_t *d, size_t n, size_t *i)
{
    uint32_t v = 0;
    for (int k = 0; k < 4 && *i < n; k++) {
        uint8_t c = d[(*i)++];
        v = v << 7 | (c & 127);
        if (!(c & 128)) break;
    }
    return v;
}

static double tick_sec(const Tempo *t, int nt, uint32_t tick, int div)
{
    double s = 0;
    uint32_t at = 0, tempo = 500000;
    for (int i = 0; i < nt && t[i].tick <= tick; i++) {
        s += (double)(t[i].tick - at) * tempo / div / 1e6;
        at = t[i].tick;
        tempo = t[i].tempo;
    }
    return s + (double)(tick - at) * tempo / div / 1e6;
}

static int tempo_cmp(const void *a, const void *b) { const Tempo *x = a, *y = b; return x->tick < y->tick ? -1 : x->tick > y->tick; }
static int note_cmp(const void *a, const void *b)
{
    const Note *x = a, *y = b;
    if (x->on != y->on) return x->on < y->on ? -1 : 1;
    if (x->pitch != y->pitch) return x->pitch - y->pitch;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;      /* a doubled note: the one cut to length 0 first */
    return x->vel - y->vel;
}

/* two passes: tempo map from every track, then the notes */
static int smf_read(const uint8_t *d, size_t n, Song *s)
{
    memset(s, 0, sizeof *s);
    if (n < 14 || memcmp(d, "MThd", 4)) return 0;
    int ntrk = d[10] << 8 | d[11], div = d[12] << 8 | d[13];
    if (div <= 0 || div > 0x7FFF) return 0;
    s->division = div;
    Tempo tempo[256];
    int nt = 0, cap = 0;
    uint32_t endtick = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) qsort(tempo, (size_t)nt, sizeof *tempo, tempo_cmp);
        size_t o = 8 + rd32(d + 4);
        for (int t = 0; t < ntrk && o + 8 <= n; t++) {
            size_t len = rd32(d + o + 4), i = 0;
            if (memcmp(d + o, "MTrk", 4) || len > n - o - 8) return 0;
            const uint8_t *k = d + o + 8;
            uint32_t tick = 0;
            int st = 0, program[16] = {0};
            int open[16][128];
            memset(open, 0xFF, sizeof open);
            while (i < len) {
                tick += var(k, len, &i);
                if (i >= len) break;
                int c = k[i];
                if (c == 0xFF) {
                    if (i + 2 > len) break;
                    int type = k[i + 1];
                    i += 2;
                    uint32_t l = var(k, len, &i);
                    if (l > len - i) break;
                    if (pass == 0 && type == 0x51 && l == 3 && nt < 256) tempo[nt++] = (Tempo){tick, (uint32_t)k[i] << 16 | k[i + 1] << 8 | k[i + 2]};
                    if (type == 0x2F && tick > endtick) endtick = tick;
                    i += l;
                    continue;
                }
                if (c == 0xF0 || c == 0xF7) { i++; uint32_t l = var(k, len, &i); i += l; continue; }
                if (c & 0x80) { st = c; i++; }
                int kind = st & 0xF0, ch = st & 15, nb = kind == 0xC0 || kind == 0xD0 ? 1 : 2;
                if (i + (size_t)nb > len) break;
                int a = k[i], b = nb > 1 ? k[i + 1] : 0;
                i += (size_t)nb;
                if (pass == 0) continue;
                if (kind == 0xC0) program[ch] = a;
                double now = tick_sec(tempo, nt, tick, div);
                if ((kind == 0x80 || (kind == 0x90 && !b)) && open[ch][a] >= 0) { s->n[open[ch][a]].off = now; open[ch][a] = -1; }
                else if (kind == 0x90 && b) {
                    if (open[ch][a] >= 0) s->n[open[ch][a]].off = now;           /* restrike */
                    if (s->count == cap) {
                        cap = cap ? cap * 2 : 1024;
                        Note *nn = realloc(s->n, (size_t)cap * sizeof *nn);
                        if (!nn) return 0;
                        s->n = nn;
                    }
                    s->n[s->count] = (Note){ch, a, b, program[ch], now, now};
                    open[ch][a] = s->count++;
                }
            }
            o += 8 + len;
        }
    }
    s->end = tick_sec(tempo, nt, endtick, div);
    if (s->count) qsort(s->n, (size_t)s->count, sizeof *s->n, note_cmp);
    return 1;
}

/* the notes of one channel (in time order) */
static int channel_notes(const Song *s, int ch, Note *out)
{
    int k = 0;
    for (int i = 0; i < s->count; i++) if (s->n[i].ch == ch) out[k++] = s->n[i];
    return k;
}

/* ---------------------------------------------------------------------------------------------- synthetic movie */
typedef struct { uint8_t b[4096]; size_t n; } Bytes;
static void w32(Bytes *b, uint32_t v) { b->b[b->n++] = (uint8_t)(v >> 24); b->b[b->n++] = (uint8_t)(v >> 16); b->b[b->n++] = (uint8_t)(v >> 8); b->b[b->n++] = (uint8_t)v; }
static void wraw(Bytes *b, const void *d, size_t n) { memcpy(b->b + b->n, d, n); b->n += n; }
static size_t atom_begin(Bytes *b, const char *t) { size_t at = b->n; w32(b, 0); wraw(b, t, 4); return at; }
static void atom_end(Bytes *b, size_t at) { uint32_t sz = (uint32_t)(b->n - at); b->b[at] = (uint8_t)(sz >> 24); b->b[at + 1] = (uint8_t)(sz >> 16); b->b[at + 2] = (uint8_t)(sz >> 8); b->b[at + 3] = (uint8_t)sz; }

static void note_request(Bytes *b, int part, uint32_t gm)
{
    w32(b, 0xF0000000u | (uint32_t)part << 16 | 23);
    w32(b, 0x00000001);                                         /* flags, reserved, polyphony 1 */
    w32(b, 0x00010000);                                         /* typical polyphony 1.0 */
    for (int i = 0; i < 18; i++) w32(b, 0);                     /* synth type, names, instrument number */
    w32(b, gm);
    w32(b, 0xC0000000u | 1u << 16 | 23);                        /* subtype 1: note request */
}

static void synthetic(void)
{
    /* data fork: mdat with one sample; events at timescale 1000 */
    Bytes data = {{0}, 0}, mv = {{0}, 0}, rs = {{0}, 0};
    size_t md = atom_begin(&data, "mdat");
    size_t s0 = data.n;
    w32(&data, 0x40076400);                                     /* part 0 volume 100 */
    w32(&data, 0x400A4000);                                     /* part 0 pan 64 */
    w32(&data, 0x20000000u | (60u - 32) << 18 | 100u << 11 | 500); /* part 0 C4 v100 500 */
    w32(&data, 0x21000000u | (36u - 32) << 18 | 90u << 11 | 100);  /* part 1 (drums) kick v90 100 */
    w32(&data, 250);                                            /* rest 250 */
    w32(&data, 0x20000000u | (60u - 32) << 18 | 80u << 11 | 400);  /* restrike C4 at 250: ends the first */
    w32(&data, 0x40200000u | 0x0100);                           /* pitch bend +1 semitone */
    w32(&data, 0x402A0000u | 0x4000);                           /* part volume (QuickTime only): not mapped */
    w32(&data, 750);
    w32(&data, 0x90000000u | 72);                               /* extended note part 0 C5 */
    w32(&data, 0x80000000u | 70u << 22 | 200);
    w32(&data, 250);
    w32(&data, 0x60000000);                                     /* end */
    uint32_t ssz = (uint32_t)(data.n - s0);
    atom_end(&data, md);

    size_t moov = atom_begin(&mv, "moov"), trak = atom_begin(&mv, "trak"), mdia = atom_begin(&mv, "mdia");
    size_t a = atom_begin(&mv, "mdhd");
    w32(&mv, 0); w32(&mv, 0); w32(&mv, 0); w32(&mv, 1000); w32(&mv, 1500); w32(&mv, 0);
    atom_end(&mv, a);
    a = atom_begin(&mv, "hdlr");
    w32(&mv, 0); wraw(&mv, "mhlrmusi", 8); w32(&mv, 0); w32(&mv, 0); w32(&mv, 0);
    atom_end(&mv, a);
    size_t minf = atom_begin(&mv, "minf"), stbl = atom_begin(&mv, "stbl");
    a = atom_begin(&mv, "stsd");
    w32(&mv, 0); w32(&mv, 1);
    size_t desc = mv.n;
    w32(&mv, 0); wraw(&mv, "musi", 4); w32(&mv, 0); w32(&mv, 1); w32(&mv, 0);
    note_request(&mv, 0, 1);                                    /* piano */
    note_request(&mv, 1, 16385);                                /* standard drum kit */
    w32(&mv, 0x60000000);
    uint32_t dl = (uint32_t)(mv.n - desc);
    mv.b[desc] = (uint8_t)(dl >> 24); mv.b[desc + 1] = (uint8_t)(dl >> 16); mv.b[desc + 2] = (uint8_t)(dl >> 8); mv.b[desc + 3] = (uint8_t)dl;
    atom_end(&mv, a);
    a = atom_begin(&mv, "stts"); w32(&mv, 0); w32(&mv, 1); w32(&mv, 1); w32(&mv, 1500); atom_end(&mv, a);
    a = atom_begin(&mv, "stsc"); w32(&mv, 0); w32(&mv, 1); w32(&mv, 1); w32(&mv, 1); w32(&mv, 1); atom_end(&mv, a);
    a = atom_begin(&mv, "stsz"); w32(&mv, 0); w32(&mv, ssz); w32(&mv, 1); atom_end(&mv, a);
    a = atom_begin(&mv, "stco"); w32(&mv, 0); w32(&mv, 1); w32(&mv, (uint32_t)s0); atom_end(&mv, a);
    atom_end(&mv, stbl); atom_end(&mv, minf); atom_end(&mv, mdia); atom_end(&mv, trak); atom_end(&mv, moov);

    /* resource fork with one 'moov' resource id 128 */
    w32(&rs, 256); w32(&rs, (uint32_t)(256 + 4 + mv.n)); w32(&rs, (uint32_t)(4 + mv.n)); w32(&rs, 30 + 2 + 8 + 12);
    while (rs.n < 256) rs.b[rs.n++] = 0;
    w32(&rs, (uint32_t)mv.n);
    wraw(&rs, mv.b, mv.n);
    for (int i = 0; i < 24; i++) rs.b[rs.n++] = 0;              /* map header copy, handle, file ref, attrs */
    rs.b[rs.n++] = 0; rs.b[rs.n++] = 28;                        /* type list offset */
    rs.b[rs.n++] = 0; rs.b[rs.n++] = 0;                         /* name list offset */
    rs.b[rs.n++] = 0; rs.b[rs.n++] = 0;                         /* one type */
    wraw(&rs, "moov", 4);
    rs.b[rs.n++] = 0; rs.b[rs.n++] = 0;                         /* one resource */
    rs.b[rs.n++] = 0; rs.b[rs.n++] = 10;                        /* ref list at type list + 10 */
    rs.b[rs.n++] = 0; rs.b[rs.n++] = 128;                       /* id 128 */
    rs.b[rs.n++] = 0xFF; rs.b[rs.n++] = 0xFF;                   /* no name */
    w32(&rs, 0);                                                /* attrs 0, data offset 0 */
    w32(&rs, 0);

    size_t on = 0;
    MXQtStats st;
    char err[128];
    uint8_t *smf = mx_qtmusic_to_smf(data.b, data.n, rs.b, rs.n, &on, &st, err, sizeof err);
    CHECK(smf != NULL, "synthetic movie: %s", err);
    if (!smf) return;
    CHECK(st.division == 1000 && st.parts == 2 && st.notes == 4 && st.restrikes == 1 && st.unmapped == 1, "stats: div %d parts %d notes %d restrikes %d unmapped %d",
          st.division, st.parts, st.notes, st.restrikes, st.unmapped);
    Song s;
    CHECK(smf_read(smf, on, &s), "synthetic SMF unreadable");
    CHECK(s.count == 4, "notes %d", s.count);
    if (s.count == 4) {
        /* sorted by start: C4 0 (ended at 0.25 by the restrike), kick 0 (drums: channel 10), C4 0.25-0.65, C5 1.0-1.2 */
        CHECK(s.n[0].pitch == 36 && s.n[0].ch == 9 && s.n[0].vel == 90 && fabs(s.n[0].off - 0.1) < 1e-9, "kick: ch %d pitch %d off %f", s.n[0].ch, s.n[0].pitch, s.n[0].off);
        CHECK(s.n[1].pitch == 60 && s.n[1].ch == 0 && s.n[1].program == 0 && fabs(s.n[1].off - 0.25) < 1e-9, "first C4: off %f", s.n[1].off);
        CHECK(fabs(s.n[2].on - 0.25) < 1e-9 && fabs(s.n[2].off - 0.65) < 1e-9 && s.n[2].vel == 80, "restruck C4: %f-%f", s.n[2].on, s.n[2].off);
        CHECK(s.n[3].pitch == 72 && fabs(s.n[3].on - 1.0) < 1e-9 && fabs(s.n[3].off - 1.2) < 1e-9 && s.n[3].vel == 70, "extended note");
    }
    CHECK(fabs(s.end - 1.2) < 1e-9 && st.media_end == 1500, "end of track %f (the last note-off; movie 1.5 s)", s.end);
    /* the volume, pan and pitch-bend bytes */
    CHECK(memmem(smf, on, "\xB0\x07\x64", 3) && memmem(smf, on, "\xB0\x0A\x40", 3) && memmem(smf, on, "\xE0\x00\x60", 3), "controllers");
    CHECK(memmem(smf, on, "\xC9\x00", 2) != NULL, "drum kit program on channel 10");
    free(s.n);
    free(smf);

    /* damaged input is refused, never read out of bounds */
    CHECK(!mx_qtmusic_to_smf(data.b, data.n, NULL, 0, &on, NULL, err, sizeof err), "no moov must fail");
    for (size_t cut = 0; cut < rs.n; cut += 7) {
        uint8_t *x = mx_qtmusic_to_smf(data.b, data.n, rs.b, cut, &on, NULL, err, sizeof err);
        free(x);
    }
    for (size_t cut = 0; cut < data.n; cut += 5) {
        uint8_t *x = mx_qtmusic_to_smf(data.b, cut, rs.b, rs.n, &on, NULL, err, sizeof err);
        free(x);
    }
}

/* ---------------------------------------------------------------------------------------------- the disc */
static void *f_open(MXIO *io, const char *p, int64_t *size)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    return f;
}
static int f_read(MXIO *io, void *f, int64_t off, void *b, size_t n) { return !fseek(f, (long)off, SEEK_SET) && fread(b, 1, n, f) == n; }
static void f_close(MXIO *io, void *f) { fclose(f); }

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = sz > 0 ? malloc((size_t)sz) : NULL;
    if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)sz;
    return d;
}

static uint8_t *load_win(const char *dir, const char *name, size_t *n)
{
    char p[1200], up[64], lo[64];
    snprintf(up, sizeof up, "%s", name);
    snprintf(lo, sizeof lo, "%s", name);
    for (char *q = up; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
    for (char *q = lo; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
    const char *v[] = {up, lo, name};
    for (int i = 0; i < 3; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, v[i]);
        uint8_t *d = load(p, n);
        if (d) return d;
    }
    return NULL;
}

typedef struct {
    int files, notes, matched, pitch_ok, vel_ok, vel_dup, prog_ok, len_diff, ncmp;
    double on_max, on_sum, drift_max, end_max;
} Total;

/* Start times may drift: the disc's movies have the rests rounded to 1/600 s (up to 0.21 % slower than Windows).
 * Allowed: 2 ms + 0.3 % of the time. */
static int time_ok(double d, double t) { return fabs(d) <= 0.002 + 0.003 * t; }

static void compare(const char *name, const Song *mac, const Song *win, Total *T)
{
    static Note a[8192], b[8192];
    int winext = 0;
    for (int i = 0; i < win->count; i++) winext += win->n[i].ch < 10;
    int used[16] = {0}, mac_notes = 0, win_notes = 0, matched = 0, pitch_ok = 0, vel_ok = 0, vel_dup = 0, prog_ok = 0, len_diff = 0,
        rounds = 1, rounds_ok = 1, late = 0, ncmp = 0;
    double on_max = 0, on_sum = 0, drift = 0, end_w = 0;
    for (int ch = 0; ch < 16; ch++) {
        int na = channel_notes(mac, ch, a);
        if (!na) continue;
        mac_notes += na;
        /* the Windows channel (extended range 1-10 when it has notes) with the most equal pitches from the start */
        int best = -1, best_score = -1;
        for (int wc = 0; wc < 16; wc++) {
            if (used[wc] || (winext && wc >= 10)) continue;
            int nb = channel_notes(win, wc, b);
            if (!nb) continue;
            int score = 0;
            for (int i = 0; i < na && i < nb && b[i].pitch == a[i].pitch; i++) score++;
            if (score > best_score) { best_score = score; best = wc; }
        }
        if (best < 0) { CHECK(0, "%s: mac channel %d (%d notes) has no Windows channel", name, ch + 1, na); continue; }
        used[best] = 1;
        int nb = channel_notes(win, best, b);
        win_notes += nb;
        /* the Windows channel may repeat the Mac part (rounds): compare the first, check the others are equal */
        int r = nb > na && nb % na == 0 ? nb / na : 1;
        if (r > 1) {
            rounds = r;
            for (int k = 1; k < r; k++)
                for (int i = 0; i < na; i++) if (b[k * na + i].pitch != b[i].pitch || b[k * na + i].vel != b[i].vel) { rounds_ok = 0; break; }
        }
        int n = na < nb ? na : nb, p = 0, v = 0, vd = 0, pr = 0, drum = ch == 9;
        for (int i = 0; i < n; i++) {
            double d = a[i].on - b[i].on, la = a[i].off - a[i].on, lb = b[i].off - b[i].on, l = fabs(la - lb);
            p += a[i].pitch == b[i].pitch;
            if (a[i].vel == b[i].vel) v++;
            else if (la == 0 && lb == 0) vd++;              /* a doubled note (cut to 0 by its twin): see below */
            pr += drum || a[i].program == b[i].program;     /* drum channel: the kit (Windows 6, Mac Standard) */
            len_diff += l > 0.005;
            if (fabs(d) > on_max) on_max = fabs(d);
            if (b[i].on > 1 && fabs(d) / b[i].on > drift) drift = fabs(d) / b[i].on;
            late += !time_ok(d, b[i].on);
            if (b[i].off > end_w) end_w = b[i].off;
            on_sum += fabs(d);
            ncmp++;
            if (getenv("MM_QT_VERBOSE"))                    /* every note: start, length, velocity on both sides */
                printf("    %s ch %d #%d pitch %d: start mac %.4f win %.4f (%+.1f ms) length mac %.4f win %.4f velocity %d %d%s\n", name,
                       ch + 1, i, a[i].pitch, a[i].on, b[i].on, d * 1000, la, lb, a[i].vel, b[i].vel,
                       a[i].vel != b[i].vel || l > 0.005 ? "  <--" : "");
        }
        matched += n;
        pitch_ok += p;
        vel_ok += v;
        vel_dup += vd;
        prog_ok += pr;
        CHECK(n == na && (r > 1 || na == nb), "%s: mac channel %d has %d notes, Windows channel %d has %d", name, ch + 1, na, best + 1, nb);
        CHECK(p == n && v + vd == n && pr == n, "%s: mac channel %d vs Windows %d: pitches %d/%d, velocities %d/%d, programs %d/%d", name,
              ch + 1, best + 1, p, n, v + vd, n, pr, n);
    }
    double end_d = mac->end - end_w;
    printf("  %-12s notes %4d / %4d | pitch %4d velocity %4d (+%d doubled) program %4d of %4d | start diff max %5.1f ms mean %4.1f ms "
           "(drift %.2f %%) | length diff > 5 ms: %d | end %6.3f s vs %6.3f s\n",
           name, mac_notes, win_notes, pitch_ok, vel_ok, vel_dup, prog_ok, matched, on_max * 1000, ncmp ? on_sum / ncmp * 1000 : 0,
           drift * 100, len_diff, mac->end, end_w);
    if (rounds > 1) printf("               Windows plays the same part %d times in a row (%s); its first round is compared\n", rounds,
                           rounds_ok ? "identical rounds" : "rounds DIFFER");
    CHECK(rounds_ok, "%s: the Windows rounds differ", name);
    CHECK(!late, "%s: %d start times off by more than 2 ms + 0.3 %% (max %.1f ms)", name, late, on_max * 1000);
    CHECK(time_ok(end_d, end_w), "%s: end of track %.3f s vs %.3f s", name, mac->end, end_w);
    CHECK(len_diff * 50 <= matched, "%s: %d note lengths differ", name, len_diff);
    T->files++;
    T->notes += mac_notes;
    T->matched += matched;
    T->pitch_ok += pitch_ok;
    T->vel_ok += vel_ok;
    T->vel_dup += vel_dup;
    T->prog_ok += prog_ok;
    T->len_diff += len_diff;
    T->on_sum += on_sum;
    T->ncmp += ncmp;
    if (on_max > T->on_max) T->on_max = on_max;
    if (drift > T->drift_max) T->drift_max = drift;
    if (fabs(end_d) > T->end_max) T->end_max = fabs(end_d);
}

static void disc(const char *iso, const char *windir)
{
    static const char *const NAMES[] = {"INTRO.mid", "BGLOOP1.mid", "BGLOOP2.mid", "BGLOOP3.mid", "QUESTION.MID", "ATRIUM.mid",
                                        "FINALET.mid", "G-FINAL.MID", "1-2.mid", "1-3.mid", "2-1.mid", "2-3.mid", "3-1.mid", "3-2.mid", NULL};
    MXIO io = {f_open, f_read, f_close, NULL, NULL, NULL};
    int64_t size = 0;
    void *f = iso && *iso ? f_open(&io, iso, &size) : NULL;
    size_t wn = 0;
    uint8_t *probe = windir && *windir ? load_win(windir, "INTRO.MID", &wn) : NULL;
    if (!f || !probe) {
        printf("test_qtmusic: disc comparison skipped (%s)\n", !f ? "no Encarta 96 Mac image: MM_MAC96_ISO" : "no Windows MIDI folder: MM_WIN_MIDI");
        if (f) fclose(f);
        free(probe);
        return;
    }
    free(probe);
    Total T = {0};
    MXQtStats sum = {0};
    printf("test_qtmusic: %s vs %s (notes Mac / Windows; Windows: extended channels 1-10)\n", iso, windir);
    uint8_t *bg3[2] = {NULL, NULL};
    size_t bg3n[2] = {0, 0};
    for (int i = 0; NAMES[i]; i++) {
        size_t dn = 0, rn = 0, on = 0;
        uint8_t *d = mx_hfs_file(&io, f, size, "Encarta Files/MIDI", NAMES[i], 0, &dn);
        uint8_t *r = mx_hfs_file(&io, f, size, "Encarta Files/MIDI", NAMES[i], 1, &rn);
        CHECK(d && r, "%s missing on the Mac disc", NAMES[i]);
        MXQtStats st;
        char err[128] = "";
        uint8_t *smf = d && r ? mx_qtmusic_to_smf(d, dn, r, rn, &on, &st, err, sizeof err) : NULL;
        CHECK(smf != NULL || !d, "%s: %s", NAMES[i], err);
        /* the Mac disc's FINALET.mid is a copy of its BGLOOP3.mid (same data fork and music; the resource forks differ
         * only in names; Windows FINALET is other music): it is compared with Windows BGLOOP3 */
        const char *ref = NAMES[i];
        if (!strcmp(NAMES[i], "BGLOOP3.mid") && d && smf) {
            bg3[0] = malloc(dn); bg3[1] = malloc(on);
            if (bg3[0] && bg3[1]) { memcpy(bg3[0], d, dn); memcpy(bg3[1], smf, on); bg3n[0] = dn; bg3n[1] = on; }
        }
        if (!strcmp(NAMES[i], "FINALET.mid") && d && smf && bg3[0] && bg3[1] && dn == bg3n[0] && on == bg3n[1] && !memcmp(d, bg3[0], dn) &&
            !memcmp(smf, bg3[1], on)) {
            printf("  FINALET.mid on the Mac disc = BGLOOP3.mid (same data fork, same SMF); compared with Windows BGLOOP3.MID\n");
            ref = "BGLOOP3.mid";
        }
        uint8_t *w = load_win(windir, ref, &wn);
        CHECK(w != NULL, "%s missing in %s", ref, windir);
        Song ms, ws;
        if (smf && w && smf_read(smf, on, &ms) && smf_read(w, wn, &ws)) {
            CHECK(st.unmapped == 0, "%s: %d events not mapped", NAMES[i], st.unmapped);
            CHECK(st.division == 600, "%s: division %d", NAMES[i], st.division);
            compare(NAMES[i], &ms, &ws, &T);
            printf("               %d parts, %d programs, %d controllers, %d doubled notes ended early, %d samples; movie %.3f s\n", st.parts,
                   st.programs, st.controllers, st.restrikes, st.samples, st.media_end / 600.0);
            sum.parts += st.parts; sum.controllers += st.controllers; sum.restrikes += st.restrikes; sum.unmapped += st.unmapped;
            free(ms.n);
            free(ws.n);
        } else if (smf && w) CHECK(0, "%s: SMF unreadable", NAMES[i]);
        if (getenv("MM_QT_OUT") && smf) {                       /* keep the converted files for listening */
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", getenv("MM_QT_OUT"), NAMES[i]);
            FILE *o = fopen(p, "wb");
            if (o) { fwrite(smf, 1, on, o); fclose(o); }
        }
        free(smf);
        free(w);
        free(d);
        free(r);
    }
    free(bg3[0]);
    free(bg3[1]);
    fclose(f);
    printf("test_qtmusic: %d files, %d notes; equal pitch %d, velocity %d (+%d doubled notes), program %d of %d compared; start diff max "
           "%.1f ms, mean %.2f ms, drift max %.2f %%; note lengths differing > 5 ms: %d; end of track diff max %.1f ms; %d controllers, "
           "%d events not mapped\n",
           T.files, T.notes, T.pitch_ok, T.vel_ok, T.vel_dup, T.prog_ok, T.matched, T.on_max * 1000, T.ncmp ? T.on_sum / T.ncmp * 1000 : 0,
           T.drift_max * 100, T.len_diff, T.end_max * 1000, sum.controllers, sum.unmapped);
    CHECK(T.files == 14, "%d of 14 files compared", T.files);
}

int main(int argc, char **argv)
{
    synthetic();
    const char *iso = argc > 1 ? argv[1] : getenv("MM_MAC96_ISO");
    const char *win = argc > 2 ? argv[2] : getenv("MM_WIN_MIDI");
    disc(iso, win);
    printf(fails ? "test_qtmusic: %d FAILED\n" : "test_qtmusic: all passed\n", fails);
    return fails != 0;
}
