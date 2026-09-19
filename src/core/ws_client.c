/* Minimal RFC 6455 WebSocket client. See ws_client.h. */
#include "ws_client.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* SHA-1 (used only to validate the handshake Sec-WebSocket-Accept).    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t h[5];
    uint64_t len;      /* message length in bytes */
    uint8_t  block[64];
    size_t   block_len;
} sha1_ctx;

static uint32_t rol32(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

static void sha1_process(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = c->h[0], b = c->h[1], d = c->h[2], e = c->h[3], f = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t k, t;
        if (i < 20)      { t = (b & d) | (~b & e);            k = 0x5A827999; }
        else if (i < 40) { t = b ^ d ^ e;                     k = 0x6ED9EBA1; }
        else if (i < 60) { t = (b & d) | (b & e) | (d & e);   k = 0x8F1BBCDC; }
        else             { t = b ^ d ^ e;                     k = 0xCA62C1D6; }
        uint32_t tmp = rol32(a, 5) + t + f + k + w[i];
        f = e; e = d; d = rol32(b, 30); b = a; a = tmp;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;
}

static void sha1_init(sha1_ctx *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->len = 0; c->block_len = 0;
}

static void sha1_update(sha1_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    c->len += n;
    while (n) {
        size_t take = 64 - c->block_len;
        if (take > n) take = n;
        memcpy(c->block + c->block_len, p, take);
        c->block_len += take; p += take; n -= take;
        if (c->block_len == 64) { sha1_process(c, c->block); c->block_len = 0; }
    }
}

static void sha1_final(sha1_ctx *c, uint8_t out[20])
{
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->block_len != 56)
        sha1_update(c, &zero, 1);
    uint8_t lenbuf[8];
    for (int i = 0; i < 8; i++)
        lenbuf[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(c, lenbuf, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4]     = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = (int)(n - i);
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        out[o++] = b64_chars[(v >> 18) & 0x3f];
        out[o++] = b64_chars[(v >> 12) & 0x3f];
        out[o++] = (rem > 1) ? b64_chars[(v >> 6) & 0x3f] : '=';
        out[o++] = (rem > 2) ? b64_chars[v & 0x3f] : '=';
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Handshake                                                            */
/* ------------------------------------------------------------------ */

static void compute_accept(const char *key, char accept_out[32])
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    sha1_ctx c;
    uint8_t digest[20];
    sha1_init(&c);
    sha1_update(&c, key, strlen(key));
    sha1_update(&c, guid, strlen(guid));
    sha1_final(&c, digest);
    base64_encode(digest, 20, accept_out);
}

int ws_connect(ws_client *ws, const char *host, int port, const char *path,
               const char *origin, int timeout_ms)
{
    ws->fd = -1;
    ws->in_len = 0;

    int fd = net_tcp_connect(host, port, timeout_ms);
    if (fd < 0)
        return WS_CONNECT_ETCP;

    /* Build a client key: 16 pseudo-random bytes, base64-encoded. This does
     * not need cryptographic strength; it only echoes back in the accept. */
    uint8_t rnd[16];
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)(size_t)ws;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1103515245u + 12345u;
        rnd[i] = (uint8_t)(seed >> 16);
    }
    char key[32];
    base64_encode(rnd, 16, key);

    char req[512];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "%s%s%s"
        "\r\n",
        path, host, port, key,
        origin ? "Origin: " : "", origin ? origin : "", origin ? "\r\n" : "");
    if (n < 0 || (size_t)n >= sizeof(req)) {
        net_close(fd);
        return WS_CONNECT_ESEND;
    }

    if (net_send_all(fd, req, (size_t)n) != 0) {
        net_close(fd);
        return WS_CONNECT_ESEND;
    }

    /* Read the response headers up to the terminating blank line. */
    char resp[2048];
    size_t rlen = 0;
    int header_end = -1;
    while (rlen < sizeof(resp) - 1) {
        int r = net_recv(fd, resp + rlen, sizeof(resp) - 1 - rlen, timeout_ms);
        if (r == NET_TIMEOUT)
            continue;
        if (r <= 0) {
            net_close(fd);
            return WS_CONNECT_ENORESP;
        }
        rlen += (size_t)r;
        resp[rlen] = '\0';
        char *end = strstr(resp, "\r\n\r\n");
        if (end) {
            header_end = (int)(end - resp) + 4;
            break;
        }
    }
    if (header_end < 0) {
        net_close(fd);
        return WS_CONNECT_ENORESP;
    }

    /* Require a 101 status. */
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0 &&
        strncmp(resp, "HTTP/1.0 101", 12) != 0) {
        net_close(fd);
        return WS_CONNECT_ESTATUS;
    }

    /* If the server sent Sec-WebSocket-Accept, verify it. */
    const char *acc = strstr(resp, "Sec-WebSocket-Accept:");
    if (!acc) acc = strstr(resp, "sec-websocket-accept:");
    if (acc) {
        acc += strlen("Sec-WebSocket-Accept:");
        while (*acc == ' ') acc++;
        char got[64];
        size_t gi = 0;
        while (*acc && *acc != '\r' && *acc != '\n' && gi < sizeof(got) - 1)
            got[gi++] = *acc++;
        got[gi] = '\0';
        char want[32];
        compute_accept(key, want);
        if (strcmp(got, want) != 0) {
            net_close(fd);
            return WS_CONNECT_EACCEPT;
        }
    }

    /* Any bytes past the header belong to the WebSocket stream; keep them. */
    size_t extra = rlen - (size_t)header_end;
    if (extra > 0 && extra <= WS_INBUF_SIZE) {
        memcpy(ws->in, resp + header_end, extra);
        ws->in_len = extra;
    }

    ws->dbg_rx_bytes = (unsigned)extra;
    ws->dbg_close_frame = 0;
    ws->dbg_net_result = 99;
    ws->dbg_first_len = extra < sizeof(ws->dbg_first) ? (unsigned)extra
                                                      : sizeof(ws->dbg_first);
    memcpy(ws->dbg_first, ws->in, ws->dbg_first_len);
    ws->fd = fd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Frame I/O                                                            */
