/* Audio output via sceAudioOut, fed from the jitter buffer.
 *
 * The Vita audio hardware is happiest at 48 kHz stereo, and some firmware
 * rejects a 12 kHz mono port outright (which left the jitter buffer full and
 * silent). So we always open a 48 kHz stereo port and resample the KiwiSDR's
 * ~12 kHz mono stream up to it (nearest-neighbour; fine for voice/SSB), and
 * duplicate mono to both channels.
 *
 * A dedicated thread pulls from the jitter buffer and hands fixed-size grains
 * to sceAudioOutOutput, which blocks until the previous grain has played, so
 * the thread is paced by the audio clock. On underrun it outputs silence.
 */
#include "app.h"
#include "log.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

#define OUT_RATE   48000
#define OUT_GRAIN  512    /* samples per channel per output (multiple of 64) */

static app_state *s_app = NULL;
static int        s_port = -1;
static SceUID     s_thread = -1;
static volatile int s_run = 0;
static int        s_src_rate = 12000;

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int16_t src[OUT_GRAIN];        /* source (mono) samples for one grain */
    int16_t out[OUT_GRAIN * 2];    /* interleaved stereo output */
    unsigned long outputs = 0;

    /* Source samples consumed per output grain. */
    int need = (int)((long)OUT_GRAIN * s_src_rate / OUT_RATE);
    if (need < 1) need = 1;
    if (need > OUT_GRAIN) need = OUT_GRAIN;

    vlog("audio_thread start: src_rate=%d out=%d need=%d", s_src_rate,
         OUT_RATE, need);

    while (s_run) {
        size_t got = jitter_pop(&s_app->jitter, src, (size_t)need);
        if (got < (size_t)need) {
            memset(src + got, 0, ((size_t)need - got) * sizeof(int16_t));
            if (s_app->conn_status == CONN_CONNECTED && got == 0)
                s_app->audio_underruns++;
        }

        int vol = s_app->volume;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;

        /* Upsample need -> OUT_GRAIN (nearest neighbour), mono -> stereo,
         * apply volume. */
        for (int i = 0; i < OUT_GRAIN; i++) {
            int si = (int)((long)i * need / OUT_GRAIN);
            if (si >= need) si = need - 1;
            int s = ((int)src[si] * vol) / 100;
            out[i * 2]     = (int16_t)s;
            out[i * 2 + 1] = (int16_t)s;
        }

        sceAudioOutOutput(s_port, out);

        if ((++outputs % 200) == 0)
            vlog("audio: outputs=%lu jitter=%u", outputs,
                 (unsigned)jitter_available(&s_app->jitter));
    }
    return 0;
}

int audio_start(app_state *app)
{
    s_app = app;
    s_src_rate = app->audio_rate > 0 ? app->audio_rate : 12000;

    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN,
                                 OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    vlog("sceAudioOutOpenPort(MAIN,%d,%d,STEREO) = 0x%08X", OUT_GRAIN,
         OUT_RATE, s_port);
    if (s_port < 0)
        return -1;

    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port,
        (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                 SCE_AUDIO_VOLUME_FLAG_R_CH), vol);

    s_run = 1;
    s_thread = sceKernelCreateThread("vitasdr_audio", audio_thread,
                                     0x10000100, 0x10000, 0, 0, NULL);
    vlog("audio thread create = 0x%08X", s_thread);
    if (s_thread < 0) {
        sceAudioOutReleasePort(s_port);
        s_port = -1;
        s_run = 0;
        return -1;
    }
    sceKernelStartThread(s_thread, 0, NULL);
    return 0;
}

void audio_stop(void)
{
    s_run = 0;
    if (s_thread >= 0) {
        sceKernelWaitThreadEnd(s_thread, NULL, NULL);
        sceKernelDeleteThread(s_thread);
        s_thread = -1;
    }
    if (s_port >= 0) {
        sceAudioOutReleasePort(s_port);
        s_port = -1;
    }
}
