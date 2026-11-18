/* LZX decompression (the Microsoft CAB / CHM / MS Help 2 variant) and the ITOL/ITLS archive (MINDMAZE.EIT).
 *
 * Written for this project from the published format descriptions (Microsoft's LZX DELTA / CAB specifications
 * and the CHM/ITOL notes by Matthew T. Russotto). Bitstream: 16-bit little-endian words, read most significant
 * bit first. Output is produced in 32 KB frames; the decoder state is reset every reset_frames frames. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"

/* ------------------------------------------------------------------------------------------------ */
/* bit reader                                                                                        */
typedef struct {
    const uint8_t *in;
    size_t n, pos;
    uint32_t buf;          /* unread bits, at the top */
    int bits;
    int overrun;           /* 16-bit words read past the end */
} Bits;

static void need(Bits *b, int n)              /* load 16-bit words until n bits are available (n <= 17) */
{
    while (b->bits < n) {
        uint32_t w = 0;
        if (b->pos + 1 < b->n) w = (uint32_t)b->in[b->pos] | (uint32_t)b->in[b->pos + 1] << 8;
        else b->overrun++;
        b->pos += 2;
        b->buf |= w << (16 - b->bits);
        b->bits += 16;
    }
}
static uint32_t peek(Bits *b, int n) { need(b, n); return b->buf >> (32 - n); }
static void drop(Bits *b, int n) { b->buf <<= n; b->bits -= n; }
static uint32_t get(Bits *b, int n)
{
    if (!n) return 0;
    uint32_t v = peek(b, n);
    drop(b, n);
    return v;
}

/* ------------------------------------------------------------------------------------------------ */
/* canonical Huffman codes: shorter codes first, equal lengths in symbol order; decoded bit by bit     */
#define MAXBITS 16
typedef struct {
    short count[MAXBITS + 1];
    short sym[656];
    int nsym;
} Huff;

static int huff_build(Huff *h, const uint8_t *len, int n)   /* 0 if the lengths over-subscribe the code */
{
    short offs[MAXBITS + 1];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l <= MAXBITS; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return 0;
    }
    offs[1] = 0;
    for (int l = 1; l < MAXBITS; l++) offs[l + 1] = (short)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (short)i;
    h->nsym = n;
    return 1;
}

