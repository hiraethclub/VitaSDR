/* Server picker + settings screens, and the on-screen keyboard helper.
 *
 * The picker lists KiwiSDR receivers fetched live from the public directory
 * (http://rx.linkfanel.net/kiwisdr_com.js). Settings exposes QoL options and
 * the author credits.
 */
#include "app.h"
#include "httpget.h"
#include "kiwidir.h"
#include "bandplan.h"
#include "build_info.h"
#include "font.h"

#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>

#include <vita2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIR_HOST "rx.linkfanel.net"
#define DIR_PORT 80
#define DIR_PATH "/kiwisdr_com.js"

#define MAX_SERVERS 400

/* ---- colours (mirror ui.c) ---- */
#define COL_BG     RGBA8(10, 12, 16, 255)
#define COL_BAR    RGBA8(24, 28, 36, 255)
#define COL_TEXT   RGBA8(230, 235, 240, 255)
#define COL_DIM    RGBA8(140, 150, 160, 255)
#define COL_ACCENT RGBA8(80, 200, 255, 255)
#define COL_GREEN  RGBA8(60, 220, 120, 255)
#define COL_AMBER  RGBA8(240, 180, 60, 255)
#define COL_SELBG  RGBA8(40, 70, 90, 255)

static kiwi_server s_srv[MAX_SERVERS];
static int         s_nsrv = 0;
static int         s_settings_cur = 0;
static int         s_band_cur = 0;

/* Favourites: user-curated receivers shown at the top of the picker. Stored in
 * VITASDR_DATA_DIR/favourites.txt, one per line as
 *   host,port,proto,path,name
 * where proto is 0 (KiwiSDR) or 1 (OpenWebRX) and path is the OpenWebRX
 * WebSocket path. Older 3-field lines ("host,port,name") are still read and
 * treated as KiwiSDR. Seeded on first run with a known-good reference so
 * there's always something that connects. */
#define MAX_FAV 32
#define FAV_FILE VITASDR_DATA_DIR "/favourites.txt"
static kiwi_server s_fav[MAX_FAV];
static int         s_nfav = 0;

static void fav_save(void)
{
    FILE *f = fopen(FAV_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_nfav; i++)
        fprintf(f, "%s,%d,%d,%d,%s,%s\n", s_fav[i].host, s_fav[i].port,
                s_fav[i].proto, s_fav[i].tls, s_fav[i].path, s_fav[i].name);
    fclose(f);
}

static void fav_add_full(const char *host, int port, int proto, int tls,
                         const char *path, const char *name)
{
    if (s_nfav >= MAX_FAV || !host[0]) return;
    kiwi_server *s = &s_fav[s_nfav++];
    memset(s, 0, sizeof(*s));
    strncpy(s->host, host, sizeof(s->host) - 1);
    strncpy(s->name, name && name[0] ? name : host, sizeof(s->name) - 1);
    s->port = port > 0 ? port : 8073;
    s->proto = proto;
    s->tls = tls;
    if (path && path[0])
        strncpy(s->path, path, sizeof(s->path) - 1);
    else if (proto == PROTO_OWRX)
        strncpy(s->path, "/ws/", sizeof(s->path) - 1);
    s->snr = -1;
    s->online = 1;
}

/* Is `s` a bare integer field ("0"/"1"...) terminated by a comma or the end of
 * the line? Used to tell the "host,port,proto,tls,path,name" format from a
 * legacy "host,port,name" line (a name is never a bare integer field). */
static int is_int_field(const char *s)
{
    if (*s < '0' || *s > '9') return 0;
    while (*s >= '0' && *s <= '9') s++;
    return (*s == ',' || *s == '\0');
}

