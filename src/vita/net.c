/* SceNet implementation of the transport interface (core/net.h).
 *
 * Vita-only. Provides the same signatures the portable core is written
 * against, backed by the Vita's BSD-style SceNet API. Connection handles are
 * SceNet socket ids.
 *
 * NOTE: written to the VitaSDK SceNet API but not yet verified on hardware.
 */
#include "net.h"

#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <psp2/kernel/clib.h>

#include <string.h>

#define NET_POOL_SIZE (1 * 1024 * 1024)

static char  s_net_pool[NET_POOL_SIZE] __attribute__((aligned(16)));
static int   s_net_up = 0;
static int   s_fail_stage = NET_STAGE_NONE;

int net_last_fail_stage(void)
{
    return s_fail_stage;
}

int net_global_init(void)
{
    if (s_net_up)
        return 0;

    int ret = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (ret < 0)
        return NET_ERR;

    SceNetInitParam param;
    param.memory = s_net_pool;
    param.size = NET_POOL_SIZE;
    param.flags = 0;
    ret = sceNetInit(&param);
    if (ret < 0 && ret != (int)SCE_NET_ERROR_EBUSY /* already initialised */)
        return NET_ERR;

    sceNetCtlInit();
    s_net_up = 1;
    return 0;
}

void net_global_fini(void)
{
    if (!s_net_up)
        return;
    sceNetCtlTerm();
    sceNetTerm();
    sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
    s_net_up = 0;
}

/* Resolve a hostname (or dotted-quad) to a 32-bit network-order address. */
static int resolve_host(const char *host, SceNetInAddr *out)
{
    /* Try dotted-quad first. */
    if (sceNetInetPton(SCE_NET_AF_INET, host, out) == 1)
        return 0;

    int rid = sceNetResolverCreate("vitasdr", NULL, 0);
    if (rid < 0)
        return NET_ERR;
    /* Finite timeout (microseconds) and retries so a dead/unsupported resolver
     * fails cleanly instead of hanging the connect forever. */
    int ret = sceNetResolverStartNtoa(rid, host, out, 5 * 1000 * 1000, 2, 0);
    sceNetResolverDestroy(rid);
    return (ret < 0) ? NET_ERR : 0;
}

int net_tcp_connect(const char *host, int port, int timeout_ms)
{
    s_fail_stage = NET_STAGE_NONE;

    SceNetInAddr addr;
    if (resolve_host(host, &addr) != 0) {
        s_fail_stage = NET_STAGE_RESOLVE;
        return NET_ERR;
    }

    int sock = sceNetSocket("vitasdr_tcp", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM,
                            0);
    if (sock < 0) {
        s_fail_stage = NET_STAGE_SOCKET;
        return NET_ERR;
    }

    /* Bound connect wait: use non-blocking connect + epoll for writability. */
    int nb = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb));

    SceNetSockaddrIn sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = sceNetHtons((unsigned short)port);
    sa.sin_addr = addr;

    int ret = sceNetConnect(sock, (SceNetSockaddr *)&sa, sizeof(sa));
    if (ret < 0) {
        /* In progress: wait for writability via epoll. */
        int ep = sceNetEpollCreate("vitasdr_ep", 0);
        if (ep < 0) {
            s_fail_stage = NET_STAGE_CONNECT;
            sceNetSocketClose(sock);
            return NET_ERR;
        }
        SceNetEpollEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = SCE_NET_EPOLLOUT;
        ev.data.fd = sock;
        sceNetEpollControl(ep, SCE_NET_EPOLL_CTL_ADD, sock, &ev);

        SceNetEpollEvent out_ev;
        memset(&out_ev, 0, sizeof(out_ev));
        int n = sceNetEpollWait(ep, &out_ev, 1, timeout_ms * 1000);
        sceNetEpollDestroy(ep);
        if (n <= 0) {
            s_fail_stage = NET_STAGE_CONNECT;
            sceNetSocketClose(sock);
            return NET_ERR;
        }

        /* Check the connection actually succeeded. */
        int err = 0;
        unsigned int elen = sizeof(err);
        sceNetGetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &err, &elen);
        if (err != 0) {
            s_fail_stage = NET_STAGE_CONNECT;
            sceNetSocketClose(sock);
            return NET_ERR;
        }
    }

    /* Back to blocking, with TCP_NODELAY for low-latency control. */
    nb = 0;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &nb, sizeof(nb));
    int one = 1;
    sceNetSetsockopt(sock, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one,
                     sizeof(one));
    return sock;
}

int net_send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t sent = 0;
    while (sent < len) {
        int n = sceNetSend(fd, p + sent, len - sent, 0);
        if (n > 0)
            sent += (size_t)n;
        else if (n < 0 && ((unsigned)n == SCE_NET_ERROR_EAGAIN ||
                           (unsigned)n == SCE_NET_ERROR_EWOULDBLOCK))
            continue;
        else
            return NET_ERR;
    }
    return 0;
}

int net_recv(int fd, void *buf, size_t len, int timeout_ms)
{
    /* Apply the caller's timeout to this socket (microseconds). */
    unsigned int tv = (unsigned int)timeout_ms * 1000u;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &tv,
                     sizeof(tv));

    int n = sceNetRecv(fd, buf, len, 0);
    if (n > 0)
        return n;
    if (n == 0)
        return NET_CLOSED;
    /* EAGAIN / EWOULDBLOCK / ETIMEDOUT => no data within the timeout. */
    if ((unsigned)n == SCE_NET_ERROR_EAGAIN ||
        (unsigned)n == SCE_NET_ERROR_EWOULDBLOCK ||
        (unsigned)n == SCE_NET_ERROR_ETIMEDOUT)
        return NET_TIMEOUT;
    return NET_ERR;
}

void net_close(int fd)
{
    if (fd >= 0)
        sceNetSocketClose(fd);
}
