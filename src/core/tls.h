/* Minimal TLS client, layered on top of the net.h transport.
 *
 * Portable core (BearSSL under the hood, no Vita headers). OpenWebRX receivers
 * are usually fronted by HTTPS, so the WebSocket must run over TLS (wss://);
 * KiwiSDR uses plain ws:// and does not need this.
 *
 * A session wraps an already-connected net.h fd: tls_open() performs the TLS
 * handshake, then tls_send_all()/tls_recv() replace net_send_all()/net_recv()
 * for the rest of the connection. tls_recv() returns the same sentinels as
 * net_recv() (NET_CLOSED / NET_ERR / NET_TIMEOUT) so callers can treat a TLS
 * stream exactly like a plain one.
 *
 * Single session at a time (one network thread drives one connection). The
 * session and its ~33 KB I/O buffer are a file-local static, so there is no
 * dynamic allocation.
 */
#ifndef VITASDR_TLS_H
#define VITASDR_TLS_H

#include <stddef.h>

typedef struct tls_session tls_session;

/* Wrap a connected TCP fd in TLS and complete the handshake. `sni` is the
 * server hostname (for SNI and certificate matching). If `verify` is nonzero
 * the server certificate chain is validated against the bundled CA set;
 * otherwise the handshake proceeds without trust-anchor checks (the escape
 * hatch for a device where verification misbehaves). `timeout_ms` bounds each
 * network read during the handshake. Returns a session handle on success (a
 * static singleton) or NULL on failure. Does not take ownership of `fd`. */
tls_session *tls_open(int fd, const char *sni, int verify, int timeout_ms);

/* Send the entire buffer over TLS. Returns 0 on success, <0 on error. */
int tls_send_all(tls_session *s, const void *buf, size_t len);

/* Receive up to `len` bytes of decrypted application data, waiting at most
 * timeout_ms. Returns the number of bytes read (>0), or NET_CLOSED(0) /
 * NET_ERR(-1) / NET_TIMEOUT(-2) exactly like net_recv. */
int tls_recv(tls_session *s, void *buf, size_t len, int timeout_ms);

/* Tear down the TLS session. Does NOT close the underlying fd (the caller owns
 * it and closes it via net_close). */
void tls_close(tls_session *s);

#endif /* VITASDR_TLS_H */
