# VitaSDR

A PS Vita homebrew client for web SDR receivers. Point it at a
[KiwiSDR](http://kiwisdr.com/) and it streams the audio, tunes it, and shows a
live RF waterfall — a shortwave/HF radio in your hands, using someone else's
antenna over the internet.

> Status: **v0.2.0 — working on real hardware.** Verified on a PS Vita PCH-1000:
> browse a live directory of public KiwiSDRs, connect, play audio, tune, and
> see the waterfall. The radio core also has host-side unit tests. See
> [Testing status](#testing-status).

## What works today

- KiwiSDR audio: IMA-ADPCM decode, windowed-sinc resampler, jitter-buffered
  playback via `sceAudioOut` (double-buffered); per-mode make-up gain
- On-startup **server picker** with the live public KiwiSDR directory, plus a
  user **favourites** list and manual add via the on-screen keyboard
- **Band-jump selector** (HF amateur/broadcast bands; VHF entries for later)
- Tuning (D-pad step + accelerated analog sweep), mode switching, keepalive
- Live RF waterfall (1024-bin, viridis) + spectrum, S-meter, passband overlay
- **Settings** screen: palette, waterfall speed, audio bandwidth, auto-connect/
  reconnect, keep-screen-awake, direct frequency entry, and more
- Bottom-left status panel: server + live battery / CPU / FPS / buffer stats
- Custom LiveArea icon & splash; crisp bundled UI font
- Config/favourites persisted under `ux0:data/vitasdr/`

Not yet: OpenWebRX support (protocol researched), touchscreen, per-button
remapping.

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
hand · **Square** refresh directory · **Triangle** Settings. Band selector:
Up/Down choose (hold to scroll, L/R skip 5), **X** jump, **Circle** back.

## Testing status

Verified on the host by `vitasdr_test` (68 assertions):

- IMA-ADPCM decode against a hand-traced vector
- jitter buffer FIFO order and drop-oldest overflow
- KiwiSDR `SND`, `MSG`, and `W/F` frame parsing, and band-plan lookup
- windowed-sinc resampler: image suppression, unity DC and mid-band gain
- plain-HTTP GET client and the KiwiSDR directory parser (chunk-split safe)
- full RFC 6455 handshake (with `Sec-WebSocket-Accept` verification) and
  binary frame round-trip against a loopback server, including auto ping/pong
  and the recv-timeout path that once misaligned the frame stream

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
- `wf_comp=0` (uncompressed bins) only
- OpenWebRX not yet implemented (protocol researched; VHF bands like FM are in
  the band list ready for it + a VHF-capable receiver)

## License

See `LICENSE`.
