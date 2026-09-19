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

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <vita2d.h>

#include <string.h>

static app_state g_app;

#define JITTER_CAP (48000 * 2)   /* ~4 s at 12 kHz, headroom */

static uint64_t now_ms(void)
{
    return sceKernelGetProcessTimeWide() / 1000ull;
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
    kiwi_client k;
    int connected = 0;
    double last_freq = 0;
    char   last_mode[8] = {0};
    uint64_t last_ka = 0, last_tune = 0;

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

                if (kiwi_connect(&k, g_app.host, g_app.port, g_app.password,
                                 &g_app.jitter, f, m, 6000) == 0) {
                    connected = 1;
                    g_app.conn_status = CONN_CONNECTED;
                    audio_start(&g_app);
                    last_freq = f;
                    strncpy(last_mode, m, sizeof(last_mode));
                    last_ka = last_tune = now_ms();
                    ui_show_message(&g_app, "connected");
                } else {
                    g_app.conn_status = CONN_ERROR;
                    ui_show_message(&g_app, "connect failed (Start to retry)");
                }
            } else {
                sceKernelDelayThread(50 * 1000);
            }
            continue;
        }

        /* connected */
        int r = kiwi_poll(&k, 100);
        if (r == KIWI_ERR) {
            net_disconnect(&k, &connected, 1);
            continue;
        }
        if (r == KIWI_AUDIO) {
            g_app.rssi_dbm = k.rssi_dbm;
            g_app.smeter_raw = k.smeter;
            g_app.samples_rx = k.samples_rx;
        }

        if (g_app.cmd_disconnect) {
            g_app.cmd_disconnect = 0;
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
    kiwi_wf w;
    int wf_conn = 0;
    double last_freq = 0;
    uint64_t last_ka = 0, last_center = 0;
    static unsigned char bins[KIWI_WF_BINS];

    while (g_app.running) {
        if (g_app.conn_status == CONN_CONNECTED && !wf_conn) {
            sceKernelLockMutex(g_app.lock, 1, NULL);
            double f = g_app.freq_khz;
            int zoom = g_app.zoom;
            sceKernelUnlockMutex(g_app.lock, 1);
            if (kiwi_wf_connect(&w, g_app.host, g_app.port, g_app.password,
                                f, zoom, 6000) == 0) {
                wf_conn = 1;
                last_freq = f;
                last_ka = last_center = now_ms();
            } else {
                sceKernelDelayThread(500 * 1000);
            }
        } else if (g_app.conn_status != CONN_CONNECTED && wf_conn) {
            kiwi_wf_disconnect(&w);
            wf_conn = 0;
        } else if (wf_conn) {
            int nb = kiwi_wf_poll(&w, bins, KIWI_WF_BINS, 100);
            if (nb < 0) {
                kiwi_wf_disconnect(&w);
                wf_conn = 0;
                continue;
            }
            if (nb > 0) {
                memcpy(g_app.wf_bins, bins, (size_t)nb);
                g_app.wf_nbins = nb;
                g_app.wf_have_row = 1;
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
    config_load(&g_app);
    g_app.conn_status = CONN_IDLE;
    g_app.rssi_dbm = -140.0f;
    g_app.audio_rate = 12000;
    g_app.running = 1;

    if (jitter_init(&g_app.jitter, JITTER_CAP) != 0)
        return -1;
    g_app.lock = sceKernelCreateMutex("vitasdr_lock", 0, 0, NULL);

    if (net_global_init() != 0) {
        /* Networking unavailable; still show the UI with an error state. */
        g_app.conn_status = CONN_ERROR;
    }

    vita2d_init();
    vita2d_set_clear_color(RGBA8(10, 12, 16, 255));
    wf_render_init();
    input_init();

    SceUID net_tid = sceKernelCreateThread("vitasdr_net", net_thread,
                                           0x10000100, 0x10000, 0, 0, NULL);
    SceUID wf_tid = sceKernelCreateThread("vitasdr_wf", wf_thread,
                                          0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(net_tid, 0, NULL);
    sceKernelStartThread(wf_tid, 0, NULL);

    /* Auto-connect on launch if a host is configured (config_load guarantees a
     * real default, upgrading the old placeholder automatically). */
    if (g_app.host[0]) {
        g_app.cmd_connect = 1;
        ui_show_message(&g_app, g_app.host);
    } else {
        ui_show_message(&g_app, "Set host in ux0:data/vitasdr/config.ini");
    }

    while (g_app.running) {
        input_poll(&g_app);

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

    sceKernelExitProcess(0);
    return 0;
}
