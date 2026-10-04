/* TLS client over net.h, using BearSSL. See tls.h. */
#include "tls.h"
#include "net.h"
#include "ca_bundle.h"

#include "bearssl.h"

#include <string.h>

/* ---- x509 "no anchor" wrapper: run the normal minimal X.509 engine (so the
 * server public key is still extracted) but treat an untrusted chain as OK.
 * This is the verify-off path. Adapted from BearSSL's tools/certs.c. ---- */
typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
} noanchor_context;

static void na_start_chain(const br_x509_class **ctx, const char *sn) {
    noanchor_context *c = (noanchor_context *)ctx;
    (*c->inner)->start_chain(c->inner, sn);
}
static void na_start_cert(const br_x509_class **ctx, uint32_t len) {
    noanchor_context *c = (noanchor_context *)ctx;
    (*c->inner)->start_cert(c->inner, len);
}
static void na_append(const br_x509_class **ctx, const unsigned char *b, size_t n) {
    noanchor_context *c = (noanchor_context *)ctx;
    (*c->inner)->append(c->inner, b, n);
}
static void na_end_cert(const br_x509_class **ctx) {
    noanchor_context *c = (noanchor_context *)ctx;
    (*c->inner)->end_cert(c->inner);
}
static unsigned na_end_chain(const br_x509_class **ctx) {
    noanchor_context *c = (noanchor_context *)ctx;
    unsigned r = (*c->inner)->end_chain(c->inner);
    if (r == BR_ERR_X509_NOT_TRUSTED)
        r = 0;
    return r;
}
static const br_x509_pkey *na_get_pkey(const br_x509_class *const *ctx, unsigned *u) {
    noanchor_context *c = (noanchor_context *)ctx;
    return (*c->inner)->get_pkey(c->inner, u);
}
static const br_x509_class na_vtable = {
    sizeof(noanchor_context),
    na_start_chain, na_start_cert, na_append, na_end_cert,
    na_end_chain, na_get_pkey
};

/* ---- the single session ---- */
struct tls_session {
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    noanchor_context       na;
    unsigned char          iobuf[BR_SSL_BUFSIZE_BIDI];
    int                    fd;
};

static struct tls_session S;
static int s_active;

/* Push any pending outgoing TLS records to the socket. Returns 0 on success,
 * <0 on a socket error. */
static int flush_out(struct tls_session *s)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&s->sc.eng);
        if (!(st & BR_SSL_SENDREC))
            return 0;
        size_t len;
        unsigned char *buf = br_ssl_engine_sendrec_buf(&s->sc.eng, &len);
        if (net_send_all(s->fd, buf, len) != 0)
            return -1;
        br_ssl_engine_sendrec_ack(&s->sc.eng, len);
    }
}

/* Feed one chunk of incoming record bytes to the engine, honoring timeout_ms.
 * Returns 1 on progress, NET_TIMEOUT, NET_CLOSED, or NET_ERR. */
static int pump_in(struct tls_session *s, int timeout_ms)
{
    size_t len;
    unsigned char *buf = br_ssl_engine_recvrec_buf(&s->sc.eng, &len);
    int n = net_recv(s->fd, buf, len, timeout_ms);
    if (n == NET_TIMEOUT)
        return NET_TIMEOUT;
    if (n == NET_CLOSED)
        return NET_CLOSED;
    if (n < 0)
        return NET_ERR;
    br_ssl_engine_recvrec_ack(&s->sc.eng, (size_t)n);
    return 1;
}

tls_session *tls_open(int fd, const char *sni, int verify, int timeout_ms)
{
    memset(&S, 0, sizeof(S));
    S.fd = fd;

    /* init_full sets up the minimal X.509 engine with our trust anchors. For
     * verify-off we leave that in place (it still parses the chain and yields
     * the server key) but swap in the no-anchor wrapper around it. */
    br_ssl_client_init_full(&S.sc, &S.xc, VITASDR_TAs, VITASDR_TAs_NUM);
    if (!verify) {
        S.na.vtable = &na_vtable;
        S.na.inner = &S.xc.vtable;
        br_ssl_engine_set_x509(&S.sc.eng, &S.na.vtable);
    }
    br_ssl_engine_set_buffer(&S.sc.eng, S.iobuf, sizeof(S.iobuf), 1);
    if (!br_ssl_client_reset(&S.sc, sni, 0))
        return NULL;
    s_active = 1;

    /* Drive the handshake to completion (engine ready for app data) or error. */
    for (int guard = 0; guard < 64; guard++) {
        unsigned st = br_ssl_engine_current_state(&S.sc.eng);
        if (st & BR_SSL_CLOSED) {
            s_active = 0;
            return NULL;   /* handshake failed (bad cert, alert, ...) */
        }
        if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))
            return &S;     /* handshake complete */
        if (st & BR_SSL_SENDREC) {
            if (flush_out(&S) != 0) { s_active = 0; return NULL; }
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            int r = pump_in(&S, timeout_ms);
            if (r == NET_TIMEOUT) continue;      /* keep waiting for the peer */
            if (r < 0)           { s_active = 0; return NULL; }
            continue;
        }
    }
    s_active = 0;
    return NULL;
}

int tls_send_all(tls_session *s, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        unsigned st = br_ssl_engine_current_state(&s->sc.eng);
        if (st & BR_SSL_CLOSED)
            return -1;
        if (st & BR_SSL_SENDAPP) {
            size_t alen;
            unsigned char *ab = br_ssl_engine_sendapp_buf(&s->sc.eng, &alen);
            size_t c = alen < len ? alen : len;
            memcpy(ab, p, c);
            br_ssl_engine_sendapp_ack(&s->sc.eng, c);
            p += c; len -= c;
            br_ssl_engine_flush(&s->sc.eng, 0);
            if (flush_out(s) != 0)
                return -1;
            continue;
        }
        if (st & BR_SSL_SENDREC) {
            if (flush_out(s) != 0)
                return -1;
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            /* handshake renegotiation / needs peer data before accepting app */
            int r = pump_in(s, 6000);
            if (r == NET_TIMEOUT) continue;
            if (r < 0) return -1;
            continue;
        }
        /* Only RECVAPP pending and output buffer full: let the caller drain it
         * via tls_recv, then retry the send. Avoid a busy spin. */
        break;
    }
    return (len == 0) ? 0 : -1;
}

int tls_recv(tls_session *s, void *buf, size_t len, int timeout_ms)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&s->sc.eng);
        if (st & BR_SSL_CLOSED)
            return (br_ssl_engine_last_error(&s->sc.eng) == 0) ? NET_CLOSED : NET_ERR;
        if (st & BR_SSL_RECVAPP) {
            size_t alen;
            unsigned char *ab = br_ssl_engine_recvapp_buf(&s->sc.eng, &alen);
            size_t c = alen < len ? alen : len;
            memcpy(buf, ab, c);
            br_ssl_engine_recvapp_ack(&s->sc.eng, c);
            return (int)c;
        }
        if (st & BR_SSL_SENDREC) {
            if (flush_out(s) != 0)
                return NET_ERR;
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            int r = pump_in(s, timeout_ms);
            if (r == NET_TIMEOUT) return NET_TIMEOUT;
            if (r == NET_CLOSED) return NET_CLOSED;
            if (r < 0)           return NET_ERR;
            continue;
        }
        /* Nothing available and nothing to do (only SENDAPP): no data yet. */
        return NET_TIMEOUT;
    }
}

void tls_close(tls_session *s)
{
    if (s && s_active) {
        br_ssl_engine_close(&s->sc.eng);
        flush_out(s);        /* best-effort close_notify */
        s_active = 0;
    }
}