static void fav_load(void)
{
    s_nfav = 0;
    FILE *f = fopen(FAV_FILE, "r");
    if (!f) {
        /* First run: seed with a reliable reference. */
        fav_add_full("gw0kax.proxy.kiwisdr.com", 8073, PROTO_KIWI, 0, "",
                     "gw0kax (reference)");
        fav_save();
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && s_nfav < MAX_FAV) {
        char *nl = line; while (*nl && *nl != '\n' && *nl != '\r') nl++; *nl = '\0';
        if (!line[0]) continue;
        char *c1 = strchr(line, ',');
        if (!c1) continue;
        *c1 = '\0';
        char *c2 = strchr(c1 + 1, ',');
        if (!c2) continue;
        *c2 = '\0';
        const char *host = line;
        int port = atoi(c1 + 1);
        char *rest = c2 + 1;   /* "proto,tls,path,name" or a legacy bare name */
        if (!is_int_field(rest)) {
            fav_add_full(host, port, PROTO_KIWI, 0, "", rest); /* legacy 3-field */
            continue;
        }
        int proto = atoi(rest);
        char *c3 = strchr(rest, ',');
        if (!c3) { fav_add_full(host, port, proto, 0, "", host); continue; }
        char *tlsf = c3 + 1;
        int tls = 0;
        char *pathf;
        if (is_int_field(tlsf)) {
            tls = atoi(tlsf);
            char *c4 = strchr(tlsf, ',');
            pathf = c4 ? c4 + 1 : NULL;
        } else {
            pathf = tlsf;      /* tolerate an interim line without the tls field */
        }
        if (!pathf) { fav_add_full(host, port, proto, tls, "", host); continue; }
        char *c5 = strchr(pathf, ',');
        if (!c5) { fav_add_full(host, port, proto, tls, pathf, host); continue; }
        *c5 = '\0';
        fav_add_full(host, port, proto, tls, pathf, c5 + 1);
    }
    fclose(f);
}

static int fav_find(const char *host, int port)
{
    for (int i = 0; i < s_nfav; i++)
        if (s_fav[i].port == port && strcmp(s_fav[i].host, host) == 0)
            return i;
    return -1;
}

static void fav_toggle(const kiwi_server *s)
{
    if (!s) return;
    int idx = fav_find(s->host, s->port);
    if (idx >= 0) {
        for (int i = idx; i < s_nfav - 1; i++) s_fav[i] = s_fav[i + 1];
        s_nfav--;
    } else {
        fav_add_full(s->host, s->port, s->proto, s->tls, s->path, s->name);
    }
    fav_save();
}

static const int   STEPS[] = { 1, 10, 100, 1000, 5000, 10000, 100000 };
static const int   NSTEPS = (int)(sizeof(STEPS) / sizeof(STEPS[0]));

/* ================= directory fetch (called on the net thread) ================= */

static kiwidir_parser s_parser;

static int dir_sink(const char *data, size_t len, void *user)
{
    (void)user;
    kiwidir_feed(&s_parser, data, len);
    return 0;
}

static int cmp_srv(const void *a, const void *b)
{
    const kiwi_server *x = a, *y = b;
    if (x->online != y->online) return y->online - x->online;   /* online first */
    /* Connectable (has a free channel) before full ones: a full KiwiSDR
     * completes the handshake but never streams, so surfacing full receivers
     * first just leads users to dead connections. */
    int xf = (x->users_max > 0 && x->users < x->users_max) ? 1 : 0;
    int yf = (y->users_max > 0 && y->users < y->users_max) ? 1 : 0;
    if (xf != yf) return yf - xf;
    if (x->snr != y->snr) return y->snr - x->snr;               /* best SNR first */
    return 0;
}

int servers_fetch(void)
{
    s_nsrv = 0;   /* hide the (possibly stale) list while (re)fetching */
    kiwidir_init(&s_parser, s_srv, MAX_SERVERS);
    int rc = http_get(DIR_HOST, DIR_PORT, DIR_PATH, 15000, dir_sink, NULL);
    int n = kiwidir_count(&s_parser);
    if (rc != HTTP_OK && n == 0)
        return rc;   /* negative; favourites are still shown */
    qsort(s_srv, (size_t)n, sizeof(s_srv[0]), cmp_srv);
    s_nsrv = n;
    return n;
}

int servers_count(void) { return s_nsrv; }

/* The picker shows favourites first, then the fetched directory. These map a
 * combined row index onto the right list. */
static int picker_total(void) { return s_nfav + s_nsrv; }
static const kiwi_server *picker_at(int i)
{
    if (i < 0) return NULL;
    if (i < s_nfav) return &s_fav[i];
    i -= s_nfav;
    if (i < s_nsrv) return &s_srv[i];
    return NULL;
}
const kiwi_server *servers_at(int i) { return picker_at(i); }

void menu_init(void)
{
    s_nsrv = 0;
    s_settings_cur = 0;
    fav_load();
}

/* ================= on-screen keyboard (IME) ================= */

