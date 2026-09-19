/* Bounded audio jitter buffer.
 *
 * A fixed-size ring of int16 PCM samples, allocated once at init. The network
 * thread pushes decoded samples; the audio thread pulls fixed-size chunks.
 *
 * On overflow the oldest samples are dropped so the newest audio is always
 * kept and the producer never blocks. This is single-producer/single-consumer
 * safe on platforms with atomic word-sized loads/stores (the Vita's Cortex-A9
 * qualifies for aligned 32-bit access); the read/write indices are the only
 * shared state.
 */
#ifndef VITASDR_JITTER_H
#define VITASDR_JITTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int16_t *buf;      /* sample storage, capacity+1 slots */
    size_t   cap;      /* usable capacity in samples */
    volatile size_t r; /* read index */
    volatile size_t w; /* write index */
} jitter_buf;

/* Allocate the ring to hold `capacity` samples. Returns 0 on success, -1 on
 * allocation failure. This is the one heap allocation permitted after startup;
 * call it once at init. */
int jitter_init(jitter_buf *jb, size_t capacity);

/* Free the ring. */
void jitter_free(jitter_buf *jb);

/* Discard all buffered samples (e.g. on reconnect or stream reset). */
void jitter_clear(jitter_buf *jb);

/* Number of samples currently available to read. */
size_t jitter_available(const jitter_buf *jb);

/* Push `n` samples. If the ring would overflow, the oldest samples are dropped
 * to make room. Never blocks. Returns the number of samples dropped. */
size_t jitter_push(jitter_buf *jb, const int16_t *samples, size_t n);

/* Pop up to `n` samples into `out`. Returns the number actually written. */
size_t jitter_pop(jitter_buf *jb, int16_t *out, size_t n);

#endif /* VITASDR_JITTER_H */
