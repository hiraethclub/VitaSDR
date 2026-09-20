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
#include "log.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

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
 * ux0:data/vitasdr/adpcm.raw, so the exact received compressed audio can be
 * decoded off-device with a reference decoder to validate our decode. */
static FILE  *s_adpcm_cap = NULL;
static long   s_adpcm_left = 0;
static void adpcm_capture(unsigned char flags, const unsigned char *audio,
                          int audio_len)
{
    if (!s_adpcm_cap || s_adpcm_left <= 0 || audio_len <= 0)
        return;
    if (!(flags & 0x10))   /* only capture COMPRESSED (ADPCM) payloads */
        return;
    int w = audio_len < s_adpcm_left ? audio_len : (int)s_adpcm_left;
    fwrite(audio, 1, (size_t)w, s_adpcm_cap);
    s_adpcm_left -= w;
    if (s_adpcm_left <= 0) {
        fclose(s_adpcm_cap);
        s_adpcm_cap = NULL;
        vlog("adpcm capture complete");
    }
}

/* ---------------- network thread (SND) ---------------- */

static void net_disconnect(kiwi_client *k, int *connected, int error)
{
    if (*connected) {
        audio_stop();
        kiwi_disconnect(k);
        *connected = 0;
    }
    g_app.conn_status = error ? CONN_ERROR : CONN_IDLE;
    g_app.rssi_dbm = -140.0f;
}

