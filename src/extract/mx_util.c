/* SHA-256 (FIPS 180-4), zlib inflate (RFC 1950/1951) and MS ADPCM -> PCM for the built-in extractor. */
#include <stdlib.h>
#include <string.h>
#include "mx.h"

/* ---- SHA-256 ---- */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))

static void sha_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void mx_sha256(const void *data, size_t n, uint8_t out[32])
{
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t *p = data;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) sha_block(h, p + i);
    uint8_t last[128] = {0};
    size_t rest = n - i;
    memcpy(last, p + i, rest);
    last[rest] = 0x80;
    size_t blocks = rest + 9 > 64 ? 2 : 1;
    uint64_t bits = (uint64_t)n * 8;
    for (int b = 0; b < 8; b++) last[blocks * 64 - 1 - b] = (uint8_t)(bits >> (8 * b));
    for (size_t b = 0; b < blocks; b++) sha_block(h, last + 64 * b);
    for (int j = 0; j < 8; j++) for (int b = 0; b < 4; b++) out[4 * j + b] = (uint8_t)(h[j] >> (24 - 8 * b));
}

/* ---- inflate (bits are read least significant first; Huffman codes are canonical) ---- */
typedef struct {
    const uint8_t *in;
    size_t n, pos;
    uint32_t buf;
    int bits;
    uint8_t *out;
    size_t olen, ocap;
    int bad;
    size_t max;                  /* output limit (0 = none) */
} Inf;
typedef struct { short count[16], sym[320]; } Code;

static int ibits(Inf *s, int need)
{
    uint32_t v = s->buf;
    while (s->bits < need) {
        if (s->pos >= s->n) { s->bad = 1; return 0; }
        v |= (uint32_t)s->in[s->pos++] << s->bits;
        s->bits += 8;
    }
    s->buf = v >> need;
    s->bits -= need;
    return (int)(v & ((1u << need) - 1));
}

static int build(Code *c, const short *len, int n)
{
    short offs[16];
    memset(c->count, 0, sizeof c->count);
    for (int i = 0; i < n; i++) c->count[len[i]]++;
    c->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= c->count[l]; if (left < 0) return 0; }
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = (short)(offs[l] + c->count[l]);
    for (int i = 0; i < n; i++) if (len[i]) c->sym[offs[len[i]]++] = (short)i;
    return 1;
}

static int decode(Inf *s, const Code *c)
{
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= ibits(s, 1);
        int count = c->count[l];
        if (code - count < first) return c->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->bad = 1;
    return -1;
}

static int put(Inf *s, uint8_t b)
{
    if (s->max && s->olen >= s->max) return 0;
    if (s->olen == s->ocap) {
        size_t nc = s->ocap ? s->ocap * 2 : 1 << 16;
        uint8_t *no = realloc(s->out, nc);
        if (!no) return 0;
        s->out = no;
        s->ocap = nc;
    }
    s->out[s->olen++] = b;
    return 1;
}

static const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(Inf *s, const Code *lc, const Code *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (s->bad || sym < 0) return 0;
        if (sym < 256) { if (!put(s, (uint8_t)sym)) return 0; continue; }
        if (sym == 256) return 1;
        sym -= 257;
        if (sym >= 29) return 0;
        int len = LBASE[sym] + ibits(s, LEXT[sym]);
        int ds = decode(s, dc);
        if (ds < 0 || ds >= 30) return 0;
        size_t dist = (size_t)(DBASE[ds] + ibits(s, DEXT[ds]));
        if (s->bad || dist > s->olen) return 0;
        while (len--) if (!put(s, s->out[s->olen - dist])) return 0;
    }
}

static uint8_t *inflate_run(Inf *s, size_t maxout, size_t *outlen);
uint8_t *mx_inflate_zlib(const uint8_t *in, size_t inlen, size_t *outlen)
{
    if (inlen < 6 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20)) return NULL;
    Inf s = {in, inlen - 4, 2, 0, 0, NULL, 0, 0, 0};
    return inflate_run(&s, (size_t)-1, outlen);
}

uint8_t *mx_inflate_raw(const uint8_t *in, size_t inlen, size_t maxout, size_t *outlen, size_t *used)
{
    Inf s = {in, inlen, 0, 0, 0, NULL, 0, 0, 0};
    uint8_t *o = inflate_run(&s, maxout, outlen);
    if (o) *used = s.pos - (size_t)(s.bits / 8);
    return o;
}

