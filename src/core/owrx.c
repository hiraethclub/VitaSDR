/* OpenWebRX / OpenWebRX+ protocol client. See owrx.h. */
#include "owrx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* csdr pads the compressed FFT with this many lead-in samples so the ADPCM
 * predictor has settled before the real bins start; we drop them. Must match
 * COMPRESS_FFT_PAD_N in the reference client. */
#define OWRX_FFT_PAD 10

/* dB window mapped onto the 0..255 waterfall byte. The live server's bins sat
 * around -83..-23 dB; this window brackets that with headroom, and the
 * renderer's adaptive contrast does the fine scaling. */
#define OWRX_DB_LO (-90.0)
#define OWRX_DB_HI (-20.0)

/* ---------------- small helpers ---------------- */

static int send_json(owrx_client *o, const char *s)
{
    return ws_send_text(&o->ws, s);
}

/* Skip spaces/tabs. */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Find the value position just after `"key"` and its colon. Returns NULL if the
 * quoted key is not present. `body` must be NUL-terminated. */
static const char *json_value_pos(const char *body, const char *key)
{
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\"", key);
    const char *p = strstr(body, quoted);
    if (!p)
        return NULL;
    p += strlen(quoted);
    const char *end = body + strlen(body);
    p = skip_ws(p, end);
    if (p >= end || *p != ':')
        return NULL;
    p++;
    return skip_ws(p, end);
}

int owrx_json_number(const char *body, const char *key, double *out)
{
    const char *p = json_value_pos(body, key);
    if (!p)
        return 0;
    if (*p == 'n' || *p == '"')   /* null, or a string where we expect a number */
        return 0;
    char *endp = NULL;
    double v = strtod(p, &endp);
    if (endp == p)
        return 0;
    if (out)
        *out = v;
    return 1;
}

int owrx_json_string(const char *body, const char *key, char *out, size_t cap)
{
    const char *p = json_value_pos(body, key);
    if (!p || *p != '"')
        return 0;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1])   /* unescape one level, enough for our fields */
            p++;
        out[n++] = *p++;
    }
    if (cap)
        out[n] = '\0';
    return 1;
}

const char *owrx_map_mode(const char *mode)
{
    if (!mode)
        return "am";
    if (strcmp(mode, "nbfm") == 0 || strcmp(mode, "fm") == 0)
        return "nfm";
    /* usb, lsb, am, cw, wfm, nfm pass straight through */
    return mode;
}

/* Default passband edges (Hz, relative to the demod offset) for a mode. */
static void owrx_passband(const char *mode, int *low, int *high)
{
    const char *m = owrx_map_mode(mode);
    if (strcmp(m, "usb") == 0)      { *low = 0;      *high = 3000;  }
    else if (strcmp(m, "lsb") == 0) { *low = -3000;  *high = 0;     }
    else if (strcmp(m, "cw") == 0)  { *low = -250;   *high = 250;   }
    else if (strcmp(m, "nfm") == 0) { *low = -6250;  *high = 6250;  }
    else if (strcmp(m, "wfm") == 0) { *low = -75000; *high = 75000; }
    else                            { *low = -4500;  *high = 4500;  } /* am */
}

long owrx_offset_hz(double freq_khz, double center_hz, double samp_rate)
{
    double off = freq_khz * 1000.0 - center_hz;
    /* Leave ~5% guard at each band edge where the demodulator can't reach. */
    double lim = samp_rate * 0.45;
    if (off > lim)  off = lim;
    if (off < -lim) off = -lim;
    return (long)(off >= 0 ? off + 0.5 : off - 0.5);
}

/* ---------------- DSP start / config ---------------- */

static void owrx_maybe_start(owrx_client *o)
{
    if (o->dsp_started || !o->have_center)
        return;

    /* If the requested frequency falls outside the SDR's tunable window, adopt
     * the server's start frequency so the UI reflects where we actually are. */
    double lim = o->samp_rate * 0.45;
    double reqoff = o->freq_khz * 1000.0 - o->center_freq;
    if (reqoff > lim || reqoff < -lim) {
        double base = o->center_freq + (o->have_start_offset ? o->start_offset : 0.0);
        o->freq_khz = base / 1000.0;
    }

    int lc, hc;
    owrx_passband(o->mode, &lc, &hc);
    long off = owrx_offset_hz(o->freq_khz, o->center_freq, o->samp_rate);
    const char *mod = owrx_map_mode(o->mode);

    send_json(o, "{\"type\":\"dspcontrol\",\"action\":\"start\"}");
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"dspcontrol\",\"params\":{\"mod\":\"%s\","
             "\"offset_freq\":%ld,\"low_cut\":%d,\"high_cut\":%d,"
             "\"squelch_level\":-150}}",
             mod, off, lc, hc);
    send_json(o, buf);
    o->dsp_started = 1;
}