/* ------------------------------------------------------------------ */

static int ws_send_frame(ws_client *ws, int opcode, const void *data,
                         size_t len)
{
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | (opcode & 0x0f)); /* FIN + opcode */

    if (len < 126) {
        hdr[h++] = (uint8_t)(0x80 | len);         /* MASK bit + len */
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)(len);
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            hdr[h++] = (uint8_t)((uint64_t)len >> (i * 8));
    }

    /* Mask key. */
    uint8_t mask[4];
    static unsigned mseed = 0x12345678u;
    for (int i = 0; i < 4; i++) {
        mseed = mseed * 1103515245u + 12345u;
        mask[i] = (uint8_t)(mseed >> 17);
        hdr[h++] = mask[i];
    }

    if (net_send_all(ws->fd, hdr, h) != 0)
        return -1;

    /* Send the payload masked, in chunks so we never need a big temp buffer. */
    const uint8_t *p = (const uint8_t *)data;
    uint8_t chunk[1024];
    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > sizeof(chunk)) take = sizeof(chunk);
        for (size_t i = 0; i < take; i++)
            chunk[i] = (uint8_t)(p[off + i] ^ mask[(off + i) & 3]);
        if (net_send_all(ws->fd, chunk, take) != 0)
            return -1;
        off += take;
    }
    return 0;
}

int ws_send_text(ws_client *ws, const char *text)
{
    if (ws->fd < 0) return -1;
    return ws_send_frame(ws, WS_OP_TEXT, text, strlen(text));
}

int ws_send_binary(ws_client *ws, const void *data, size_t len)
{
    if (ws->fd < 0) return -1;
    return ws_send_frame(ws, WS_OP_BINARY, data, len);
}

/* Ensure at least `need` bytes are buffered in ws->in, reading from the socket
 * as required. Returns 0 on success, WS_ERROR on close/error, WS_NONE on
 * timeout with insufficient data. */
