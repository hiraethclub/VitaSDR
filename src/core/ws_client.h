/* Minimal RFC 6455 WebSocket client, written against the net.h transport.
 *
 * Portable: no Vita headers. Supports the client subset needed for SDR
 * receivers: a single connection, masked client frames, text and binary
 * messages, automatic ping/pong, and close handling. Messages are delivered
 * whole (fragmented frames are reassembled into the caller's buffer).
 *
 * Not thread-safe: one ws_client is driven from one thread. A typical layout
 * is one network thread per connection.
 */
#ifndef VITASDR_WS_CLIENT_H
#define VITASDR_WS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/* Size of the internal frame-reassembly buffer. Larger than any single SDR
 * audio or waterfall frame we expect. */
#define WS_INBUF_SIZE 65536

/* WebSocket opcodes (the data ones the caller sees). */
enum {
    WS_OP_TEXT   = 0x1,
    WS_OP_BINARY = 0x2
};

/* ws_recv result codes (in addition to a positive message length). */
enum {
    WS_NONE  = 0,   /* no complete message within the timeout */
    WS_ERROR = -1   /* connection closed or protocol error */
};

typedef struct {
    int      fd;                 /* transport handle, <0 when not connected */
    uint8_t  in[WS_INBUF_SIZE];  /* buffered bytes from the socket */
    size_t   in_len;             /* valid bytes in `in` */
} ws_client;

/* Connect to ws://host:port/path and complete the WebSocket handshake.
 * `origin` may be NULL. Returns 0 on success, <0 on failure. */
int ws_connect(ws_client *ws, const char *host, int port, const char *path,
               const char *origin, int timeout_ms);

/* Send a NUL-terminated text message. Returns 0 on success, <0 on error. */
int ws_send_text(ws_client *ws, const char *text);

/* Send a binary message. Returns 0 on success, <0 on error. */
int ws_send_binary(ws_client *ws, const void *data, size_t len);

/* Receive one complete message. Waits up to timeout_ms. On success returns the
 * message length (>0), writes the payload into `out` (up to out_cap bytes), and
 * sets *opcode to WS_OP_TEXT or WS_OP_BINARY. Returns WS_NONE if nothing
 * arrived in time, or WS_ERROR on close/error. Ping frames are answered with a
 * pong internally and do not surface to the caller. */
int ws_recv(ws_client *ws, void *out, size_t out_cap, int *opcode,
            int timeout_ms);

/* Send a close frame (best effort) and close the transport. */
void ws_close(ws_client *ws);

#endif /* VITASDR_WS_CLIENT_H */