static void owrx_handle_config(owrx_client *o, const char *body)
{
    double v;
    char s[32];
    if (owrx_json_number(body, "center_freq", &v)) o->center_freq = v;
    if (owrx_json_number(body, "samp_rate", &v))   o->samp_rate   = v;
    if (owrx_json_number(body, "fft_size", &v))    o->fft_size    = (int)v;
    if (owrx_json_number(body, "start_offset_freq", &v)) {
        o->start_offset = v; o->have_start_offset = 1;
    }
    if (owrx_json_string(body, "audio_compression", s, sizeof(s)))
        o->audio_adpcm_on = (strcmp(s, "adpcm") == 0);
    if (owrx_json_string(body, "fft_compression", s, sizeof(s)))
        o->fft_adpcm_on = (strcmp(s, "adpcm") == 0);

    if (o->center_freq > 0.0 && o->samp_rate > 0.0)
        o->have_center = 1;
}

int owrx_handle_text(owrx_client *o, const char *body, size_t len)
{
    (void)len;
    if (strncmp(body, "CLIENT DE SERVER", 16) == 0) {
        o->greeted = 1;
        snprintf(o->last_msg, sizeof(o->last_msg), "%.159s", body);
        o->msg_seq++;
        return 0;
    }
    if (body[0] != '{')
        return 0;   /* unknown text, ignore */

    snprintf(o->last_msg, sizeof(o->last_msg), "%.159s", body);
    o->msg_seq++;

    char type[32];
    if (owrx_json_string(body, "type", type, sizeof(type))) {
        if (strcmp(type, "config") == 0) {
            owrx_handle_config(o, body);
        } else if (strcmp(type, "smeter") == 0) {
            double v;
            if (owrx_json_number(body, "value", &v)) {
                o->rssi_dbm = (float)v;
                o->smeter = (int)v;
            }
        }
    }
    owrx_maybe_start(o);
    return 0;
}

/* ---------------- audio: streaming SYNC ADPCM decode ---------------- */

/* Set the ADPCM decoder state from a 4-byte sync header: int16 LE step index,
 * then int16 LE predictor. The index is clamped so the step-table lookup on the
 * next nibble stays in range. */
static void audio_apply_sync(owrx_client *o)
{
    int idx = (int)(int16_t)((unsigned)o->sync_hdr[0] | ((unsigned)o->sync_hdr[1] << 8));
    int pred = (int)(int16_t)((unsigned)o->sync_hdr[2] | ((unsigned)o->sync_hdr[3] << 8));
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    o->audio_adpcm.index = idx;
    o->audio_adpcm.predictor = pred;
    o->sync_count = 1000;   /* nibble-bytes until the next embedded sync */
}

static void audio_flush(owrx_client *o, int16_t *buf, int *n)
{
    if (*n > 0) {
        jitter_push(o->sink, buf, (size_t)*n);
        o->samples_rx += (unsigned long)*n;
        *n = 0;
    }
}

/* Decode a continuous-stream audio payload (bytes after the type tag), honoring
 * the "SYNC" + state-header resync markers. State persists across calls because
 * the markers and the 1000-byte segments may straddle WebSocket messages.
 * Returns the number of PCM samples pushed to the sink. */
