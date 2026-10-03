/* Shared application state for the Vita app.
 *
 * The app runs three threads:
 *   - main:  input polling + rendering (owns UI/tuning state)
 *   - net:   drives the KiwiSDR SND connection, decodes audio into the jitter
 *            buffer, and applies tuning commands
 *   - wf:    drives the KiwiSDR W/F connection, fills the waterfall bin buffer
 *   - audio: pulls from the jitter buffer and feeds sceAudioOut
 *
 * Cross-thread fields are marked volatile. Tuning commands from the main thread
 * to the net/wf threads are passed under `lock` to avoid torn reads of the
 * double frequency.
 */
#ifndef VITASDR_APP_H
#define VITASDR_APP_H

#include "jitter.h"
#include "kiwi.h"
#include "kiwidir.h"
#include "owrx.h"

#include <psp2/kernel/threadmgr.h>

#define SCREEN_W 960
#define SCREEN_H 544

/* Connection status. */
enum {
    CONN_IDLE = 0,
    CONN_CONNECTING,
    CONN_CONNECTED,
    CONN_ERROR
};

/* Which screen the UI is showing. */
enum {
    SCREEN_SERVERS = 0,   /* directory picker (startup) */
    SCREEN_RADIO,         /* the radio client */
    SCREEN_SETTINGS,      /* settings + credits */
    SCREEN_BANDS          /* band-jump selector */
};

/* Directory fetch state. */
enum {
    DIR_IDLE = 0,
    DIR_FETCHING,
    DIR_DONE,
    DIR_ERROR
};

/* Audio bandwidth presets (low-pass corner). */
enum { AUDIO_BW_NARROW = 0, AUDIO_BW_NORMAL, AUDIO_BW_WIDE };

/* Receiver protocol. Must match kiwi_server.proto (0 = Kiwi, 1 = OpenWebRX). */
enum { PROTO_KIWI = 0, PROTO_OWRX = 1 };

typedef struct {
    /* ---- server config ---- */
    char   host[128];
    int    port;
    char   password[64];
    int    proto;        /* PROTO_KIWI or PROTO_OWRX */
    char   path[64];     /* OpenWebRX WebSocket path (e.g. "/ws/") */

    /* ---- tuning / UI state (owned by main thread) ---- */
    double freq_khz;
    char   mode[8];
    int    step_hz;      /* current tuning step */
    int    zoom;         /* waterfall zoom */
    int    volume;       /* 0..100 */
    int    squelch;      /* 0..100 (display only for now) */
    int    palette;      /* waterfall palette index */
    int    show_spectrum;

    /* ---- QoL settings (persisted) ---- */
    int    wf_speed;      /* waterfall update rate 1..4 */
    int    audio_bw;      /* AUDIO_BW_* low-pass preset */
    int    auto_connect;  /* skip picker, connect to last server on launch */
    int    auto_reconnect;/* reconnect automatically on a dropped link */
    int    keep_awake;    /* hold off Vita auto-dim/suspend while running */

    /* ---- screen / directory (main-thread owns screen, sel) ---- */
    volatile int screen;
    volatile int dir_status;
    volatile int cmd_fetch_dir;
    int    sel;           /* selected row in the server picker */

    /* ---- command handoff main -> worker threads (under lock) ---- */
    SceUID lock;
    int    cmd_connect;
    int    cmd_disconnect;
    int    cmd_retune;      /* frequency and/or mode changed */
    double cmd_freq_khz;
    char   cmd_mode[8];

    /* ---- status mirrored from worker threads ---- */
    volatile int   conn_status;
    char           last_err[64];   /* human-readable reason when conn_status==ERROR */
    volatile float rssi_dbm;
    volatile int   smeter_raw;
    volatile unsigned long samples_rx;
    volatile int   audio_underruns;

    /* ---- audio ---- */
    jitter_buf     jitter;
    volatile int   audio_rate;

    /* ---- waterfall bins (written by wf thread, read by render) ---- */
    volatile int   wf_have_row;
    unsigned char  wf_bins[KIWI_WF_BINS];
    volatile int   wf_nbins;
    volatile int   wf_stalled;   /* connected but no W/F frames (receiver limit) */

    volatile int   running;
} app_state;

/* config.c */
void config_defaults(app_state *app);
int  config_load(app_state *app);       /* from ux0:data/vitasdr/config.ini */
int  config_save(const app_state *app);

/* audio.c */
int  audio_start(app_state *app);
void audio_stop(void);      /* stop thread, keep the port open for reuse */
void audio_shutdown(void);  /* stop thread AND release the port (app exit) */

/* waterfall.c */
int  wf_render_init(void);
void wf_render_shutdown(void);
void wf_push_bins(const unsigned char *bins, int nbins, int palette);
void wf_render_draw(int x, int y, int w, int h);
void wf_render_reset(void);   /* reset adaptive contrast (e.g. on retune) */
unsigned char wf_level(unsigned char v); /* noise-floor + gain applied to a bin */

/* ui.c */
void ui_draw(app_state *app);
void ui_show_message(app_state *app, const char *msg);

/* menu.c — server picker + settings screens */
void menu_init(void);
int  servers_fetch(void);              /* net thread: fetch+parse; returns count/<0 */
int  servers_count(void);
const kiwi_server *servers_at(int i);
void menu_draw(app_state *app);        /* draws picker or settings by app->screen */
void menu_handle(app_state *app, unsigned int pressed); /* button edges */
const char *ime_get_text(const char *title, const char *initial); /* on-screen kbd */

/* input.c */
void input_init(void);
void input_poll(app_state *app);

/* net threads (main.c starts them) */
int  net_thread(SceSize args, void *argp);
int  wf_thread(SceSize args, void *argp);
int  dir_thread(SceSize args, void *argp);  /* directory fetch worker */

#endif /* VITASDR_APP_H */
