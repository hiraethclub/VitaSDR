/* POSIX BSD-socket implementation of the transport interface (net.h).
 * Used by the native test build. Not compiled on the Vita. */
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int net_global_init(void)
{
    return 0; /* nothing to do on POSIX */
}

void net_global_fini(void)
{
}

static int set_nonblocking(int fd, int nonblocking)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (nonblocking)
        flags |= O_NONBLOCK;
    else
        flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

int net_tcp_connect(const char *host, int port, int timeout_ms)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return NET_ERR;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        set_nonblocking(fd, 1);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0)
            goto connected;

        if (errno == EINPROGRESS) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(fd, &wf);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            rc = select(fd + 1, NULL, &wf, NULL, &tv);
            if (rc > 0) {
                int err = 0;
                socklen_t elen = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 &&
                    err == 0)
                    goto connected;
            }
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return NET_ERR;

connected:
    set_nonblocking(fd, 0);
    {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    freeaddrinfo(res);
    return fd;
}

int net_send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                             errno == EWOULDBLOCK)) {
            continue;
        } else {
            return NET_ERR;
        }
    }
    return 0;
}

int net_recv(int fd, void *buf, size_t len, int timeout_ms)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = select(fd + 1, &rf, NULL, NULL, &tv);
    if (rc == 0)
        return NET_TIMEOUT;
    if (rc < 0)
        return (errno == EINTR) ? NET_TIMEOUT : NET_ERR;

    ssize_t n = recv(fd, buf, len, 0);
    if (n > 0)
        return (int)n;
    if (n == 0)
        return NET_CLOSED;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return NET_TIMEOUT;
    return NET_ERR;
}

void net_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