static void ascii_to_u16(const char *s, uint16_t *out, int cap)
{
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++)
        out[i] = (uint16_t)(unsigned char)s[i];
    out[i] = 0;
}
static void u16_to_ascii(const uint16_t *s, char *out, int cap)
{
    int i = 0;
    for (; s[i] && i < cap - 1; i++)
        out[i] = (s[i] < 128) ? (char)s[i] : '?';
    out[i] = '\0';
}

/* Blocking on-screen text entry. Returns a static UTF-8 buffer, or NULL if the
 * user cancelled. Drives its own render frames while the dialog is up. */
const char *ime_get_text(const char *title, const char *initial)
{
    static char result[256];
    static uint16_t inbuf[256];
    uint16_t wtitle[64], winit[128];
    ascii_to_u16(title, wtitle, 64);
    ascii_to_u16(initial ? initial : "", winit, 128);

    SceImeDialogParam p;
    sceImeDialogParamInit(&p);
    p.supportedLanguages = 0;
    p.languagesForced = SCE_TRUE;
    p.type = SCE_IME_TYPE_DEFAULT;
    /* Don't auto-capitalise: it mangles hostnames and URLs (e.g. turns
     * "https://..." into "Https://..."). */
    p.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    p.title = wtitle;
    p.maxTextLength = 120;
    p.initialText = winit;
    p.inputTextBuffer = inbuf;
    if (sceImeDialogInit(&p) < 0)
        return NULL;

    for (;;) {
        SceCommonDialogStatus st = sceImeDialogGetStatus();
        if (st == SCE_COMMON_DIALOG_STATUS_FINISHED)
            break;
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_end_drawing();
        /* Composite the IME dialog over our (cleared) frame. Without this the
         * dialog never draws and its status never advances: a black screen. */
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }

    SceImeDialogResult res;
    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER)
        return NULL;
    u16_to_ascii(inbuf, result, sizeof(result));
    return result;
}

/* ================= server picker ================= */

#define ROW_H   26
#define LIST_Y  70
#define VIS_ROWS 16

static void draw_picker(app_state *app)
{
    font_ensure();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_BAR);
    font_drawf(12, 28, COL_TEXT, 1.2f, "Select a KiwiSDR");

    int status = app->dir_status;
    if (status == DIR_FETCHING) {
        font_drawf(300, 28, COL_AMBER, 1.0f,
                              "fetching directory...");
    } else if (status == DIR_ERROR) {
        font_drawf(300, 28, COL_AMBER, 1.0f,
                              "fetch failed - press [] to retry");
    } else {
        font_drawf(300, 28, COL_DIM, 0.9f,
                              "%d receivers", s_nsrv);
    }

    /* Combined list: favourites first, then the directory (hidden while a
     * refresh is in flight so a half-parsed list isn't shown). */
    int dir_n = (status == DIR_FETCHING) ? 0 : s_nsrv;
    int n = s_nfav + dir_n;
    if (n > 0) {
        int top = app->sel - VIS_ROWS / 2;
        if (top < 0) top = 0;
        if (top > n - VIS_ROWS) top = n - VIS_ROWS;
        if (top < 0) top = 0;

        for (int r = 0; r < VIS_ROWS && top + r < n; r++) {
            int idx = top + r;
            int is_fav = (idx < s_nfav);
            const kiwi_server *s = is_fav ? &s_fav[idx] : &s_srv[idx - s_nfav];
            int y = LIST_Y + r * ROW_H;
            if (idx == app->sel)
                vita2d_draw_rectangle(0, y - 18, SCREEN_W, ROW_H, COL_SELBG);

            if (is_fav)
                font_drawf(12, y, COL_AMBER, 0.9f, "*");
            unsigned int name_col = (idx == app->sel) ? COL_TEXT : COL_DIM;
            font_drawf(28, y, name_col, 0.9f,
                                  "%.38s", s->name[0] ? s->name : s->host);
            font_drawf(560, y, COL_DIM, 0.8f,
                                  "%.22s", s->loc);
            if (!is_fav) {
                unsigned int ucol = (s->users >= s->users_max && s->users_max > 0)
                                    ? COL_AMBER : COL_GREEN;
                font_drawf(770, y, ucol, 0.8f,
                                      "%d/%d", s->users, s->users_max);
                if (s->snr >= 0)
                    font_drawf(850, y, COL_DIM, 0.8f,
                                          "snr%d", s->snr);
            }
        }
    } else if (status != DIR_FETCHING) {
        font_drawf(12, LIST_Y + 20, COL_DIM, 1.0f,
                              "No receivers. Press [] to fetch the directory.");
    }

    vita2d_draw_rectangle(0, SCREEN_H - 28, SCREEN_W, 28, COL_BAR);
    font_drawf(12, SCREEN_H - 9, COL_DIM, 0.72f,
        "X connect  START fav(*)  SELECT add  [] refresh  /\\ settings  O radio");
}