uint32_t mx_crc32(const void *data, size_t n)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ c >> 1 : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (const uint8_t *p = data; n--; p++) c = t[(c ^ *p) & 255] ^ c >> 8;
    return c ^ 0xFFFFFFFFu;
}

static uint8_t *inflate_run(Inf *ps, size_t maxout, size_t *outlen)
{
    Inf s = *ps;
    if (maxout != (size_t)-1) s.max = maxout;
    int last;
    do {
        last = ibits(&s, 1);
        int type = ibits(&s, 2);
        if (s.bad) break;
        if (type == 0) {
            s.buf = 0;
            s.bits = 0;
            if (s.pos + 4 > s.n) { s.bad = 1; break; }
            unsigned len = s.in[s.pos] | s.in[s.pos + 1] << 8;
            s.pos += 4;
            if (s.pos + len > s.n) { s.bad = 1; break; }
            for (unsigned i = 0; i < len; i++) if (!put(&s, s.in[s.pos + i])) { s.bad = 1; break; }
            s.pos += len;
        } else if (type == 1 || type == 2) {
            short len[320];
            Code lc, dc;
            int nl = 288, nd = 30;
            if (type == 1) {
                for (int i = 0; i < 144; i++) len[i] = 8;
                for (int i = 144; i < 256; i++) len[i] = 9;
                for (int i = 256; i < 280; i++) len[i] = 7;
                for (int i = 280; i < 288; i++) len[i] = 8;
                build(&lc, len, 288);
                for (int i = 0; i < 30; i++) len[i] = 5;
                build(&dc, len, 30);
            } else {
                static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                nl = ibits(&s, 5) + 257;
                nd = ibits(&s, 5) + 1;
                int nc = ibits(&s, 4) + 4;
                if (nl > 286 || nd > 30) { s.bad = 1; break; }
                for (int i = 0; i < 19; i++) len[order[i]] = i < nc ? (short)ibits(&s, 3) : 0;
                Code cc;
                if (!build(&cc, len, 19)) { s.bad = 1; break; }
                for (int i = 0; i < nl + nd;) {
                    int sym = decode(&s, &cc), rep = 0;
                    short v = 0;
                    if (s.bad || sym < 0) { s.bad = 1; break; }
                    if (sym < 16) { len[i++] = (short)sym; continue; }
                    if (sym == 16) { if (!i) { s.bad = 1; break; } v = len[i - 1]; rep = 3 + ibits(&s, 2); }
                    else if (sym == 17) rep = 3 + ibits(&s, 3);
                    else rep = 11 + ibits(&s, 7);
                    if (i + rep > nl + nd) { s.bad = 1; break; }
                    while (rep--) len[i++] = v;
                }
                if (s.bad) break;
                short dl[30];
                memcpy(dl, len + nl, sizeof(short) * (size_t)nd);
                if (!build(&lc, len, nl) || !build(&dc, dl, nd)) { s.bad = 1; break; }
            }
            if (!codes(&s, &lc, &dc)) { s.bad = 1; break; }
        } else s.bad = 1;
    } while (!last && !s.bad);
    *ps = s;
    if (s.bad) { free(s.out); return NULL; }
    *outlen = s.olen;
    return s.out ? s.out : calloc(1, 1);
}

/* ---- MS ADPCM -> 16-bit PCM WAVE (same decoding as ffmpeg: prediction / 256 rounded toward zero) ---- */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int le16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