static int huff_decode(Bits *b, const Huff *h)                /* -1 = invalid code */
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= MAXBITS; l++) {
        code |= (int)get(b, 1);
        int count = h->count[l];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

/* ------------------------------------------------------------------------------------------------ */
/* LZX                                                                                               */
enum { BLOCK_VERBATIM = 1, BLOCK_ALIGNED = 2, BLOCK_UNCOMPRESSED = 3 };
enum { FRAME = 32768, NUM_CHARS = 256, PRETREE_N = 20, LENGTH_N = 249, ALIGNED_N = 8, MIN_MATCH = 2 };

typedef struct {
    uint8_t main_len[NUM_CHARS + 50 * 8 + 32], length_len[LENGTH_N + 32], aligned_len[ALIGNED_N];
    Huff main, length, aligned;
    int length_empty;
    uint32_t R0, R1, R2;
    int header_read, intel_started;
    uint32_t intel_filesize;
    int block_type;
    long block_remaining, block_length;
} Lzx;

static uint8_t extra_bits[52];
static uint32_t position_base[52];

static void tables_init(void)
{
    for (int i = 0, j = 0; i < 52; i += 2) {
        extra_bits[i] = extra_bits[i + 1] = (uint8_t)j;
        if (i != 0 && j < 17) j++;
    }
    for (int i = 0, j = 0; i < 52; i++) {
        position_base[i] = (uint32_t)j;
        j += 1 << extra_bits[i];
    }
}

static void lzx_reset(Lzx *z)
{
    z->R0 = z->R1 = z->R2 = 1;
    z->header_read = 0;
    z->intel_started = 0;
    z->block_remaining = 0;
    z->block_type = 0;
    memset(z->main_len, 0, sizeof z->main_len);
    memset(z->length_len, 0, sizeof z->length_len);
}

/* path lengths through the pretree (delta against the previous block's lengths) */
static int read_lens(Bits *b, uint8_t *lens, int first, int last)
{
    uint8_t pl[PRETREE_N];
    Huff pre;
    for (int i = 0; i < PRETREE_N; i++) pl[i] = (uint8_t)get(b, 4);
    if (!huff_build(&pre, pl, PRETREE_N)) return 0;
    for (int x = first; x < last;) {
        int z = huff_decode(b, &pre), y;
        if (z < 0) return 0;
        if (z == 17) { y = 4 + (int)get(b, 4); while (y-- && x < last + 32) lens[x++] = 0; }
        else if (z == 18) { y = 20 + (int)get(b, 5); while (y-- && x < last + 32) lens[x++] = 0; }
        else if (z == 19) {
            y = 4 + (int)get(b, 1);
            z = huff_decode(b, &pre);
            if (z < 0 || z > 16) return 0;
            z = lens[x] - z;
            if (z < 0) z += 17;
            while (y-- && x < last + 32) lens[x++] = (uint8_t)z;
        } else {
            z = lens[x] - z;
            if (z < 0) z += 17;
            lens[x++] = (uint8_t)z;
        }
    }
    return 1;
}

static int read_block_header(Lzx *z, Bits *b, int main_n)
{
    z->block_type = (int)get(b, 3);
    uint32_t hi = get(b, 16), lo = get(b, 8);
    z->block_remaining = z->block_length = (long)(hi << 8 | lo);
    switch (z->block_type) {
    case BLOCK_ALIGNED:
        for (int i = 0; i < ALIGNED_N; i++) z->aligned_len[i] = (uint8_t)get(b, 3);
        if (!huff_build(&z->aligned, z->aligned_len, ALIGNED_N)) return 0;
        /* fall through: the rest is as a verbatim block */ /* FALLTHROUGH */
    case BLOCK_VERBATIM:
        if (!read_lens(b, z->main_len, 0, NUM_CHARS) || !read_lens(b, z->main_len, NUM_CHARS, main_n)) return 0;
        if (!huff_build(&z->main, z->main_len, main_n)) return 0;
        if (z->main_len[0xE8]) z->intel_started = 1;
        if (!read_lens(b, z->length_len, 0, LENGTH_N)) return 0;
        z->length_empty = 1;
        for (int i = 0; i < LENGTH_N; i++) if (z->length_len[i]) z->length_empty = 0;
        if (!z->length_empty && !huff_build(&z->length, z->length_len, LENGTH_N)) return 0;
        return 1;
    case BLOCK_UNCOMPRESSED: {
        z->intel_started = 1;
        if (b->bits == 0) need(b, 16);           /* 1..16 bits of padding to the next 16-bit boundary */
        b->bits = 0;
        b->buf = 0;
        if (b->pos + 12 > b->n) return 0;
        const uint8_t *p = b->in + b->pos;
        z->R0 = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
        z->R1 = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
        z->R2 = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
        b->pos += 12;
        return 1;
    }
    default:
        return 0;
    }
}

int mx_lzx_decompress(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen, int window_bits,
                      int reset_frames, char *err, int errlen)
{
    if (!position_base[1]) tables_init();
    if (window_bits < 15 || window_bits > 21) { snprintf(err, errlen, "LZX: unsupported window size"); return 0; }
    int slots = window_bits == 20 ? 42 : window_bits == 21 ? 50 : window_bits * 2;
    int main_n = NUM_CHARS + slots * 8;
    uint32_t window = 1u << window_bits;
    Lzx *z = calloc(1, sizeof *z);
    /* decoded bytes; one spare frame because the last frame may be encoded at full length */
    size_t cap = outlen + FRAME + 512;
    uint8_t *win = malloc(cap);
    if (!z || !win) { free(z); free(win); snprintf(err, errlen, "out of memory"); return 0; }
    if (0) { snprintf(err, errlen, "out of memory"); return 0; }
    Bits b = {in, inlen, 0, 0, 0, 0};
    size_t pos = 0;
    long frame = 0;
    const char *problem = NULL;

    while (pos < outlen && !problem) {
        if (reset_frames && frame % reset_frames == 0) {
            if (z->block_remaining) { problem = "block runs over a reset point"; break; }
            lzx_reset(z);
        }
        if (!z->header_read) {
            if (get(&b, 1)) { uint32_t hi = get(&b, 16), lo = get(&b, 16); z->intel_filesize = hi << 16 | lo; }
            else z->intel_filesize = 0;
            z->header_read = 1;
        }
        size_t frame_end = (size_t)(frame + 1) * FRAME;
        if (frame_end > outlen) frame_end = outlen;
        while (pos < frame_end && !problem) {
            if (z->block_remaining == 0) {
                if (z->block_type == BLOCK_UNCOMPRESSED && (z->block_length & 1)) b.pos++;   /* pad byte */
                if (!read_block_header(z, &b, main_n)) { problem = "bad block header"; break; }
            }
            long run = z->block_remaining < (long)(frame_end - pos) ? z->block_remaining : (long)(frame_end - pos);
            size_t stop = pos + (size_t)run;
            if (z->block_type == BLOCK_UNCOMPRESSED) {
                if (b.pos + (size_t)run > b.n) { problem = "data ends early"; break; }
                memcpy(win + pos, b.in + b.pos, (size_t)run);
                b.pos += (size_t)run;
                pos = stop;
                z->block_remaining -= run;
                continue;
            }
            while (pos < stop) {
                int sym = huff_decode(&b, &z->main);
                if (sym < 0) { problem = "bad code"; break; }
                if (sym < NUM_CHARS) { win[pos++] = (uint8_t)sym; continue; }
                sym -= NUM_CHARS;
                long len = sym & 7;
                if (len == 7) {
                    if (z->length_empty) { problem = "length code without a length tree"; break; }
                    int f = huff_decode(&b, &z->length);
                    if (f < 0) { problem = "bad length code"; break; }
                    len += f;
                }
                len += MIN_MATCH;
                int slot = sym >> 3;
                uint32_t off;
                if (slot == 0) off = z->R0;
                else if (slot == 1) { off = z->R1; z->R1 = z->R0; z->R0 = off; }
                else if (slot == 2) { off = z->R2; z->R2 = z->R0; z->R0 = off; }
                else {
                    int extra = extra_bits[slot];
                    off = position_base[slot] - 2;
                    if (z->block_type == BLOCK_ALIGNED) {
                        if (extra > 3) {
                            off += get(&b, extra - 3) << 3;
                            int a = huff_decode(&b, &z->aligned);
                            if (a < 0) { problem = "bad aligned code"; break; }
                            off += (uint32_t)a;
                        } else if (extra == 3) {
                            int a = huff_decode(&b, &z->aligned);
                            if (a < 0) { problem = "bad aligned code"; break; }
                            off += (uint32_t)a;
                        } else if (extra > 0) off += get(&b, extra);
                        else off = 1;
                    } else if (extra) {
                        /* up to 17 bits: read in two parts so a read never needs more than 16 bits */
                        if (extra > 16) { uint32_t hi = get(&b, extra - 16); off += hi << 16 | get(&b, 16); }
                        else off += get(&b, extra);
                    }
                    z->R2 = z->R1;
                    z->R1 = z->R0;
                    z->R0 = off;
                }
                if (off == 0 || off > window || pos + (size_t)len > cap) { problem = "match out of range"; break; }
                for (long i = 0; i < len; i++, pos++) win[pos] = off <= pos ? win[pos - off] : 0;
            }
            /* a match may run past this stretch: it is taken from the rest of the block */
            long used = (long)(pos - (stop - (size_t)run));
            z->block_remaining -= used;
            if (z->block_remaining < 0) { problem = "match runs past the end of its block"; break; }
        }
        if (problem) break;
        frame++;
        if (b.bits > 0) { need(&b, 16); drop(&b, b.bits & 15); }   /* frames end on a 16-bit boundary */
        if (b.overrun > 2) { problem = "compressed data ends early"; break; }
    }
    /* E8 call translation (used for program code; the frames of this archive normally do not have it) */
    if (!problem && z->intel_filesize) {
        problem = "E8 translation is not supported";
    }
    if (!problem) memcpy(out, win, outlen);
    free(win);
    free(z);
    if (problem) { snprintf(err, errlen, "LZX: %s (at byte %lu of %lu)", problem, (unsigned long)pos, (unsigned long)outlen); return 0; }
    return 1;
}

/* ------------------------------------------------------------------------------------------------ */
/* ITOL/ITLS (MS Help 2) archive                                                                     */
static uint32_t r32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const uint8_t *p) { return r32(p) | (uint64_t)r32(p + 4) << 32; }

