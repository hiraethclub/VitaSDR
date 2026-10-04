/* OpenWebRX / OpenWebRX+ protocol client.
 *
 * Portable: no Vita headers. Unlike KiwiSDR (two sockets: SND + W/F), OpenWebRX
 * carries everything on ONE WebSocket at /ws/, so this one client decodes both
 * the audio and the FFT/waterfall. The Vita side runs it from the net thread
 * and leaves the (KiwiSDR-only) waterfall thread idle.
 *
 * Wire format (confirmed against a live OpenWebRX v1.2.x server):
 *   handshake, client -> server:
 *     TEXT  "SERVER DE CLIENT client=vitasdr type=receiver"
 *     JSON  {"type":"connectionproperties","params":{"output_rate":12000,
 *            "hd_output_rate":12000}}
 *   server -> client:
 *     TEXT  "CLIENT DE SERVER server=openwebrx version=..."
 *     JSON  {"type":"config","value":{...}}  (sent in SEVERAL partial messages;
 *            we accumulate samp_rate, center_freq, fft_size, audio_compression,
 *            fft_compression, start_mod, start_offset_freq)
 *     JSON  {"type":"smeter","value":<dB>}   (signal level)
 *     BIN   type byte then payload: 1 = FFT, 2 = audio, 3 = secondary FFT,
 *            4 = HD audio. Audio and FFT are IMA-ADPCM when the matching
 *            *_compression == "adpcm".
 *
 * Tuning is by OFFSET from the SDR's fixed center frequency (the dongle's
 * hardware tune), bounded to roughly +/- samp_rate/2. To start audio we send
 *     JSON {"type":"dspcontrol","action":"start"}
 *     JSON {"type":"dspcontrol","params":{"mod":..,"offset_freq":..,
 *           "low_cut":..,"high_cut":..,"squelch_level":-150}}
 * and retune/mode-change by re-sending the changed params.
 *
 * We request output_rate == hd_output_rate == 12000, so every mode (including
 * wideband FM) arrives as mono 12 kHz IMA-ADPCM, matching the KiwiSDR audio
 * pipeline exactly (no stereo, no per-mode rate switching).
 *
 * Single-threaded: drive one owrx_client from one thread.
 */
#ifndef VITASDR_OWRX_H
#define VITASDR_OWRX_H

#include "adpcm.h"
#include "jitter.h"
#include "ws_client.h"

/* Waterfall bins we expose (the server FFT is decimated down to this). Matches
 * KIWI_WF_BINS so the renderer and app buffer are shared. */
#define OWRX_WF_BINS 1024

/* Largest server FFT we handle; bigger ones are clamped to this many bins. */
#define OWRX_MAX_FFT 8192

/* owrx_poll return codes. */
enum {
    OWRX_IDLE  = 0,   /* control frame / timeout / nothing to surface */
    OWRX_AUDIO = 1,   /* decoded and pushed an audio frame */
    OWRX_WF    = 2,   /* produced a fresh waterfall row (see wf_bins) */
    OWRX_ERR   = -1   /* connection closed or error */
};