uint8_t *mx_wav_to_pcm(const uint8_t *d, size_t n, size_t *outlen)
{
    if (n < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return NULL;
    const uint8_t *fmt = NULL, *body = NULL;
    size_t fmtlen = 0, bodylen = 0;
    long fact = -1;
    for (size_t p = 12; p + 8 <= n;) {
        size_t len = le32(d + p + 4);
        if (len > n - p - 8) len = n - p - 8;
        if (!memcmp(d + p, "fmt ", 4)) { fmt = d + p + 8; fmtlen = len; }
        else if (!memcmp(d + p, "fact", 4) && len >= 4) fact = (long)le32(d + p + 8);
        else if (!memcmp(d + p, "data", 4)) { body = d + p + 8; bodylen = len; }
        p += 8 + len + (len & 1);
    }
    if (!fmt || !body || fmtlen < 16) return NULL;
    int tag = fmt[0] | fmt[1] << 8, ch = fmt[2] | fmt[3] << 8, bits = fmt[14] | fmt[15] << 8;
    uint32_t rate = le32(fmt + 4);
    unsigned align = fmt[12] | fmt[13] << 8;
    int16_t *pcm;
    size_t frames;
    if (tag == 1 && bits == 16) {
        frames = bodylen / (2 * (size_t)(ch ? ch : 1));
        pcm = malloc(bodylen + 2);
        if (!pcm) return NULL;
        memcpy(pcm, body, bodylen);
    } else {
        if (tag != 2 || (ch != 1 && ch != 2) || fmtlen < 22 || align < 7u * ch) return NULL;
        int ncoef = fmt[20] | fmt[21] << 8;
        if (ncoef < 1 || ncoef > 32 || fmtlen < 22 + 4u * ncoef) return NULL;
        int c1[32], c2[32];
        for (int i = 0; i < ncoef; i++) { c1[i] = le16s(fmt + 22 + 4 * i); c2[i] = le16s(fmt + 24 + 4 * i); }
        static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
        size_t maxs = (bodylen / align + 1) * ((align - 7 * ch) * 2 + 2 * ch) + 8;
        pcm = malloc(maxs * 2);
        if (!pcm) return NULL;
        size_t k = 0;
        for (size_t start = 0; start + 7u * ch <= bodylen; start += align) {
            const uint8_t *b = body + start;
            size_t blen = bodylen - start < align ? bodylen - start : align;
            int pi[2], delta[2], s1[2], s2[2], p = 0;
            for (int c = 0; c < ch; c++) { pi[c] = b[p++]; if (pi[c] >= ncoef) { free(pcm); return NULL; } }
            for (int c = 0; c < ch; c++, p += 2) delta[c] = le16s(b + p);
            for (int c = 0; c < ch; c++, p += 2) s1[c] = le16s(b + p);
            for (int c = 0; c < ch; c++, p += 2) s2[c] = le16s(b + p);
            for (int c = 0; c < ch; c++) pcm[k++] = (int16_t)s2[c];
            for (int c = 0; c < ch; c++) pcm[k++] = (int16_t)s1[c];
            int c = 0;
            for (size_t q = (size_t)p; q < blen; q++)
                for (int half = 0; half < 2; half++) {
                    int nib = half ? b[q] & 15 : b[q] >> 4;
                    int sgn = nib & 8 ? nib - 16 : nib;
                    int pred = s1[c] * c1[pi[c]] + s2[c] * c2[pi[c]];
                    pred = pred >= 0 ? pred >> 8 : -(-pred >> 8);
                    int v = pred + sgn * delta[c];
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    s2[c] = s1[c];
                    s1[c] = v;
                    delta[c] = adapt[nib] * delta[c] >> 8;
                    if (delta[c] < 16) delta[c] = 16;
                    pcm[k++] = (int16_t)v;
                    c = (c + 1) % ch;
                }
        }
        frames = k / (size_t)ch;
        if (fact > 0 && (size_t)fact <= frames) frames = (size_t)fact;
    }
    size_t bytes = frames * (size_t)ch * 2;
    uint8_t *w = malloc(44 + bytes);
    if (!w) { free(pcm); return NULL; }
    memcpy(w, "RIFF", 4);
    uint32_t v[] = {36 + (uint32_t)bytes};
    memcpy(w + 4, v, 4);
    memcpy(w + 8, "WAVEfmt ", 8);
    uint8_t f[20] = {16, 0, 0, 0, 1, 0, (uint8_t)ch, 0};
    uint32_t br = rate * (uint32_t)ch * 2;
    for (int i = 0; i < 4; i++) { f[8 + i] = (uint8_t)(rate >> 8 * i); f[12 + i] = (uint8_t)(br >> 8 * i); }
    f[16] = (uint8_t)(ch * 2);
    f[18] = 16;
    memcpy(w + 16, f, 20);
    memcpy(w + 36, "data", 4);
    for (int i = 0; i < 4; i++) w[40 + i] = (uint8_t)(bytes >> 8 * i);
    for (int i = 0; i < 4; i++) w[4 + i] = (uint8_t)((36 + bytes) >> 8 * i);
    memcpy(w + 44, pcm, bytes);                      /* little-endian hosts (x86, ARM, wasm) */
    free(pcm);
    *outlen = 44 + bytes;
    return w;
}
