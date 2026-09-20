/* Minimal streaming base64 encoder to a file. Used to write binary debug
 * captures as ASCII text so they can be transferred through channels that only
 * accept text files. */
#ifndef VITASDR_B64_H
#define VITASDR_B64_H

#include <stdio.h>

typedef struct {
    FILE         *f;
    unsigned char rem[3];
    int           nrem;
} b64_enc;

int  b64_open(b64_enc *e, const char *path);   /* 0 on success */
void b64_write(b64_enc *e, const void *data, unsigned len);
void b64_close(b64_enc *e);                    /* flush padding + close */

#endif /* VITASDR_B64_H */
