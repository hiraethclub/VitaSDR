/* Screen drawing: status bar, waterfall, spectrum, frequency ruler, sliders.
 * All drawing happens between vita2d_start_drawing/end_drawing in main.c. */
#include "app.h"
#include "bandplan.h"
#include "font.h"

#include <vita2d.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>

#include <stdio.h>
#include <string.h>

/* Layout bands (960x544). */
#define STATUS_H   30
#define WF_Y       STATUS_H
#define WF_H       320
#define SPEC_Y     (WF_Y + WF_H)
#define SPEC_H     100
#define BOTTOM_Y   (SPEC_Y + SPEC_H)
#define BOTTOM_H   (SCREEN_H - BOTTOM_Y)

#define COL_BG     RGBA8(10, 12, 16, 255)
#define COL_BAR    RGBA8(24, 28, 36, 255)
#define COL_TEXT   RGBA8(230, 235, 240, 255)
#define COL_DIM    RGBA8(140, 150, 160, 255)
#define COL_ACCENT RGBA8(80, 200, 255, 255)
#define COL_GREEN  RGBA8(60, 220, 120, 255)
#define COL_RED    RGBA8(230, 70, 70, 255)
#define COL_AMBER  RGBA8(240, 180, 60, 255)
#define COL_SPEC   RGBA8(90, 220, 160, 255)

static char s_msg[128] = {0};
static int  s_msg_frames = 0;

void ui_show_message(app_state *app, const char *msg)
{
    (void)app;
    strncpy(s_msg, msg, sizeof(s_msg) - 1);
    s_msg[sizeof(s_msg) - 1] = '\0';
    s_msg_frames = 180; /* ~3 seconds at 60fps */
}

/* Format frequency (kHz) as "M.kkk.hhh" e.g. 7074.0 -> "7.074.000". */
static void format_freq(double freq_khz, char *out, size_t n)
{
    long hz = (long)(freq_khz * 1000.0 + 0.5);
    long mhz = hz / 1000000;
    long khz3 = (hz / 1000) % 1000;
    long hz3 = hz % 1000;
    snprintf(out, n, "%ld.%03ld.%03ld", mhz, khz3, hz3);
}

/* Map rssi (dBm) to an S-meter value 0..15 (S9 = -73 dBm, 6 dB/unit). */
static int rssi_to_s(float rssi)
{
    int s = (int)((rssi + 127.0f) / 6.0f);
    if (s < 0) s = 0;
    if (s > 15) s = 15;
    return s;
}

static void draw_status_bar(app_state *app)
{
    vita2d_draw_rectangle(0, 0, SCREEN_W, STATUS_H, COL_BAR);

    char freq[32];
    format_freq(app->freq_khz, freq, sizeof(freq));
    font_drawf(8, 22, COL_TEXT, 1.3f, "%s MHz", freq);

    /* Mode + step. "nbfm" shows as the friendlier "FM". */
    char mode_up[8];
    if (strcmp(app->mode, "nbfm") == 0) {
        strcpy(mode_up, "FM");
    } else {
        size_t i;
        for (i = 0; i < sizeof(mode_up) - 1 && app->mode[i]; i++)
            mode_up[i] = (app->mode[i] >= 'a' && app->mode[i] <= 'z')
                         ? (char)(app->mode[i] - 32) : app->mode[i];
        mode_up[i] = '\0';
    }
    font_drawf(300, 22, COL_ACCENT, 1.0f, "%s", mode_up);

    char stepbuf[24];
    if (app->step_hz >= 1000)
        snprintf(stepbuf, sizeof(stepbuf), "step %dk", app->step_hz / 1000);
    else
        snprintf(stepbuf, sizeof(stepbuf), "step %dHz", app->step_hz);
    font_drawf(360, 22, COL_DIM, 1.0f, "%s", stepbuf);

    /* S-meter bar. */
    int s = rssi_to_s(app->rssi_dbm);
    int mx = 490, my = 8, mw = 160, mh = 14;
    vita2d_draw_rectangle(mx, my, mw, mh, RGBA8(40, 44, 52, 255));
    int fillw = mw * s / 15;
    unsigned int scol = (s >= 9) ? COL_RED : COL_GREEN;
    vita2d_draw_rectangle(mx, my, fillw, mh, scol);
    if (s <= 9)
        font_drawf(mx + mw + 6, 22, COL_TEXT, 0.9f,
                              "S%d", s);
    else
        font_drawf(mx + mw + 6, 22, COL_TEXT, 0.9f,
                              "S9+%d", (s - 9) * 6);

    /* Connection status dot + label. */
    unsigned int dot;
    const char *label;
    switch (app->conn_status) {
    case CONN_CONNECTED:  dot = COL_GREEN; label = "KIWI"; break;
    case CONN_CONNECTING: dot = COL_AMBER; label = "..."; break;
    case CONN_ERROR:      dot = COL_RED;   label = "ERR"; break;
    default:              dot = COL_DIM;   label = "OFF"; break;
    }
    vita2d_draw_rectangle(SCREEN_W - 70, 9, 12, 12, dot);
    font_drawf(SCREEN_W - 52, 22, COL_TEXT, 0.9f,
                          "%s", label);
}

