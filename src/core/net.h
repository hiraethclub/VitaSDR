/* Transport interface: a thin TCP layer the portable core is written against.
 *
 * The core (ws_client, kiwi, owrx) never calls sockets directly; it calls these
 * functions. Two implementations exist:
 *   - net_posix.c : BSD sockets, used by the native test build (vitasdr_test).
 *   - vita/net.c  : SceNet wrappers, used on the Vita.
 * Both present these identical signatures, so the core is platform-agnostic.
 *
 * A connection is identified by a non-negative int handle (a real fd on POSIX,
 * a SceNet socket id on the Vita).
 */
#ifndef VITASDR_NET_H
#define VITASDR_NET_H

#include <stddef.h>

/* Result codes for net_recv. */
enum {
    NET_CLOSED  = 0,   /* peer closed the connection */
    NET_ERR     = -1,  /* fatal socket error */
    NET_TIMEOUT = -2   /* no data within the timeout, connection still open */
};

/* Which stage of net_tcp_connect last failed (for diagnostics). */
enum {
    NET_STAGE_NONE = 0,
    NET_STAGE_RESOLVE,   /* DNS lookup failed */
    NET_STAGE_SOCKET,    /* socket() failed */
    NET_STAGE_CONNECT    /* connect() failed / timed out */
};

/* Returns the stage recorded by the most recent failed net_tcp_connect. */
int net_last_fail_stage(void);

/* One-time global init/teardown. On POSIX these are no-ops; on the Vita they
 * load the net module and bring up the stack. Return 0 on success, <0 on error. */
int  net_global_init(void);
void net_global_fini(void);

/* Open a TCP connection to host:port, giving up after timeout_ms. Returns a
 * non-negative handle on success, <0 on failure. */
int  net_tcp_connect(const char *host, int port, int timeout_ms);

/* Send the entire buffer. Returns 0 on success, <0 if the connection failed. */
int  net_send_all(int fd, const void *buf, size_t len);

/* Read up to `len` bytes, waiting at most timeout_ms. Returns the number of
 * bytes read (>0), or one of NET_CLOSED / NET_ERR / NET_TIMEOUT. */
int  net_recv(int fd, void *buf, size_t len, int timeout_ms);

/* Close a connection handle. Safe to call with a negative handle (no-op). */
void net_close(int fd);

#endif /* VITASDR_NET_H */