int net_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* Static, not on the stack: kiwi_client embeds the 64 KB websocket receive
     * buffer, which would overflow the thread stack. Single net thread, so a
     * single static instance is safe. */
    static kiwi_client k;
    int connected = 0;
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

                sceKernelLockMutex(g_app.lock, 1, NULL);
                double f = g_app.freq_khz;
                char m[8]; strncpy(m, g_app.mode, sizeof(m)); m[7] = '\0';
                sceKernelUnlockMutex(g_app.lock, 1);

                int rc = kiwi_connect(&k, g_app.host, g_app.port,
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
                    vlog("kiwi_connect OK, audio started");
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
                    case -6:                 why = "auth send failed"; break;
                    default:                 why = "connect failed"; break;
                    }
                    snprintf(g_app.last_err, sizeof(g_app.last_err), "%s", why);
                    vlog("kiwi_connect rc=%d stage=%d -> %s", rc,
                         net_last_fail_stage(), why);
                    g_app.conn_status = CONN_ERROR;
                    ui_show_message(&g_app, why);
                }
            } else {
                sceKernelDelayThread(50 * 1000);
            }
            continue;
        }

        /* connected */
        int r = kiwi_poll(&k, 100);

        /* Surface any new server MSG control text. */
        if (k.msg_seq != last_msg_seq) {
            last_msg_seq = k.msg_seq;
            vlog("MSG: %s", k.last_msg);
        }

        if (r == KIWI_ERR) {
            vlog("kiwi_poll ERR after %lu ms, samples=%lu, rx_bytes=%u close=%d net=%d",
                 (unsigned long)(now_ms() - conn_start), k.samples_rx,
                 k.ws.dbg_rx_bytes, k.ws.dbg_close_frame, k.ws.dbg_net_result);
            /* Hex + ASCII of the first bytes the server sent, if any. */
            char hex[3 * 32 + 1], asc[32 + 1];
            unsigned nf = k.ws.dbg_first_len;
            for (unsigned i = 0; i < nf; i++) {
                snprintf(hex + i * 3, 4, "%02x ", k.ws.dbg_first[i]);
                unsigned char c = k.ws.dbg_first[i];
                asc[i] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            asc[nf] = '\0';
            if (nf == 0)
                hex[0] = '\0';
            vlog("first %u bytes: %s | %s", nf, hex, asc);
            net_disconnect(&k, &connected, 1);
            continue;
        }
        if (r == KIWI_AUDIO) {
            g_app.rssi_dbm = k.rssi_dbm;
            g_app.smeter_raw = k.smeter;
            g_app.samples_rx = k.samples_rx;
        }

        /* Periodic status so we can see whether audio is actually flowing. */
        if (now_ms() - last_stat >= 2000) {
            last_stat = now_ms();
            vlog("status: samples=%lu rssi=%.0f jitter=%u",
                 k.samples_rx, (double)k.rssi_dbm,
                 (unsigned)jitter_available(&g_app.jitter));
        }

        if (g_app.cmd_disconnect) {
            g_app.cmd_disconnect = 0;
            vlog("cmd_disconnect (user)");
            net_disconnect(&k, &connected, 0);
            continue;
        }

        uint64_t t = now_ms();
        if (t - last_tune >= 150) {
            sceKernelLockMutex(g_app.lock, 1, NULL);
            double f = g_app.freq_khz;
            char m[8]; strncpy(m, g_app.mode, sizeof(m)); m[7] = '\0';
            sceKernelUnlockMutex(g_app.lock, 1);

            if (f != last_freq) {
                kiwi_set_frequency(&k, f);
                last_freq = f;
            }
            if (strcmp(m, last_mode) != 0) {
                kiwi_set_mode(&k, m, 0, 0);
                strncpy(last_mode, m, sizeof(last_mode));
            }
            last_tune = t;
        }

        if (t - last_ka >= 1000) {
            kiwi_keepalive(&k);
            last_ka = t;
        }
    }

    net_disconnect(&k, &connected, 0);
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
    uint64_t last_ka = 0, last_center = 0, wf_start = 0;
    unsigned long wf_frames = 0;
    static unsigned char bins[KIWI_WF_BINS];

    while (g_app.running) {
        if (g_app.conn_status == CONN_CONNECTED && !wf_conn) {
            sceKernelLockMutex(g_app.lock, 1, NULL);
            double f = g_app.freq_khz;
            int zoom = g_app.zoom;
            sceKernelUnlockMutex(g_app.lock, 1);
            int wrc = kiwi_wf_connect(&w, g_app.host, g_app.port,
                                      g_app.password, f, zoom, 6000);
            vlog("wf_connect rc=%d", wrc);
            if (wrc == 0) {
                wf_conn = 1;
                last_freq = f;
                last_ka = last_center = wf_start = now_ms();
                wf_frames = 0;
            } else {
                sceKernelDelayThread(500 * 1000);
            }
        } else if (g_app.conn_status != CONN_CONNECTED && wf_conn) {
            kiwi_wf_disconnect(&w);
            wf_conn = 0;
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
                if ((++wf_frames % 30) == 0)
                    vlog("wf frames=%lu (nb=%d)", wf_frames, nb);
            }
            uint64_t t = now_ms();
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
    s_adpcm_cap = fopen("ux0:data/vitasdr/adpcm.raw", "wb");
    s_adpcm_left = 12000 / 2 * 6;   /* ~6s of ADPCM (2 samples/byte @ 12kHz) */
    kiwi_snd_tap = adpcm_capture;
    vlog("adpcm capture %s", s_adpcm_cap ? "open" : "FAILED");

    int net_ok = (net_global_init() == 0);
    if (!net_ok) {
        /* Networking unavailable; still show the UI with an error state. */
        g_app.conn_status = CONN_ERROR;
        snprintf(g_app.last_err, sizeof(g_app.last_err),
                 "network init failed (WiFi on?)");
    }

    vita2d_init();
    vita2d_set_clear_color(RGBA8(10, 12, 16, 255));
    wf_render_init();
    input_init();

    SceUID net_tid = sceKernelCreateThread("vitasdr_net", net_thread,
                                           0x10000100, 0x40000, 0, 0, NULL);
    SceUID wf_tid = sceKernelCreateThread("vitasdr_wf", wf_thread,
                                          0x10000100, 0x40000, 0, 0, NULL);
    sceKernelStartThread(net_tid, 0, NULL);
    sceKernelStartThread(wf_tid, 0, NULL);

    /* Auto-connect on launch if the network is up and a host is configured
     * (config_load guarantees a real default). */
    if (net_ok && g_app.host[0]) {
        g_app.cmd_connect = 1;
        ui_show_message(&g_app, g_app.host);
    } else if (!net_ok) {
        ui_show_message(&g_app, "no network - check WiFi");
    } else {
        ui_show_message(&g_app, "Set host in ux0:data/vitasdr/config.ini");
    }

    while (g_app.running) {
        input_poll(&g_app);

        /* Note: we deliberately do NOT reset the waterfall contrast on retune.
         * The percentile floor eases to the new band within ~1s, and resetting
         * caused a full-scale "green flash" in the spectrum while scanning. */

        if (g_app.wf_have_row) {
            wf_push_bins(g_app.wf_bins, g_app.wf_nbins, g_app.palette);
            g_app.wf_have_row = 0;
        }

        vita2d_start_drawing();
        vita2d_clear_screen();
        ui_draw(&g_app);
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    /* Shut down. */
    sceKernelWaitThreadEnd(net_tid, NULL, NULL);
    sceKernelWaitThreadEnd(wf_tid, NULL, NULL);
    sceKernelDeleteThread(net_tid);
    sceKernelDeleteThread(wf_tid);

    config_save(&g_app);
    wf_render_shutdown();
    vita2d_fini();
    jitter_free(&g_app.jitter);
    net_global_fini();
    vlog("VitaSDR exiting");
    log_close();

    sceKernelExitProcess(0);
    return 0;
}