typedef struct {
    ws_client   ws;
    jitter_buf *sink;          /* decoded PCM goes here; owned by caller */

    /* Two independent ADPCM decoders: the FFT stream is reset per frame, the
     * audio stream is continuous and resynced on embedded "SYNC" markers. */
    adpcm_state audio_adpcm;
    adpcm_state fft_adpcm;

    /* ---- handshake / config accumulation ---- */
    int    greeted;            /* saw "CLIENT DE SERVER" */
    int    have_center;        /* have both center_freq and samp_rate */
    int    dsp_started;        /* sent dspcontrol start + initial params */
    double center_freq;        /* Hz, the dongle's hardware center */
    double samp_rate;          /* Hz, span of the FFT / tunable window */
    int    fft_size;           /* server FFT size (e.g. 4096) */
    int    audio_adpcm_on;     /* audio_compression == "adpcm" */
    int    fft_adpcm_on;       /* fft_compression == "adpcm" */
    double start_offset;       /* start_offset_freq from config (Hz) */
    int    have_start_offset;

    /* ---- streaming SYNC state for the audio ADPCM decoder ---- */
    int    sync_match;         /* bytes of "SYNC" matched (0..4) */
    int    sync_hdr_need;      /* state-header bytes still to collect (0..4) */
    int    sync_hdr_have;      /* state-header bytes collected so far */
    unsigned char sync_hdr[4];
    int    sync_count;         /* nibble-bytes left before the next resync */

    /* ---- current tuning (absolute) ---- */
    double freq_khz;
    char   mode[8];

    /* ---- status ---- */
    float  rssi_dbm;           /* from JSON "smeter" (already dB) */
    int    smeter;             /* same, scaled to an int for the UI */
    unsigned long samples_rx;  /* total PCM samples decoded */
    char   last_msg[160];      /* most recent control text, for logging */
    unsigned msg_seq;          /* bumps on each control message */

    /* ---- waterfall output (decimated to OWRX_WF_BINS, 0..255 bytes) ---- */
    unsigned char wf_bins[OWRX_WF_BINS];
    int           wf_nbins;

    /* scratch (kept off the stack: this struct lives as one static instance) */
    int16_t fft_tmp[OWRX_MAX_FFT + 32];
    int16_t audio_tmp[8192];
} owrx_client;

/* Connect to ws://host:port<path> (path is usually "/ws/"), complete the
 * WebSocket upgrade, and send the OpenWebRX handshake. `sink` receives decoded
 * audio. `freq_khz`/`mode` are the desired initial tuning; the actual start
 * frequency is resolved once config arrives (see owrx_poll). Returns 0 on
 * success, or a negative WS_CONNECT_* code. `tls` runs the link over wss://
 * (OpenWebRX behind HTTPS); `verify` validates the server certificate. */
int owrx_connect(owrx_client *o, const char *host, int port, const char *path,
                 int tls, int verify, jitter_buf *sink, double freq_khz,
                 const char *mode, int timeout_ms);

/* Process at most one incoming frame, waiting up to timeout_ms. Drives the
 * handshake, starts the DSP once config is known, and routes audio/FFT frames.
 * Returns an OWRX_* code. */
int owrx_poll(owrx_client *o, int timeout_ms);

/* Retune. freq is in kHz (absolute). Clamped into the SDR's tunable window.
 * Returns 0 on success, <0 on error. */
int owrx_set_frequency(owrx_client *o, double freq_khz);

/* Change demodulation mode (our mode names; mapped to OpenWebRX internally).
 * Returns 0 on success, <0 on error. */
int owrx_set_mode(owrx_client *o, const char *mode);

/* OpenWebRX needs no application keepalive (the WebSocket ping/pong handled by
 * ws_client suffices); provided for symmetry with the kiwi client. */
int owrx_keepalive(owrx_client *o);

/* Close the connection. */
void owrx_disconnect(owrx_client *o);

/* ================= exposed for unit testing ================= */

/* Handle one TEXT control message (greeting or JSON). Returns 0. */
int owrx_handle_text(owrx_client *o, const char *body, size_t len);

/* Handle one BINARY frame (leading type byte + payload). Returns an OWRX_*
 * code (AUDIO / WF / IDLE / ERR). */
int owrx_handle_binary(owrx_client *o, const unsigned char *frame, size_t len);

/* Minimal flat-JSON field extractors. They search for the quoted key anywhere
 * in `body` and read the value that follows. Return 1 on success, 0 if absent.
 * Good enough for OpenWebRX's small, flat config objects. */
int owrx_json_number(const char *body, const char *key, double *out);
int owrx_json_string(const char *body, const char *key, char *out, size_t cap);

/* Map one of our mode strings to the OpenWebRX modulation name. */
const char *owrx_map_mode(const char *mode);

/* Compute the (clamped) offset frequency in Hz for an absolute kHz tuning,
 * given center and sample rate. Exposed so the clamp is testable. */
long owrx_offset_hz(double freq_khz, double center_hz, double samp_rate);

#endif /* VITASDR_OWRX_H */
