/* Tests of the updater's portable part (src/platform/common/update.c): release JSON, versions, checksums and the
 * two archive readers. Usage: test_update <dir> with <dir>/t.tar.gz and <dir>/t.zip made by tests/make_update_fixtures.sh
 * (each holds MindMaze/a.txt "hello", MindMaze/sub/b.bin 70000 bytes, MindMaze/run.sh mode 755, a symlink). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update.h"
#include "mx.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the shape of api.github.com/repos/<o>/<r>/releases/latest (shortened; a body with quotes and key-like text) */
static const char JSON[] =
    "{\n  \"url\": \"https://api.github.com/repos/o/r/releases/1\",\n"
    "  \"html_url\": \"https://github.com/o/r/releases/tag/v1.2.10\",\n"
    "  \"id\": 1,\n  \"author\": {\"login\": \"o\", \"html_url\": \"https://github.com/o\"},\n"
    "  \"tag_name\": \"v1.2.10\",\n  \"name\": \"MindMaze 1.2.10\",\n"
    "  \"assets\": [\n"
    "    {\"url\": \"x\", \"id\": 2, \"name\": \"MindMaze.exe\", \"label\": \"\", \"uploader\": {\"login\": \"o\"},\n"
    "     \"size\": 5, \"browser_download_url\": \"https://github.com/o/r/releases/download/v1.2.10/MindMaze.exe\"},\n"
    "    {\"url\": \"y\", \"name\": \"MindMaze.exe.sha256\",\n"
    "     \"browser_download_url\": \"https://github.com/o/r/releases/download/v1.2.10/MindMaze.exe.sha256\"}\n"
    "  ],\n"
    "  \"body\": \"say \\\"name\\\": and \\\"browser_download_url\\\": \\\"evil\\\" \\u00e9\"\n}\n";

static void *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *d = malloc((size_t)len + 1);
    if (d && fread(d, 1, (size_t)len, f) != (size_t)len) { free(d); d = NULL; }
    fclose(f);
    *n = (size_t)len;
    return d;
}

typedef struct { int a, b, run, link, dirs; } Seen;
static int entry(void *ud, const char *path, char type, unsigned mode, const uint8_t *d, size_t n)
{
    Seen *s = ud;
    if (type == 'd') s->dirs++;
    else if (!strcmp(path, "MindMaze/a.txt")) s->a = type == 'f' && n == 5 && !memcmp(d, "hello", 5);
    else if (!strcmp(path, "MindMaze/sub/b.bin")) {
        int ok = type == 'f' && n == 70000;
        for (size_t i = 0; ok && i < n; i++) ok = d[i] == (uint8_t)(i * 7);
        s->b = ok;
    } else if (!strcmp(path, "MindMaze/run.sh")) s->run = type == 'f' && (mode & 0777) == 0755;
    else if (!strcmp(path, "MindMaze/link")) s->link = type == 'l' && n == 5 && !memcmp(d, "a.txt", 5);
    return 1;
}

int main(int argc, char **argv)
{
    UpdRelease r;
    CHECK(upd_parse(JSON, &r));
    CHECK(!strcmp(r.tag, "v1.2.10"));
    CHECK(!strcmp(r.page, "https://github.com/o/r/releases/tag/v1.2.10"));
    CHECK(r.nassets == 2);
    CHECK(upd_asset(&r, "MindMaze.exe") && strstr(upd_asset(&r, "MindMaze.exe"), "/v1.2.10/MindMaze.exe"));
    CHECK(upd_asset(&r, "MindMaze.exe.sha256") != NULL);
    CHECK(!upd_asset(&r, "evil"));
    CHECK(!upd_parse("{\"message\": \"Not Found\"}", &r));
    CHECK(!upd_parse(NULL, &r));

    CHECK(upd_newer("v1.2.10", "1.2.9"));
    CHECK(upd_newer("1.3", "1.2.9"));
    CHECK(upd_newer("v2.0.0", "1.9.9"));
    CHECK(!upd_newer("v1.2.10", "1.2.10"));
    CHECK(!upd_newer("v1.2.9", "1.2.10"));
    CHECK(!upd_newer("v1.0.2", "dev"));                /* a local build: nothing to compare */
    CHECK(!upd_newer("latest", "1.0.0"));

    char api[600];
    CHECK(upd_api_url("http://127.0.0.1:8000/latest.json", api, sizeof api) && !strcmp(api, "http://127.0.0.1:8000/latest.json"));

    static const char ABC_SUM[] = "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  abc\n";
    CHECK(upd_sha_ok("abc", 3, ABC_SUM, sizeof ABC_SUM - 1));
    CHECK(!upd_sha_ok("abd", 3, ABC_SUM, sizeof ABC_SUM - 1));
    CHECK(!upd_sha_ok("abc", 3, "", 0));
    CHECK(!upd_sha_ok("abc", 3, "ba7816bf", 8));

    if (argc > 1) {
        const char *kinds[2] = {"t.tar.gz", "t.zip"};
        for (int k = 0; k < 2; k++) {
            char p[1024];
            size_t n = 0;
            snprintf(p, sizeof p, "%s/%s", argv[1], kinds[k]);
            uint8_t *d = slurp(p, &n);
            CHECK(d != NULL);
            if (!d) continue;
            Seen s = {0};
            int ok = k ? upd_unzip(d, n, entry, &s) : upd_untar_gz(d, n, entry, &s);
            CHECK(ok);
            CHECK(s.a && s.b && s.run && s.link && s.dirs >= 1);
            if (!(s.a && s.b && s.run && s.link)) fprintf(stderr, "  %s: a %d b %d run %d link %d dirs %d\n", kinds[k], s.a, s.b, s.run, s.link, s.dirs);
            d[n / 2] ^= 0x55;                                  /* damaged: refused, not half-read */
            Seen s2 = {0};
            CHECK(!(k ? upd_unzip(d, n, entry, &s2) : upd_untar_gz(d, n, entry, &s2)) || !(s2.a && s2.b));
            free(d);
        }
    }
    if (fails) { fprintf(stderr, "update tests: %d failed\n", fails); return 1; }
    printf("update tests: all passed\n");
    return 0;
}
