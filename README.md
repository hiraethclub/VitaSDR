# VitaSDR

A PS Vita homebrew client for web SDR receivers. Point it at a
[KiwiSDR](http://kiwisdr.com/) or an [OpenWebRX](https://www.openwebrx.de/)
receiver and it streams the audio, tunes it, and shows a live RF waterfall — a
shortwave/HF (and, via OpenWebRX, VHF) radio in your hands, using someone
else's antenna over the internet.

> Status: **v0.3.0 — working on real hardware.** Verified on a PS Vita PCH-1000:
> browse a live directory of public KiwiSDRs, connect, play audio, tune, and
> see the waterfall. v0.3.0 adds a server-picker filter system (free slots, SNR,
> location, distance) and the OpenWebRX/TLS client (host-validated). The radio
> core also has host-side unit tests. See [Testing status](#testing-status).

## What works today

- **Two receiver protocols**, tagged per server: **KiwiSDR** (two sockets,
  audio + waterfall) and **OpenWebRX / OpenWebRX+** (one socket carrying both).
- OpenWebRX over **`wss://` (TLS)** as well as plain `ws://`, so HTTPS-fronted
  receivers work; certificate verification is on by default with a Settings
  toggle. TLS is a vendored BearSSL with a compiled-in CA set (no cert file).
- Audio: IMA-ADPCM decode (continuous, SYNC-resynced for OpenWebRX),
  windowed-sinc resampler, jitter-buffered `sceAudioOut` playback
  (double-buffered); per-mode make-up gain
- On-startup **server picker** with the live public KiwiSDR directory, plus a
  user **favourites** list and manual add via the on-screen keyboard — type a
  bare host for a KiwiSDR, or an `https://…` URL for an OpenWebRX
- **Picker filters** (press L): hide receivers without room for both our
  connections (free slots ≥ 2), set a minimum SNR, match a location/name
  substring, or keep only receivers within a distance of a home point you set
  (as a Maidenhead grid or lat,lon); optional best-first sorting. Favourites are
  never filtered. Choices persist in `config.ini`
- **Band-jump selector** (HF amateur/broadcast bands — everything a KiwiSDR can
  reach; VHF bands are omitted until OpenWebRX support lands)
- Tuning (D-pad step + accelerated analog sweep), mode switching, keepalive
- Live RF waterfall (1024-bin, viridis) + spectrum, S-meter, passband overlay
- **Settings** screen: palette, waterfall speed, audio bandwidth, auto-connect/
  reconnect, keep-screen-awake, verify-TLS-certificate, direct frequency entry
- Bottom-left status panel: server + live battery / CPU / FPS / buffer stats
- Custom LiveArea icon & splash; crisp bundled UI font
- Config/favourites persisted under `ux0:data/vitasdr/`

OpenWebRX tunes by offset within the receiver's fixed sample window, so the
waterfall shows the whole span (not re-centred on the tuned signal yet).

Not yet: touchscreen, per-button remapping, OpenWebRX profile/band switching.

## Building

### Core tests (host, no VitaSDK)

```sh
cmake -B build-native -DVITASDR_NATIVE=1 -DCMAKE_BUILD_TYPE=Release
cmake --build build-native
./build-native/vitasdr_test
```

### VPK (requires VitaSDK)

Install VitaSDK + vita2d (see `CLAUDE.md` for the full recipe, including a
`chmod` workaround if vdpm's package client is missing its execute bit), then:

```sh
export VITASDK=/usr/local/vitasdk
cmake -B build-vita \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita
```

The VPK is `build-vita/VitaSDR.vpk`. Install it with VitaShell on a
HENkaku/enso Vita.

## First run

On launch VitaSDR fetches the live public KiwiSDR directory and shows a
**server picker**. Choose one (free receivers are listed first) and press **X**
to connect. Star receivers you like with **Start**, or add one by hand (e.g.
your own) with **Select** and the on-screen keyboard. Settings and favourites
are saved under `ux0:data/vitasdr/`; you can still edit `config.ini` directly if
you prefer. Turn on "auto-connect" in Settings to skip the picker and reconnect
to the last receiver on launch.

## Controls

Radio screen:

| Input            | Action                                              |
|------------------|-----------------------------------------------------|
| D-pad L/R        | Tune down/up by one step (hold to repeat)           |
| D-pad U/D        | Change tuning step (1 Hz … 100 kHz)                 |
| Left stick L/R   | Sweep tuning (rate scales with how far you push)    |
| Right stick      | Volume (L/R) or Squelch (U/D) — dominant axis only  |
| L + R together   | Cycle mode (USB/LSB/AM/CW/FM)                       |
| Start            | Connect / disconnect                                |
| Select           | Toggle spectrum                                     |
| Square           | Open the band-jump selector                         |
| Triangle         | Open Settings                                       |
| Circle           | Open the server picker (PS button exits the app)    |

Server picker: **X** connect · **Start** toggle favourite · **Select** add by
hand · **Square** refresh directory · **L** filters · **Triangle** Settings.
Band selector:
Up/Down choose (hold to scroll, L/R skip 5), **X** jump, **Circle** back.

## Testing status

Verified on the host by `vitasdr_test` (110 assertions):

- IMA-ADPCM decode against a hand-traced vector
- jitter buffer FIFO order and drop-oldest overflow
- KiwiSDR `SND`, `MSG`, and `W/F` frame parsing, and band-plan lookup
- OpenWebRX: config JSON extraction, offset/clamp tuning maths, FFT decimation,
  and the continuous SYNC-resynced audio decode across a split message
- windowed-sinc resampler: image suppression, unity DC and mid-band gain
- plain-HTTP GET client and the KiwiSDR directory parser (chunk-split safe)
- full RFC 6455 handshake (with `Sec-WebSocket-Accept` verification) and
  binary frame round-trip against a loopback server, including auto ping/pong
  and the recv-timeout path that once misaligned the frame stream

The OpenWebRX client and the TLS layer were additionally checked end-to-end
against a live OpenWebRX server from the build host (handshake, config, FFT and
audio decode), with certificate verification both on and off.

Verified on real hardware (PS Vita PCH-1000):

- connects over the real network (SceNet init, DNS resolve, ws handshake)
- audio plays at the correct pitch; a windowed-sinc upsampler and double-
  buffered `sceAudioOut` output removed the earlier metallic/buzzing artifact
- waterfall renders and scrolls (viridis palette, adaptive contrast)
- tuning, mode changes, S-meter, spectrum, passband and band labels all live

Known rough edges (not blocking, next on the list):

- some public receivers allow only one connection per IP, so you get audio but
  no waterfall; the UI labels this ("Audio only")
- verbose diagnostics and audio captures still write to `ux0:data/vitasdr/`
- KiwiSDR waterfall is `wf_comp=0` (uncompressed bins) only
- OpenWebRX support is new: validated end-to-end against a live server from the
  build host (TLS handshake, config, FFT + audio decode) but not yet shaken out
  on real Vita hardware; the waterfall shows the full sample-rate span rather
  than re-centring on the tuned offset

## License

See `LICENSE`.
