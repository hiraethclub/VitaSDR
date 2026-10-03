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

#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>
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

static vita2d_pgf *s_font = NULL;
static kiwi_server s_srv[MAX_SERVERS];
static int         s_nsrv = 0;
static int         s_settings_cur = 0;
static int         s_band_cur = 0;

/* Favourites: user-curated receivers shown at the top of the picker. Stored in
 * VITASDR_DATA_DIR/favourites.txt as "host,port,name" per line. Seeded on first
 * run with a known-good reference so there's always something that connects. */
#define MAX_FAV 32
#define FAV_FILE VITASDR_DATA_DIR "/favourites.txt"
static kiwi_server s_fav[MAX_FAV];
static int         s_nfav = 0;

static void fav_save(void)
{
    FILE *f = fopen(FAV_FILE, "w");
    if (!f) return;
    for (int i = 0; i < s_nfav; i++)
        fprintf(f, "%s,%d,%s\n", s_fav[i].host, s_fav[i].port, s_fav[i].name);
    fclose(f);
}

static void fav_add_fields(const char *host, int port, const char *name)
{
    if (s_nfav >= MAX_FAV || !host[0]) return;
    kiwi_server *s = &s_fav[s_nfav++];
    memset(s, 0, sizeof(*s));
    strncpy(s->host, host, sizeof(s->host) - 1);
    strncpy(s->name, name && name[0] ? name : host, sizeof(s->name) - 1);
    s->port = port > 0 ? port : 8073;
    s->snr = -1;
    s->online = 1;
}

static void fav_load(void)
{
    s_nfav = 0;
    FILE *f = fopen(FAV_FILE, "r");
    if (!f) {
        /* First run: seed with a reliable reference. */
        fav_add_fields("gw0kax.proxy.kiwisdr.com", 8073, "gw0kax (reference)");
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
        fav_add_fields(line, atoi(c1 + 1), c2 + 1);
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
        fav_add_fields(s->host, s->port, s->name);
    }
    fav_save();
}

static const int   STEPS[] = { 1, 10, 100, 1000, 5000, 10000, 100000 };
static const int   NSTEPS = (int)(sizeof(STEPS) / sizeof(STEPS[0]));

static void ensure_font(void)
{
    if (!s_font)
        s_font = vita2d_load_default_pgf();
}

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
        vita2d_swap_buffers();
        sceKernelDelayThread(2000);
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
    ensure_font();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_BAR);
    vita2d_pgf_draw_textf(s_font, 12, 28, COL_TEXT, 1.2f, "Select a KiwiSDR");

    int status = app->dir_status;
    if (status == DIR_FETCHING) {
        vita2d_pgf_draw_textf(s_font, 300, 28, COL_AMBER, 1.0f,
                              "fetching directory...");
    } else if (status == DIR_ERROR) {
        vita2d_pgf_draw_textf(s_font, 300, 28, COL_AMBER, 1.0f,
                              "fetch failed - press [] to retry");
    } else {
        vita2d_pgf_draw_textf(s_font, 300, 28, COL_DIM, 0.9f,
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
                vita2d_pgf_draw_textf(s_font, 12, y, COL_AMBER, 0.9f, "*");
            unsigned int name_col = (idx == app->sel) ? COL_TEXT : COL_DIM;
            vita2d_pgf_draw_textf(s_font, 28, y, name_col, 0.9f,
                                  "%.38s", s->name[0] ? s->name : s->host);
            vita2d_pgf_draw_textf(s_font, 560, y, COL_DIM, 0.8f,
                                  "%.22s", s->loc);
            if (!is_fav) {
                unsigned int ucol = (s->users >= s->users_max && s->users_max > 0)
                                    ? COL_AMBER : COL_GREEN;
                vita2d_pgf_draw_textf(s_font, 770, y, ucol, 0.8f,
                                      "%d/%d", s->users, s->users_max);
                if (s->snr >= 0)
                    vita2d_pgf_draw_textf(s_font, 850, y, COL_DIM, 0.8f,
                                          "snr%d", s->snr);
            }
        }
    } else if (status != DIR_FETCHING) {
        vita2d_pgf_draw_textf(s_font, 12, LIST_Y + 20, COL_DIM, 1.0f,
                              "No receivers. Press [] to fetch the directory.");
    }

    vita2d_draw_rectangle(0, SCREEN_H - 28, SCREEN_W, 28, COL_BAR);
    vita2d_pgf_draw_textf(s_font, 12, SCREEN_H - 9, COL_DIM, 0.72f,
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
    "Tuning step",
    "Waterfall zoom",
    "Direct frequency entry",
};