static int ensure_buffered(ws_client *ws, size_t need, int timeout_ms)
{
    while (ws->in_len < need) {
        if (ws->in_len >= WS_INBUF_SIZE)
            return WS_ERROR; /* frame larger than our buffer */
        uint8_t *dst = ws->in + ws->in_len;
        int r = net_recv(ws->fd, dst, WS_INBUF_SIZE - ws->in_len, timeout_ms);
        if (r == NET_TIMEOUT)
            return WS_NONE;
        if (r <= 0) {
            ws->dbg_net_result = r;
            return WS_ERROR;
        }
        ws->in_len += (size_t)r;
        ws->dbg_rx_bytes += (unsigned)r;
        for (int i = 0; i < r && ws->dbg_first_len < sizeof(ws->dbg_first); i++)
            ws->dbg_first[ws->dbg_first_len++] = dst[i];
    }
    return 0;
}

/* Drop `n` consumed bytes from the front of the buffer. */
static void consume(ws_client *ws, size_t n)
{
    if (n >= ws->in_len) {
        ws->in_len = 0;
    } else {
        memmove(ws->in, ws->in + n, ws->in_len - n);
        ws->in_len -= n;
    }
}

int ws_recv(ws_client *ws, void *out, size_t out_cap, int *opcode,
            int timeout_ms)
{
    if (ws->fd < 0)
        return WS_ERROR;

    size_t out_len = 0;
    int msg_opcode = 0;

    for (;;) {
        /* Minimum header is 2 bytes. */
        int rc = ensure_buffered(ws, 2, timeout_ms);
        if (rc != 0)
            return rc; /* WS_NONE or WS_ERROR */

        uint8_t b0 = ws->in[0];
        uint8_t b1 = ws->in[1];
        int fin = (b0 & 0x80) != 0;
        int op = b0 & 0x0f;
        int masked = (b1 & 0x80) != 0;
        uint64_t plen = b1 & 0x7f;
        size_t hdr = 2;

        if (plen == 126) {
            rc = ensure_buffered(ws, 4, timeout_ms);
            if (rc != 0) return rc;
            plen = ((uint64_t)ws->in[2] << 8) | ws->in[3];
            hdr = 4;
        } else if (plen == 127) {
            rc = ensure_buffered(ws, 10, timeout_ms);
            if (rc != 0) return rc;
            plen = 0;
            for (int i = 0; i < 8; i++)
                plen = (plen << 8) | ws->in[2 + i];
            hdr = 10;
        }

        size_t mask_off = hdr;
        if (masked)
            hdr += 4;

        if (hdr + plen > WS_INBUF_SIZE)
            return WS_ERROR; /* frame too large for buffer */

        rc = ensure_buffered(ws, hdr + (size_t)plen, timeout_ms);
        if (rc != 0)
            return rc;

        uint8_t *payload = ws->in + hdr;
        if (masked) {
            const uint8_t *mk = ws->in + mask_off;
            for (uint64_t i = 0; i < plen; i++)
                payload[i] ^= mk[i & 3];
        }

        if (op == 0x8) {                 /* close */
            ws->dbg_close_frame = 1;
            consume(ws, hdr + (size_t)plen);
            return WS_ERROR;
        } else if (op == 0x9) {          /* ping -> pong */
            ws_send_frame(ws, 0xA, payload, (size_t)plen);
            consume(ws, hdr + (size_t)plen);
            continue;
        } else if (op == 0xA) {          /* pong: ignore */
            consume(ws, hdr + (size_t)plen);
            continue;
        }

        /* Data frame (text/binary) or continuation. */
        if (op != 0x0)
            msg_opcode = op;

        size_t copy = (size_t)plen;
        if (out_len + copy > out_cap)
            copy = (out_cap > out_len) ? (out_cap - out_len) : 0;
        if (copy > 0) {
            memcpy((uint8_t *)out + out_len, payload, copy);
            out_len += copy;
        }
        consume(ws, hdr + (size_t)plen);

        if (fin) {
            if (opcode)
                *opcode = msg_opcode;
            return (int)out_len;
        }
        /* else: continuation frame follows; keep looping. */
    }
}

void ws_close(ws_client *ws)
{
    if (ws->fd >= 0) {
        uint8_t empty = 0;
        ws_send_frame(ws, 0x8, &empty, 0);
        net_close(ws->fd);
        ws->fd = -1;
    }
    ws->in_len = 0;
}