static int audio_decode(owrx_client *o, const unsigned char *d, size_t n)
{
    static const char SYNC[4] = { 'S', 'Y', 'N', 'C' };
    int pushed = 0;
    int fill = 0;
    size_t i = 0;

    while (i < n) {
        if (o->sync_count > 0) {
            /* decode phase: two samples per byte until the segment ends */
            while (i < n && o->sync_count > 0) {
                if (fill + 2 > (int)(sizeof(o->audio_tmp) / sizeof(o->audio_tmp[0])))
                    audio_flush(o, o->audio_tmp, &fill);
                o->audio_tmp[fill++] = adpcm_decode_nibble(&o->audio_adpcm, d[i] & 0x0F);
                o->audio_tmp[fill++] = adpcm_decode_nibble(&o->audio_adpcm, (d[i] >> 4) & 0x0F);
                pushed += 2;
                o->sync_count--;
                i++;
            }
            if (o->sync_count == 0) {
                o->sync_match = 0;   /* now look for the next marker */
                o->sync_hdr_need = 0;
            }
            continue;
        }

        /* sync phase: find the 4-byte "SYNC" word */
        if (o->sync_hdr_need == 0) {
            while (o->sync_match < 4 && i < n) {
                if (d[i] == (unsigned char)SYNC[o->sync_match])
                    o->sync_match++;
                else
                    o->sync_match = 0;
                i++;
                if (o->sync_match == 4) {
                    o->sync_hdr_need = 4;
                    o->sync_hdr_have = 0;
                    break;
                }
            }
            if (o->sync_match < 4)
                break;   /* consumed the chunk still hunting; resume next time */
        }

        /* collect the 4-byte state header (may straddle a message boundary) */
        while (o->sync_hdr_need > 0 && i < n) {
            o->sync_hdr[o->sync_hdr_have++] = d[i++];
            o->sync_hdr_need--;
        }
        if (o->sync_hdr_need == 0 && o->sync_hdr_have == 4)
            audio_apply_sync(o);   /* sets sync_count = 1000 */
        else
            break;   /* header not complete yet */
    }

    audio_flush(o, o->audio_tmp, &fill);
    return pushed;
}

/* ---------------- FFT: ADPCM decode + decimate to the waterfall ---------------- */

static unsigned char db_to_byte(double db)
{
    double t = (db - OWRX_DB_LO) / (OWRX_DB_HI - OWRX_DB_LO);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (unsigned char)(t * 255.0 + 0.5);
}

static int fft_decode(owrx_client *o, const unsigned char *payload, size_t plen)
{
    int cap = (int)(sizeof(o->fft_tmp) / sizeof(o->fft_tmp[0]));
    int nsamp;

    if (o->fft_adpcm_on) {
        adpcm_reset(&o->fft_adpcm);
        if ((int)(plen * 2) > cap)
            plen = (size_t)(cap / 2);
        nsamp = (int)adpcm_decode(&o->fft_adpcm, payload, plen, o->fft_tmp);
    } else {
        /* uncompressed: payload is int16 LE power*100 values */
        nsamp = (int)(plen / 2);
        if (nsamp > cap) nsamp = cap;
        for (int i = 0; i < nsamp; i++)
            o->fft_tmp[i] = (int16_t)((unsigned)payload[i * 2] |
                                      ((unsigned)payload[i * 2 + 1] << 8));
    }

    int bins = nsamp - OWRX_FFT_PAD;
    const int16_t *v = o->fft_tmp + OWRX_FFT_PAD;
    if (bins <= 0)
        return 0;

    /* Decimate `bins` down to OWRX_WF_BINS, keeping the peak in each group so
     * narrow signals survive, and map dB (value/100) to a 0..255 byte. */
    int out = OWRX_WF_BINS;
    if (bins < out)
        out = bins;
    for (int j = 0; j < out; j++) {
        int lo = (int)((long)j * bins / out);
        int hi = (int)((long)(j + 1) * bins / out);
        if (hi <= lo) hi = lo + 1;
        if (hi > bins) hi = bins;
        int16_t peak = v[lo];
        for (int k = lo + 1; k < hi; k++)
            if (v[k] > peak) peak = v[k];
        o->wf_bins[j] = db_to_byte((double)peak / 100.0);
    }
    o->wf_nbins = out;
    return out;
}

