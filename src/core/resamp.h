/* Streaming fractional-rate upsampler with a windowed-sinc polyphase kernel.
 *
 * Portable core (no Vita headers): built and unit-tested on the host.
 *
 * Why this exists: the KiwiSDR audio arrives at ~12 kHz and the Vita audio
 * port runs at 48 kHz, so the stream is upsampled ~4x. Linear interpolation
 * (the first implementation) is a weak anti-imaging filter: with real receiver
 * audio, which carries strong energy right up to the 6 kHz Nyquist, it folds
 * roughly a fifth of that content back in around 6.5-9 kHz. That high-frequency
 * image is audible as a harsh "metallic" fizz. A short windowed-sinc kernel
 * (24 taps here) suppresses the image by ~40 dB, removing the artifact.
 *
 * The resampler is streaming and block-friendly: push source samples as they
 * arrive from the jitter buffer, pull as many output samples as are ready. It
 * carries history across pushes so there are no block-boundary discontinuities,
 * and it holds a fixed table built once at init (no allocation, no per-call
 * math beyond a dot product), which suits the real-time audio thread.
 */
#ifndef VITASDR_RESAMP_H
#define VITASDR_RESAMP_H

#include <stdint.h>

#define RESAMP_TAPS   24    /* source samples per output (even) */
#define RESAMP_PHASES 256   /* fractional-position quantisation */
#define RESAMP_HIST   512   /* source history capacity (samples) */

typedef struct {
    float  tab[RESAMP_PHASES][RESAMP_TAPS]; /* precomputed unity-gain kernels */
    int16_t hist[RESAMP_HIST];              /* source history (linear, shifted) */
    int     nhist;                          /* valid samples in hist */
    double  pos;                            /* read position within hist (samples) */
    double  step;                           /* source samples per output sample */
} resamp;

/* Build the kernel table and reset state. in_rate/out_rate set the nominal
 * step (source samples advanced per output). */
void resamp_init(resamp *r, double in_rate, double out_rate);

/* Clear buffered history/position (e.g. on a new stream). Keeps the table. */
void resamp_reset(resamp *r);

/* Override the step (source samples per output). Used for fine drift/rate
 * correction; the change is tiny (<1%) so it is inaudible. */
void resamp_set_step(resamp *r, double step);

/* Append n source samples to the history. The caller must pull often enough
 * that the history never overflows (samples beyond capacity are dropped). */
void resamp_push(resamp *r, const int16_t *src, int n);

/* Produce up to nout output samples. Returns the number actually produced,
 * which is less than nout when there is not yet enough buffered input. */
int  resamp_pull(resamp *r, int16_t *out, int nout);

#endif /* VITASDR_RESAMP_H */
