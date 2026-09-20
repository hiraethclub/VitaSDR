/* IMA-ADPCM decoder. See adpcm.h. */
#include "adpcm.h"

/* Standard IMA/DVI ADPCM tables. */
static const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

void adpcm_reset(adpcm_state *st)
{
    st->predictor = 0;
    st->index = 0;
}

/* Decode a single 4-bit nibble, advancing the decoder state. */
static int16_t decode_nibble(adpcm_state *st, uint8_t nibble)
{
    int step = step_table[st->index];
    int diff = step >> 3;

    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;

    if (nibble & 8)
        st->predictor -= diff;
    else
        st->predictor += diff;

    if (st->predictor > 32767)
        st->predictor = 32767;
    else if (st->predictor < -32768)
        st->predictor = -32768;

    st->index += index_table[nibble & 0x0f];
    if (st->index < 0)
        st->index = 0;
    else if (st->index > 88)
        st->index = 88;

    return (int16_t)st->predictor;
}

size_t adpcm_decode(adpcm_state *st, const uint8_t *in, size_t in_len,
                    int16_t *out)
{
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        uint8_t byte = in[i];
        /* Low nibble first, then high nibble (KiwiSDR / OpenWebRX order). */
        out[n++] = decode_nibble(st, byte & 0x0f);
        out[n++] = decode_nibble(st, (byte >> 4) & 0x0f);
    }
    return n;
}
