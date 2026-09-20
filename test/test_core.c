/* Native unit tests for the portable core. Built as the `vitasdr_test` target.
 *
 * Covers: ADPCM decode (hand-computed vector), the jitter buffer (including
 * drop-oldest on overflow), the KiwiSDR SND/MSG parsers with synthetic frames,
 * and a full WebSocket handshake + frame round-trip against a loopback server
 * running in a second thread. No external network is used. */
#include "adpcm.h"
#include "bandplan.h"
#include "jitter.h"
#include "kiwi.h"
#include "net.h"
#include "resamp.h"
#include "ws_client.h"

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL: %s (%s:%d)\n",             \
                            msg, __FILE__, __LINE__); }         \
} while (0)

/* -------------------- ADPCM -------------------- */

static void test_adpcm(void)
{
    printf("[adpcm]\n");
    adpcm_state st;
    adpcm_reset(&st);
    /* Bytes {0x44, 0x0C} => nibbles (low first) 4,4,12,0.
     * Traced through the IMA-ADPCM step algorithm (index_table[4]=2):
     *   n=4 idx0 step7  -> +7          = 7,  idx->2
     *   n=4 idx2 step9  -> +(1+9)=10   = 17, idx->4
     *   n=12 idx4 step11-> -(1+11)=12  = 5,  idx->6
     *   n=0 idx6 step13 -> +1          = 6,  idx->5  */
    uint8_t in[2] = { 0x44, 0x0C };
    int16_t out[4];
    size_t n = adpcm_decode(&st, in, 2, out);
    CHECK(n == 4, "decoded 4 samples from 2 bytes");
    CHECK(out[0] == 7,  "sample 0 == 7");
    CHECK(out[1] == 17, "sample 1 == 17");
    CHECK(out[2] == 5,  "sample 2 == 5");
    CHECK(out[3] == 6,  "sample 3 == 6");
    CHECK(st.index == 5, "index advanced to 5");
    CHECK(st.predictor == 6, "predictor == 6");

    /* Reset returns to the initial state. */
    adpcm_reset(&st);
    CHECK(st.index == 0 && st.predictor == 0, "reset clears state");
}

/* -------------------- jitter -------------------- */

static void test_jitter(void)
{
    printf("[jitter]\n");
    jitter_buf jb;
    CHECK(jitter_init(&jb, 8) == 0, "init capacity 8");
    CHECK(jitter_available(&jb) == 0, "starts empty");

    int16_t in[4] = { 1, 2, 3, 4 };
    size_t dropped = jitter_push(&jb, in, 4);
    CHECK(dropped == 0, "no drop under capacity");
    CHECK(jitter_available(&jb) == 4, "4 available");

    int16_t out[4];
    size_t got = jitter_pop(&jb, out, 4);
    CHECK(got == 4, "popped 4");
    CHECK(out[0] == 1 && out[3] == 4, "fifo order preserved");
    CHECK(jitter_available(&jb) == 0, "empty after pop");

    /* Overflow: push 12 into capacity-8 ring; oldest 4 dropped, newest kept. */
    int16_t big[12];
    for (int i = 0; i < 12; i++) big[i] = (int16_t)(100 + i);
    dropped = jitter_push(&jb, big, 12);
    CHECK(dropped == 4, "dropped 4 oldest on overflow");
    CHECK(jitter_available(&jb) == 8, "ring holds capacity");
    int16_t o8[8];
    got = jitter_pop(&jb, o8, 8);
    CHECK(got == 8, "popped 8");
    CHECK(o8[0] == 104, "oldest surviving sample is 104");
    CHECK(o8[7] == 111, "newest sample is 111");

    jitter_free(&jb);
}

/* -------------------- kiwi parsers -------------------- */

