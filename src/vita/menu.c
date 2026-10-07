/* Server picker + settings screens, and the on-screen keyboard helper.
 *
 * The picker lists KiwiSDR receivers fetched live from the public directory
 * (http://rx.linkfanel.net/kiwisdr_com.js). Settings exposes QoL options and
 * the author credits.
 */
#include "app.h"
#include "httpget.h"
#include "kiwidir.h"
#include "geo.h"
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
static int         s_filter_cur = 0;

/* Filtered/sorted view over the directory (s_srv). Favourites are never
 * filtered; only the directory list is. s_view holds indices into s_srv that
 * pass the current filters, in display order. It is rebuilt lazily by
 * ensure_view() when the directory is refetched (s_srv_gen changes) or a filter
 * is edited (s_view_dirty). s_view_dist mirrors the great-circle distance (km)
 * from the home point for each visible row, for display; <0 if unknown. */
static int    s_view[MAX_SERVERS];
static float  s_view_dist[MAX_SERVERS];
static int    s_nview = 0;
static int    s_srv_gen = 0;    /* bumped each successful (re)fetch */
static int    s_view_gen = -1;  /* s_srv_gen the current view was built from */
static int    s_view_dirty = 1; /* a filter changed; rebuild needed */

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
    s_srv_gen++;   /* invalidate the filtered view */
    return n;
}

int servers_count(void) { return s_nsrv; }

/* ================= directory filtering + ranking ================= */