/* ================= settings ================= */

enum {
    SET_SERVERLIST = 0,
    SET_PALETTE,
    SET_WFSPEED,
    SET_SPECTRUM,
    SET_KEEPAWAKE,
    SET_AUDIOBW,
    SET_VOLUME,
    SET_SQUELCH,
    SET_AUTOCONNECT,
    SET_AUTORECONNECT,
    SET_TLSVERIFY,
    SET_STEP,
    SET_ZOOM,
    SET_FREQENTRY,
    SET_COUNT
};

static const char *PALETTE_NAMES[] = { "Viridis", "Greyscale", "Hot", "Classic" };
static const char *BW_NAMES[]      = { "Narrow (3k)", "Normal (4.5k)", "Wide (6k)" };
static const char *ONOFF[]         = { "Off", "On" };

static void step_label(int hz, char *out, size_t n)
{
    if (hz >= 1000) snprintf(out, n, "%d kHz", hz / 1000);
    else            snprintf(out, n, "%d Hz", hz);
}

static void setting_value(app_state *app, int item, char *out, size_t n)
{
    char tmp[24];
    switch (item) {
    case SET_SERVERLIST:  snprintf(out, n, "%d receivers >", s_nsrv); break;
    case SET_PALETTE:     snprintf(out, n, "%s", PALETTE_NAMES[app->palette & 3]); break;
    case SET_WFSPEED:     snprintf(out, n, "%d", app->wf_speed); break;
    case SET_SPECTRUM:    snprintf(out, n, "%s", ONOFF[app->show_spectrum ? 1 : 0]); break;
    case SET_KEEPAWAKE:   snprintf(out, n, "%s", ONOFF[app->keep_awake ? 1 : 0]); break;
    case SET_AUDIOBW:     snprintf(out, n, "%s", BW_NAMES[app->audio_bw % 3]); break;
    case SET_VOLUME:      snprintf(out, n, "%d", app->volume); break;
    case SET_SQUELCH:     snprintf(out, n, "%d", app->squelch); break;
    case SET_AUTOCONNECT: snprintf(out, n, "%s", ONOFF[app->auto_connect ? 1 : 0]); break;
    case SET_AUTORECONNECT: snprintf(out, n, "%s", ONOFF[app->auto_reconnect ? 1 : 0]); break;
    case SET_TLSVERIFY:   snprintf(out, n, "%s", ONOFF[app->tls_verify ? 1 : 0]); break;
    case SET_STEP:        step_label(app->step_hz, tmp, sizeof(tmp));
                          snprintf(out, n, "%s", tmp); break;
    case SET_ZOOM:        snprintf(out, n, "%d", app->zoom); break;
    case SET_FREQENTRY:   snprintf(out, n, "enter... >"); break;
    default:              out[0] = '\0'; break;
    }
}

static const char *SET_LABELS[SET_COUNT] = {
    "Server list",
    "Waterfall palette",
    "Waterfall speed",
    "Spectrum overlay",
    "Keep screen awake",
    "Audio bandwidth",
    "Volume",
    "Squelch",
    "Auto-connect on launch",
    "Auto-reconnect on drop",
    "Verify TLS certificate",
    "Tuning step",
    "Waterfall zoom",
    "Direct frequency entry",
};

