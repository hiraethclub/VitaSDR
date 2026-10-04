/* VitaSDR entry point: initialises subsystems, spawns the network/waterfall
 * threads, and runs the input + render loop.
 *
 * Threads:
 *   main  - input polling and rendering (this file's main())
 *   net   - SND connection lifecycle, tuning, keepalive (net_thread)
 *   wf    - W/F connection, waterfall bins (wf_thread)
 *   audio - jitter -> sceAudioOut (audio.c)
 */
#include "app.h"
#include "net.h"
#include "tls.h"
#include "log.h"
#include "b64.h"
#include "build_info.h"
#include "font.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/system_param.h>

#include <vita2d.h>

#include <stdio.h>
#include <string.h>

static app_state g_app;

#define JITTER_CAP (12000 * 2)   /* 2 s at 12 kHz: smooths jitter, caps latency */

static uint64_t now_ms(void)
{
    return sceKernelGetProcessTimeWide() / 1000ull;
}

/* Debug: capture the raw ADPCM payload of the first ~6s of SND frames to
 * ux0:data/vitasdr/adpcm.log, so the exact received compressed audio can be
 * decoded off-device with a reference decoder to validate our decode. Written
 * base64-encoded (as text) so it can be attached in clients that reject binary
 * files; decode with `base64 -d adpcm.log > adpcm.raw`. */
static b64_enc s_adpcm_cap;
static int     s_adpcm_open = 0;
static long    s_adpcm_left = 0;
static void adpcm_capture(unsigned char flags, const unsigned char *audio,
                          int audio_len)
{
    if (!s_adpcm_open || s_adpcm_left <= 0 || audio_len <= 0)
        return;
    if (!(flags & 0x10))   /* only capture COMPRESSED (ADPCM) payloads */
        return;
    int w = audio_len < s_adpcm_left ? audio_len : (int)s_adpcm_left;
    b64_write(&s_adpcm_cap, audio, (unsigned)w);
    s_adpcm_left -= w;
    if (s_adpcm_left <= 0) {
        b64_close(&s_adpcm_cap);
        s_adpcm_open = 0;
        vlog("adpcm capture complete");
    }
}

/* ---------------- network thread (SND) ---------------- */

static void net_disconnect(int proto, kiwi_client *k, owrx_client *o,
                           int *connected, int error)
{
    if (*connected) {
        audio_stop();
        if (proto == PROTO_OWRX)
            owrx_disconnect(o);
        else
            kiwi_disconnect(k);
        *connected = 0;
    }
    g_app.conn_status = error ? CONN_ERROR : CONN_IDLE;
    g_app.rssi_dbm = -140.0f;
    if (proto == PROTO_OWRX) {
        /* OpenWebRX multiplexes the waterfall on the audio socket, so there is
         * no separate wf thread to clear this. */
        g_app.wf_have_row = 0;
        g_app.wf_stalled = 0;
    }
}

