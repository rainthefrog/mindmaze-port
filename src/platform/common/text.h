/* Text with open fonts (text.c, shared by the web and SDL layers): the GDI-like layer the platform's plat_text* functions call. */
#pragma once
#include "mm.h"

int text_init(const char *font_dir);                     /* 0 if a font file is missing (fallback metrics then) */
void text_box(MMCanvas *c, int x, int y, int w, int h, const char *utf8, int font, uint32_t rgb, int flags);
int text_width(const char *utf8, int font, int scale);   /* logical pixels */
int text_px_width(MMCanvas *c, const char *utf8, int font);
int text_px_height(MMCanvas *c, int font);
int text_px_ascent(MMCanvas *c, int font);
void text_px(MMCanvas *c, int X, int Y, const char *utf8, int font, uint32_t rgb, int cx0, int cy0, int cx1, int cy1);