static void test_kiwi_parse(void)
{
    printf("[kiwi]\n");
    kiwi_client k;
    memset(&k, 0, sizeof(k));
    jitter_buf jb;
    jitter_init(&jb, 48000);
    k.sink = &jb;
    k.audio_rate = 12000;
    adpcm_reset(&k.adpcm);

    /* MSG audio_adpcm_state resync. */
    const char *msg = "audio_adpcm_state=5,100 sample_rate=12001.5";
    kiwi_handle_msg(&k, msg, strlen(msg));
    CHECK(k.adpcm.index == 5, "adpcm index set from MSG");
    CHECK(k.adpcm.predictor == 100, "adpcm predictor set from MSG");

    /* Synthetic uncompressed SND frame: "SND" + flags + seq + smeter + 2 int16. */
    adpcm_reset(&k.adpcm);
    uint8_t frame[10 + 4];
    memcpy(frame, "SND", 3);
    frame[3] = 0x00;                 /* flags: not compressed */
    frame[4] = 0x01; frame[5] = 0x00; frame[6] = 0x00; frame[7] = 0x00; /* seq=1 LE */
    frame[8] = 0x05; frame[9] = 0x39; /* smeter=0x0539=1337 BE */
    /* two samples: 1000 and -1000, little-endian */
    int16_t s0 = 1000, s1 = -1000;
    frame[10] = (uint8_t)(s0 & 0xff); frame[11] = (uint8_t)((s0 >> 8) & 0xff);
    frame[12] = (uint8_t)(s1 & 0xff); frame[13] = (uint8_t)((s1 >> 8) & 0xff);

    int n = kiwi_handle_snd(&k, frame, sizeof(frame));
    CHECK(n == 2, "SND decoded 2 raw samples");
    CHECK(k.seq == 1, "seq parsed");
    CHECK(k.smeter == 1337, "smeter parsed (big-endian)");
    int16_t got[2];
    size_t g = jitter_pop(&jb, got, 2);
    CHECK(g == 2 && got[0] == 1000 && got[1] == -1000, "raw samples pushed");

    /* audio_rate triggers a SET AR OK; ar_sent latches. */
    kiwi_handle_msg(&k, "audio_rate=12000", strlen("audio_rate=12000"));
    CHECK(k.audio_rate == 12000, "audio_rate parsed");

    /* default passband for usb */
    int lc, hc;
    kiwi_default_passband("usb", &lc, &hc);
    CHECK(lc == 300 && hc == 2700, "usb default passband");

    /* W/F frame parse: "W/F" + 12-byte header + 4 bin bytes. */
    unsigned char wf[15 + 4];
    memcpy(wf, "W/F", 3);
    memset(wf + 3, 0, 12);
    wf[15] = 10; wf[16] = 20; wf[17] = 200; wf[18] = 255;
    unsigned char bins[KIWI_WF_BINS];
    int nb = kiwi_wf_parse(wf, sizeof(wf), bins, KIWI_WF_BINS);
    CHECK(nb == 4, "W/F parsed 4 bins");
    CHECK(bins[0] == 10 && bins[3] == 255, "W/F bin values intact");
    CHECK(kiwi_wf_parse(wf, 10, bins, KIWI_WF_BINS) < 0, "short W/F rejected");

    jitter_free(&jb);
}

/* -------------------- loopback websocket server -------------------- */
/* A minimal RFC6455 server, just enough to test the client: performs the
 * handshake with a correct Sec-WebSocket-Accept, sends a ping (client must
 * auto-pong), then echoes back frames the client sends. */

/* Compact SHA-1 + base64 (independent copy for the test server). */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t b[64]; size_t bl; } sh;
static uint32_t rl(uint32_t v,int b){return (v<<b)|(v>>(32-b));}
static void shp(sh*c,const uint8_t*p){uint32_t w[80];for(int i=0;i<16;i++)w[i]=(p[i*4]<<24)|(p[i*4+1]<<16)|(p[i*4+2]<<8)|p[i*4+3];for(int i=16;i<80;i++)w[i]=rl(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);uint32_t a=c->h[0],b=c->h[1],d=c->h[2],e=c->h[3],f=c->h[4];for(int i=0;i<80;i++){uint32_t t,k;if(i<20){t=(b&d)|(~b&e);k=0x5A827999;}else if(i<40){t=b^d^e;k=0x6ED9EBA1;}else if(i<60){t=(b&d)|(b&e)|(d&e);k=0x8F1BBCDC;}else{t=b^d^e;k=0xCA62C1D6;}uint32_t tm=rl(a,5)+t+f+k+w[i];f=e;e=d;d=rl(b,30);b=a;a=tm;}c->h[0]+=a;c->h[1]+=b;c->h[2]+=d;c->h[3]+=e;c->h[4]+=f;}
static void shi(sh*c){c->h[0]=0x67452301;c->h[1]=0xEFCDAB89;c->h[2]=0x98BADCFE;c->h[3]=0x10325476;c->h[4]=0xC3D2E1F0;c->len=0;c->bl=0;}
static void shu(sh*c,const void*d,size_t n){const uint8_t*p=d;c->len+=n;while(n){size_t t=64-c->bl;if(t>n)t=n;memcpy(c->b+c->bl,p,t);c->bl+=t;p+=t;n-=t;if(c->bl==64){shp(c,c->b);c->bl=0;}}}
static void shf(sh*c,uint8_t o[20]){uint64_t bits=c->len*8;uint8_t pad=0x80;shu(c,&pad,1);uint8_t z=0;while(c->bl!=56)shu(c,&z,1);uint8_t lb[8];for(int i=0;i<8;i++)lb[i]=(uint8_t)(bits>>(56-i*8));shu(c,lb,8);for(int i=0;i<5;i++){o[i*4]=c->h[i]>>24;o[i*4+1]=c->h[i]>>16;o[i*4+2]=c->h[i]>>8;o[i*4+3]=c->h[i];}}
static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const uint8_t*in,size_t n,char*out){size_t o=0;for(size_t i=0;i<n;i+=3){uint32_t v=in[i]<<16;int r=(int)(n-i);if(r>1)v|=in[i+1]<<8;if(r>2)v|=in[i+2];out[o++]=B64[(v>>18)&63];out[o++]=B64[(v>>12)&63];out[o++]=(r>1)?B64[(v>>6)&63]:'=';out[o++]=(r>2)?B64[v&63]:'=';}out[o]=0;}