int net_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* Static, not on the stack: each client embeds a 64 KB websocket receive
     * buffer (owrx_client also carries FFT/audio scratch), which would overflow
     * the thread stack. One net thread, so single static instances are safe.
     * Only one is active at a time, chosen by the receiver's protocol. */
    static kiwi_client k;
    static owrx_client o;
    int connected = 0;
    int proto = PROTO_KIWI;
    int owrx_adopted = 0;      /* adopted the server's resolved start freq yet */
    double last_freq = 0;
    char   last_mode[8] = {0};
    uint64_t last_ka = 0, last_tune = 0, last_stat = 0, conn_start = 0;
    unsigned last_msg_seq = 0;

    while (g_app.running) {
        if (!connected) {
            if (g_app.cmd_connect) {
                g_app.cmd_connect = 0;
                g_app.conn_status = CONN_CONNECTING;
                jitter_clear(&g_app.jitter);
                g_app.audio_rate = 12000;
                proto = g_app.proto;
                owrx_adopted = 0;

                sceKernelLockMutex(g_app.lock, 1, NULL);
                double f = g_app.freq_khz;
                char m[8]; strncpy(m, g_app.mode, sizeof(m)); m[7] = '\0';
                sceKernelUnlockMutex(g_app.lock, 1);

                int rc;
                if (proto == PROTO_OWRX)
                    rc = owrx_connect(&o, g_app.host, g_app.port,
                                      g_app.path[0] ? g_app.path : "/ws/",
                                      g_app.tls, g_app.tls_verify,
                                      &g_app.jitter, f, m, 6000);
                else
                    rc = kiwi_connect(&k, g_app.host, g_app.port,
                                      g_app.password, &g_app.jitter, f, m, 6000);
                if (rc == 0) {
                    connected = 1;
                    g_app.conn_status = CONN_CONNECTED;
                    if (audio_start(&g_app) != 0)
                        vlog("audio_start FAILED (no sound)");
                    last_freq = f;
                    strncpy(last_mode, m, sizeof(last_mode));
                    last_ka = last_tune = last_stat = conn_start = now_ms();
                    last_msg_seq = 0;
                    ws_client *ws = (proto == PROTO_OWRX) ? &o.ws : &k.ws;
                    vlog("%s connect OK, audio started",
                         proto == PROTO_OWRX ? "owrx" : "kiwi");
                    vlog("handshake: rx=%u resp='%s'",
                         ws->dbg_rx_bytes, ws->dbg_resp);
                    ui_show_message(&g_app, "connected");
                } else {
                    const char *why;
                    switch (rc) {
                    case WS_CONNECT_ETCP:
                        switch (net_last_fail_stage()) {
                        case NET_STAGE_RESOLVE: why = "DNS lookup failed"; break;
                        case NET_STAGE_SOCKET:  why = "socket create failed"; break;
                        case NET_STAGE_CONNECT: why = "TCP connect refused/timeout"; break;
                        default:                why = "TCP connect failed"; break;
                        }
                        break;
                    case WS_CONNECT_ESEND:   why = "request send failed"; break;
                    case WS_CONNECT_ENORESP: why = "no HTTP response"; break;
                    case WS_CONNECT_ESTATUS: why = "not a websocket (bad HTTP status)"; break;
                    case WS_CONNECT_EACCEPT: why = "bad ws accept key"; break;
                    case WS_CONNECT_ETLS:
                        why = "TLS handshake failed";
                        vlog("TLS last_error = %d (BR_ERR_*; -1 socket, -2 timeout)",
                             tls_last_error());
                        break;
                    case -6:                 why = "handshake send failed"; break;
                    default:                 why = "connect failed"; break;
                    }
                    snprintf(g_app.last_err, sizeof(g_app.last_err), "%s", why);
                    vlog("%s connect rc=%d stage=%d -> %s",
                         proto == PROTO_OWRX ? "owrx" : "kiwi", rc,
                         net_last_fail_stage(), why);
                    g_app.conn_status = CONN_ERROR;
                    ui_show_message(&g_app, why);
                }
            } else {
                sceKernelDelayThread(50 * 1000);
            }
            continue;
        }

        /* ---- connected: poll the active client ---- */
        int r;
        unsigned msg_seq;
        unsigned long samples_rx;
        float rssi;
        int smeter;
        const char *last_msg;
        ws_client *ws;

        if (proto == PROTO_OWRX) {
            r = owrx_poll(&o, 100);
            if (r == OWRX_WF) {
                /* OpenWebRX carries the waterfall on this same socket. */
                memcpy(g_app.wf_bins, o.wf_bins, sizeof(g_app.wf_bins));
                g_app.wf_nbins = o.wf_nbins;
                g_app.wf_have_row = 1;
                g_app.wf_stalled = 0;
            }
            /* Once the DSP starts, the client may have snapped the tuning into
             * the SDR's window; adopt that so the UI shows the real frequency. */
            if (!owrx_adopted && o.dsp_started) {
                owrx_adopted = 1;
                sceKernelLockMutex(g_app.lock, 1, NULL);
                g_app.freq_khz = o.freq_khz;
                sceKernelUnlockMutex(g_app.lock, 1);
                last_freq = o.freq_khz;
            }
            msg_seq = o.msg_seq; samples_rx = o.samples_rx;
            rssi = o.rssi_dbm; smeter = o.smeter;
            last_msg = o.last_msg; ws = &o.ws;
        } else {
            r = kiwi_poll(&k, 100);
            msg_seq = k.msg_seq; samples_rx = k.samples_rx;
            rssi = k.rssi_dbm; smeter = k.smeter;
            last_msg = k.last_msg; ws = &k.ws;
        }

        /* Surface any new server control text. */
        if (msg_seq != last_msg_seq) {
            last_msg_seq = msg_seq;
            vlog("MSG: %s", last_msg);
        }

        if (r == OWRX_ERR) {   /* == KIWI_ERR == -1 for both clients */
            vlog("%s poll ERR after %lu ms, samples=%lu, rx_bytes=%u close=%d net=%d",
                 proto == PROTO_OWRX ? "owrx" : "kiwi",
                 (unsigned long)(now_ms() - conn_start), samples_rx,
                 ws->dbg_rx_bytes, ws->dbg_close_frame, ws->dbg_net_result);
            /* Hex + ASCII of the first bytes the server sent, if any. */
            char hex[3 * 32 + 1], asc[32 + 1];
            unsigned nf = ws->dbg_first_len;
            for (unsigned i = 0; i < nf; i++) {
                snprintf(hex + i * 3, 4, "%02x ", ws->dbg_first[i]);
                unsigned char c = ws->dbg_first[i];
                asc[i] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            asc[nf] = '\0';
            if (nf == 0)
                hex[0] = '\0';
            vlog("first %u bytes: %s | %s", nf, hex, asc);
            net_disconnect(proto, &k, &o, &connected, 1);
            if (g_app.auto_reconnect && g_app.host[0]) {
                sceKernelDelayThread(3000 * 1000);   /* back off, then retry */
                if (g_app.running && !g_app.cmd_disconnect)
                    g_app.cmd_connect = 1;
            }
            continue;
        }
        if (r == OWRX_AUDIO) {   /* == KIWI_AUDIO == 1 for both clients */
            g_app.rssi_dbm = rssi;
            g_app.smeter_raw = smeter;
            g_app.samples_rx = samples_rx;
        }

        /* Periodic status so we can see whether audio is actually flowing. */
        if (now_ms() - last_stat >= 2000) {
            last_stat = now_ms();
            vlog("status: samples=%lu rssi=%.0f jitter=%u rx=%u inlen=%u net=%d msg=%u",
                 samples_rx, (double)rssi,
                 (unsigned)jitter_available(&g_app.jitter),
                 ws->dbg_rx_bytes, (unsigned)ws->in_len,
                 ws->dbg_net_result, msg_seq);
        }

        if (g_app.cmd_disconnect) {
            g_app.cmd_disconnect = 0;
            vlog("cmd_disconnect (user)");
            net_disconnect(proto, &k, &o, &connected, 0);
            continue;
        }

        /* No-response detector: a receiver with no free channel completes the
         * WebSocket handshake but then streams nothing. If we've had neither a
         * control message nor an audio sample within a few seconds, give up and
         * return to the picker rather than sit silent forever. A deliberate
         * stop, so it does NOT trigger auto-reconnect. */
        if (msg_seq == 0 && samples_rx == 0 &&
            now_ms() - conn_start >= 7000) {
            vlog("no data 7s after connect (receiver full/declined) -> picker");
            snprintf(g_app.last_err, sizeof(g_app.last_err),
                     "no response (receiver full or offline?)");
            net_disconnect(proto, &k, &o, &connected, 1);
            g_app.screen = SCREEN_SERVERS;
            ui_show_message(&g_app, "No response - pick another receiver");
            continue;
        }

        uint64_t t = now_ms();
        if (t - last_tune >= 150) {
            sceKernelLockMutex(g_app.lock, 1, NULL);
            double f = g_app.freq_khz;
            char m[8]; strncpy(m, g_app.mode, sizeof(m)); m[7] = '\0';
            sceKernelUnlockMutex(g_app.lock, 1);

            if (f != last_freq) {
                if (proto == PROTO_OWRX) owrx_set_frequency(&o, f);
                else                     kiwi_set_frequency(&k, f);
                last_freq = f;
            }
            if (strcmp(m, last_mode) != 0) {
                if (proto == PROTO_OWRX) owrx_set_mode(&o, m);
                else                     kiwi_set_mode(&k, m, 0, 0);
                strncpy(last_mode, m, sizeof(last_mode));
            }
            last_tune = t;
        }

        if (t - last_ka >= 1000) {
            if (proto == PROTO_OWRX) owrx_keepalive(&o);
            else                     kiwi_keepalive(&k);
            last_ka = t;
        }
    }

    net_disconnect(proto, &k, &o, &connected, 0);
    return 0;
}

