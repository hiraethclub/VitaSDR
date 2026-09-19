/* KiwiSDR protocol client. See kiwi.h. */
#include "kiwi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SND flag bits (from the reference client). */
#define SND_FLAG_ADC_OVFL      0x02
#define SND_FLAG_STEREO        0x08
#define SND_FLAG_COMPRESSED    0x10
#define SND_FLAG_LITTLE_ENDIAN 0x80

/* Scratch buffer for one decoded audio frame. A KiwiSDR audio payload is a
 * few hundred bytes of ADPCM (~2 samples/byte); 8192 samples is ample. */
#define KIWI_MAX_SAMPLES 8192

void kiwi_default_passband(const char *mode, int *low_cut, int *high_cut)
{
    if (strcmp(mode, "lsb") == 0)      { *low_cut = -2700; *high_cut = -300; }
    else if (strcmp(mode, "usb") == 0) { *low_cut = 300;   *high_cut = 2700; }
    else if (strcmp(mode, "cw") == 0)  { *low_cut = 300;   *high_cut = 700; }
    else if (strcmp(mode, "am") == 0)  { *low_cut = -4900; *high_cut = 4900; }
    else if (strcmp(mode, "nbfm") == 0 || strcmp(mode, "fm") == 0)
                                       { *low_cut = -6000; *high_cut = 6000; }
    else                               { *low_cut = -4000; *high_cut = 4000; }
}

static int send_set(kiwi_client *k, const char *cmd)
{
    return ws_send_text(&k->ws, cmd);
}

/* Send the mod/passband/frequency line. */
static int send_mod(kiwi_client *k)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "SET mod=%s low_cut=%d high_cut=%d freq=%.3f",
             k->mode, k->low_cut, k->high_cut, k->freq_khz);
    return send_set(k, cmd);
}

/* Send everything needed once the sample rate is known. */
static int send_rx_params(kiwi_client *k)
{
    char cmd[64];
    if (send_set(k, "SET squelch=0 max=0") != 0) return -1;
    if (send_set(k, "SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50")
        != 0) return -1;
    if (send_mod(k) != 0) return -1;
    snprintf(cmd, sizeof(cmd), "SET compression=1");
    if (send_set(k, cmd) != 0) return -1;
    if (send_set(k, "SET keepalive") != 0) return -1;
    k->configured = 1;
    return 0;
}

int kiwi_connect(kiwi_client *k, const char *host, int port,
                 const char *password, jitter_buf *sink,
                 double freq_khz, const char *mode, int timeout_ms)
{
    memset(k, 0, sizeof(*k));
    k->sink = sink;
    k->audio_rate = 12000; /* until the server tells us otherwise */
    k->freq_khz = freq_khz;
    strncpy(k->mode, mode, sizeof(k->mode) - 1);
    kiwi_default_passband(k->mode, &k->low_cut, &k->high_cut);
    adpcm_reset(&k->adpcm);

    /* Path is /<timestamp>/SND; the timestamp is only a cache-buster. */
    char path[32];
    snprintf(path, sizeof(path), "/%ld/SND", (long)time(NULL));

    if (ws_connect(&k->ws, host, port, path, NULL, timeout_ms) != 0)
        return -1;

    /* First substantive message is auth. */
    char auth[128];
    snprintf(auth, sizeof(auth), "SET auth t=kiwi p=%s",
             password ? password : "");
    if (send_set(k, auth) != 0) {
        ws_close(&k->ws);
        return -1;
    }
    return 0;
}

/* Handle one "name=value" token from a MSG frame. */
static void handle_token(kiwi_client *k, char *tok)
{
    char *eq = strchr(tok, '=');
    if (!eq)
        return;
    *eq = '\0';
    const char *name = tok;
    const char *value = eq + 1;

    if (strcmp(name, "audio_rate") == 0) {
        int ar = atoi(value);
        if (ar > 0)
            k->audio_rate = ar;
        if (!k->ar_sent) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "SET AR OK in=%d out=%d",
                     k->audio_rate, k->audio_rate);
            send_set(k, cmd);
            k->ar_sent = 1;
        }
    } else if (strcmp(name, "sample_rate") == 0) {
        if (!k->configured)
            send_rx_params(k);
    } else if (strcmp(name, "audio_adpcm_state") == 0) {
        /* value is "index,prev" — resync the decoder. */
        int index = 0, prev = 0;
        if (sscanf(value, "%d,%d", &index, &prev) == 2) {
            if (index < 0) index = 0;
            if (index > 88) index = 88;
            k->adpcm.index = index;
            k->adpcm.predictor = prev;
        }
    }
}

