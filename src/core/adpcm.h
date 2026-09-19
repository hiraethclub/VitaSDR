/* IMA-ADPCM decoder (DVI/IMA variant, as used by KiwiSDR and OpenWebRX).
 *
 * Portable: no Vita headers. Decodes 4-bit ADPCM nibbles (low nibble of each
 * byte first) into signed 16-bit PCM samples.
 *
 * The decoder carries state (predictor + step index) across calls, so a single
 * adpcm_state must be used for one continuous stream. Call adpcm_reset()
 * whenever the server begins a new stream (e.g. a mode change), otherwise the
 * predictor will be wrong for the first samples of the new stream.
 */
#ifndef VITASDR_ADPCM_H
#define VITASDR_ADPCM_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t predictor; /* current predicted sample, clamped to int16 range */
    int32_t index;     /* index into the step-size table, 0..88 */
} adpcm_state;

/* Reset the decoder to its initial state. Call before decoding a new stream. */
void adpcm_reset(adpcm_state *st);

/* Decode `in_len` bytes of ADPCM (two samples per byte) into `out`.
 * `out` must hold at least in_len*2 int16 samples. Returns the number of
 * samples written (== in_len*2). */
size_t adpcm_decode(adpcm_state *st, const uint8_t *in, size_t in_len,
                    int16_t *out);

#endif /* VITASDR_ADPCM_H */