int owrx_handle_binary(owrx_client *o, const unsigned char *frame, size_t len)
{
    if (len < 1)
        return OWRX_IDLE;
    unsigned char type = frame[0];
    const unsigned char *payload = frame + 1;
    size_t plen = len - 1;

    switch (type) {
    case 1:   /* FFT / waterfall */
        return fft_decode(o, payload, plen) > 0 ? OWRX_WF : OWRX_IDLE;
    case 2:   /* audio */
    case 4:   /* HD audio (same codec; we force 12k so it is mono too) */
        if (o->audio_adpcm_on) {
            return audio_decode(o, payload, plen) > 0 ? OWRX_AUDIO : OWRX_IDLE;
        } else {
            /* raw int16 LE */
            int ns = (int)(plen / 2);
            int cap = (int)(sizeof(o->audio_tmp) / sizeof(o->audio_tmp[0]));
            int done = 0;
            while (done < ns) {
                int chunk = ns - done;
                if (chunk > cap) chunk = cap;
                for (int i = 0; i < chunk; i++)
                    o->audio_tmp[i] = (int16_t)((unsigned)payload[(done + i) * 2] |
                                     ((unsigned)payload[(done + i) * 2 + 1] << 8));
                jitter_push(o->sink, o->audio_tmp, (size_t)chunk);
                done += chunk;
            }
            o->samples_rx += (unsigned long)ns;
            return ns > 0 ? OWRX_AUDIO : OWRX_IDLE;
        }
    case 3:   /* secondary FFT (digital modes) — not used */
    default:
        return OWRX_IDLE;
    }
}

/* ---------------- public API ---------------- */

int owrx_connect(owrx_client *o, const char *host, int port, const char *path,
                 int tls, int verify, jitter_buf *sink, double freq_khz,
                 const char *mode, int timeout_ms)
{
    memset(o, 0, sizeof(*o));
    o->sink = sink;
    o->freq_khz = freq_khz;
    o->fft_size = 4096;
    o->rssi_dbm = -140.0f;
    strncpy(o->mode, mode ? mode : "wfm", sizeof(o->mode) - 1);
    adpcm_reset(&o->audio_adpcm);
    adpcm_reset(&o->fft_adpcm);

    const char *p = (path && path[0]) ? path : "/ws/";
    int rc = ws_connect_ex(&o->ws, host, port, p, NULL, tls, verify, timeout_ms);
    if (rc != WS_CONNECT_OK) {
        o->ws.fd = -1;
        return rc;
    }

    /* OpenWebRX handshake. output_rate == hd_output_rate == 12000 keeps every
     * mode at mono 12 kHz, matching the KiwiSDR audio path. */
    if (ws_send_text(&o->ws, "SERVER DE CLIENT client=vitasdr type=receiver") != 0)
        return -6;
    if (ws_send_text(&o->ws,
            "{\"type\":\"connectionproperties\",\"params\":"
            "{\"output_rate\":12000,\"hd_output_rate\":12000}}") != 0)
        return -6;
    return 0;
}

int owrx_poll(owrx_client *o, int timeout_ms)
{
    static unsigned char frame[WS_INBUF_SIZE];
    int opcode = 0;
    int r = ws_recv(&o->ws, frame, sizeof(frame), &opcode, timeout_ms);
    if (r == WS_NONE)
        return OWRX_IDLE;
    if (r < 0)
        return OWRX_ERR;

    if (opcode == WS_OP_TEXT) {
        frame[r < (int)sizeof(frame) ? r : (int)sizeof(frame) - 1] = '\0';
        owrx_handle_text(o, (const char *)frame, (size_t)r);
        return OWRX_IDLE;
    }
    if (opcode == WS_OP_BINARY)
        return owrx_handle_binary(o, frame, (size_t)r);
    return OWRX_IDLE;
}

int owrx_set_frequency(owrx_client *o, double freq_khz)
{
    o->freq_khz = freq_khz;
    if (!o->dsp_started)
        return 0;
    long off = owrx_offset_hz(freq_khz, o->center_freq, o->samp_rate);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"dspcontrol\",\"params\":{\"offset_freq\":%ld}}", off);
    return send_json(o, buf);
}

int owrx_set_mode(owrx_client *o, const char *mode)
{
    strncpy(o->mode, mode, sizeof(o->mode) - 1);
    o->mode[sizeof(o->mode) - 1] = '\0';
    if (!o->dsp_started)
        return 0;
    int lc, hc;
    owrx_passband(mode, &lc, &hc);
    long off = owrx_offset_hz(o->freq_khz, o->center_freq, o->samp_rate);
    const char *mod = owrx_map_mode(mode);
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"dspcontrol\",\"params\":{\"mod\":\"%s\","
             "\"low_cut\":%d,\"high_cut\":%d,\"offset_freq\":%ld}}",
             mod, lc, hc, off);
    return send_json(o, buf);
}

int owrx_keepalive(owrx_client *o)
{
    (void)o;
    return 0;   /* ws_client answers server pings; no app keepalive needed */
}

void owrx_disconnect(owrx_client *o)
{
    ws_close(&o->ws);
}