static void draw_spectrum(app_state *app)
{
    vita2d_draw_rectangle(0, SPEC_Y, SCREEN_W, SPEC_H, RGBA8(14, 16, 22, 255));
    if (!app->show_spectrum || app->wf_nbins <= 0)
        return;

    int n = app->wf_nbins;
    for (int x = 0; x < SCREEN_W; x++) {
        int idx = (int)((long)x * n / SCREEN_W);
        if (idx >= n) idx = n - 1;
        unsigned char v = wf_level(app->wf_bins[idx]); /* floor-subtracted, sensitive */
        int barh = SPEC_H * v / 255;
        vita2d_draw_rectangle(x, SPEC_Y + (SPEC_H - barh), 1, barh, COL_SPEC);
    }
}

static void draw_ruler(app_state *app)
{
    /* Frequency ruler across the span shown by the waterfall zoom. */
    double span_khz = 30000.0 / (double)(1 << app->zoom); /* ~ADC max / 2^zoom */
    double lo = app->freq_khz - span_khz / 2.0;
    int ry = BOTTOM_Y;
    int panel_w = SCREEN_W * 2 / 3;
    const int pad = 6;
    vita2d_draw_rectangle(0, ry, panel_w, BOTTOM_H, RGBA8(18, 20, 26, 255));

    for (int t = 0; t <= 4; t++) {
        double f = lo + span_khz * t / 4.0;
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "%.0f", f);

        /* Centre the label on its tick, but clamp so the first/last labels stay
         * inside the panel instead of clipping into the slider column. */
        int est_w = (int)strlen(lbl) * 8; /* approx width at 0.7 scale */
        int center = panel_w * t / 4;
        int x = center - est_w / 2;
        if (x < pad)
            x = pad;
        if (x + est_w > panel_w - pad)
            x = panel_w - pad - est_w;

        font_drawf(x, ry + 20, COL_DIM, 0.7f, "%s", lbl);
    }
}

/* Status panel in the bottom-left, under the ruler: current server and live
 * Vita hardware stats. */
static void draw_status_panel(app_state *app)
{
    int x = 8;
    int y = BOTTOM_Y + 40;   /* below the ruler tick labels */

    /* --- server line --- */
    const char *st; unsigned int stc;
    switch (app->conn_status) {
    case CONN_CONNECTED:  st = "connected";  stc = COL_GREEN; break;
    case CONN_CONNECTING: st = "connecting"; stc = COL_AMBER; break;
    case CONN_ERROR:      st = "error";      stc = COL_RED;   break;
    default:              st = "off";        stc = COL_DIM;   break;
    }
    font_drawf(x, y, COL_DIM, 0.72f, "SRV");
    font_drawf(x + 34, y, COL_TEXT, 0.72f, "%.30s:%d", app->host, app->port);
    font_drawf(x + 420, y, stc, 0.72f, "%s", st);

    /* --- hardware line: battery, CPU clock, framerate --- */
    static uint64_t last_t = 0; static int fcount = 0, fps = 0;
    fcount++;
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - last_t >= 500000) {
        fps = (int)((uint64_t)fcount * 1000000ull / (now - last_t));
        fcount = 0; last_t = now;
    }
    int bat = scePowerGetBatteryLifePercent();
    int chg = scePowerIsBatteryCharging();
    int cpu = scePowerGetArmClockFrequency();
    unsigned int batc = (bat >= 0 && bat <= 20) ? COL_RED
                      : (bat <= 40) ? COL_AMBER : COL_GREEN;
    y += 18;
    font_drawf(x, y, COL_DIM, 0.72f, "BAT");
    font_drawf(x + 34, y, batc, 0.72f, "%d%%%s", bat >= 0 ? bat : 0,
               chg ? " +" : "");
    font_drawf(x + 150, y, COL_DIM, 0.72f, "CPU");
    font_drawf(x + 190, y, COL_TEXT, 0.72f, "%dMHz", cpu);
    font_drawf(x + 330, y, COL_DIM, 0.72f, "FPS");
    font_drawf(x + 370, y, COL_TEXT, 0.72f, "%d", fps);

    /* --- stream line: buffer depth and underruns (only when connected) --- */
    y += 18;
    if (app->conn_status == CONN_CONNECTED) {
        unsigned jit_ms = (unsigned)(jitter_available(&app->jitter) / 12); /* 12k */
        font_drawf(x, y, COL_DIM, 0.72f, "BUF");
        font_drawf(x + 34, y, COL_TEXT, 0.72f, "%ums", jit_ms);
        font_drawf(x + 150, y, COL_DIM, 0.72f, "UR");
        font_drawf(x + 190, y, COL_TEXT, 0.72f, "%d", app->audio_underruns);
        font_drawf(x + 330, y, COL_DIM, 0.72f, "RX");
        font_drawf(x + 370, y, COL_TEXT, 0.72f, "%ldk",
                   (long)(app->samples_rx / 1000));
    }
}