/* ---------------- waterfall thread (W/F) ---------------- */

int wf_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* Static for the same reason as net_thread's kiwi_client: kiwi_wf embeds a
     * 64 KB websocket receive buffer. */
    static kiwi_wf w;
    int wf_conn = 0;
    double last_freq = 0;
    uint64_t last_ka = 0, last_center = 0, wf_start = 0, last_wf_stat = 0;
    unsigned long wf_frames = 0;
    static unsigned char bins[KIWI_WF_BINS];

    while (g_app.running) {
        /* OpenWebRX multiplexes the waterfall onto the audio socket (handled in
         * net_thread), so this KiwiSDR-only W/F connection stays idle for it. */
        if (g_app.proto == PROTO_OWRX) {
            if (wf_conn) { kiwi_wf_disconnect(&w); wf_conn = 0; }
            sceKernelDelayThread(100 * 1000);
            continue;
        }
        if (g_app.conn_status == CONN_CONNECTED && !wf_conn) {
            sceKernelLockMutex(g_app.lock, 1, NULL);
            double f = g_app.freq_khz;
            int zoom = g_app.zoom;
            sceKernelUnlockMutex(g_app.lock, 1);
            int wrc = kiwi_wf_connect(&w, g_app.host, g_app.port,
                                      g_app.password, f, zoom,
                                      g_app.wf_speed, 6000);
            vlog("wf_connect rc=%d resp='%s'", wrc, w.ws.dbg_resp);
            if (wrc == 0) {
                wf_conn = 1;
                last_freq = f;
                last_ka = last_center = wf_start = last_wf_stat = now_ms();
                wf_frames = 0;
                g_app.wf_stalled = 0;
            } else {
                sceKernelDelayThread(500 * 1000);
            }
        } else if (g_app.conn_status != CONN_CONNECTED && wf_conn) {
            kiwi_wf_disconnect(&w);
            wf_conn = 0;
            g_app.wf_stalled = 0;
        } else if (wf_conn) {
            int nb = kiwi_wf_poll(&w, bins, KIWI_WF_BINS, 100);
            if (nb < 0) {
                vlog("wf DROP after %lu ms, frames=%lu, rx_bytes=%u close=%d net=%d",
                     (unsigned long)(now_ms() - wf_start), wf_frames,
                     w.ws.dbg_rx_bytes, w.ws.dbg_close_frame, w.ws.dbg_net_result);
                kiwi_wf_disconnect(&w);
                wf_conn = 0;
                continue;
            }
            if (nb > 0) {
                memcpy(g_app.wf_bins, bins, (size_t)nb);
                g_app.wf_nbins = nb;
                g_app.wf_have_row = 1;
                g_app.wf_stalled = 0;
                if ((++wf_frames % 30) == 0)
                    vlog("wf frames=%lu (nb=%d)", wf_frames, nb);
            }
            uint64_t t = now_ms();
            /* Flag an audio-only receiver: connected to W/F but no frames after
             * a few seconds (receiver limits us to one connection per IP). */
            if (wf_frames == 0 && t - wf_start > 6000)
                g_app.wf_stalled = 1;
            /* Periodic WF receive diagnostics (even when no frames parse), so we
             * can tell 'server sends nothing' from 'frames arrive but drop'. */
            if (t - last_wf_stat >= 2000) {
                last_wf_stat = t;
                vlog("wf stat: frames=%lu rx=%u inlen=%u net=%d last_nb=%d",
                     wf_frames, w.ws.dbg_rx_bytes, (unsigned)w.ws.in_len,
                     w.ws.dbg_net_result, nb);
            }
            if (t - last_center >= 200) {
                sceKernelLockMutex(g_app.lock, 1, NULL);
                double f = g_app.freq_khz;
                int zoom = g_app.zoom;
                sceKernelUnlockMutex(g_app.lock, 1);
                if (f != last_freq) {
                    kiwi_wf_set_center(&w, f, zoom);
                    last_freq = f;
                }
                last_center = t;
            }
            if (t - last_ka >= 1000) {
                kiwi_wf_keepalive(&w);
                last_ka = t;
            }
        } else {
            sceKernelDelayThread(100 * 1000);
        }
    }

    if (wf_conn)
        kiwi_wf_disconnect(&w);
    return 0;
}

