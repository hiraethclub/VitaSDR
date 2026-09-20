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
static float           s_floor = -1.0f; /* adaptive noise floor for contrast */

/* Viridis-style control points (perceptually smooth, colour-blind friendly,
 * easy on the eyes): deep blue/purple -> blue -> teal -> green -> yellow. */
static const unsigned char VIR_R[5] = {  68,  59,  33,  94, 253 };
static const unsigned char VIR_G[5] = {   1,  82, 145, 201, 231 };
static const unsigned char VIR_B[5] = {  84, 139, 140,  98,  37 };

/* Map a power byte (0..255) to a colour for the given palette. */
static unsigned int palette_color(int palette, unsigned char v)
{
    int r, g, b;
    switch (palette) {
    case 1: /* grayscale */
        r = g = b = v;
        break;
    case 2: /* hot: black -> red -> yellow -> white */
        if (v < 85)        { r = v * 3;          g = 0;              b = 0; }
        else if (v < 170)  { r = 255;            g = (v - 85) * 3;   b = 0; }
        else               { r = 255;            g = 255;            b = (v - 170) * 3; }
        break;
    case 3: /* classic: blue -> cyan -> green -> yellow -> red */
        if (v < 64)        { r = 0;              g = v * 4;          b = 255; }
        else if (v < 128)  { r = 0;              g = 255;            b = 255 - (v - 64) * 4; }
        else if (v < 192)  { r = (v - 128) * 4;  g = 255;            b = 0; }
        else               { r = 255;            g = 255 - (v - 192) * 4; b = 0; }
        break;
    default: { /* 0: viridis (default, eye-friendly) */
        float t = (float)v / 255.0f * 4.0f;   /* 0..4 across 5 stops */
        int seg = (int)t;
        if (seg > 3) seg = 3;
        float f = t - (float)seg;
        r = (int)(VIR_R[seg] + (VIR_R[seg + 1] - VIR_R[seg]) * f);
        g = (int)(VIR_G[seg] + (VIR_G[seg + 1] - VIR_G[seg]) * f);
        b = (int)(VIR_B[seg] + (VIR_B[seg + 1] - VIR_B[seg]) * f);
        break;
    }
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

#define WF_GAIN 3.0f   /* contrast above the noise floor */

/* Map a raw bin byte to a 0..255 display level using the current noise floor
 * and a fixed gain. Shared with the spectrum so both react the same way. */
unsigned char wf_level(unsigned char v)
{
    /* Before the floor has been seeded, use a sensible mid default rather than
     * 0 (which would amplify every bin to full scale -> a green flash). */
    float f = s_floor < 0.0f ? 64.0f : s_floor;
    int dv = (int)(((float)v - f) * WF_GAIN);
    if (dv < 0) dv = 0;
    if (dv > 255) dv = 255;
    return (unsigned char)dv;
}

/* Estimate the noise floor as a low percentile of the row. Using a percentile
 * (not the minimum) is robust: a single deep null no longer drags the floor
 * down and blows the whole display out to white. */
static float estimate_floor(const unsigned char *bins, int nbins)
{
    unsigned hist[256];
    memset(hist, 0, sizeof(hist));
    for (int i = 0; i < nbins; i++)
        hist[bins[i]]++;
    unsigned target = (unsigned)nbins / 5; /* 20th percentile */
    unsigned cum = 0;
    for (int v = 0; v < 256; v++) {
        cum += hist[v];
        if (cum >= target)
            return (float)v;
    }
    return 0.0f;
}

void wf_push_bins(const unsigned char *bins, int nbins, int palette)
{
    if (!s_data || nbins <= 0)
        return;

    float target = estimate_floor(bins, nbins);
    if (s_floor < 0.0f)
        s_floor = target;                       /* seed on first row */
    else
        s_floor += (target - s_floor) * 0.10f;  /* ease toward it */

    /* Scroll everything down by one row. */
    memmove(s_data + s_stride, s_data, s_stride * (WF_TEX_H - 1));

    /* Write the new top row. */
    unsigned int *row = (unsigned int *)s_data;
    for (int x = 0; x < WF_TEX_W; x++) {
        int idx = (int)((long)x * nbins / WF_TEX_W);
        if (idx >= nbins) idx = nbins - 1;
        row[x] = palette_color(palette, wf_level(bins[idx]));
    }
}

/* Reset the adaptive noise floor (e.g. on retune) so the display re-settles. */
void wf_render_reset(void)
{
    s_floor = -1.0f;
}

void wf_render_draw(int x, int y, int w, int h)
{
    if (!s_tex)
        return;
    float sx = (float)w / (float)WF_TEX_W;
    float sy = (float)h / (float)WF_TEX_H;
    vita2d_draw_texture_scale(s_tex, (float)x, (float)y, sx, sy);
}
