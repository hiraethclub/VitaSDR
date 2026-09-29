/* Minimal streaming HTTP/1.0 GET over the net.h transport.
 *
 * Portable core: used to fetch the KiwiSDR public directory (plain HTTP, no
 * TLS). The body is delivered in chunks to a sink callback so the caller can
 * parse it incrementally without buffering the whole (~1 MB) response.
 */
#ifndef VITASDR_HTTPGET_H
#define VITASDR_HTTPGET_H

#include <stddef.h>

enum {
    HTTP_OK        =  0,
    HTTP_ECONNECT  = -1,   /* TCP connect failed */
    HTTP_ESEND     = -2,   /* request send failed */
    HTTP_ENORESP   = -3,   /* no/short response before close */
    HTTP_ESTATUS   = -4,   /* non-2xx status */
    HTTP_ETIMEOUT  = -5    /* stalled */
};

/* Called for each body chunk. `len` is >0. Return 0 to continue, non-zero to
 * abort the transfer early (http_get then returns HTTP_OK). */
typedef int (*http_sink)(const char *data, size_t len, void *user);

/* GET http://host:port/path, streaming the body to `sink`. Returns HTTP_OK on a
 * 2xx response fully (or deliberately) read, else a negative HTTP_E* code. */
int http_get(const char *host, int port, const char *path,
             int timeout_ms, http_sink sink, void *user);

#endif /* VITASDR_HTTPGET_H */
