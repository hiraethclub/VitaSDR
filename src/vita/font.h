/* Shared UI font. Loads a bundled TTF (crisp, via vita2d/freetype) and provides
 * a drop-in replacement for the old pgf text calls. Falls back to the system
 * PGF font if the TTF can't be loaded, so text never disappears. */
#ifndef VITASDR_FONT_H
#define VITASDR_FONT_H

/* Load the font once (idempotent). Safe to call before every draw. */
void font_ensure(void);

/* Draw formatted text. x,y is the text baseline; `scale` keeps the old pgf
 * scale convention (1.0 ~ the previous default size) so call sites are a direct
 * swap for vita2d_pgf_draw_textf(font, ...). */
void font_drawf(int x, int y, unsigned int color, float scale,
                const char *fmt, ...) __attribute__((format(printf, 5, 6)));

#endif /* VITASDR_FONT_H */
