/* See resamp.h. */
#include "resamp.h"

#include <math.h>
#include <string.h>

#define HALF (RESAMP_TAPS / 2)

/* Cutoff of the anti-imaging low-pass, as a fraction of the SOURCE sample rate.
 * The source Nyquist is 0.5; 0.467 (~5.6 kHz at 12 kHz) passes essentially all
 * of the receiver audio while placing the stopband over the image region that
 * begins just above 6 kHz. */
#define FC_SRC 0.46667

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Normalised sinc: sin(pi*x)/(pi*x), =1 at x=0. */
static double sinc_pi(double x)
{
    if (x > -1e-9 && x < 1e-9)
        return 1.0;
    double a = M_PI * x;
    return sin(a) / a;
}

/* Modified Bessel function of the first kind, order 0 (for the Kaiser window).
 * Series expansion; converges quickly for the arguments used here. */
static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    double halfx = x * 0.5;
    int k;
    for (k = 1; k < 40; k++) {
        double t = halfx / (double)k;
        term *= t * t;
        sum += term;
        if (term < sum * 1e-12)
            break;
    }
    return sum;
}

void resamp_init(resamp *r, double in_rate, double out_rate)
{
    const double beta = 8.0;           /* Kaiser beta: ~ -60 dB sidelobes */
    double i0b = bessel_i0(beta);
    int p, k;

    for (p = 0; p < RESAMP_PHASES; p++) {
        double frac = (double)p / (double)RESAMP_PHASES;
        double sum = 0.0;
        for (k = 0; k < RESAMP_TAPS; k++) {
            /* Tap offset relative to the interpolation point, in source
             * samples: taps span [-(HALF-1) .. HALF]. */
            double u = (double)(k - (HALF - 1)) - frac;
            double s = 2.0 * FC_SRC * sinc_pi(2.0 * FC_SRC * u);
            /* Kaiser window over the tap span (|u| <= HALF). */
            double ratio = u / (double)HALF;
            double arg = 1.0 - ratio * ratio;
            double w = (arg > 0.0)
                       ? bessel_i0(beta * sqrt(arg)) / i0b
                       : 0.0;
            double h = s * w;
            r->tab[p][k] = (float)h;
            sum += h;
        }
        /* Normalise this phase to unity DC gain. */
        if (sum != 0.0) {
            float inv = (float)(1.0 / sum);
            for (k = 0; k < RESAMP_TAPS; k++)
                r->tab[p][k] *= inv;
        }
    }

    resamp_set_step(r, in_rate / out_rate);
    resamp_reset(r);
}

void resamp_reset(resamp *r)
{
    r->nhist = 0;
    /* Start the read point at the first fully-supported interpolation index so
     * the earliest outputs have a complete left wing of real samples. */
    r->pos = (double)(HALF - 1);
}

void resamp_set_step(resamp *r, double step)
{
    if (step > 0.0)
        r->step = step;
}

void resamp_push(resamp *r, const int16_t *src, int n)
{
    if (n <= 0)
        return;
    if (n > RESAMP_HIST - r->nhist)
        n = RESAMP_HIST - r->nhist;   /* clamp: caller should pull more often */
    if (n <= 0)
        return;
    memcpy(r->hist + r->nhist, src, (size_t)n * sizeof(int16_t));
    r->nhist += n;
}

int resamp_pull(resamp *r, int16_t *out, int nout)
{
    int produced = 0;

    while (produced < nout) {
        int i0 = (int)r->pos;
        double frac = r->pos - (double)i0;
        int p = (int)(frac * RESAMP_PHASES + 0.5);
        if (p >= RESAMP_PHASES) {   /* rounded up to the next sample */
            p = 0;
            i0 += 1;
        }
        int base = i0 - (HALF - 1);

        /* Need the full tap span present: base .. base+TAPS-1 (== i0+HALF). */
        if (base < 0)
            break;                  /* not warmed up yet */
        if (i0 + HALF > r->nhist - 1)
            break;                  /* not enough lookahead buffered */

        const float *h = r->tab[p];
        const int16_t *s = r->hist + base;
        float acc = 0.0f;
        int k;
        for (k = 0; k < RESAMP_TAPS; k++)
            acc += h[k] * (float)s[k];

        int v = (int)(acc + (acc >= 0.0f ? 0.5f : -0.5f));
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        out[produced++] = (int16_t)v;

        r->pos += r->step;

        /* Retire history no longer needed as a left wing, keeping the buffer
         * bounded. Everything before (floor(pos) - (HALF-1)) is spent. */
        int drop = (int)r->pos - (HALF - 1);
        if (drop > 0) {
            if (drop > r->nhist) drop = r->nhist;
            memmove(r->hist, r->hist + drop,
                    (size_t)(r->nhist - drop) * sizeof(int16_t));
            r->nhist -= drop;
            r->pos -= drop;
        }
    }

    return produced;
}
