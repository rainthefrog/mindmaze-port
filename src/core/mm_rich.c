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
static int g_rich_height;                                  /* pixels of the last wrapped text */

static Piece g_pc[400];                                    /* the last layout (mm_rich_page reads it) */
static struct { int first, end, w; } g_ln[200];
static int g_nl, g_np;                                    /* lines, pieces of the last layout */

void mm_rich(MMCanvas *c, int x, int y, int w, int h, const char *text, int font, int ifont, uint32_t rgb, int flags)
{
    Piece *pc = g_pc;
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
    /* word wrap: lines first (CHANGE 2026-11-21: then each line is placed, so wrapped text can be centred - TX_CENTER -
     * and the block centred vertically - TX_VCENTER - for the touch layout's buttons; mm_rich_height measures it) */
    int nl = 0, cx = 0;
#define ln g_ln
    for (int i = 0; i < np && nl < 200; i++) {
        int gw = pc[i].w, j = i + 1;                       /* pieces glued to this one (no space between) */
        while (j < np && !pc[j].space_before) gw += pc[j++].w;
        int lead = (cx > 0 && pc[i].space_before) ? sp : 0;
        if (cx > 0 && cx + lead + gw > W) { ln[nl - 1].w = cx; cx = 0; lead = 0; if (nl < 200) ln[nl].first = i; nl++; }
        else if (!nl) { ln[0].first = i; nl = 1; }
        cx += lead + gw;
        if (nl <= 200) { ln[nl - 1].end = j; ln[nl - 1].w = cx; }
        i = j - 1;
    }
    if (nl > 200) nl = 200;
    g_nl = nl;
    g_np = np;
    g_rich_height = nl * lh;
    if (flags & TX_MEASURE) return;
    int top = (flags & TX_VCENTER) && nl * lh < H ? (H - nl * lh) / 2 : 0;
    for (int l = 0; l < nl; l++) {
        int x = (flags & TX_CENTER) ? (W - ln[l].w) / 2 : (flags & TX_RIGHT) ? W - ln[l].w : 0;
        if (x < 0) x = 0;
        for (int k = ln[l].first; k < ln[l].end; k++) {
            if (k > ln[l].first && pc[k].space_before) x += sp;
            plat_text_px(c, X0 + x, Y0 + top + l * lh, pc[k].txt, pc[k].italic ? ifont : font, rgb, X0, Y0, X0 + W, Y0 + H);
            x += pc[k].w;
        }
    }
}
#undef ln

int mm_rich_height(MMCanvas *c, int w, const char *text, int font, int ifont)
{
    mm_rich(c, 0, 0, w, 1 << 20, text, font, ifont, 0, TX_WRAP | TX_MEASURE);
    return (g_rich_height + c->scale - 1) / c->scale;
}

/* ---- paged text (CHANGE 2026-11-21, user: the typewriter cut off the end, the size changed while typing, and a larger
 * font needs Next / Previous; then: "try to cut on the end of a sentence"): the WHOLE text is cut into words once at
 * its final font; pages are filled down to their last line (user: half a page cut off too much); a page ends after a sentence
 * that ends on that last line ('.', '!', '?', also before a closing quote), otherwise at the end of the last line.
 * Each page is wrapped on its own from its first word. One page is drawn, with at most `chars` of its letters (the
 * typewriter; -1 = all); positions never depend on how much is shown. Returns the letters on that page (spaces do not
 * count); *npages = the number of pages. Top-left aligned, like the original pop-ups. ---- */
static int utf8_count(const char *s) { int n = 0; for (; *s; s++) n += (*s & 0xC0) != 0x80; return n; }

static int sentence_end(int k)                             /* piece k ends a sentence (and the next is a new word) */
{
    if (k + 1 < g_np && !g_pc[k + 1].space_before) return 0;
    const char *t = g_pc[k].txt;
    size_t n = strlen(t);
    while (n && (t[n - 1] == '"' || t[n - 1] == '\'' || t[n - 1] == ')' || (unsigned char)t[n - 1] >= 0x80)) {
        if ((unsigned char)t[n - 1] >= 0x80) {             /* a closing curly quote (UTF-8 E2 80 9D / 99) */
            if (n >= 3 && (unsigned char)t[n - 3] == 0xE2 && (unsigned char)t[n - 2] == 0x80) { n -= 3; continue; }
            break;
        }
        n--;
    }
    return n && (t[n - 1] == '.' || t[n - 1] == '!' || t[n - 1] == '?');
}

