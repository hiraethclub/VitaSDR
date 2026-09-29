/* See httpget.h. */
#include "httpget.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int http_get(const char *host, int port, const char *path,
             int timeout_ms, http_sink sink, void *user)
{
    int fd = net_tcp_connect(host, port, timeout_ms);
    if (fd < 0)
        return HTTP_ECONNECT;

    /* HTTP/1.0 + Connection: close: the server sends an identity body and
     * closes when done, so we need not parse chunked transfer-encoding. */
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.0\r\n"
                     "Host: %s\r\n"
                     "User-Agent: VitaSDR\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n\r\n",
                     path, host);
    if (n <= 0 || net_send_all(fd, req, (size_t)n) != 0) {
        net_close(fd);
        return HTTP_ESEND;
    }

    char buf[4096];
    int  header_done = 0;
    int  status_ok = 0;
    /* Small rolling window to find the CRLFCRLF header terminator even when it
     * straddles two recv() buffers. */
    char tail[4] = {0};
    int  taillen = 0;
    int  got_any = 0;

    for (;;) {
        int r = net_recv(fd, buf, sizeof(buf), timeout_ms);
        if (r == NET_TIMEOUT) {
            net_close(fd);
            return got_any ? HTTP_OK : HTTP_ETIMEOUT;
        }
        if (r == NET_CLOSED) break;
        if (r < 0) {
            net_close(fd);
            return got_any ? HTTP_OK : HTTP_ENORESP;
        }
        got_any = 1;

        int off = 0;
        if (!header_done) {
            /* Scan this buffer for the end of headers. Combine with `tail` so a
             * split "\r\n\r\n" is still found. */
            for (int i = 0; i < r; i++) {
                char c = buf[i];
                /* maintain last-4 window */
                if (taillen < 4) tail[taillen++] = c;
                else { tail[0]=tail[1]; tail[1]=tail[2]; tail[2]=tail[3]; tail[3]=c; }
                if (taillen == 4 && tail[0]=='\r' && tail[1]=='\n' &&
                    tail[2]=='\r' && tail[3]=='\n') {
                    header_done = 1;
                    off = i + 1;
                    break;
                }
            }
            if (!status_ok) {
                /* Status line is at the very start: "HTTP/1.x NNN ...". */
                if (r >= 12 && strncmp(buf, "HTTP/", 5) == 0) {
                    int code = 0;
                    /* find first space then parse 3-digit code */
                    const char *sp = memchr(buf, ' ', (size_t)r);
                    if (sp) code = (int)strtol(sp + 1, NULL, 10);
                    if (code >= 200 && code < 300) status_ok = 1;
                    else { net_close(fd); return HTTP_ESTATUS; }
                }
            }
            if (!header_done)
                continue;   /* still in headers */
        }

        if (header_done && off < r) {
            if (sink(buf + off, (size_t)(r - off), user) != 0) {
                net_close(fd);
                return HTTP_OK;   /* caller asked to stop */
            }
        }
    }

    net_close(fd);
    if (!header_done)
        return HTTP_ENORESP;
    return HTTP_OK;
}