static void draw_settings(app_state *app)
{
    font_ensure();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_BAR);
    font_drawf(12, 28, COL_TEXT, 1.2f, "Settings");

    for (int i = 0; i < SET_COUNT; i++) {
        int y = 68 + i * 26;
        if (i == s_settings_cur)
            vita2d_draw_rectangle(0, y - 18, SCREEN_W, 26, COL_SELBG);
        unsigned int col = (i == s_settings_cur) ? COL_TEXT : COL_DIM;
        font_drawf(20, y, col, 0.95f, "%s", SET_LABELS[i]);
        char val[40];
        setting_value(app, i, val, sizeof(val));
        font_drawf(520, y, COL_ACCENT, 0.95f, "%s", val);
    }

    /* Author credits + version. */
    int cy = SCREEN_H - 92;
    vita2d_draw_rectangle(0, cy - 6, SCREEN_W, 92, COL_BAR);
    font_drawf(20, cy + 16, COL_TEXT, 0.9f,
                          "%s (r%s)  -  by Aisling de Gr\xC3\xA1s",
                          VITASDR_APP_LABEL, VITASDR_BUILD_REV);
    font_drawf(20, cy + 40, COL_DIM, 0.85f,
                          "aisling@hiraeth.club");
    font_drawf(20, cy + 62, COL_DIM, 0.85f,
                          "Threads: @hiraeth.clwb");
    font_drawf(560, cy + 62, COL_DIM, 0.8f,
        "Up/Down move   L/R change   X select   /\\ or O back");
}

/* ================= public draw / input ================= */

/* ================= band selector ================= */

static void draw_bands(app_state *app)
{
    (void)app;
    font_ensure();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_BAR);
    font_drawf(12, 28, COL_TEXT, 1.2f, "Jump to band");

    int nb = bandplan_count();
    int top = s_band_cur - VIS_ROWS / 2;
    if (top < 0) top = 0;
    if (top > nb - VIS_ROWS) top = nb - VIS_ROWS;
    if (top < 0) top = 0;

    for (int r = 0; r < VIS_ROWS && top + r < nb; r++) {
        int idx = top + r;
        const char *name, *mode; double f;
        bandplan_get(idx, &name, &f, &mode);
        int y = LIST_Y + r * ROW_H;
        if (idx == s_band_cur)
            vita2d_draw_rectangle(0, y - 18, SCREEN_W, ROW_H, COL_SELBG);
        unsigned int col = (idx == s_band_cur) ? COL_TEXT : COL_DIM;
        font_drawf(20, y, col, 0.95f, "%s", name);
        /* mode in upper case */
        char mu[8]; size_t i = 0;
        for (; i < sizeof(mu) - 1 && mode[i]; i++)
            mu[i] = (mode[i] >= 'a' && mode[i] <= 'z') ? (char)(mode[i] - 32) : mode[i];
        mu[i] = '\0';
        font_drawf(420, y, COL_ACCENT, 0.9f, "%.0f kHz", f);
        font_drawf(600, y, COL_DIM, 0.9f, "%s", mu);
    }

    vita2d_draw_rectangle(0, SCREEN_H - 28, SCREEN_W, 28, COL_BAR);
    font_drawf(12, SCREEN_H - 9, COL_DIM, 0.8f,
        "Up/Down select   X jump to band   O / Triangle back");
}

void menu_draw(app_state *app)
{
    if (app->screen == SCREEN_SETTINGS)   draw_settings(app);
    else if (app->screen == SCREEN_BANDS) draw_bands(app);
    else                                  draw_picker(app);
}

static void adjust(int *v, int delta, int lo, int hi)
{
    *v += delta;
    if (*v < lo) *v = lo;
    if (*v > hi) *v = hi;
}

static void settings_change(app_state *app, int item, int dir)
{
    switch (item) {
    case SET_PALETTE:       app->palette = (app->palette + dir + 4) & 3; break;
    case SET_WFSPEED:       adjust(&app->wf_speed, dir, 1, 4); break;
    case SET_SPECTRUM:      app->show_spectrum = !app->show_spectrum; break;
    case SET_KEEPAWAKE:     app->keep_awake = !app->keep_awake; break;
    case SET_AUDIOBW:       app->audio_bw = (app->audio_bw + dir + 3) % 3; break;
    case SET_VOLUME:        adjust(&app->volume, dir * 5, 0, 100); break;
    case SET_SQUELCH:       adjust(&app->squelch, dir * 5, 0, 100); break;
    case SET_AUTOCONNECT:   app->auto_connect = !app->auto_connect; break;
    case SET_AUTORECONNECT: app->auto_reconnect = !app->auto_reconnect; break;
    case SET_TLSVERIFY:     app->tls_verify = !app->tls_verify; break;
    case SET_STEP: {
        int idx = 2;
        for (int i = 0; i < NSTEPS; i++) if (STEPS[i] == app->step_hz) idx = i;
        idx = (idx + dir + NSTEPS) % NSTEPS;
        app->step_hz = STEPS[idx];
        break;
    }
    case SET_ZOOM:          adjust(&app->zoom, dir, 0, 14); break;
    default: break;
    }
}