/* lines from piece s, at most maxl, pieces before `stop`: first / end piece of each line; returns the lines */
static int wrap_from(int s, int stop, int W, int sp, int maxl, int *first, int *end)
{
    int nl = 0, cx = 0;
    for (int i = s; i < stop; i++) {
        int gw = g_pc[i].w, j = i + 1;                     /* pieces glued to this one (no space between) */
        while (j < stop && !g_pc[j].space_before) gw += g_pc[j++].w;
        int lead = (cx > 0 && g_pc[i].space_before) ? sp : 0;
        if (!nl || (cx > 0 && cx + lead + gw > W)) {
            if (nl == maxl) break;
            first[nl] = i;
            nl++;
            cx = 0;
            lead = 0;
        }
        cx += lead + gw;
        end[nl - 1] = j;
        i = j - 1;
    }
    return nl;
}

int mm_rich_overflow;                                       /* (tests) words drawn past the right edge of the box */
int mm_rich_page(MMCanvas *c, int x, int y, int w, int h, const char *text, int font, int ifont, uint32_t rgb,
                 int page, int chars, int *npages, int *nlines)
{
    mm_rich(c, 0, 0, w, 1 << 20, text, font, ifont, 0, TX_WRAP | TX_MEASURE);   /* the words and their widths */
    int s = c->scale, X0 = x * s, Y0 = y * s, W = w * s, H = h * s;
    int lh = plat_text_px_height(c, font), sp = plat_text_px_width(c, " ", font);
    int per = H / (lh > 0 ? lh : 1);
    if (per < 1) per = 1;
    if (per > 200) per = 200;
    static int start[65], first[201], end[201];
    int pages = 0, p0 = 0;
    while (pages < 64) {                                    /* page starts */
        start[pages++] = p0;
        int nl = wrap_from(p0, g_np, W, sp, per, first, end);
        if (!nl || end[nl - 1] >= g_np) break;              /* the rest fits: the last page */
        int cut = end[nl - 1];
        {                                                   /* a sentence that ends on the page's last line */
            int l = nl - 1, k = end[l] - 1;
            while (k >= first[l] && !sentence_end(k)) k--;
            if (k >= first[l]) cut = k + 1;
        }
        p0 = cut;
    }
    start[pages] = g_np;
    if (npages) *npages = pages;
    if (page >= pages) page = pages - 1;
    if (page < 0) page = 0;
    int nl = wrap_from(start[page], start[page + 1], W, sp, per, first, end), shown = 0;
    if (nlines) *nlines = nl;
    for (int l = 0; l < nl; l++) {
        int px = 0;
        for (int k = first[l]; k < end[l]; k++) {
            if (k > first[l] && g_pc[k].space_before) px += sp;
            int n = utf8_count(g_pc[k].txt), f = g_pc[k].italic ? ifont : font;
            if (chars < 0 || shown + n <= chars)
                plat_text_px(c, X0 + px, Y0 + l * lh, g_pc[k].txt, f, rgb, X0, Y0, X0 + W, Y0 + H);
            else if (shown < chars) {                      /* the word being typed: its first letters */
                char t[256];
                size_t b = 0;
                for (int m = 0; g_pc[k].txt[b] && m <= chars - shown; b++)
                    if ((g_pc[k].txt[b] & 0xC0) != 0x80 && ++m > chars - shown) break;
                memcpy(t, g_pc[k].txt, b);
                t[b] = 0;
                plat_text_px(c, X0 + px, Y0 + l * lh, t, f, rgb, X0, Y0, X0 + W, Y0 + H);
            }
            shown += n;
            px += g_pc[k].w;
            if (px > W) mm_rich_overflow++;
        }
    }
    return shown;
}
