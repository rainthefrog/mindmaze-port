/* MindMaze - built-in updater, the portable part (see update.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "update.h"
#include "mx.h"

#ifndef MM_REPO_URL
#define MM_REPO_URL "https://github.com/OWNER/mindmaze-port"
#endif

int upd_api_url(const char *setting, char *out, size_t n)
{
    if (setting && *setting) { snprintf(out, n, "%s", setting); return 1; }
    static const char GH[] = "https://github.com/";
    const char *r = MM_REPO_URL;
    if (strncmp(r, GH, sizeof GH - 1) || strstr(r, "/OWNER/")) return 0;
    snprintf(out, n, "https://api.github.com/repos/%s/releases/latest", r + sizeof GH - 1);
    return 1;
}

/* ---- the few JSON strings needed: "key": "value" ---- */
static const char *find_key(const char *from, const char *end, const char *key)   /* just after the key's colon */
{
    size_t k = strlen(key);
    for (const char *p = from; p && p < end; p++) {
        p = strstr(p, key);
        if (!p || p >= end) return NULL;
        if (p[-1] != '"' || p[k] != '"' || (p - 2 >= from && p[-2] == '\\')) continue;
        const char *q = p + k + 1;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        return q;
    }
    return NULL;
}

static int read_str(const char *p, char *out, size_t n)          /* p at the opening quote; 1 = ok */
{
    if (!p || *p != '"' || !n) return 0;
    size_t o = 0;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            c = *++p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') {                                      /* not in names or links: keep a '?' */
                for (int i = 0; i < 4 && p[1]; i++) p++;
                c = '?';
            } else if (!c) return 0;
        }
        if (o + 1 < n) out[o++] = c;
    }
    out[o] = 0;
    return *p == '"';
}

int upd_parse(const char *json, UpdRelease *r)
{
    memset(r, 0, sizeof *r);
    if (!json) return 0;
    const char *end = json + strlen(json);
    const char *assets = find_key(json, end, "assets");
    const char *top_end = assets ? assets : end;
    /* the release's own fields come before "assets" (author / uploader objects have no tag_name; the release's
     * html_url is its first one) */
    if (!read_str(find_key(json, top_end, "tag_name"), r->tag, sizeof r->tag)) return 0;
    read_str(find_key(json, top_end, "html_url"), r->page, sizeof r->page);
    if (!assets) return 1;
    for (const char *p = assets; r->nassets < UPD_MAX_ASSETS;) {
        const char *u = find_key(p, end, "browser_download_url");
        if (!u) break;
        /* the asset's name: the last "name" key between the previous asset and this link */
        const char *nm = NULL;
        for (const char *q = p; (q = find_key(q, u, "name")) != NULL; q++) nm = q;
        if (nm && read_str(nm, r->asset[r->nassets].name, sizeof r->asset[0].name) &&
            read_str(u, r->asset[r->nassets].url, sizeof r->asset[0].url))
            r->nassets++;
        p = u + 1;
    }
    return 1;
}

static int version_parts(const char *v, long part[4])
{
    int n = 0;
    if (*v == 'v' || *v == 'V') v++;
    for (; n < 4; n++) {
        if (*v < '0' || *v > '9') break;
        char *e;
        part[n] = strtol(v, &e, 10);
        v = e;
        if (*v != '.') { n++; break; }
        v++;
    }
    for (int i = n; i < 4; i++) part[i] = 0;
    return n;
}

