/* QuickTime music movie -> Standard MIDI file (CHANGE 2026-11-21: the Encarta 96 Macintosh disc keeps MindMaze's
 * music as QuickTime music movies, "Encarta Files/MIDI/<name>.mid", HFS type MooV/TVOD, instead of the Windows
 * editions' SMF files; mx_qtmusic_to_smf converts one so a data folder made from that disc has music too).
 *
 * Where the parts are (all big-endian):
 *   moov atom      in the resource fork ('moov' resource, id 128 on the disc) or, for a flattened movie, a top-level
 *                  atom of the data fork. moov > trak > mdia > {mdhd (timescale, duration), hdlr (subtype 'musi'),
 *                  minf > stbl > {stsd, stts, stsc, stsz, stco}}.
 *   samples        in the data fork ('mdat' atom) at the stco chunk offsets; the disc's movies have 1..4 samples
 *                  of up to 1024 bytes, each ending with an end marker; a sample starts at the sum of the stts
 *                  durations before it.
 *   stsd 'musi'    u32 size, 'musi', 6 reserved, u16 dref index, u32 flags, then the tune header: QuickTime Music
 *                  Architecture events, one note request per part.
 *   QTMA events    32-bit words; top 3 bits 000 rest (24-bit duration), 001 note (5-bit part, 6-bit pitch - 32,
 *                  7-bit velocity, 11-bit duration), 010 controller (5-bit part, 8-bit controller, 8.8 value),
 *                  011 marker (8-bit subtype, 0 = end of the sample). Top 4 bits 1001 extended note, 1010 extended
 *                  controller, 1011 knob (two words each; 12-bit part), 1111 general event (12-bit part, 16-bit
 *                  length in words incl. the first and the last word; last word = 11b | 14-bit subtype << 16 |
 *                  length). General subtype 1 note request = {u8 flags, u8 reserved, u16 polyphony, Fixed typical
 *                  polyphony, OSType synth, Str31 synth name, Str31 instrument name, u32 instrument, u32 GM number}
 *                  (GM 1..128 = program 0..127, 16385.. = drum kit -> MIDI channel 10); subtype 8 = MIDI channel.
 *                  Durations are in the media timescale (600 per second on the disc).
 * Output: SMF format 0, one track; division = the media timescale and tempo 1,000,000 us per quarter, so one tick
 * is exactly one QuickTime time unit. Each part gets its own channel (drum kits share channel 10), a program change
 * from its GM number, the controllers that are MIDI controllers (1..31, 64..95: value = the 8.8 value's integer
 * part; 32 pitch bend in semitones -> 14-bit bend at the default +-2 range; 33 aftertouch -> channel pressure).
 * Not mapped (counted in MXQtStats): QuickTime-only controllers (transpose, part/tune volume, master settings),
 * knob events, notes with velocity 0 or duration 0. A note that starts again while the same pitch still sounds on
 * its channel ends the earlier one first (one note-off per note-on; the disc's movies hold a few doubled notes). The
 * track ends at the last note-off like the Windows editions' files (the movies add 1.0 - 3.3 s of silence, a gap at
 * every repeat of a background loop). Every read is bounds-checked.
 * Compared with the Windows MIDI files (tests/test_qtmusic.c): same notes, pitches, programs; velocities equal except
 * the doubled notes; the disc's own rounding of the rests to 1/600 s makes the Mac music up to 0.21 % slower. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"
#include "mx_oldext.h"

enum {
    MAX_PARTS = 64,              /* the disc's movies use up to 5 */
    MAX_SAMPLES = 4096,
    MAX_EVENTS = 1 << 20,
    DRUM_CHANNEL = 9,            /* MIDI channel 10 */
    GM_FIRST_DRUMKIT = 16384,    /* kFirstDrumkit: GM numbers above it are drum kits */
    GEN_NOTE_REQUEST = 1,        /* kGeneralEventNoteRequest */
    GEN_MIDI_CHANNEL = 8,        /* kGeneralEventMIDIChannel */
    QT_PITCH_BEND = 32, QT_AFTERTOUCH = 33,
    SMF_TEMPO_US = 1000000       /* one quarter = one second: a tick = 1 / division seconds */
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static unsigned be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* ---------------------------------------------------------------------------------------------- atoms */
/* the first child atom `type` inside [d, d + n) (QuickTime atoms: u32 size incl. the 8-byte header, char[4] type;
 * size 0 = to the end) -> its payload */