typedef struct { int listen_fd; int port; } srv_ctx;

static void srv_send_frame(int fd, int opcode, const uint8_t *data, size_t len)
{
    uint8_t hdr[10]; size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | opcode);
    if (len < 126) hdr[h++] = (uint8_t)len;
    else if (len < 65536) { hdr[h++] = 126; hdr[h++] = (uint8_t)(len>>8); hdr[h++] = (uint8_t)len; }
    else { hdr[h++] = 127; for (int i=7;i>=0;i--) hdr[h++]=(uint8_t)((uint64_t)len>>(i*8)); }
    send(fd, hdr, h, 0);
    if (len) send(fd, data, len, 0);
}

/* Read one client frame (masked), unmask into out. Returns payload len or -1. */
static int srv_read_frame(int fd, uint8_t *out, size_t cap, int *opcode)
{
    uint8_t hdr[2];
    if (recv(fd, hdr, 2, MSG_WAITALL) != 2) return -1;
    *opcode = hdr[0] & 0x0f;
    int masked = hdr[1] & 0x80;
    uint64_t len = hdr[1] & 0x7f;
    if (len == 126) { uint8_t e[2]; recv(fd,e,2,MSG_WAITALL); len=(e[0]<<8)|e[1]; }
    else if (len == 127) { uint8_t e[8]; recv(fd,e,8,MSG_WAITALL); len=0; for(int i=0;i<8;i++) len=(len<<8)|e[i]; }
    uint8_t mask[4] = {0,0,0,0};
    if (masked) recv(fd, mask, 4, MSG_WAITALL);
    if (len > cap) return -1;
    if (len) { if ((uint64_t)recv(fd, out, len, MSG_WAITALL) != len) return -1; }
    if (masked) for (uint64_t i=0;i<len;i++) out[i] ^= mask[i&3];
    return (int)len;
}

