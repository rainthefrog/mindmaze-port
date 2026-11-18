/* Tests of the built-in extractor's parts that run without Encarta files (make test).
 * The full extractor is compared with tools/extract/extract.py on a real disc by `make test-extract`. */
#define _POSIX_C_SOURCE 200809L                 /* strdup */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx.h"
#include "mx_tables.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(const uint8_t *h, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]); }

int main(void)
{
    /* SHA-256: FIPS 180-2 test vectors (one block, two blocks, a million 'a') */
    uint8_t h[32];
    char s[65];
    mx_sha256("abc", 3, h);
    hex(h, s);
    CHECK(!strcmp(s, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "sha256 abc: %s", s);
    const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    mx_sha256(two, strlen(two), h);
    hex(h, s);
    CHECK(!strcmp(s, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "sha256 448 bits: %s", s);
    char *m = malloc(1000000);
    memset(m, 'a', 1000000);
    mx_sha256(m, 1000000, h);
    hex(h, s);
    CHECK(!strcmp(s, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "sha256 million a: %s", s);
    free(m);

    /* inflate: every embedded file decompresses (zlib written by Python, levels with all block types) */
    int n = 0;
    size_t total = 0;
    for (const MXFile *f = mx_embedded; f->name; f++) {
        size_t len = f->size;
        if (f->zlib) {
            uint8_t *d = mx_inflate_zlib(f->data, f->size, &len);
            CHECK(d != NULL, "inflate %s", f->name);
            if (d && !strcmp(f->name, "packs/index.txt")) CHECK(strstr((char *)d, "opentdb") != NULL, "pack index content");
            if (d && !strcmp(f->name, "art/START-HERE.txt")) CHECK(!memcmp(d, "MindMaze HD", 11), "art kit content");
            free(d);
        }
        total += len;
        n++;
    }
    CHECK(n >= 8, "embedded files: %d", n);
    static const uint8_t junk[] = {0x78, 0x9c, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};
    size_t jl;
    CHECK(mx_inflate_zlib(junk, sizeof junk, &jl) == NULL, "broken zlib stream refused");

    /* MS ADPCM: one block, 4 samples of header + 2 nibbles -> 4 samples; prediction rounds toward zero */
    static const uint8_t wav[] = {
        'R', 'I', 'F', 'F', 74, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 50, 0, 0, 0,
        2, 0, 1, 0, 0x22, 0x56, 0, 0, 0, 0, 0, 0, 8, 0, 4, 0, 32, 0, 4, 0, 7, 0,
        0, 1, 0, 0, 0, 2, 0, 0xff, 0, 0, 0, 0, 0xc0, 0, 0x40, 0, 0xf0, 0, 0, 0, 0xcc, 1, 0x30, 0xff, 0x88, 1, 0x18, 0xff,
        'd', 'a', 't', 'a', 8, 0, 0, 0,
        1, 16, 0, 0xFF, 0xFF, 0x01, 0x00, 0x1F};   /* predictor 1, delta 16, s1 = -1, s2 = 1, nibbles 1 and F */
    size_t pl;
    uint8_t *pcm = mx_wav_to_pcm(wav, sizeof wav, &pl);
    CHECK(pcm && pl == 44 + 8, "adpcm decode size (%zu)", pl);
    if (pcm) {
        CHECK(!memcmp(pcm, "RIFF", 4) && !memcmp(pcm + 12, "fmt ", 4) && !memcmp(pcm + 36, "data", 4), "PCM header");
        int16_t v[4];
        memcpy(v, pcm + 44, 8);
        /* s2=1, s1=-1, then pred = (-1*512 + 1*-256)/256 = -3 (toward zero) + 1*16 = 13, then (13*512 + -1*-256)/256
         * = 27 (6912/256), nibble F = -1 * delta (16*230>>8 = 14 -> min 16) = 27 - 16 = 11 */
        CHECK(v[0] == 1 && v[1] == -1 && v[2] == 13 && v[3] == 11, "adpcm samples %d %d %d %d", v[0], v[1], v[2], v[3]);
        free(pcm);
    }
    CHECK(mx_wav_to_pcm((const uint8_t *)"RIFFxxxxWAVE", 12, &pl) == NULL, "WAVE without chunks refused");

    char err[200];
    static const uint8_t notitol[600] = {'I', 'T', 'S', 'F'};
    CHECK(!mx_itol_files(notitol, sizeof notitol, NULL, NULL, err, sizeof err) && strstr(err, "not an ITOL"), "ITOL check: %s", err);

    /* CHANGE 2026-11-18: older editions - CRC-32, raw deflate (Wise installer), the tables.json spellings */
    CHECK(mx_crc32("123456789", 9) == 0xCBF43926u, "crc32: %08x", mx_crc32("123456789", 9));
    static const uint8_t raw[] = {0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x07, 0x00, 0xff, 0xff};   /* "hello", then other bytes */
    size_t rl = 0, used = 0;
    uint8_t *ro = mx_inflate_raw(raw, sizeof raw, 1000, &rl, &used);
    CHECK(ro && rl == 5 && !memcmp(ro, "hello", 5) && used == 7, "raw inflate (%zu bytes, %zu used)", rl, used);
    free(ro);
    CHECK(mx_old_files((const uint8_t *)"not a baggage file", 18, NULL, NULL) < 0, "baggage check");
    static MXTables t;
    t.counts[0] = t.nobjects = 1;
    t.obj[0].name = strdup("NDOOR.DI$");
    t.music[0] = strdup("intro.MID");
    t.nspeech[0] = 1;
    t.speech[0] = calloc(1, sizeof(char *));
    t.speech[0][0] = strdup("<squawk!> Hi\r\nthere");
    t.str[29009 - MXT_FIRST_STRING] = strdup("MINDMAZE");
    size_t jl2 = 0;
    char *js = mx_tables_json(&t, "test", &jl2);
    CHECK(js && strstr(js, "\"NDOOR.DIB\"") && strstr(js, "\"INTRO.MID\"") && strstr(js, "&lt;squawk!&gt; Hi\\\\nthere") &&
              strstr(js, "\"29009\": \"MINDMAZE\"") && strstr(js, "\"29068\": \"New Game\""),
          "tables.json spellings and defaults");
    free(js);
    mx_tables_free(&t);

    printf("extractor parts: sha256, inflate (%d embedded files, %zu bytes), adpcm, archive check: %s\n", n, total,
           fails ? "FAILED" : "ok");
    return fails != 0;
}