static const uint8_t *atom(const uint8_t *d, size_t n, const char *type, size_t *len)
{
    for (size_t o = 0; d && o + 8 <= n;) {
        size_t sz = be32(d + o);
        if (sz == 0) sz = n - o;
        if (sz < 8 || sz > n - o) return NULL;
        if (!memcmp(d + o + 4, type, 4)) { *len = sz - 8; return d + o + 8; }
        o += sz;
    }
    return NULL;
}

/* a path of nested atoms, e.g. "mdia/minf/stbl/stts" */
static const uint8_t *atom_path(const uint8_t *d, size_t n, const char *path, size_t *len)
{
    for (; d && *path; path += path[4] == '/' ? 5 : 4) {
        if (strlen(path) < 4) return NULL;
        char t[5];
        memcpy(t, path, 4);
        t[4] = 0;
        d = atom(d, n, t, &n);
    }
    *len = n;
    return d;
}

/* the 'moov' resource of a resource fork (layout: mx_tables_mac.c) */
static const uint8_t *rsrc_moov(const uint8_t *r, size_t n, size_t *len)
{
    if (!r || n < 16) return NULL;
    uint32_t doff = be32(r), moff = be32(r + 4);
    if (doff > n || moff > n || n - moff < 30) return NULL;
    const uint8_t *map = r + moff;
    size_t mlen = n - moff;
    unsigned tl = be16(map + 24);
    if (tl + 2 > mlen) return NULL;
    unsigned ntypes = be16(map + tl) + 1u;
    for (unsigned i = 0; i < ntypes && tl + 2 + 8 * (i + 1) <= mlen; i++) {
        const uint8_t *e = map + tl + 2 + 8 * i;
        if (memcmp(e, "moov", 4)) continue;
        unsigned ref = tl + be16(e + 6);
        if (ref + 12 > mlen) return NULL;
        uint32_t off = be32(map + ref + 4) & 0xFFFFFF;            /* first resource of the type */
        if (off > n - doff || n - doff - off < 4) return NULL;
        uint32_t l = be32(r + doff + off);
        if (l > n - doff - off - 4) return NULL;
        *len = l;
        return r + doff + off + 4;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- events */
typedef struct { uint32_t time; uint8_t order, len, b[3]; uint32_t seq; int note; } Ev;   /* note: id for on/off */
typedef struct { int number, channel, program, drum; } Part;

typedef struct {
    Part part[MAX_PARTS];
    int nparts, next_channel;
    Ev *ev;
    int nev, cap, nnotes;
    uint32_t seq;
    MXQtStats *st;
    int full;
} Conv;

/* order of events at the same time: note-offs first, then programs, controllers, note-ons */
enum { ORD_OFF = 0, ORD_PROGRAM = 1, ORD_CONTROL = 2, ORD_ON = 3 };

static void add(Conv *c, uint32_t time, int order, int len, int b0, int b1, int b2, int note)
{
    if (c->nev == c->cap) {
        int nc = c->cap ? c->cap * 2 : 256;
        Ev *ne = nc <= MAX_EVENTS ? realloc(c->ev, (size_t)nc * sizeof *ne) : NULL;
        if (!ne) { c->full = 1; return; }
        c->ev = ne;
        c->cap = nc;
    }
    Ev *e = &c->ev[c->nev++];
    e->time = time;
    e->order = (uint8_t)order;
    e->len = (uint8_t)len;
    e->b[0] = (uint8_t)b0;
    e->b[1] = (uint8_t)b1;
    e->b[2] = (uint8_t)b2;
    e->seq = c->seq++;
    e->note = note;
}

/* the part's entry; a part without a note request (not seen on the disc) gets a channel and program 0 */
static Part *part_get(Conv *c, int number)
{
    for (int i = 0; i < c->nparts; i++) if (c->part[i].number == number) return &c->part[i];
    if (c->nparts == MAX_PARTS) return &c->part[MAX_PARTS - 1];
    Part *p = &c->part[c->nparts++];
    p->number = number;
    p->drum = 0;
    p->program = 0;
    p->channel = c->next_channel;
    c->next_channel = (c->next_channel + 1) % 16;
    if (c->next_channel == DRUM_CHANNEL) c->next_channel++;
    return p;
}

static void note_request(Conv *c, int number, const uint8_t *body, size_t n)
{
    if (n < 84) return;
    uint32_t gm = be32(body + 80);
    if (!gm) gm = be32(body + 76);                                    /* instrument number */
    Part *p = part_get(c, number);
    p->drum = gm > GM_FIRST_DRUMKIT;
    if (p->drum) { p->channel = DRUM_CHANNEL; p->program = (int)((gm - GM_FIRST_DRUMKIT - 1) & 127); }
    else p->program = gm >= 1 && gm <= 128 ? (int)gm - 1 : 0;
    c->st->programs++;
}

static void controller(Conv *c, uint32_t t, int part, unsigned ctl, unsigned value)
{
    Part *p = part_get(c, part);
    int v = (int16_t)value >> 8;                                      /* 8.8 fixed */
    if (ctl == QT_PITCH_BEND) {
        int b = 8192 + (int)((int16_t)value) * 4096 / 256;           /* semitones, +-2 = full range */
        if (b < 0) b = 0;
        if (b > 16383) b = 16383;
        add(c, t, ORD_CONTROL, 3, 0xE0 | p->channel, b & 127, b >> 7, -1);
    } else if (ctl == QT_AFTERTOUCH) {
        add(c, t, ORD_CONTROL, 2, 0xD0 | p->channel, v < 0 ? 0 : v > 127 ? 127 : v, 0, -1);
    } else if ((ctl >= 1 && ctl <= 31) || (ctl >= 64 && ctl <= 95)) {
        add(c, t, ORD_CONTROL, 3, 0xB0 | p->channel, (int)ctl, v < 0 ? 0 : v > 127 ? 127 : v, -1);
        c->st->controllers++;
    } else c->st->unmapped++;
}

static void note(Conv *c, uint32_t t, int part, int pitch, int velocity, uint32_t duration)
{
    if (!velocity || !duration || pitch < 0 || pitch > 127) { c->st->unmapped++; return; }
    Part *p = part_get(c, part);
    int id = c->nnotes++;
    add(c, t, ORD_ON, 3, 0x90 | p->channel, pitch, velocity, id);
    add(c, t + duration, ORD_OFF, 3, 0x80 | p->channel, pitch, 0, id);
    c->st->notes++;
}

/* one QTMA event stream (the tune header or a sample) starting at time t0; returns its end time */
static uint32_t events(Conv *c, const uint8_t *d, size_t n, uint32_t t0)
{
    uint32_t t = t0;
    size_t nw = n / 4;
    for (size_t i = 0; i < nw && !c->full;) {
        uint32_t x = be32(d + 4 * i);
        switch (x >> 29) {
        case 0: t += x & 0xFFFFFF; i++; continue;                                                 /* rest */
        case 1: note(c, t, (x >> 24) & 31, (int)((x >> 18) & 63) + 32, (x >> 11) & 127, x & 0x7FF); i++; continue;
        case 2: controller(c, t, (x >> 24) & 31, (x >> 16) & 255, x & 0xFFFF); i++; continue;
        case 3: if (((x >> 16) & 255) == 0) return t; i++; continue;                              /* marker; 0 = end */
        default: break;
        }
        unsigned type = x >> 28, part = (x >> 16) & 0xFFF;
        if (type == 0xF) {                                                                        /* general event */
            size_t len = x & 0xFFFF;
            if (len < 2 || i + len > nw) return t;
            unsigned sub = (be32(d + 4 * (i + len - 1)) >> 16) & 0x3FFF;
            const uint8_t *body = d + 4 * (i + 1);
            size_t bn = 4 * (len - 2);
            if (sub == GEN_NOTE_REQUEST) note_request(c, (int)part, body, bn);
            else if (sub == GEN_MIDI_CHANNEL && bn >= 4) {
                uint32_t ch = be32(body);
                if (ch >= 1 && ch <= 16) part_get(c, (int)part)->channel = (int)ch - 1;
            }
            i += len;
            continue;
        }
        if (i + 1 >= nw) return t;
        uint32_t y = be32(d + 4 * (i + 1));
        if (type == 0x9) {                                                                        /* extended note */
            unsigned pitch = x & 0xFFFF;
            note(c, t, (int)part, (int)(pitch > 127 ? pitch >> 8 : pitch), (y >> 22) & 127, y & 0x3FFFFF);
        } else if (type == 0xA) controller(c, t, (int)part, x & 0xFFFF, y & 0xFFFF);           /* extended ctl */
        else c->st->unmapped++;                                                                   /* knob, other */
        i += 2;
    }
    return t;
}

static int ev_cmp(const void *a, const void *b)
{
    const Ev *x = a, *y = b;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    if (x->order != y->order) return x->order - y->order;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* duration of sample s from the stts runs {u32 count, u32 delta} */
static uint32_t sample_delta(const uint8_t *stts, uint32_t nstts, uint32_t s)
{
    for (uint32_t k = 0; k < nstts; k++) {
        uint32_t cnt = be32(stts + 8 + 8 * k);
        if (s < cnt) return be32(stts + 12 + 8 * k);
        s -= cnt;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- SMF output */
typedef struct { uint8_t *p; size_t n, cap; int bad; } Buf;

static void put(Buf *b, const void *d, size_t n)
{
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + n) nc *= 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) { b->bad = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put_var(Buf *b, uint32_t v)                                  /* SMF variable-length quantity */
{
    uint8_t t[5];
    int k = 0;
    t[k++] = v & 127;
    while ((v >>= 7)) t[k++] = (uint8_t)(0x80 | (v & 127));
    while (k) put(b, &t[--k], 1);
}

uint8_t *mx_qtmusic_to_smf(const uint8_t *data, size_t dn, const uint8_t *rsrc, size_t rn, size_t *outlen, MXQtStats *stats,
                           char *err, int errlen)
{
    MXQtStats dummy;
    MXQtStats *st = stats ? stats : &dummy;
    memset(st, 0, sizeof *st);
    char e0[8];
    if (!err) { err = e0; errlen = sizeof e0; }
    err[0] = 0;
    size_t mn = 0, tn = 0;
    const uint8_t *moov = atom(data, dn, "moov", &mn);
    if (!moov && (moov = rsrc_moov(rsrc, rn, &mn))) moov = atom(moov, mn, "moov", &mn);   /* the resource = the atom */
    if (!moov) { snprintf(err, (size_t)errlen, "not a QuickTime movie (no moov)"); return NULL; }

    /* the first track whose media handler is 'musi' */
    const uint8_t *mdia = NULL;
    size_t an = 0;
    for (size_t o = 0; o + 8 <= mn;) {
        size_t sz = be32(moov + o);
        if (sz < 8 || sz > mn - o) break;
        size_t hn = 0;
        const uint8_t *m, *h;
        if (!memcmp(moov + o + 4, "trak", 4) && (m = atom(moov + o + 8, sz - 8, "mdia", &tn)) &&
            (h = atom(m, tn, "hdlr", &hn)) && hn >= 12 && !memcmp(h + 8, "musi", 4)) { mdia = m; an = tn; break; }
        o += sz;
    }
    if (!mdia) { snprintf(err, (size_t)errlen, "the movie has no music track"); return NULL; }

    size_t hn = 0, sdn = 0, ttn = 0, scn = 0, szn = 0, con = 0;
    const uint8_t *mdhd = atom(mdia, an, "mdhd", &hn);
    const uint8_t *stsd = atom_path(mdia, an, "minf/stbl/stsd", &sdn);
    const uint8_t *stts = atom_path(mdia, an, "minf/stbl/stts", &ttn);
    const uint8_t *stsc = atom_path(mdia, an, "minf/stbl/stsc", &scn);
    const uint8_t *stsz = atom_path(mdia, an, "minf/stbl/stsz", &szn);
    const uint8_t *stco = atom_path(mdia, an, "minf/stbl/stco", &con);
    if (!mdhd || hn < 20 || !stsd || sdn < 8 + 24 || !stts || ttn < 8 || !stsc || scn < 8 || !stsz || szn < 12 || !stco || con < 8) {
        snprintf(err, (size_t)errlen, "the music track's tables are incomplete");
        return NULL;
    }
    uint32_t timescale = mdhd[0] == 1 && hn >= 32 ? be32(mdhd + 20) : be32(mdhd + 12);
    uint32_t duration = mdhd[0] == 1 && hn >= 32 ? be32(mdhd + 28) : be32(mdhd + 16);       /* low half of u64 */
    const uint8_t *desc = stsd + 8;
    size_t dsz = be32(desc);
    if (memcmp(desc + 4, "musi", 4) || dsz < 20 || dsz > sdn - 8 || !timescale) {
        snprintf(err, (size_t)errlen, "the music track has no tune header");
        return NULL;
    }

    Conv c;
    memset(&c, 0, sizeof c);
    c.st = st;
    events(&c, desc + 20, dsz - 20, 0);                                  /* tune header: note requests */

    /* the samples: offset from stsc + stco + stsz, start time from stts */
    uint32_t nsamp = be32(stsz + 8), fixed = be32(stsz + 4), nchunk = be32(stco + 4), nsc = be32(stsc + 4);
    uint32_t nstts = be32(stts + 4);
    if (nsamp > MAX_SAMPLES || (!fixed && (szn - 12) / 4 < nsamp) || (con - 8) / 4 < nchunk || (scn - 8) / 12 < nsc ||
        (ttn - 8) / 8 < nstts) {
        snprintf(err, (size_t)errlen, "the music track's sample tables are damaged");
        free(c.ev);
        return NULL;
    }
    uint32_t s = 0, t = 0, end = 0;
    for (uint32_t ch = 0; ch < nchunk && s < nsamp; ch++) {
        uint32_t per = 0;                                                /* samples in chunk ch (1-based in stsc) */
        for (uint32_t k = 0; k < nsc; k++) if (be32(stsc + 8 + 12 * k) <= ch + 1) per = be32(stsc + 8 + 12 * k + 4);
        size_t off = be32(stco + 8 + 4 * ch);
        for (uint32_t k = 0; k < per && s < nsamp; k++, s++) {
            size_t sz = fixed ? fixed : be32(stsz + 12 + 4 * s);
            if (off > dn || sz > dn - off) {
                snprintf(err, (size_t)errlen, "a music sample lies outside the data fork");
                free(c.ev);
                return NULL;
            }
            uint32_t e = events(&c, data + off, sz, t);
            if (e > end) end = e;
            off += sz;
            t += sample_delta(stts, nstts, s);
        }
    }
    st->samples = (int)s;
    if (c.full) { snprintf(err, (size_t)errlen, "out of memory"); free(c.ev); return NULL; }
    if (t > end) end = t;
    if (duration > end) end = duration;
    st->media_end = (int)end;
    /* the track ends with the last note (as the Windows editions' files do): the movies carry 1.0 - 3.3 s of silence
     * after it, which would be a gap every time a background loop repeats */
    uint32_t last_off = 0;
    for (int i = 0; i < c.nev; i++) if ((c.ev[i].b[0] & 0xF0) == 0x80 && c.ev[i].time > last_off) last_off = c.ev[i].time;
    if (c.nnotes) end = last_off;

    /* programs at time 0 (one per channel, in part order) */
    for (int i = 0; i < c.nparts; i++) {
        int dup = 0;
        for (int k = 0; k < i; k++) dup |= c.part[k].channel == c.part[i].channel;
        if (!dup) add(&c, 0, ORD_PROGRAM, 2, 0xC0 | c.part[i].channel, c.part[i].program, 0, -1);
    }
    if (c.full) { snprintf(err, (size_t)errlen, "out of memory"); free(c.ev); return NULL; }
    if (c.nev) qsort(c.ev, (size_t)c.nev, sizeof *c.ev, ev_cmp);

    /* division: the timescale (scaled down if it does not fit 15 bits; not the case on the disc) */
    uint32_t div = timescale;
    double scale = 1.0;
    while (div > 0x7FFF) div /= 2;
    if (div != timescale) scale = (double)div / timescale;
    st->division = (int)div;

    Buf trk = {0};
    static const uint8_t tempo[] = {0, 0xFF, 0x51, 3, SMF_TEMPO_US >> 16, (SMF_TEMPO_US >> 8) & 255, SMF_TEMPO_US & 255};
    put(&trk, tempo, sizeof tempo);
    /* sounding[channel][pitch] = id of the note that owns it (-1 none) */
    int (*sounding)[128] = malloc(16 * sizeof *sounding);
    if (!sounding) { free(c.ev); snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
    memset(sounding, 0xFF, 16 * sizeof *sounding);
    uint32_t last = 0;
    for (int i = 0; i < c.nev; i++) {
        Ev *e = &c.ev[i];
        uint32_t tk = (uint32_t)(e->time * scale + 0.5);
        int kind = e->b[0] & 0xF0, chn = e->b[0] & 15;
        if (kind == 0x80 && sounding[chn][e->b[1]] != e->note) continue;          /* ended early by a restrike */
        if (kind == 0x90 && sounding[chn][e->b[1]] >= 0) {                        /* restrike: end the old note */
            put_var(&trk, tk - last);
            last = tk;
            uint8_t off[3] = {(uint8_t)(0x80 | chn), e->b[1], 0};
            put(&trk, off, 3);
            st->restrikes++;
        }
        if (kind == 0x90) sounding[chn][e->b[1]] = e->note;
        if (kind == 0x80) sounding[chn][e->b[1]] = -1;
        put_var(&trk, tk - last);
        last = tk;
        put(&trk, e->b, e->len);
    }
    free(sounding);
    free(c.ev);
    uint32_t etk = (uint32_t)(end * scale + 0.5);
    put_var(&trk, etk > last ? etk - last : 0);
    static const uint8_t eot[] = {0xFF, 0x2F, 0};
    put(&trk, eot, 3);
    st->end_ticks = (int)etk;
    st->parts = c.nparts;

    Buf out = {0};
    uint8_t hdr[22] = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, (uint8_t)(div >> 8), (uint8_t)div,
                       'M', 'T', 'r', 'k', (uint8_t)(trk.n >> 24), (uint8_t)(trk.n >> 16), (uint8_t)(trk.n >> 8), (uint8_t)trk.n};
    put(&out, hdr, sizeof hdr);
    put(&out, trk.p, trk.n);
    free(trk.p);
    if (out.bad || trk.bad) { free(out.p); snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
    *outlen = out.n;
    return out.p;
}