static void *server_thread(void *arg)
{
    srv_ctx *ctx = (srv_ctx *)arg;
    int cfd = accept(ctx->listen_fd, NULL, NULL);
    if (cfd < 0) return NULL;

    /* Read handshake request. */
    char req[2048]; size_t rl_ = 0;
    while (rl_ < sizeof(req) - 1) {
        ssize_t r = recv(cfd, req + rl_, sizeof(req) - 1 - rl_, 0);
        if (r <= 0) { close(cfd); return NULL; }
        rl_ += (size_t)r; req[rl_] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    /* Extract key, compute accept. */
    char accept[64] = {0};
    char *kp = strstr(req, "Sec-WebSocket-Key:");
    if (kp) {
        kp += strlen("Sec-WebSocket-Key:");
        while (*kp == ' ') kp++;
        char key[128]; size_t ki = 0;
        while (*kp && *kp != '\r' && ki < sizeof(key)-1) key[ki++] = *kp++;
        key[ki] = 0;
        char concat[256];
        snprintf(concat, sizeof(concat), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        sh c; uint8_t dig[20]; shi(&c); shu(&c, concat, strlen(concat)); shf(&c, dig);
        b64(dig, 20, accept);
    }
    char resp[256];
    int rn = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    send(cfd, resp, rn, 0);

    /* Send a ping the client must auto-pong (and not surface). */
    uint8_t ping[3] = { 'p','n','g' };
    srv_send_frame(cfd, 0x9, ping, 3);

    /* Echo two DATA frames the client sends, skipping control frames such as
     * the pong the client sends in reply to our ping. */
    int echoed = 0;
    while (echoed < 2) {
        uint8_t buf[70000]; int op;
        int n = srv_read_frame(cfd, buf, sizeof(buf), &op);
        if (n < 0) break;
        if (op == 0x1 || op == 0x2) {   /* text or binary */
            srv_send_frame(cfd, op, buf, (size_t)n);
            echoed++;
        }
        /* control frames (ping/pong/close) are ignored */
    }

    /* Give the client time to read before closing. */
    usleep(100000);
    close(cfd);
    return NULL;
}

static void test_websocket_loopback(void)
{
    printf("[websocket loopback]\n");
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(lfd >= 0, "listen socket created");
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK(bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) == 0, "bind ephemeral");
    socklen_t alen = sizeof(addr);
    getsockname(lfd, (struct sockaddr*)&addr, &alen);
    int port = ntohs(addr.sin_port);
    listen(lfd, 1);

    srv_ctx ctx = { lfd, port };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &ctx);

    ws_client ws;
    int rc = ws_connect(&ws, "127.0.0.1", port, "/test/SND", NULL, 3000);
    CHECK(rc == 0, "handshake succeeded (accept verified)");

    if (rc == 0) {
        /* Small binary frame round-trip. */
        uint8_t small[5] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42 };
        CHECK(ws_send_binary(&ws, small, 5) == 0, "sent small binary");
        uint8_t out[70000]; int op = 0;
        int n = ws_recv(&ws, out, sizeof(out), &op, 3000);
        CHECK(n == 5 && op == WS_OP_BINARY, "received small binary echo");
        CHECK(memcmp(out, small, 5) == 0, "small payload intact (ping auto-ponged)");

        /* Medium frame (>125, forces 16-bit length). */
        uint8_t med[300];
        for (int i = 0; i < 300; i++) med[i] = (uint8_t)(i * 7 + 1);
        CHECK(ws_send_binary(&ws, med, 300) == 0, "sent 300-byte binary");
        n = ws_recv(&ws, out, sizeof(out), &op, 3000);
        CHECK(n == 300, "received 300-byte echo (16-bit length path)");
        CHECK(memcmp(out, med, 300) == 0, "medium payload intact");

        ws_close(&ws);
    }
    pthread_join(th, NULL);
    close(lfd);
}

/* Regression test for the ensure_buffered timeout bug: a ws_recv that times out
 * on an empty socket must return WS_NONE cleanly and must NOT misalign the
 * stream, so a frame that arrives afterwards is read intact. */
static void test_websocket_recv_timeout(void)
{
    printf("[websocket recv timeout]\n");
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

    ws_client ws;
    memset(&ws, 0, sizeof(ws));
    ws.fd = sv[0];
    ws.dbg_net_result = 99;

    /* Nothing sent yet: must time out to WS_NONE, not parse garbage/spin. */
    uint8_t out[64];
    int op = 0;
    int r = ws_recv(&ws, out, sizeof(out), &op, 100);
    CHECK(r == WS_NONE, "empty socket -> WS_NONE (no misalign)");

    /* Now deliver a complete unmasked binary frame: "MSG ab". */
    const char *payload = "MSG ab";
    uint8_t frame[8];
    frame[0] = 0x82;                 /* FIN + binary */
    frame[1] = (uint8_t)strlen(payload);
    memcpy(frame + 2, payload, strlen(payload));
    ssize_t wn = write(sv[1], frame, 2 + strlen(payload));
    CHECK(wn == (ssize_t)(2 + strlen(payload)), "server wrote frame");

    r = ws_recv(&ws, out, sizeof(out), &op, 500);
    CHECK(r == (int)strlen(payload) && op == WS_OP_BINARY,
          "frame after timeout read intact");
    CHECK(memcmp(out, payload, strlen(payload)) == 0, "payload correct");

    close(sv[0]);
    close(sv[1]);
}