int kiwi_handle_msg(kiwi_client *k, const char *body, size_t len)
{
    /* Copy to a NUL-terminated scratch buffer we can tokenize. */
    char buf[1024];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, body, n);
    buf[n] = '\0';

    char *save = NULL;
    char *tok = strtok_r(buf, " ", &save);
    while (tok) {
        handle_token(k, tok);
        tok = strtok_r(NULL, " ", &save);
    }
    return 0;
}

int kiwi_handle_snd(kiwi_client *k, const unsigned char *frame, size_t len)
{
    /* "SND" + flags(1) + seq(4 LE) + smeter(2 BE) = 10 bytes before audio. */
    if (len < 10)
        return -1;

    const unsigned char *b = frame + 3; /* skip "SND" tag */
    unsigned char flags = b[0];
    unsigned seq = (unsigned)b[1] | ((unsigned)b[2] << 8) |
                   ((unsigned)b[3] << 16) | ((unsigned)b[4] << 24);
    int smeter = ((int)b[5] << 8) | (int)b[6];

    k->seq = seq;
    k->smeter = smeter;
    k->rssi_dbm = 0.1f * (float)smeter - 127.0f;

    const unsigned char *audio = b + 7;
    size_t audio_len = len - 10;

    static int16_t pcm[KIWI_MAX_SAMPLES];
    size_t nsamp = 0;

    if (flags & SND_FLAG_COMPRESSED) {
        if (audio_len > KIWI_MAX_SAMPLES / 2)
            audio_len = KIWI_MAX_SAMPLES / 2;
        nsamp = adpcm_decode(&k->adpcm, audio, audio_len, pcm);
    } else {
        /* Raw int16 little-endian. */
        size_t cnt = audio_len / 2;
        if (cnt > KIWI_MAX_SAMPLES)
            cnt = KIWI_MAX_SAMPLES;
        for (size_t i = 0; i < cnt; i++)
            pcm[i] = (int16_t)((unsigned)audio[i * 2] |
                               ((unsigned)audio[i * 2 + 1] << 8));
        nsamp = cnt;
    }

    if (nsamp && k->sink)
        jitter_push(k->sink, pcm, nsamp);
    k->samples_rx += nsamp;

    return (int)nsamp;
}

int kiwi_poll(kiwi_client *k, int timeout_ms)
{
    static unsigned char frame[WS_INBUF_SIZE];
    int opcode = 0;
    int r = ws_recv(&k->ws, frame, sizeof(frame), &opcode, timeout_ms);
    if (r == WS_NONE)
        return KIWI_IDLE;
    if (r < 0)
        return KIWI_ERR;
    if (r < 3)
        return KIWI_IDLE;

    /* Dispatch on the 3-byte tag. */
    if (memcmp(frame, "SND", 3) == 0) {
        int n = kiwi_handle_snd(k, frame, (size_t)r);
        return (n > 0) ? KIWI_AUDIO : KIWI_IDLE;
    } else if (memcmp(frame, "MSG", 3) == 0) {
        /* Body is after the tag; a leading space is common. */
        const char *body = (const char *)frame + 3;
        size_t blen = (size_t)r - 3;
        while (blen > 0 && *body == ' ') { body++; blen--; }
        kiwi_handle_msg(k, body, blen);
        return KIWI_IDLE;
    }
    /* "W/F" and others: ignored on the audio connection for now. */
    return KIWI_IDLE;
}

int kiwi_set_frequency(kiwi_client *k, double freq_khz)
{
    k->freq_khz = freq_khz;
    return send_mod(k);
}

int kiwi_set_mode(kiwi_client *k, const char *mode, int low_cut, int high_cut)
{
    strncpy(k->mode, mode, sizeof(k->mode) - 1);
    k->mode[sizeof(k->mode) - 1] = '\0';
    if (low_cut == 0 && high_cut == 0)
        kiwi_default_passband(k->mode, &k->low_cut, &k->high_cut);
    else {
        k->low_cut = low_cut;
        k->high_cut = high_cut;
    }
    /* A mode change starts a new audio stream: resync the decoder. */
    adpcm_reset(&k->adpcm);
    return send_mod(k);
}

int kiwi_keepalive(kiwi_client *k)
{
    return send_set(k, "SET keepalive");
}

void kiwi_disconnect(kiwi_client *k)
{
    ws_close(&k->ws);
}