static void draw_settings(app_state *app)
{
    ensure_font();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_BAR);
    vita2d_pgf_draw_textf(s_font, 12, 28, COL_TEXT, 1.2f, "Settings");

    for (int i = 0; i < SET_COUNT; i++) {
        int y = 68 + i * 26;
        if (i == s_settings_cur)
            vita2d_draw_rectangle(0, y - 18, SCREEN_W, 26, COL_SELBG);
        unsigned int col = (i == s_settings_cur) ? COL_TEXT : COL_DIM;
        vita2d_pgf_draw_textf(s_font, 20, y, col, 0.95f, "%s", SET_LABELS[i]);
        char val[40];
        setting_value(app, i, val, sizeof(val));
        vita2d_pgf_draw_textf(s_font, 520, y, COL_ACCENT, 0.95f, "%s", val);
    }

    /* Author credits + version. */
    int cy = SCREEN_H - 92;
    vita2d_draw_rectangle(0, cy - 6, SCREEN_W, 92, COL_BAR);
    vita2d_pgf_draw_textf(s_font, 20, cy + 16, COL_TEXT, 0.9f,
                          "%s (r%s)  -  by Aisling de Gr\xC3\xA1s",
                          VITASDR_APP_LABEL, VITASDR_BUILD_REV);
    vita2d_pgf_draw_textf(s_font, 20, cy + 40, COL_DIM, 0.85f,
                          "aisling@hiraeth.club");
    vita2d_pgf_draw_textf(s_font, 20, cy + 62, COL_DIM, 0.85f,
                          "Threads: @hiraeth.clwb");
    vita2d_pgf_draw_textf(s_font, 560, cy + 62, COL_DIM, 0.8f,
        "Up/Down move   L/R change   X select   /\\ or O back");
}

/* ================= public draw / input ================= */

/* ================= band selector ================= */

static void draw_bands(app_state *app)
{
    (void)app;
    ensure_font();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_BAR);
    vita2d_pgf_draw_textf(s_font, 12, 28, COL_TEXT, 1.2f, "Jump to band");

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
        vita2d_pgf_draw_textf(s_font, 20, y, col, 0.95f, "%s", name);
        /* mode in upper case */
        char mu[8]; size_t i = 0;
        for (; i < sizeof(mu) - 1 && mode[i]; i++)
            mu[i] = (mode[i] >= 'a' && mode[i] <= 'z') ? (char)(mode[i] - 32) : mode[i];
        mu[i] = '\0';
        vita2d_pgf_draw_textf(s_font, 420, y, COL_ACCENT, 0.9f, "%.0f kHz", f);
        vita2d_pgf_draw_textf(s_font, 600, y, COL_DIM, 0.9f, "%s", mu);
    }

    vita2d_draw_rectangle(0, SCREEN_H - 28, SCREEN_W, 28, COL_BAR);
    vita2d_pgf_draw_textf(s_font, 12, SCREEN_H - 9, COL_DIM, 0.8f,
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
    app->screen = SCREEN_RADIO;
    app->cmd_connect = 1;
    ui_show_message(app, s->name[0] ? s->name : s->host);
    config_save(app);   /* persist chosen host (no in-app quit saves on exit) */
}

/* Prompt for a host and port and add it to favourites (for receivers not in the
 * public directory, e.g. your own). */
static void picker_add_manual(app_state *app)
{
    const char *host = ime_get_text("Receiver host (e.g. my.sdr.net)", "");
    if (!host || !host[0]) return;
    char hostbuf[96];
    strncpy(hostbuf, host, sizeof(hostbuf) - 1);
    hostbuf[sizeof(hostbuf) - 1] = '\0';
    const char *ports = ime_get_text("Port", "8073");
    int port = ports ? atoi(ports) : 8073;
    if (port <= 0) port = 8073;
    fav_add_fields(hostbuf, port, hostbuf);
    fav_save();
    app->sel = 0;   /* new favourite lands at the top */
    ui_show_message(app, "added to favourites");
}

void menu_handle(app_state *app, unsigned int pressed)
{
    if (app->screen == SCREEN_SERVERS) {
        int n = picker_total();
        if ((pressed & SCE_CTRL_UP) && n > 0)    adjust(&app->sel, -1, 0, n - 1);
        if ((pressed & SCE_CTRL_DOWN) && n > 0)  adjust(&app->sel, +1, 0, n - 1);
        if ((pressed & SCE_CTRL_LEFT) && n > 0)  adjust(&app->sel, -10, 0, n - 1);
        if ((pressed & SCE_CTRL_RIGHT) && n > 0) adjust(&app->sel, +10, 0, n - 1);
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
        if ((pressed & SCE_CTRL_UP) && nb > 0)   adjust(&s_band_cur, -1, 0, nb - 1);
        if ((pressed & SCE_CTRL_DOWN) && nb > 0) adjust(&s_band_cur, +1, 0, nb - 1);
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
