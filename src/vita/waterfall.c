/* Scrolling waterfall renderer (vita2d).
 *
 * Maintains a full-width texture; each new FFT row is written at the top and
 * the existing rows shift down one line, so the display scrolls downward. The
 * texture is allocated once. All calls happen on the render thread.
 */
#include "app.h"

#include <vita2d.h>

#include <string.h>

#define WF_TEX_W KIWI_WF_BINS   /* 1024 bins */
#define WF_TEX_H 340            /* rows of history */

static vita2d_texture *s_tex = NULL;
static unsigned int    s_stride = 0;  /* bytes per row */
static uint8_t        *s_data = NULL;

/* Map a power byte (0..255) to a colour for the given palette. */
static unsigned int palette_color(int palette, unsigned char v)
{
    int r, g, b;
    switch (palette) {
    case 1: /* grayscale */
        r = g = b = v;
        break;
    case 2: /* classic: blue -> cyan -> green -> yellow -> red */
        if (v < 64)        { r = 0;              g = v * 4;          b = 255; }
        else if (v < 128)  { r = 0;              g = 255;            b = 255 - (v - 64) * 4; }
        else if (v < 192)  { r = (v - 128) * 4;  g = 255;            b = 0; }
        else               { r = 255;            g = 255 - (v - 192) * 4; b = 0; }
        break;
    default: /* 0: hot: black -> red -> yellow -> white */
        if (v < 85)        { r = v * 3;          g = 0;              b = 0; }
        else if (v < 170)  { r = 255;            g = (v - 85) * 3;   b = 0; }
        else               { r = 255;            g = 255;            b = (v - 170) * 3; }
        break;
    }
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return RGBA8(r, g, b, 255);
}

int wf_render_init(void)
{
    s_tex = vita2d_create_empty_texture(WF_TEX_W, WF_TEX_H);
    if (!s_tex)
        return -1;
    s_stride = vita2d_texture_get_stride(s_tex);
    s_data = (uint8_t *)vita2d_texture_get_datap(s_tex);
    memset(s_data, 0, s_stride * WF_TEX_H);
    return 0;
}

void wf_render_shutdown(void)
{
    if (s_tex) {
        vita2d_free_texture(s_tex);
        s_tex = NULL;
        s_data = NULL;
    }
}

void wf_push_bins(const unsigned char *bins, int nbins, int palette)
{
    if (!s_data || nbins <= 0)
        return;

    /* Adaptive contrast: map the observed level range onto the full palette so
     * the waterfall stays visible whatever the absolute byte levels are. Track
     * a slowly-adapting floor (min) and peak (max); stretch each row across
     * them. Without this, a quiet band or an unexpected dB window renders as a
     * flat near-black block. */
    static float a_min = 40.0f, a_max = 200.0f;
    int rmin = 255, rmax = 0;
    for (int i = 0; i < nbins; i++) {
        int v = bins[i];
        if (v < rmin) rmin = v;
        if (v > rmax) rmax = v;
    }
    /* Ease the adaptive window toward this row (floor faster than peak so
     * transient signals stay bright). */
    a_min += ((float)rmin - a_min) * 0.10f;
    a_max += ((float)rmax - a_max) * 0.02f;
    if (a_max < a_min + 16.0f) a_max = a_min + 16.0f;
    float scale = 255.0f / (a_max - a_min);

    /* Scroll everything down by one row. */
    memmove(s_data + s_stride, s_data, s_stride * (WF_TEX_H - 1));

    /* Write the new top row. */
    unsigned int *row = (unsigned int *)s_data;
    for (int x = 0; x < WF_TEX_W; x++) {
        int idx = (int)((long)x * nbins / WF_TEX_W);
        if (idx >= nbins) idx = nbins - 1;
        int dv = (int)(((float)bins[idx] - a_min) * scale);
        if (dv < 0) dv = 0;
        if (dv > 255) dv = 255;
        row[x] = palette_color(palette, (unsigned char)dv);
    }
}

void wf_render_draw(int x, int y, int w, int h)
{
    if (!s_tex)
        return;
    float sx = (float)w / (float)WF_TEX_W;
    float sy = (float)h / (float)WF_TEX_H;
    vita2d_draw_texture_scale(s_tex, (float)x, (float)y, sx, sy);
}