static void settings_activate(app_state *app, int item)
{
    if (item == SET_SERVERLIST) {
        app->screen = SCREEN_SERVERS;
        if (s_nsrv == 0 && app->dir_status != DIR_FETCHING)
            app->cmd_fetch_dir = 1;
    } else if (item == SET_FREQENTRY) {
        char init[24];
        snprintf(init, sizeof(init), "%.3f", app->freq_khz);
        const char *txt = ime_get_text("Frequency (kHz)", init);
        if (txt && txt[0]) {
            double f = atof(txt);
            if (f > 0.0 && f < 30000.0) {
                sceKernelLockMutex(app->lock, 1, NULL);
                app->freq_khz = f;
                sceKernelUnlockMutex(app->lock, 1);
                ui_show_message(app, "frequency set");
            }
        }
    }
}

/* Connect to the currently-selected receiver. */
static void picker_connect(app_state *app)
{
    const kiwi_server *s = servers_at(app->sel);
    if (!s) return;
    /* If already connected, tear down first so the net thread reconnects. */
    if (app->conn_status == CONN_CONNECTED || app->conn_status == CONN_CONNECTING)
        app->cmd_disconnect = 1;
    strncpy(app->host, s->host, sizeof(app->host) - 1);
    app->host[sizeof(app->host) - 1] = '\0';
    app->port = s->port;
    app->password[0] = '\0';
    app->proto = s->proto;
    app->tls = s->tls;
    app->path[0] = '\0';
    if (s->proto == PROTO_OWRX)
        snprintf(app->path, sizeof(app->path), "%s",
                 s->path[0] ? s->path : "/ws/");
    app->screen = SCREEN_RADIO;
    app->cmd_connect = 1;
    ui_show_message(app, s->name[0] ? s->name : s->host);
    config_save(app);   /* persist chosen host (no in-app quit saves on exit) */
}

/* If `in` carries an http/https/ws/wss scheme, parse it as an OpenWebRX URL
 * (host, port, and the /ws/ endpoint) and return 1. A bare host returns 0.
 * The OpenWebRX WebSocket endpoint is always /ws/ regardless of the page URL. */
/* Case-insensitive prefix test (pfx must be lowercase). Returns the matched
 * length, or 0 if `s` does not start with `pfx`. */
static int ci_prefix(const char *s, const char *pfx)
{
    int i = 0;
    for (; pfx[i]; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != pfx[i]) return 0;
    }
    return i;
}

static int parse_owrx_url(const char *in, char *host, size_t hcap,
                          int *port, int *out_tls, char *path, size_t pcap)
{
    const char *p = in;
    int tls = 0, n;
    /* Scheme match is case-insensitive: the IME may capitalise the input. */
    if      ((n = ci_prefix(p, "https://"))) { p += n; tls = 1; }
    else if ((n = ci_prefix(p, "wss://")))   { p += n; tls = 1; }
    else if ((n = ci_prefix(p, "http://")))  { p += n; tls = 0; }
    else if ((n = ci_prefix(p, "ws://")))    { p += n; tls = 0; }
    else                                     { return 0; }

    char hb[128];
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && i + 1 < sizeof(hb)) hb[i++] = *p++;
    hb[i] = '\0';
    int pr = tls ? 443 : 80;
    if (*p == ':') {
        p++;
        pr = atoi(p);
    }
    snprintf(host, hcap, "%s", hb);
    *port = pr > 0 ? pr : (tls ? 443 : 80);
    *out_tls = tls;
    snprintf(path, pcap, "%s", "/ws/");
    return 1;
}

/* Prompt for a receiver and add it to favourites (for receivers not in the
 * public directory, e.g. your own). A bare hostname is treated as a KiwiSDR
 * (with a port prompt); an http(s)://host URL is treated as OpenWebRX. */
