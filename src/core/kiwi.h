/* KiwiSDR protocol client (audio / SND stream).
 *
 * Portable: no Vita headers. Drives one WebSocket connection to a KiwiSDR's
 * SND stream, performs the handshake, tunes, and decodes IMA-ADPCM audio into
 * a caller-supplied jitter buffer.
 *
 * Wire format follows the reference client (jks-prv/kiwiclient), not the loose
 * summary in the project README:
 *   - All frames are binary and begin with a 3-byte ASCII tag: "MSG", "SND",
 *     "W/F", ...
 *   - MSG frames carry space-separated key=value control tokens.
 *   - SND frames: tag "SND", then flags(u8), seq(u32 LE), smeter(u16 BE),
 *     then the audio payload (IMA-ADPCM when the COMPRESSED flag is set,
 *     otherwise raw int16 little-endian).
 *
 * Handshake:
 *   -> SET auth t=kiwi p=<password>
 *   <- MSG audio_rate=<N>      => SET AR OK in=<N> out=<N>
 *   <- MSG sample_rate=<f>     => SET mod=..., SET compression=1, SET keepalive
 *   <- MSG audio_adpcm_state=<index>,<prev>  (decoder resync, optional)
 *
 * Single-threaded: all sends happen from whichever thread calls kiwi_poll /
 * kiwi_set_*. Do not call from two threads concurrently.
 */
#ifndef VITASDR_KIWI_H
#define VITASDR_KIWI_H

#include "adpcm.h"
#include "jitter.h"
#include "ws_client.h"

/* kiwi_poll return codes. */
enum {
    KIWI_IDLE  = 0,   /* processed a control/other frame, or timed out */
    KIWI_AUDIO = 1,   /* decoded and pushed an audio frame */
    KIWI_ERR   = -1   /* connection closed or error */
};

typedef struct {
    ws_client   ws;
    adpcm_state adpcm;
    jitter_buf *sink;        /* where decoded PCM goes; owned by caller */

    int         audio_rate;  /* negotiated input sample rate, Hz (e.g. 12000) */
    int         ar_sent;     /* whether SET AR OK has been sent */
    int         configured;  /* whether rx params have been sent */

    /* current tuning state (mirrors what we last told the server) */
    double      freq_khz;
    char        mode[8];
    int         low_cut;
    int         high_cut;

    /* status, updated as SND frames arrive */
    int         smeter;      /* raw s-meter value from the last SND frame */
    float       rssi_dbm;    /* 0.1*smeter - 127 */
    unsigned    seq;         /* last audio sequence number */
    unsigned long samples_rx;/* total PCM samples decoded */

    /* Diagnostics: most recent MSG control text and a counter that bumps each
     * time a new one arrives, so the caller can log server-side chatter. */
    char        last_msg[160];
    unsigned    msg_seq;
} kiwi_client;

/* Default passband edges (Hz) for a mode. */
void kiwi_default_passband(const char *mode, int *low_cut, int *high_cut);

/* Connect and start the handshake. `password` may be "" for open receivers.
 * `sink` receives decoded audio. `freq_khz` and `mode` set the initial tuning.
 * Returns 0 on success, <0 on failure. */
int kiwi_connect(kiwi_client *k, const char *host, int port,
                 const char *password, jitter_buf *sink,
                 double freq_khz, const char *mode, int timeout_ms);

/* Process at most one incoming frame, waiting up to timeout_ms. Returns one of
 * the KIWI_* codes. Drives the handshake automatically. */
int kiwi_poll(kiwi_client *k, int timeout_ms);

/* Retune. freq is in kHz. Returns 0 on success, <0 on error. */
int kiwi_set_frequency(kiwi_client *k, double freq_khz);

/* Change demodulation mode and passband. Passing low_cut==high_cut==0 uses the
 * mode's default passband. Returns 0 on success, <0 on error. */
int kiwi_set_mode(kiwi_client *k, const char *mode, int low_cut, int high_cut);

/* Send a keepalive. Call about once per second. Returns 0 on success. */
int kiwi_keepalive(kiwi_client *k);

/* Close the connection. */
void kiwi_disconnect(kiwi_client *k);

/* ---- Exposed for unit testing (parsing without a live server) ---- */

/* Parse a text MSG body (the bytes after the "MSG" tag). Updates handshake
 * state and may send responses. Returns 0. */
int kiwi_handle_msg(kiwi_client *k, const char *body, size_t len);

/* Parse a binary SND frame (the full frame including the 3-byte "SND" tag),
 * decode audio into the sink, and update status. Returns number of samples
 * pushed, or <0 on malformed input. */
int kiwi_handle_snd(kiwi_client *k, const unsigned char *frame, size_t len);

/* Optional debug tap: if set, called for each SND frame with the flags byte
 * and the raw audio payload (bytes after the 10-byte header, i.e. the raw
 * IMA-ADPCM when the COMPRESSED flag is set). NULL by default. */
extern void (*kiwi_snd_tap)(unsigned char flags, const unsigned char *audio,
                            int audio_len);

/* ================= Waterfall (W/F) stream ================= */

#define KIWI_WF_BINS 1024

/* A separate connection to the KiwiSDR's /<ts>/W/F stream, delivering the RF
 * waterfall as one byte of power per FFT bin (uncompressed; wf_comp=0). */
typedef struct {
    ws_client ws;
    int       configured;
    double    freq_khz;   /* center frequency */
    int       zoom;       /* 0..14; span = ~30 MHz / 2^zoom */
    unsigned  seq;
} kiwi_wf;

/* Connect and configure the waterfall centered at freq_khz with the given
 * zoom. Returns 0 on success, <0 on failure. */
int kiwi_wf_connect(kiwi_wf *w, const char *host, int port,
                    const char *password, double freq_khz, int zoom,
                    int timeout_ms);

/* Process one incoming frame. On a W/F frame, copies up to max_bins power
 * bytes into `bins` and returns the count (>0). Returns 0 for idle/other/
 * timeout, <0 on error. */
int kiwi_wf_poll(kiwi_wf *w, unsigned char *bins, int max_bins, int timeout_ms);

/* Recenter/zoom the waterfall. Returns 0 on success. */
int kiwi_wf_set_center(kiwi_wf *w, double freq_khz, int zoom);

/* Keepalive for the waterfall connection. */
int kiwi_wf_keepalive(kiwi_wf *w);

void kiwi_wf_disconnect(kiwi_wf *w);

/* Parse a W/F frame (including the 3-byte "W/F" tag): "W/F" + x_bin(u32 LE) +
 * flags_zoom(u32 LE) + seq(u32 LE) + bin bytes. Copies up to max_bins bytes
 * into `bins`, returns the number copied, or <0 if malformed. Testable. */
int kiwi_wf_parse(const unsigned char *frame, size_t len, unsigned char *bins,
                  int max_bins);

#endif /* VITASDR_KIWI_H */
