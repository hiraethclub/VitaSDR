/* Audio output via sceAudioOut, fed from the jitter buffer.
 *
 * A dedicated thread pulls fixed-size grains from the jitter buffer and hands
 * them to sceAudioOutOutput, which blocks until the previous grain has played,
 * so the thread is paced by the audio clock. On underrun it outputs silence to
 * keep the port fed rather than stalling.
 */
#include "app.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

#define AUDIO_GRAIN 256   /* samples per output (multiple of 64) */

static app_state *s_app = NULL;
static int        s_port = -1;
static SceUID     s_thread = -1;
static volatile int s_run = 0;

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int16_t grain[AUDIO_GRAIN];

    while (s_run) {
        size_t got = jitter_pop(&s_app->jitter, grain, AUDIO_GRAIN);
        if (got < AUDIO_GRAIN) {
            memset(grain + got, 0, (AUDIO_GRAIN - got) * sizeof(int16_t));
            if (s_app->conn_status == CONN_CONNECTED && got == 0)
                s_app->audio_underruns++;
        }

        /* Software volume (0..100). */
        int vol = s_app->volume;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        if (vol != 100) {
            for (int i = 0; i < AUDIO_GRAIN; i++)
                grain[i] = (int16_t)((int)grain[i] * vol / 100);
        }

        sceAudioOutOutput(s_port, grain);
    }
    return 0;
}

int audio_start(app_state *app)
{
    s_app = app;
    int freq = app->audio_rate > 0 ? app->audio_rate : 12000;

    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN,
                                 freq, SCE_AUDIO_OUT_MODE_MONO);
    if (s_port < 0)
        return -1;

    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    sceAudioOutSetVolume(s_port,
        (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                 SCE_AUDIO_VOLUME_FLAG_R_CH), vol);

    s_run = 1;
    s_thread = sceKernelCreateThread("vitasdr_audio", audio_thread,
                                     0x10000100, 0x10000, 0, 0, NULL);
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