static void test_bandplan(void)
{
    printf("[bandplan]\n");
    CHECK(strcmp(band_lookup(7074.0), "40m Amateur") == 0, "7074 -> 40m Amateur");
    CHECK(strcmp(band_lookup(17735.0), "16m SWBC") == 0, "17735 -> 16m SWBC");
    CHECK(strcmp(band_lookup(14200.0), "20m Amateur") == 0, "14200 -> 20m Amateur");
    CHECK(strcmp(band_lookup(1000.0), "MW Broadcast") == 0, "1000 -> MW Broadcast");
    CHECK(band_lookup(100000.0)[0] == '\0', "out-of-plan -> empty");
}

/* -------------------- resampler -------------------- */

/* Goertzel: magnitude of frequency `f` in an int16 buffer sampled at `rate`. */
static double goertzel(const int16_t *x, int n, double f, double rate)
{
    double w = 2.0 * M_PI * f / rate;
    double c = 2.0 * cos(w);
    double s0 = 0.0, s1 = 0.0, s2 = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        s0 = (double)x[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2);
}

/* Run `nin` source samples through the resampler and collect all outputs. */
static int run_resamp(resamp *r, const int16_t *in, int nin,
                      int16_t *out, int outcap)
{
    int i = 0, produced = 0;
    while (i < nin && produced < outcap) {
        int chunk = 128;
        if (chunk > nin - i) chunk = nin - i;
        resamp_push(r, in + i, chunk);
        i += chunk;
        produced += resamp_pull(r, out + produced, outcap - produced);
    }
    return produced;
}

static void test_resamp(void)
{
    printf("[resamp]\n");
    resamp r;
    resamp_init(&r, 12000.0, 48000.0, 0.0);

    /* DC in -> DC out at unity gain (kernels are unity-DC). */
    int16_t dc_in[400], dc_out[2048];
    for (int i = 0; i < 400; i++) dc_in[i] = 8000;
    int n = run_resamp(&r, dc_in, 400, dc_out, 2048);
    CHECK(n > 1200, "resamp produced ~4x output for DC");
    /* Skip warm-up; check a steady sample near the end. */
    CHECK(dc_out[n - 10] > 7900 && dc_out[n - 10] < 8100,
          "resamp DC gain ~= unity");

    /* 5.5 kHz full-scale sine (near the 6 kHz source Nyquist). A linear
     * interpolator would image this to 6.5 kHz at ~20% amplitude; the
     * windowed-sinc kernel must suppress that image hard. */
    resamp_reset(&r);
    int16_t si_in[2000], si_out[8192];
    for (int i = 0; i < 2000; i++)
        si_in[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * 5500.0 * i / 12000.0));
    n = run_resamp(&r, si_in, 2000, si_out, 8192);
    CHECK(n > 6000, "resamp produced output for sine");
    /* Measure over a steady interior window (avoid the warm-up transient). */
    int off = 512, win = n - 1024;
    double sig = goertzel(si_out + off, win, 5500.0, 48000.0);
    double img = goertzel(si_out + off, win, 6500.0, 48000.0);
    CHECK(sig > 0.0 && img / sig < 0.05,
          "resamp suppresses 6.5 kHz image below 5% of signal");

    /* A mid-band 1 kHz tone passes through with its amplitude intact. */
    resamp_reset(&r);
    for (int i = 0; i < 2000; i++)
        si_in[i] = (int16_t)(15000.0 * sin(2.0 * M_PI * 1000.0 * i / 12000.0));
    n = run_resamp(&r, si_in, 2000, si_out, 8192);
    off = 512; win = n - 1024;
    double pass = goertzel(si_out + off, win, 1000.0, 48000.0);
    double ref  = goertzel(si_in + 100, 1800, 1000.0, 12000.0);
    /* Output window is ~4x longer, so its Goertzel sum scales ~4x. Compare the
     * per-sample-normalised magnitudes. */
    double out_norm = pass / win, in_norm = ref / 1800.0;
    CHECK(out_norm > 0.85 * in_norm && out_norm < 1.15 * in_norm,
          "resamp passes 1 kHz tone with amplitude intact");
}

int main(void)
{
    printf("VitaSDR core tests\n==================\n");
    test_adpcm();
    test_jitter();
    test_kiwi_parse();
    test_bandplan();
    test_resamp();
    test_websocket_loopback();
    test_websocket_recv_timeout();
    printf("==================\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