/* ---------------- directory fetch thread ---------------- */

int dir_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (g_app.running) {
        if (g_app.cmd_fetch_dir) {
            g_app.cmd_fetch_dir = 0;
            g_app.dir_status = DIR_FETCHING;
            vlog("directory fetch start");
            int n = servers_fetch();
            if (n > 0) {
                g_app.dir_status = DIR_DONE;
                if (g_app.sel >= n) g_app.sel = 0;
                vlog("directory fetch OK: %d receivers", n);
            } else {
                g_app.dir_status = DIR_ERROR;
                vlog("directory fetch FAILED rc=%d", n);
            }
        } else {
            sceKernelDelayThread(50 * 1000);
        }
    }
    return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    memset(&g_app, 0, sizeof(g_app));
    log_init();
    vlog("VitaSDR starting");
    config_load(&g_app);
    vlog("config: host=%s port=%d freq=%.3f mode=%s",
         g_app.host, g_app.port, g_app.freq_khz, g_app.mode);
    g_app.conn_status = CONN_IDLE;
    g_app.rssi_dbm = -140.0f;
    g_app.audio_rate = 12000;
    g_app.running = 1;

    if (jitter_init(&g_app.jitter, JITTER_CAP) != 0)
        return -1;
    g_app.lock = sceKernelCreateMutex("vitasdr_lock", 0, 0, NULL);

    /* Install the ADPCM debug capture (first ~6s of compressed payload). */
    s_adpcm_open = (b64_open(&s_adpcm_cap, VITASDR_DATA_DIR "/adpcm.log") == 0);
    s_adpcm_left = 12000 / 2 * 6;   /* ~6s of ADPCM (2 samples/byte @ 12kHz) */
    kiwi_snd_tap = adpcm_capture;
    vlog("adpcm capture %s", s_adpcm_open ? "open" : "FAILED");

    int net_ok = (net_global_init() == 0);
    if (!net_ok) {
        /* Networking unavailable; still show the UI with an error state. */
        g_app.conn_status = CONN_ERROR;
        snprintf(g_app.last_err, sizeof(g_app.last_err),
                 "network init failed (WiFi on?)");
    }

    vita2d_init();
    vita2d_set_clear_color(RGBA8(10, 12, 16, 255));

    /* Common-dialog subsystem must be configured once before any system dialog
     * (the IME on-screen keyboard used by the server picker's manual add).
     * Without this the IME never composites and the screen just goes black. */
    {
        SceCommonDialogConfigParam cfg;
        sceCommonDialogConfigParamInit(&cfg);
        cfg.language = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
        cfg.enterButtonAssign = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
        sceCommonDialogSetConfigParam(&cfg);
    }

    wf_render_init();
    font_ensure();
    input_init();
    menu_init();

    SceUID net_tid = sceKernelCreateThread("vitasdr_net", net_thread,
                                           0x10000100, 0x40000, 0, 0, NULL);
    SceUID wf_tid = sceKernelCreateThread("vitasdr_wf", wf_thread,
                                          0x10000100, 0x40000, 0, 0, NULL);
    SceUID dir_tid = sceKernelCreateThread("vitasdr_dir", dir_thread,
                                           0x10000100, 0x40000, 0, 0, NULL);
    sceKernelStartThread(net_tid, 0, NULL);
    sceKernelStartThread(wf_tid, 0, NULL);
    sceKernelStartThread(dir_tid, 0, NULL);

    if (!net_ok) {
        g_app.screen = SCREEN_SERVERS;
        ui_show_message(&g_app, "no network - check WiFi");
    } else if (g_app.auto_connect && g_app.host[0]) {
        /* Skip the picker and reconnect to the last-used receiver. */
        g_app.screen = SCREEN_RADIO;
        g_app.cmd_connect = 1;
        ui_show_message(&g_app, g_app.host);
    } else {
        /* Show the directory picker and start fetching the list. */
        g_app.screen = SCREEN_SERVERS;
        g_app.cmd_fetch_dir = 1;
    }

    while (g_app.running) {
        input_poll(&g_app);

        /* Keep the screen/system awake while running, if enabled, so listening
         * to a station isn't cut short by the Vita auto-dimming/suspending. */
        if (g_app.keep_awake)
            sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND |
                               SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);

        /* Note: we deliberately do NOT reset the waterfall contrast on retune.
         * The percentile floor eases to the new band within ~1s, and resetting
         * caused a full-scale "green flash" in the spectrum while scanning. */

        if (g_app.wf_have_row) {
            wf_push_bins(g_app.wf_bins, g_app.wf_nbins, g_app.palette);
            g_app.wf_have_row = 0;
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        if (g_app.screen == SCREEN_RADIO)
            ui_draw(&g_app);
        else
            menu_draw(&g_app);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    /* Shut down. */
    sceKernelWaitThreadEnd(net_tid, NULL, NULL);
    sceKernelWaitThreadEnd(wf_tid, NULL, NULL);
    sceKernelWaitThreadEnd(dir_tid, NULL, NULL);
    sceKernelDeleteThread(net_tid);
    sceKernelDeleteThread(wf_tid);
    sceKernelDeleteThread(dir_tid);

    config_save(&g_app);
    audio_shutdown();
    wf_render_shutdown();
    vita2d_fini();
    jitter_free(&g_app.jitter);
    net_global_fini();
    vlog("VitaSDR exiting");
    log_close();

    sceKernelExitProcess(0);
    return 0;
}