static void picker_add_manual(app_state *app)
{
    const char *in = ime_get_text(
        "Receiver host, or http(s):// URL for OpenWebRX", "");
    if (!in || !in[0]) return;

    char host[128], path[64];
    int port = 0, proto = PROTO_KIWI, tls = 0;
    path[0] = '\0';
    if (parse_owrx_url(in, host, sizeof(host), &port, &tls, path, sizeof(path))) {
        proto = PROTO_OWRX;
    } else {
        snprintf(host, sizeof(host), "%s", in);
        const char *ports = ime_get_text("Port", "8073");
        port = ports ? atoi(ports) : 8073;
        if (port <= 0) port = 8073;
    }
    fav_add_full(host, port, proto, tls, path, host);
    fav_save();
    app->sel = 0;   /* new favourite lands at the top */
    ui_show_message(app, proto == PROTO_OWRX ? "added OpenWebRX favourite"
                                             : "added to favourites");
}

void menu_handle(app_state *app, unsigned int pressed)
{
    if (app->screen == SCREEN_SERVERS) {
        int n = picker_total();
        if ((pressed & SCE_CTRL_UP) && n > 0)    adjust(&app->sel, -1, 0, n - 1);
        if ((pressed & SCE_CTRL_DOWN) && n > 0)  adjust(&app->sel, +1, 0, n - 1);
        if ((pressed & SCE_CTRL_LEFT) && n > 0)  adjust(&app->sel, -5, 0, n - 1);
        if ((pressed & SCE_CTRL_RIGHT) && n > 0) adjust(&app->sel, +5, 0, n - 1);
        if (pressed & SCE_CTRL_CROSS)            picker_connect(app);
        if (pressed & SCE_CTRL_START) {          /* toggle favourite on selection */
            const kiwi_server *s = picker_at(app->sel);
            if (s) { fav_toggle(s); if (app->sel >= picker_total()) app->sel = picker_total() - 1; }
        }
        if (pressed & SCE_CTRL_SELECT)           picker_add_manual(app);
        if (pressed & SCE_CTRL_SQUARE) {
            if (app->dir_status != DIR_FETCHING) app->cmd_fetch_dir = 1;
        }
        if (pressed & SCE_CTRL_TRIANGLE)         app->screen = SCREEN_SETTINGS;
        if (pressed & SCE_CTRL_CIRCLE) {
            if (app->conn_status == CONN_CONNECTED) app->screen = SCREEN_RADIO;
        }
        return;
    }

    if (app->screen == SCREEN_BANDS) {
        int nb = bandplan_count();
        if ((pressed & SCE_CTRL_UP) && nb > 0)    adjust(&s_band_cur, -1, 0, nb - 1);
        if ((pressed & SCE_CTRL_DOWN) && nb > 0)  adjust(&s_band_cur, +1, 0, nb - 1);
        if ((pressed & SCE_CTRL_LEFT) && nb > 0)  adjust(&s_band_cur, -5, 0, nb - 1);
        if ((pressed & SCE_CTRL_RIGHT) && nb > 0) adjust(&s_band_cur, +5, 0, nb - 1);
        if (pressed & SCE_CTRL_CROSS) {
            double f; const char *mode;
            bandplan_get(s_band_cur, NULL, &f, &mode);
            sceKernelLockMutex(app->lock, 1, NULL);
            app->freq_khz = f;
            strncpy(app->mode, mode, sizeof(app->mode) - 1);
            app->mode[sizeof(app->mode) - 1] = '\0';
            sceKernelUnlockMutex(app->lock, 1);
            app->screen = SCREEN_RADIO;
            ui_show_message(app, "tuned to band");
        }
        if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE))
            app->screen = SCREEN_RADIO;
        return;
    }

    /* SCREEN_SETTINGS */
    if (pressed & SCE_CTRL_UP)    adjust(&s_settings_cur, -1, 0, SET_COUNT - 1);
    if (pressed & SCE_CTRL_DOWN)  adjust(&s_settings_cur, +1, 0, SET_COUNT - 1);
    if (pressed & SCE_CTRL_LEFT)  settings_change(app, s_settings_cur, -1);
    if (pressed & SCE_CTRL_RIGHT) settings_change(app, s_settings_cur, +1);
    if (pressed & SCE_CTRL_CROSS) settings_activate(app, s_settings_cur);
    if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE)) {
        config_save(app);   /* persist QoL changes on leaving settings */
        app->screen = (app->conn_status == CONN_CONNECTED) ? SCREEN_RADIO
                                                           : SCREEN_SERVERS;
    }
}
