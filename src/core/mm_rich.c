/* Encarta question text: words with <it>...</it> italics (also <i>), other tags (<scp>, <sup>, <sub>, <sc>,
 * <dyear>, <date ...>) are shown as plain text. A '<' that does not start a known tag is dropped (one
 * question has a broken "<Whistler's Mother</it>"). Layout is done in canvas pixels so word spacing is exact
 * at every scale. Entities were already decoded by the question loader. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mm.h"

static const char *TAGS[] = {"it", "i", "scp", "scap", "sc", "sup", "sub", "dyear", "date", "b"};   /* scap: Encarta 98 */

/* if s starts a known tag, return its length and set *italic_on (1 on, 0 off, -1 not italic) */
static int tag_at(const char *s, int *italic)
{
    const char *end = strchr(s, '>');
    if (!end || end - s > 40) return 0;
    const char *n = s + 1;
    int closing = *n == '/' || *n == '\\';                   /* one Encarta 98 question has "<\it>" */
    if (closing) n++;
    size_t len = 0;
    while (n[len] && n[len] != '>' && n[len] != ' ' && n[len] != '/') len++;
    if (n[len] == '/' && n[len + 1] == '>') closing = 1;      /* Encarta 98 closes italics with "<it/>" */
    for (size_t i = 0; i < sizeof TAGS / sizeof TAGS[0]; i++)
        if (strlen(TAGS[i]) == len && !strncmp(n, TAGS[i], len)) {
            *italic = (!strcmp(TAGS[i], "it") || !strcmp(TAGS[i], "i")) ? !closing : -1;
            return (int)(end - s + 1);
        }
    return 0;
}

void mm_plain(const char *src, char *out, int n)
{
    int o = 0;
    for (const char *p = src; *p && o < n - 1;) {
        int it, t = *p == '<' ? tag_at(p, &it) : 0;
        if (t) { p += t; continue; }
        if (*p == '<') { p++; continue; }
        out[o++] = *p++;
    }
    out[o] = 0;
}

typedef struct { char txt[256]; int italic, space_before, w; } Piece;

void mm_rich(MMCanvas *c, int x, int y, int w, int h, const char *text, int font, int ifont, uint32_t rgb, int flags)
{
    static Piece pc[400];
    int np = 0, italic = 0, space = 0, len = 0;
    char buf[256];
    for (const char *p = text;; p++) {
        int it = -1, t = (*p == '<') ? tag_at(p, &it) : 0;
        int brk = !*p || *p == ' ' || *p == '\n' || t;
        if (brk && len && np < 400) {
            buf[len] = 0;
            snprintf(pc[np].txt, sizeof pc[0].txt, "%s", buf);
            pc[np].italic = italic;
            pc[np].space_before = space;
            np++;
            len = 0;
            space = 0;
        }
        if (!*p) break;
        if (t) { if (it >= 0) italic = it; p += t - 1; continue; }
        if (*p == ' ' || *p == '\n') { space = 1; continue; }
        if (*p == '<') continue;
        if (len < 255) buf[len++] = *p;
    }
    int s = c->scale;
    int X0 = x * s, Y0 = y * s, W = w * s, H = h * s;
    int lh = plat_text_px_height(c, font), sp = plat_text_px_width(c, " ", font);
    for (int i = 0; i < np; i++) pc[i].w = plat_text_px_width(c, pc[i].txt, pc[i].italic ? ifont : font);

    if (!(flags & TX_WRAP)) {                               /* one line, cut with an ellipsis */
        int cx = 0, total = 0;
        for (int i = 0; i < np; i++) total += pc[i].w + (i && pc[i].space_before ? sp : 0);
        if ((flags & TX_CENTER) && total < W) cx = (W - total) / 2;     /* centred (answer buttons) */
        int Y = (flags & TX_VCENTER) ? Y0 + (H - lh) / 2 : Y0;
        int ell = plat_text_px_width(c, "\xE2\x80\xA6", font);
        for (int i = 0; i < np; i++) {
            if (i && pc[i].space_before) cx += sp;
            int f = pc[i].italic ? ifont : font;
            if (total > W && cx + pc[i].w > W - ell) {       /* shorten this piece and stop */
                char t[260];
                snprintf(t, sizeof t, "%s", pc[i].txt);
                size_t n = strlen(t);
                while (n) {
                    do n--; while (n && (t[n] & 0xC0) == 0x80);
                    t[n] = 0;
                    if (cx + plat_text_px_width(c, t, f) <= W - ell) break;
                }
                strcat(t, "\xE2\x80\xA6");
                plat_text_px(c, X0 + cx, Y, t, f, rgb, X0, Y0, X0 + W, Y0 + H);
                return;
            }
            plat_text_px(c, X0 + cx, Y, pc[i].txt, f, rgb, X0, Y0, X0 + W, Y0 + H);
            cx += pc[i].w;
        }
        return;
    }
    int cx = 0, cy = 0;                                    /* word wrap */
    for (int i = 0; i < np; i++) {
        int gw = pc[i].w, j = i + 1;                       /* pieces glued to this one (no space between) */
        while (j < np && !pc[j].space_before) gw += pc[j++].w;
        int lead = (cx > 0 && pc[i].space_before) ? sp : 0;
        if (cx > 0 && cx + lead + gw > W) { cx = 0; cy += lh; lead = 0; }
        cx += lead;
        for (int k = i; k < j; k++) {
            plat_text_px(c, X0 + cx, Y0 + cy, pc[k].txt, pc[k].italic ? ifont : font, rgb, X0, Y0, X0 + W, Y0 + H);
            cx += pc[k].w;
        }
        i = j - 1;
    }
}
