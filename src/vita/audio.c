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
#include "b64.h"
#include "resamp.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>

#define OUT_RATE   48000
#define OUT_GRAIN  512    /* samples per channel per output (multiple of 64) */
#define SOFT_KNEE   28000  /* safety soft-limit above this magnitude */
#define AUDIO_GAIN  3      /* fixed gain at volume=100 */
#define AUDIO_LP_HZ 4500.0 /* audio low-pass corner: matches the ~4.5 kHz roll-
                            * off seen on desktop clients for the AM passband */
#define DC_R        0.995f /* DC blocker pole (~10 Hz corner at 12 kHz) */

static app_state *s_app = NULL;
static int        s_port = -1;
static SceUID     s_thread = -1;
static volatile int s_run = 0;
static int        s_src_rate = 12000;

/* Upsampler 12 kHz -> 48 kHz. Static (not on the audio thread stack) because
 * its kernel table is ~25 KB and there is only ever one audio thread. */
static resamp    s_rs;

static int audio_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    int16_t src[OUT_GRAIN];         /* resampled (mono) samples for one grain */
    /* Double buffer: sceAudioOutOutput plays a grain asynchronously (DMA), so
     * we must not touch the buffer it is still reading. Ping-pong between two
     * so the next grain is built in the idle buffer. A single reused buffer
     * corrupted the tail of every grain -> a 93.75 Hz grain-rate buzz
     * (48000/512) that sounded metallic. */
    static int16_t outbuf[2][OUT_GRAIN * 2];
    int           bufidx = 0;
    unsigned long outputs = 0;

    /* Nominal source samples per output sample (~0.25 at 12k->48k). Drift
     * correction nudges this by <1% (inaudible) to keep the jitter buffer near
     * the target fill, absorbing the receiver's true ~11998.9 Hz vs our 12000. */
    const double base_step = (double)s_src_rate / (double)OUT_RATE;
    resamp_init(&s_rs, (double)s_src_rate, (double)OUT_RATE, AUDIO_LP_HZ);

    /* DC blocker state: AM demodulation and IMA-ADPCM both leave a slowly
     * wandering DC offset that eats headroom and biases the limiter. A single-
     * pole high-pass removes it without touching the audio band. */
    float dc_x1 = 0.0f, dc_y1 = 0.0f;

    /* Drift correction target: hold the jitter buffer near ~0.3s so latency is
     * bounded and the clock mismatch can't slowly fill or drain it. */
    const size_t target = (size_t)(s_src_rate * 3 / 10); /* 0.3s of audio */
    const size_t margin = (size_t)(s_src_rate / 20);     /* 0.05s hysteresis */

    vlog("audio_thread start: src_rate=%d out=%d step=%.4f target=%u",
         s_src_rate, OUT_RATE, base_step, (unsigned)target);

    /* Debug capture: dump the raw decoded 12 kHz mono PCM (straight from the
     * ADPCM decoder, before resampling/gain) for the first few seconds, so the
     * exact decoded audio can be analysed off-device. Written base64-encoded
     * (as text) so it can be attached in clients that reject binary files;
     * decode with `base64 -d audio_pcm.log > audio.raw`. */
    b64_enc capb;
    int cap_ok = (b64_open(&capb, "ux0:data/vitasdr/audio_pcm.log") == 0);
    b64_enc *cap = cap_ok ? &capb : NULL;
    long cap_left = (long)s_src_rate * 6; /* ~6 seconds */
    vlog("audio capture %s", cap ? "open (ux0:data/vitasdr/audio_pcm.log)" : "FAILED");

    while (s_run) {
        /* Nudge the resample step toward the target fill level. */
        size_t avail = jitter_available(&s_app->jitter);
        double step = base_step;
        if (avail > target + margin)
            step = base_step * 1.002;   /* running long -> drain slightly faster */
        else if (avail < target - margin)
            step = base_step * 0.998;   /* running short -> drain slightly slower */
        resamp_set_step(&s_rs, step);

        /* Produce one grain of resampled audio, pulling decoded PCM from the
         * jitter buffer on demand. The resampler carries history across pushes,
         * so there is no block-boundary discontinuity. */
        int produced = 0;
        while (produced < OUT_GRAIN) {
            produced += resamp_pull(&s_rs, src + produced, OUT_GRAIN - produced);
            if (produced >= OUT_GRAIN)
                break;
            int16_t raw[128];
            size_t got = jitter_pop(&s_app->jitter, raw, sizeof(raw) / sizeof(raw[0]));
            if (got == 0) {
                /* Underrun: pad the rest of the grain with silence. */
                memset(src + produced, 0,
                       (size_t)(OUT_GRAIN - produced) * sizeof(int16_t));
                if (s_app->conn_status == CONN_CONNECTED)
                    s_app->audio_underruns++;
                produced = OUT_GRAIN;
                break;
            }
            /* Remove DC / subsonic drift in place. */
            for (size_t j = 0; j < got; j++) {
                float x = (float)raw[j];
                float y = x - dc_x1 + DC_R * dc_y1;
                dc_x1 = x;
                dc_y1 = y;
                int v = (int)(y + (y >= 0.0f ? 0.5f : -0.5f));
                if (v > 32767) v = 32767;
                else if (v < -32768) v = -32768;
                raw[j] = (int16_t)v;
            }

            /* Capture the decoded PCM (after DC-blocking) to disk. */
            if (cap && cap_left > 0) {
                size_t w = (size_t)cap_left < got ? (size_t)cap_left : got;
                b64_write(cap, raw, (unsigned)(w * sizeof(int16_t)));
                cap_left -= (long)w;
                if (cap_left <= 0) {
                    b64_close(cap);
                    cap = NULL;
                    vlog("audio capture complete");
                }
            }
            resamp_push(&s_rs, raw, (int)got);
        }

        int vol = s_app->volume;
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;

        /* Simple fixed gain (volume slider). Deliberately NOT a per-block AGC:
         * updating gain every ~10 ms modulated speech and sounded metallic. */
        int gain = vol * (AUDIO_GAIN * 256) / 100;

        int16_t *out = outbuf[bufidx & 1];
        for (int i = 0; i < OUT_GRAIN; i++) {
            int s = ((int)src[i] * gain) >> 8;
            /* Soft limiter: above the knee, compress the excess 4:1 rather than
             * hard-clipping (which sounds harsh / like it cuts out). */
            if (s > SOFT_KNEE)
                s = SOFT_KNEE + (s - SOFT_KNEE) / 4;
            else if (s < -SOFT_KNEE)
                s = -SOFT_KNEE + (s + SOFT_KNEE) / 4;
            if (s > 32767) s = 32767;
            else if (s < -32768) s = -32768;
            out[i * 2]     = (int16_t)s;
            out[i * 2 + 1] = (int16_t)s;
        }

        sceAudioOutOutput(s_port, out);
        bufidx++;   /* next grain builds in the other buffer while this plays */

        if ((++outputs % 200) == 0)
            vlog("audio: outputs=%lu jitter=%u", outputs,
                 (unsigned)jitter_available(&s_app->jitter));
    }
    if (cap)
        b64_close(cap);
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
