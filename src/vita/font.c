/* See font.h. */
#include "font.h"

#include <vita2d.h>

#include <stdarg.h>
#include <stdio.h>

/* Pixel size for scale 1.0. Chosen to sit close to the old pgf default so the
 * existing layout coordinates still line up. */
#define FONT_BASE 21.0f

/* Bundled TTF, placed in the VPK at this path (see CMakeLists FILE entry). */
#define FONT_PATH "app0:resources/font.ttf"

static int          s_tried = 0;
static vita2d_font *s_ttf = NULL;
static vita2d_pgf  *s_pgf = NULL;

void font_ensure(void)
{
    if (s_tried)
        return;
    s_tried = 1;
    s_ttf = vita2d_load_font_file(FONT_PATH);
    if (!s_ttf)
        s_pgf = vita2d_load_default_pgf();   /* fallback: never leave UI blank */
}

void font_drawf(int x, int y, unsigned int color, float scale,
                const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (s_ttf) {
        unsigned int px = (unsigned int)(scale * FONT_BASE + 0.5f);
        if (px < 8) px = 8;
        vita2d_font_draw_text(s_ttf, x, y, color, px, buf);
    } else if (s_pgf) {
        vita2d_pgf_draw_text(s_pgf, x, y, color, scale, buf);
    }
}
