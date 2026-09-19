/* Bounded audio jitter buffer. See jitter.h. */
#include "jitter.h"

#include <stdlib.h>

int jitter_init(jitter_buf *jb, size_t capacity)
{
    /* One extra slot so full/empty are distinguishable. */
    jb->buf = (int16_t *)malloc((capacity + 1) * sizeof(int16_t));
    if (!jb->buf)
        return -1;
    jb->cap = capacity;
    jb->r = 0;
    jb->w = 0;
    return 0;
}

void jitter_free(jitter_buf *jb)
{
    free(jb->buf);
    jb->buf = NULL;
    jb->cap = 0;
    jb->r = 0;
    jb->w = 0;
}

void jitter_clear(jitter_buf *jb)
{
    jb->r = jb->w;
}

size_t jitter_available(const jitter_buf *jb)
{
    size_t r = jb->r;
    size_t w = jb->w;
    size_t slots = jb->cap + 1;
    return (w + slots - r) % slots;
}

static size_t jitter_free_space(const jitter_buf *jb)
{
    return jb->cap - jitter_available(jb);
}

size_t jitter_push(jitter_buf *jb, const int16_t *samples, size_t n)
{
    size_t slots = jb->cap + 1;
    size_t dropped = 0;

    /* Can never store more than the whole ring; keep only the newest. */
    if (n > jb->cap) {
        samples += (n - jb->cap);
        dropped += (n - jb->cap);
        n = jb->cap;
    }

    /* Drop oldest samples if there is not enough room for the new ones. */
    size_t space = jitter_free_space(jb);
    if (n > space) {
        size_t need = n - space;
        jb->r = (jb->r + need) % slots;
        dropped += need;
    }

    size_t w = jb->w;
    for (size_t i = 0; i < n; i++) {
        jb->buf[w] = samples[i];
        w = (w + 1) % slots;
    }
    jb->w = w;

    return dropped;
}

size_t jitter_pop(jitter_buf *jb, int16_t *out, size_t n)
{
    size_t slots = jb->cap + 1;
    size_t avail = jitter_available(jb);
    if (n > avail)
        n = avail;

    size_t r = jb->r;
    for (size_t i = 0; i < n; i++) {
        out[i] = jb->buf[r];
        r = (r + 1) % slots;
    }
    jb->r = r;

    return n;
}