int upd_newer(const char *tag, const char *current)
{
    long a[4], b[4];
    if (!version_parts(tag, a) || !version_parts(current, b)) return 0;
    for (int i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}

const char *upd_asset(const UpdRelease *r, const char *name)
{
    for (int i = 0; i < r->nassets; i++)
        if (!strcmp(r->asset[i].name, name)) return r->asset[i].url;
    return NULL;
}

int upd_sha_ok(const void *data, size_t n, const char *sumfile, size_t sumlen)
{
    char want[65];
    size_t k = 0;
    for (size_t i = 0; i < sumlen && k < 64; i++) {
        char c = sumfile[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) want[k++] = (char)(c | 0x20);
        else if (k) break;                                        /* the hash ends at the first other character */
    }
    if (k != 64) return 0;
    want[64] = 0;
    uint8_t h[32];
    char got[65];
    mx_sha256(data, n, h);
    for (int i = 0; i < 32; i++) snprintf(got + i * 2, 3, "%02x", h[i]);
    return !strcmp(got, want);
}

/* ---- .tar.gz (gzip RFC 1952 around a ustar / GNU tar) ---- */
static unsigned long octal(const uint8_t *p, int n)
{
    unsigned long v = 0;
    for (int i = 0; i < n && p[i]; i++)
        if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (p[i] - '0');
    return v;
}

int upd_untar_gz(const uint8_t *gz, size_t n, UpdEntry fn, void *ud)
{
    if (n < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8) return 0;
    int flg = gz[3];
    size_t o = 10;
    if (flg & 4) { if (o + 2 > n) return 0; o += 2 + (size_t)(gz[o] | gz[o + 1] << 8); }
    if (flg & 8) { while (o < n && gz[o]) o++; o++; }
    if (flg & 16) { while (o < n && gz[o]) o++; o++; }
    if (flg & 2) o += 2;
    if (o >= n - 8) return 0;
    size_t isize = (size_t)gz[n - 4] | (size_t)gz[n - 3] << 8 | (size_t)gz[n - 2] << 16 | (size_t)gz[n - 1] << 24;
    uint32_t crc = (uint32_t)gz[n - 8] | (uint32_t)gz[n - 7] << 8 | (uint32_t)gz[n - 6] << 16 | (uint32_t)gz[n - 5] << 24;
    size_t len = 0, used = 0;
    uint8_t *t = mx_inflate_raw(gz + o, n - o - 8, isize + 1, &len, &used);
    if (!t) return 0;
    int ok = len == isize && mx_crc32(t, len) == crc;
    char longname[1024] = "";
    for (size_t p = 0; ok && p + 512 <= len;) {
        const uint8_t *h = t + p;
        if (!h[0]) break;                                          /* the end: zero blocks */
        unsigned long size = octal(h + 124, 12);
        char type = (char)h[156];
        p += 512;
        if (p + size > len) { ok = 0; break; }
        const uint8_t *d = t + p;
        p += (size + 511) & ~511UL;
        if (type == 'L') {                                         /* GNU: the next entry's long name */
            snprintf(longname, sizeof longname, "%.*s", (int)(size < sizeof longname ? size : sizeof longname - 1), (const char *)d);
            continue;
        }
        if (type == 'x') {                                         /* pax: "<len> path=<name>\n" records */
            const char *s = (const char *)d, *e = s + size;
            while (s < e) {
                long rl = strtol(s, NULL, 10);
                const char *sp = memchr(s, ' ', (size_t)(e - s));
                if (rl <= 0 || !sp || s + rl > e) break;
                if (!strncmp(sp + 1, "path=", 5))
                    snprintf(longname, sizeof longname, "%.*s", (int)(s + rl - (sp + 6) - 1), sp + 6);
                s += rl;
            }
            continue;
        }
        if (type == 'g') continue;
        char name[1024];
        if (*longname) { snprintf(name, sizeof name, "%s", longname); longname[0] = 0; }
        else if (!memcmp(h + 257, "ustar", 5) && h[345])
            snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
        else snprintf(name, sizeof name, "%.100s", (const char *)h);
        unsigned mode = (unsigned)octal(h + 100, 8) & 07777;
        if (type == '0' || type == 0 || type == '7') ok = fn(ud, name, 'f', mode, d, size);
        else if (type == '5') ok = fn(ud, name, 'd', mode, NULL, 0);
        else if (type == '2') {
            char target[101];
            snprintf(target, sizeof target, "%.100s", (const char *)h + 157);
            ok = fn(ud, name, 'l', mode, (const uint8_t *)target, strlen(target));
        }                                                          /* other types (devices, hard links): skipped */
    }
    free(t);
    return ok;
}

/* ---- .zip (stored or deflated; no zip64: the downloads are far below 4 GB) ---- */
static unsigned u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static unsigned long u32(const uint8_t *p) { return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }

int upd_unzip(const uint8_t *z, size_t n, UpdEntry fn, void *ud)
{
    if (n < 22) return 0;
    size_t eocd = n - 22, stop = n > 22 + 65535 ? n - 22 - 65535 : 0;
    while (u32(z + eocd) != 0x06054b50) {
        if (eocd == stop) return 0;
        eocd--;
    }
    unsigned count = u16(z + eocd + 10);
    size_t cd = u32(z + eocd + 16);
    for (unsigned i = 0; i < count; i++) {
        if (cd + 46 > n || u32(z + cd) != 0x02014b50) return 0;
        const uint8_t *c = z + cd;
        unsigned method = u16(c + 10), nlen = u16(c + 28), xlen = u16(c + 30), clen = u16(c + 32);
        size_t csize = u32(c + 20), usize = u32(c + 24), lo = u32(c + 42);
        unsigned long crc = u32(c + 16), ext = u32(c + 38);
        if (cd + 46 + nlen > n) return 0;
        char name[1024];
        snprintf(name, sizeof name, "%.*s", (int)nlen, (const char *)c + 46);
        cd += 46 + nlen + xlen + clen;
        unsigned mode = c[5] == 3 ? (unsigned)(ext >> 16) : 0;     /* made on unix: permissions in the high half */
        if (lo + 30 > n || u32(z + lo) != 0x04034b50) return 0;
        size_t data = lo + 30 + u16(z + lo + 26) + u16(z + lo + 28);
        if (data + csize > n) return 0;
        size_t len = nlen ? strlen(name) : 0;
        if (len && name[len - 1] == '/') { if (!fn(ud, name, 'd', mode & 07777, NULL, 0)) return 0; continue; }
        uint8_t *buf = NULL;
        const uint8_t *d = z + data;
        if (method == 8) {
            size_t got = 0, used = 0;
            buf = mx_inflate_raw(z + data, csize, usize + 1, &got, &used);
            if (!buf || got != usize) { free(buf); return 0; }
            d = buf;
        } else if (method != 0 || csize != usize) return 0;
        int ok = mx_crc32(d, usize) == crc;
        if (ok) ok = fn(ud, name, (mode & 0170000) == 0120000 ? 'l' : 'f', mode & 07777, d, usize);
        free(buf);
        if (!ok) return 0;
    }
    return 1;
}
