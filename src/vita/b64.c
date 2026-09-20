/* Streaming base64 encoder. See b64.h. */
#include "b64.h"

static const char T[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Encode exactly 3 bytes to 4 output chars. */
static void emit3(FILE *f, const unsigned char *b)
{
    char out[4];
    out[0] = T[b[0] >> 2];
    out[1] = T[((b[0] & 0x03) << 4) | (b[1] >> 4)];
    out[2] = T[((b[1] & 0x0f) << 2) | (b[2] >> 6)];
    out[3] = T[b[2] & 0x3f];
    fwrite(out, 1, 4, f);
}

int b64_open(b64_enc *e, const char *path)
{
    e->nrem = 0;
    e->f = fopen(path, "wb");
    return e->f ? 0 : -1;
}

void b64_write(b64_enc *e, const void *data, unsigned len)
{
    const unsigned char *p = (const unsigned char *)data;
    if (!e->f)
        return;

    /* Top up any pending remainder from a previous call. */
    while (e->nrem && e->nrem < 3 && len) {
        e->rem[e->nrem++] = *p++;
        len--;
    }
    if (e->nrem == 3) {
        emit3(e->f, e->rem);
        e->nrem = 0;
    }

    /* Bulk-encode full triples. */
    while (len >= 3) {
        emit3(e->f, p);
        p += 3;
        len -= 3;
    }

    /* Stash the tail (0..2 bytes) for next time / close. */
    while (len) {
        e->rem[e->nrem++] = *p++;
        len--;
    }
}

void b64_close(b64_enc *e)
{
    if (!e->f)
        return;

    if (e->nrem) {
        unsigned char b[3] = {0, 0, 0};
        char out[4];
        int i;
        for (i = 0; i < e->nrem; i++)
            b[i] = e->rem[i];

        out[0] = T[b[0] >> 2];
        out[1] = T[((b[0] & 0x03) << 4) | (b[1] >> 4)];
        out[2] = (e->nrem >= 2) ? T[((b[1] & 0x0f) << 2) | (b[2] >> 6)] : '=';
        out[3] = (e->nrem == 3) ? T[b[2] & 0x3f] : '=';
        fwrite(out, 1, 4, e->f);
        e->nrem = 0;
    }

    fclose(e->f);
    e->f = NULL;
}