static void draw_slider(int x, int y, int w, const char *label, int val,
                        unsigned int col)
{
    font_drawf(x, y + 12, COL_DIM, 0.8f, "%s", label);
    int bx = x + 46, bw = w - 46, bh = 12;
    vita2d_draw_rectangle(bx, y, bw, bh, RGBA8(40, 44, 52, 255));
    vita2d_draw_rectangle(bx, y, bw * val / 100, bh, col);
}

static void draw_sliders(app_state *app)
{
    int x = SCREEN_W * 2 / 3 + 10;
    int w = SCREEN_W - x - 10;
    draw_slider(x, BOTTOM_Y + 8, w, "SQL", app->squelch, COL_AMBER);
    draw_slider(x, BOTTOM_Y + 30, w, "VOL", app->volume, COL_ACCENT);
}

/* Passband + carrier overlay: a translucent band showing the demodulator's
 * filter width and a centre line at the tuned frequency, so the user can see
 * where to put a signal and how wide the mode listens. */
static void draw_passband(app_state *app)
{
    double span_khz = 30000.0 / (double)(1 << app->zoom);
    double span_hz = span_khz * 1000.0;
    if (span_hz < 1.0)
        return;
    float pxhz = (float)SCREEN_W / (float)span_hz;
    int cx = SCREEN_W / 2;

    int lo, hi;
    kiwi_default_passband(app->mode, &lo, &hi);
    int x_lo = cx + (int)(lo * pxhz);
    int x_hi = cx + (int)(hi * pxhz);
    if (x_lo > x_hi) { int t = x_lo; x_lo = x_hi; x_hi = t; }
    if (x_lo < 0) x_lo = 0;
    if (x_hi > SCREEN_W) x_hi = SCREEN_W;

    int top = WF_Y;
    int hgt = WF_H + SPEC_H;
    if (x_hi > x_lo)
        vita2d_draw_rectangle(x_lo, top, x_hi - x_lo, hgt,
                              RGBA8(80, 200, 255, 40));   /* passband fill */
    vita2d_draw_rectangle(x_lo, top, 1, hgt, RGBA8(80, 200, 255, 170));
    vita2d_draw_rectangle(x_hi - 1, top, 1, hgt, RGBA8(80, 200, 255, 170));
    vita2d_draw_rectangle(cx, top, 1, hgt, RGBA8(255, 80, 80, 210)); /* carrier */
}

void ui_draw(app_state *app)
{
    font_ensure();

    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    wf_render_draw(0, WF_Y, SCREEN_W, WF_H);
    draw_spectrum(app);
    draw_passband(app);
    draw_ruler(app);
    draw_status_panel(app);
    draw_sliders(app);
    draw_status_bar(app);

    /* Band name (from the band plan) over the top-left of the waterfall. */
    const char *band = band_lookup(app->freq_khz);
    if (band[0])
        font_drawf(8, WF_Y + 20, COL_ACCENT, 0.9f,
                              "%s", band);

    /* Persistent error reason while disconnected in the error state. */
    if (app->conn_status == CONN_ERROR && app->last_err[0]) {
        font_drawf(8, WF_Y + 24, COL_RED, 0.9f,
                              "ERR: %s  (Start to retry)", app->last_err);
    }

    /* Audio-only receiver: connected with audio but the receiver won't give us
     * the waterfall (one-connection-per-IP limit). Say so over the dead area. */
    if (app->conn_status == CONN_CONNECTED && app->wf_stalled) {
        font_drawf(8, WF_Y + WF_H / 2, COL_AMBER, 0.9f,
                              "Audio only - this receiver isn't sending a waterfall");
    }

    if (s_msg_frames > 0) {
        vita2d_draw_rectangle(0, SCREEN_H / 2 - 18, SCREEN_W, 36,
                              RGBA8(0, 0, 0, 200));
        font_drawf(20, SCREEN_H / 2 + 6, COL_TEXT, 1.0f,
                              "%s", s_msg);
        s_msg_frames--;
    }
}