/* Case-insensitive substring test. */
static int ci_contains(const char *hay, const char *needle)
{
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

/* Great-circle distance (km) from the home point to a receiver, or -1 if we
 * can't tell (no home set, or the receiver has no position). */
static float srv_dist_km(const app_state *app, const kiwi_server *s)
{
    if (!app->home_set || !s->has_gps) return -1.0f;
    return (float)geo_haversine_km(app->home_lat, app->home_lon, s->lat, s->lon);
}

/* Does a directory receiver pass the active filters? `dist` is its home
 * distance (km, <0 if unknown), computed once by the caller. */
static int filter_pass(const app_state *app, const kiwi_server *s, float dist)
{
    if (app->flt_free2) {
        int freeslots = (s->users_max > 0) ? (s->users_max - s->users) : 0;
        if (freeslots < 2) return 0;   /* we open audio + waterfall = 2 */
    }
    if (app->flt_min_snr > 0 && (s->snr < 0 || s->snr < app->flt_min_snr))
        return 0;
    if (app->flt_loc[0] &&
        !ci_contains(s->loc, app->flt_loc) && !ci_contains(s->name, app->flt_loc))
        return 0;
    if (app->flt_dist_km > 0 && app->home_set) {
        if (dist < 0.0f || dist > (float)app->flt_dist_km) return 0;
    }
    return 1;
}

/* Quality score (higher is better) for sort-by-best. Receivers with room for
 * both our connections float to the top; SNR adds within that tier, and a
 * nearer receiver is preferred (a gentle ~1 point per 50 km penalty) when a
 * home point is set. */
static double srv_score(const kiwi_server *s, float dist)
{
    int freeslots = (s->users_max > 0) ? (s->users_max - s->users) : 0;
    if (freeslots < 0) freeslots = 0;
    double sc = (freeslots >= 2) ? 1000.0 : 0.0;
    sc += (freeslots > 8 ? 8 : freeslots) * 5.0;
    sc += (s->snr > 0) ? s->snr : 0;
    if (dist >= 0.0f) sc -= dist / 50.0;
    return sc;
}

typedef struct { int idx; double score; float dist; } view_ent;
static view_ent s_ve[MAX_SERVERS];
static int cmp_view(const void *a, const void *b)
{
    const view_ent *x = a, *y = b;
    if (x->score < y->score) return 1;
    if (x->score > y->score) return -1;
    return 0;
}

static void rebuild_view(const app_state *app)
{
    int m = 0;
    for (int i = 0; i < s_nsrv; i++) {
        float d = srv_dist_km(app, &s_srv[i]);
        if (!filter_pass(app, &s_srv[i], d)) continue;
        s_ve[m].idx = i;
        s_ve[m].dist = d;
        s_ve[m].score = srv_score(&s_srv[i], d);
        m++;
    }
    if (app->flt_sort_best)
        qsort(s_ve, (size_t)m, sizeof(s_ve[0]), cmp_view);
    for (int i = 0; i < m; i++) {
        s_view[i] = s_ve[i].idx;
        s_view_dist[i] = s_ve[i].dist;
    }
    s_nview = m;
    s_view_gen = s_srv_gen;
    s_view_dirty = 0;
}

/* Rebuild the view if the directory or the filters changed, and keep the
 * selection in range. */
static void ensure_view(app_state *app)
{
    if (s_view_dirty || s_view_gen != s_srv_gen)
        rebuild_view(app);
    int total = s_nfav + s_nview;
    if (app->sel >= total) app->sel = total - 1;
    if (app->sel < 0) app->sel = 0;
}

/* The picker shows favourites first, then the filtered directory. These map a
 * combined row index onto the right list. The directory side goes through
 * s_view so only receivers passing the filters are shown. */
static int picker_total(void) { return s_nfav + s_nview; }
static const kiwi_server *picker_at(int i)
{
    if (i < 0) return NULL;
    if (i < s_nfav) return &s_fav[i];
    i -= s_nfav;
    if (i < s_nview) return &s_srv[s_view[i]];
    return NULL;
}
const kiwi_server *servers_at(int i) { return picker_at(i); }

void menu_init(void)
{
    s_nsrv = 0;
    s_nview = 0;
    s_settings_cur = 0;
    s_filter_cur = 0;
    s_view_dirty = 1;
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

/* Picker list is pushed below the filter bar (the band selector keeps LIST_Y). */
#define PICK_LIST_Y   102
#define PICK_VIS_ROWS 15

/* One-line summary of the active filters, for the filter bar. */
static void filter_summary(const app_state *app, char *out, size_t n)
{
    int len = 0;
    const char *sep = "";
    out[0] = '\0';
    if (app->flt_free2) {
        len += snprintf(out + len, n - len, "%s2+ free", sep); sep = "  ";
    }
    if (app->flt_min_snr > 0) {
        len += snprintf(out + len, n - len, "%sSNR>=%d", sep, app->flt_min_snr);
        sep = "  ";
    }
    if (app->flt_loc[0]) {
        len += snprintf(out + len, n - len, "%sloc:%.12s", sep, app->flt_loc);
        sep = "  ";
    }
    if (app->flt_dist_km > 0 && app->home_set) {
        len += snprintf(out + len, n - len, "%s<=%dkm", sep, app->flt_dist_km);
        sep = "  ";
    }
    if (len == 0)
        snprintf(out, n, "none");
}

static void draw_picker(app_state *app)
{
    ensure_view(app);
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
                              "showing %d of %d", s_nview, s_nsrv);
    }

    /* Filter bar (just under the title): a summary of what's active, with the
     * key to open the editor. */
    vita2d_draw_rectangle(0, 40, SCREEN_W, 52, RGBA8(18, 22, 28, 255));
    char fsum[96];
    filter_summary(app, fsum, sizeof(fsum));
    font_drawf(12, 60, COL_DIM, 0.8f, "Filters:");
    font_drawf(92, 60, COL_ACCENT, 0.8f, "%s", fsum);
    font_drawf(12, 82, COL_DIM, 0.72f,
        "L edit filters%s", app->flt_sort_best ? "   (sorted best-first)" : "");

    /* Combined list: favourites first, then the filtered directory (hidden
     * while a refresh is in flight so a half-parsed list isn't shown). */
    int dir_n = (status == DIR_FETCHING) ? 0 : s_nview;
    int n = s_nfav + dir_n;
    if (n > 0) {
        int top = app->sel - PICK_VIS_ROWS / 2;
        if (top < 0) top = 0;
        if (top > n - PICK_VIS_ROWS) top = n - PICK_VIS_ROWS;
        if (top < 0) top = 0;

        for (int r = 0; r < PICK_VIS_ROWS && top + r < n; r++) {
            int idx = top + r;
            int is_fav = (idx < s_nfav);
            const kiwi_server *s = is_fav ? &s_fav[idx] : &s_srv[s_view[idx - s_nfav]];
            float dist = is_fav ? -1.0f : s_view_dist[idx - s_nfav];
            int y = PICK_LIST_Y + r * ROW_H;
            if (idx == app->sel)
                vita2d_draw_rectangle(0, y - 18, SCREEN_W, ROW_H, COL_SELBG);

            if (is_fav)
                font_drawf(12, y, COL_AMBER, 0.9f, "*");
            unsigned int name_col = (idx == app->sel) ? COL_TEXT : COL_DIM;
            font_drawf(28, y, name_col, 0.9f,
                                  "%.34s", s->name[0] ? s->name : s->host);
            font_drawf(540, y, COL_DIM, 0.8f,
                                  "%.16s", s->loc);
            if (dist >= 0.0f)
                font_drawf(690, y, COL_DIM, 0.75f, "%.0fkm", dist);
            if (!is_fav) {
                unsigned int ucol = (s->users >= s->users_max && s->users_max > 0)
                                    ? COL_AMBER : COL_GREEN;
                font_drawf(778, y, ucol, 0.8f,
                                      "%d/%d", s->users, s->users_max);
                if (s->snr >= 0)
                    font_drawf(858, y, COL_DIM, 0.8f,
                                          "snr%d", s->snr);
            }
        }
    } else if (status != DIR_FETCHING) {
        if (s_nsrv > 0)
            font_drawf(12, PICK_LIST_Y + 20, COL_DIM, 1.0f,
                "No receivers match the filters. Press L to relax them.");
        else
            font_drawf(12, PICK_LIST_Y + 20, COL_DIM, 1.0f,
                "No receivers. Press [] to fetch the directory.");
    }

    vita2d_draw_rectangle(0, SCREEN_H - 28, SCREEN_W, 28, COL_BAR);
    font_drawf(12, SCREEN_H - 9, COL_DIM, 0.72f,
        "X connect  START fav(*)  SELECT add  [] refresh  L filters  /\\ settings  O radio");
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

/* ================= server filters editor ================= */

enum {
    FLT_FREE2 = 0,
    FLT_MINSNR,
    FLT_LOC,
    FLT_DIST,
    FLT_HOME,
    FLT_SORT,
    FLT_CLEAR,
    FLT_COUNT
};

static const int DIST_PRESETS[] = { 0, 500, 1000, 2000, 5000, 10000 };
#define NDIST (int)(sizeof(DIST_PRESETS) / sizeof(DIST_PRESETS[0]))

static const char *FLT_LABELS[FLT_COUNT] = {
    "Need 2+ free slots",
    "Minimum SNR",
    "Location contains",
    "Max distance from home",
    "Home location",
    "Sort by best",
    "Clear all filters",
};

/* Count directory receivers passing the current filters (no sort), for live
 * feedback while editing. */
static int live_match_count(const app_state *app)
{
    int m = 0;
    for (int i = 0; i < s_nsrv; i++) {
        float d = srv_dist_km(app, &s_srv[i]);
        if (filter_pass(app, &s_srv[i], d)) m++;
    }
    return m;
}

static void filter_value(const app_state *app, int item, char *out, size_t n)
{
    switch (item) {
    case FLT_FREE2:
        snprintf(out, n, "%s", ONOFF[app->flt_free2 ? 1 : 0]);
        break;
    case FLT_MINSNR:
        if (app->flt_min_snr > 0) snprintf(out, n, "%d", app->flt_min_snr);
        else                      snprintf(out, n, "Off");
        break;
    case FLT_LOC:
        snprintf(out, n, "%s", app->flt_loc[0] ? app->flt_loc : "(any) >");
        break;
    case FLT_DIST:
        if (app->flt_dist_km <= 0)  snprintf(out, n, "Off");
        else if (!app->home_set)    snprintf(out, n, "%d km (set home)", app->flt_dist_km);
        else                        snprintf(out, n, "%d km", app->flt_dist_km);
        break;
    case FLT_HOME:
        if (app->home_set) snprintf(out, n, "%.2f, %.2f >",
                                    (double)app->home_lat, (double)app->home_lon);
        else               snprintf(out, n, "(not set) >");
        break;
    case FLT_SORT:
        snprintf(out, n, "%s", ONOFF[app->flt_sort_best ? 1 : 0]);
        break;
    case FLT_CLEAR:
        snprintf(out, n, "press X");
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static void draw_filters(app_state *app)
{
    font_ensure();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, COL_BG);
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_BAR);
    font_drawf(12, 28, COL_TEXT, 1.2f, "Server filters");
    font_drawf(300, 28, COL_DIM, 0.9f, "%d of %d match",
               live_match_count(app), s_nsrv);

    for (int i = 0; i < FLT_COUNT; i++) {
        int y = 80 + i * 30;
        if (i == s_filter_cur)
            vita2d_draw_rectangle(0, y - 18, SCREEN_W, 30, COL_SELBG);
        unsigned int col = (i == s_filter_cur) ? COL_TEXT : COL_DIM;
        font_drawf(20, y, col, 0.95f, "%s", FLT_LABELS[i]);
        char val[48];
        filter_value(app, i, val, sizeof(val));
        font_drawf(520, y, COL_ACCENT, 0.95f, "%s", val);
    }

    int hy = SCREEN_H - 78;
    vita2d_draw_rectangle(0, hy - 6, SCREEN_W, 78, COL_BAR);
    font_drawf(20, hy + 14, COL_DIM, 0.8f,
        "We open two connections (audio + waterfall), so \"2+ free slots\" hides");
    font_drawf(20, hy + 34, COL_DIM, 0.8f,
        "receivers that can't serve both. Home accepts a grid (IO90) or lat,lon.");
    font_drawf(20, hy + 60, COL_DIM, 0.78f,
        "Up/Down move   L/R change   X edit   O or /\\ apply & back");
}

void menu_draw(app_state *app)
{
    if (app->screen == SCREEN_SETTINGS)      draw_settings(app);
    else if (app->screen == SCREEN_BANDS)    draw_bands(app);
    else if (app->screen == SCREEN_FILTERS)  draw_filters(app);
    else                                     draw_picker(app);
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

static void filters_change(app_state *app, int item, int dir)
{
    switch (item) {
    case FLT_FREE2:  app->flt_free2 = !app->flt_free2; break;
    case FLT_MINSNR: adjust(&app->flt_min_snr, dir * 5, 0, 50); break;
    case FLT_DIST: {
        int idx = 0;
        for (int i = 0; i < NDIST; i++)
            if (DIST_PRESETS[i] == app->flt_dist_km) idx = i;
        idx += dir;
        if (idx < 0) idx = 0;
        if (idx >= NDIST) idx = NDIST - 1;
        app->flt_dist_km = DIST_PRESETS[idx];
        break;
    }
    case FLT_SORT:   app->flt_sort_best = !app->flt_sort_best; break;
    default: return;   /* text/action items don't respond to L/R */
    }
    s_view_dirty = 1;
}

static void filters_activate(app_state *app, int item)
{
    if (item == FLT_LOC) {
        const char *txt = ime_get_text("Location contains (e.g. UK)", app->flt_loc);
        if (txt) {   /* empty string clears the filter */
            strncpy(app->flt_loc, txt, sizeof(app->flt_loc) - 1);
            app->flt_loc[sizeof(app->flt_loc) - 1] = '\0';
            s_view_dirty = 1;
        }
    } else if (item == FLT_HOME) {
        char init[32];
        if (app->home_set)
            snprintf(init, sizeof(init), "%.3f,%.3f",
                     (double)app->home_lat, (double)app->home_lon);
        else
            init[0] = '\0';
        const char *txt = ime_get_text("Home: grid (IO90) or lat,lon", init);
        if (txt && txt[0]) {
            double la, lo;
            int ok = geo_parse_gps(txt, &la, &lo) ||
                     geo_maidenhead_to_latlon(txt, &la, &lo);
            if (ok) {
                app->home_lat = (float)la;
                app->home_lon = (float)lo;
                app->home_set = 1;
                s_view_dirty = 1;
                ui_show_message(app, "home location set");
            } else {
                ui_show_message(app, "couldn't read that location");
            }
        }
    } else if (item == FLT_CLEAR) {
        app->flt_free2 = 0;
        app->flt_min_snr = 0;
        app->flt_loc[0] = '\0';
        app->flt_dist_km = 0;
        s_view_dirty = 1;
        ui_show_message(app, "filters cleared");
    }
}

void menu_handle(app_state *app, unsigned int pressed)
{
    if (app->screen == SCREEN_SERVERS) {
        ensure_view(app);
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
        if (pressed & SCE_CTRL_LTRIGGER) {
            s_filter_cur = 0;
            app->screen = SCREEN_FILTERS;
        }
        if (pressed & SCE_CTRL_TRIANGLE)         app->screen = SCREEN_SETTINGS;
        if (pressed & SCE_CTRL_CIRCLE) {
            if (app->conn_status == CONN_CONNECTED) app->screen = SCREEN_RADIO;
        }
        return;
    }

    if (app->screen == SCREEN_FILTERS) {
        if (pressed & SCE_CTRL_UP)    adjust(&s_filter_cur, -1, 0, FLT_COUNT - 1);
        if (pressed & SCE_CTRL_DOWN)  adjust(&s_filter_cur, +1, 0, FLT_COUNT - 1);
        if (pressed & SCE_CTRL_LEFT)  filters_change(app, s_filter_cur, -1);
        if (pressed & SCE_CTRL_RIGHT) filters_change(app, s_filter_cur, +1);
        if (pressed & SCE_CTRL_CROSS) filters_activate(app, s_filter_cur);
        if (pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | SCE_CTRL_LTRIGGER)) {
            config_save(app);      /* persist filter choices */
            s_view_dirty = 1;      /* rebuild against the new filters */
            app->sel = 0;          /* land at the top of the fresh list */
            app->screen = SCREEN_SERVERS;
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
