/* Command-line front end of the built-in art tools (the same code the game runs), for tests and developers:
 *   mmart <data folder> prepare      write the sheets into <data>/art/work
 *   mmart <data folder> install      cut the agent's results into <data>/hd and <data>/wide
 * Build: make mmart (Linux). */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "mm.h"
#include "mm_art.h"

static const char *g_data;

void *plat_load(const char *rel, int *size)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", g_data, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc((size_t)n + 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    d[n] = 0;
    if (size) *size = (int)n;
    return d;
}

int plat_save_data(const char *rel, const void *data, int len)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", g_data, rel);
    for (char *s = p + strlen(g_data) + 1; (s = strchr(s, '/')); s++) {
        *s = 0;
        if (mkdir(p, 0755) && errno != EEXIST) return 0;
        *s = '/';
    }
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    return fclose(f) == 0 && ok;
}

long plat_data_size(const char *rel)
{
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", g_data, rel);
    return stat(p, &st) || !S_ISREG(st.st_mode) ? -1 : (long)st.st_size;
}

void mm_pictures_reload(void) {}
char *mm_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: mmart <data folder> prepare|install\n"); return 2; }
    g_data = argv[1];
    char rep[4096];
    if (!strcmp(argv[2], "prepare")) {
        int ok = mm_art_prepare(1, rep, sizeof rep);
        printf("%s\n", rep);
        return !ok;
    }
    if (!strcmp(argv[2], "install")) {
        int n = mm_art_install(rep, sizeof rep);
        printf("%s", rep);
        return n < 0;
    }
    fprintf(stderr, "unknown command %s\n", argv[2]);
    return 2;
}