static int encint(const uint8_t *d, size_t end, size_t *o, uint64_t *v)
{
    *v = 0;
    for (int i = 0; i < 9; i++) {
        if (*o >= end) return 0;
        uint8_t c = d[(*o)++];
        *v = *v << 7 | (c & 0x7f);
        if (!(c & 0x80)) return 1;
    }
    return 0;
}

typedef struct { char name[128]; uint64_t sec, off, len; } Entry;

int mx_itol_files(const uint8_t *d, size_t n, void (*fn)(void *, const char *, const uint8_t *, size_t), void *ctx,
                  char *err, int errlen)
{
#define BAD(msg) do { snprintf(err, errlen, "MINDMAZE.EIT: %s", msg); free(ent); free(out); return 0; } while (0)
    Entry *ent = NULL;
    uint8_t *out = NULL;
    if (n < 0x200 || memcmp(d, "ITOLITLS", 8) || r32(d + 8) != 1 || r32(d + 12) != 0x28 || r32(d + 16) != 5)
        BAD("not an ITOL/ITLS archive");
    uint64_t sec_off[5], sec_len[5];
    for (int i = 0; i < 5; i++) {
        sec_off[i] = r64(d + 0x28 + 16 * i);
        sec_len[i] = r64(d + 0x30 + 16 * i);
        if (sec_off[i] > n || sec_len[i] > n - sec_off[i]) BAD("bad header");
    }
    /* content offset: the ITSF record inside the CAOL record of the post-header */
    size_t post = 0x28 + 16 * 5;
    size_t caol = post + r32(d + post + 4);
    if (caol + 0x50 > n || memcmp(d + caol, "CAOL", 4) || r32(d + caol + 8) != 0x50 || memcmp(d + caol + 0x30, "ITSF", 4))
        BAD("unsupported archive variant");
    uint64_t content = r64(d + caol + 0x30 + 16);

    /* directory: section 1, IFCM header + AOLL listing chunks */
    const uint8_t *s1 = d + sec_off[1];
    if (sec_len[1] < 32 || memcmp(s1, "IFCM", 4)) BAD("directory missing");
    uint32_t chunk = r32(s1 + 8), nchunks = r32(s1 + 24);
    if (chunk < 64 || (uint64_t)nchunks * chunk + 32 > sec_len[1]) BAD("bad directory");
    int nent = 0, cap = 0;
    for (uint32_t c = 0; c < nchunks; c++) {
        const uint8_t *ch = s1 + 32 + (size_t)c * chunk;
        if (memcmp(ch, "AOLL", 4)) continue;
        uint32_t qr = r32(ch + 4);
        if (qr > chunk || qr < 2) BAD("bad directory chunk");
        size_t o = 48, end = chunk - qr;
        while (o < end) {
            uint64_t ln, sec, off, len;
            if (!encint(ch, end, &o, &ln) || ln >= 128 || o + ln > end) BAD("bad directory entry");
            if (nent == cap) {
                cap = cap ? cap * 2 : 64;
                Entry *ne = realloc(ent, sizeof *ent * (size_t)cap);
                if (!ne) BAD("out of memory");
                ent = ne;
            }
            Entry *e = &ent[nent];
            memcpy(e->name, ch + o, (size_t)ln);
            e->name[ln] = 0;
            o += (size_t)ln;
            if (!encint(ch, end, &o, &sec) || !encint(ch, end, &o, &off) || !encint(ch, end, &o, &len)) BAD("bad directory entry");
            e->sec = sec;
            e->off = off;
            e->len = len;
            nent++;
        }
    }
    const Entry *ec = NULL, *ectl = NULL, *ereset = NULL;
    for (int i = 0; i < nent; i++) {
        if (!strcmp(ent[i].name, "::DataSpace/Storage/MSCompressed/Content")) ec = &ent[i];
        else if (!strcmp(ent[i].name, "::DataSpace/Storage/MSCompressed/ControlData")) ectl = &ent[i];
        else if (strstr(ent[i].name, "::DataSpace/Storage/MSCompressed/Transform/") && strstr(ent[i].name, "/ResetTable")) ereset = &ent[i];
    }
    if (!ec || !ectl || !ereset) BAD("compressed section missing");
    for (const Entry *e = ec;; e = e == ec ? ectl : ereset) {
        if (e->sec != 0 || content + e->off > n || e->len > n - content - e->off) BAD("section out of range");
        if (e == ereset) break;
    }
    const uint8_t *ctl = d + content + ectl->off, *rt = d + content + ereset->off;
    if (ectl->len < 24 || memcmp(ctl + 4, "LZXC", 4)) BAD("not LZX compressed");
    uint32_t version = r32(ctl + 8), reset = r32(ctl + 12), wsize = r32(ctl + 16);
    if (version >= 2) { reset *= FRAME; wsize *= FRAME; }
    int wbits = 0;
    while (wbits < 25 && (1u << wbits) < wsize) wbits++;
    if ((1u << wbits) != wsize || reset % FRAME) BAD("unsupported LZX parameters");
    if (ereset->len < 40) BAD("bad reset table");
    uint64_t total = r64(rt + 16);
    if (total > 64u << 20) BAD("too large");
    out = malloc((size_t)total + 1);
    if (!out) BAD("out of memory");
    char sub[160];
    if (!mx_lzx_decompress(d + content + ec->off, (size_t)ec->len, out, (size_t)total, wbits, (int)(reset / FRAME), sub, sizeof sub))
        BAD(sub);
    for (int i = 0; i < nent; i++) {
        const Entry *e = &ent[i];
        if (e->name[0] != '/' || !e->len) continue;              /* folders and the archive's own records */
        if (e->sec == 1 && e->off + e->len <= total) fn(ctx, e->name + 1, out + e->off, (size_t)e->len);
        else if (e->sec == 0 && content + e->off + e->len <= n) fn(ctx, e->name + 1, d + content + e->off, (size_t)e->len);
        else BAD("file out of range");
    }
    free(ent);
    free(out);
    return 1;
#undef BAD
}
