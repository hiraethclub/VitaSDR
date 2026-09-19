# VitaSDR

A PS Vita homebrew app that connects to web SDR receivers (KiwiSDR, and later
OpenWebRX/OpenWebRX+) and provides a radio client UI with waterfall display,
hardware controls, and audio. Think MagicSDR but for the PS Vita.

## Target platform

- PS Vita PCH-1000 (OLED), 960x544, ARM Cortex-A9 quad-core, 512MB RAM
- VitaSDK toolchain, C99, vita2d for rendering
- Packaged as a VPK for installation via VitaShell under HENkaku/enso CFW

## Build setup

Host build system: Debian/Ubuntu x86_64. Two build targets:

### Native core tests (no VitaSDK needed)

The portable core (websocket, protocol, ADPCM, jitter) builds as a native
Linux binary for testing on the host:

```
cmake -B build-native -DVITASDR_NATIVE=1 -DCMAKE_BUILD_TYPE=Release
cmake --build build-native
./build-native/vitasdr_test      # runs the unit tests
```

`-DVITASDR_NATIVE=1` forces the native path even when `$VITASDK` is set.

### Vita VPK

Requires VitaSDK. **It is NOT pre-installed** on a fresh environment (an
earlier version of this document wrongly claimed it lived at
`/usr/local/vitasdk`). Install it with vdpm:

```
git clone https://github.com/vitasdk/vdpm && cd vdpm
export VITASDK=/usr/local/vitasdk
export PATH=$VITASDK/bin:$PATH
./bootstrap-vitasdk.sh
# vdpm's pacman client may land without the execute bit; if install-all.sh
# reports "package client is not executable", fix it and retry:
chmod +x $VITASDK/libexec/vdpm/*
yes | vdpm libvita2d          # pulls freetype/png/jpeg/zlib too
```

Then build:

```
export VITASDK=/usr/local/vitasdk
cmake -B build-vita \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita       # produces build-vita/VitaSDR.vpk
```

## Architecture

```
src/
  core/            # portable, no Vita headers, gcc-buildable on Linux
    net.h          # transport interface the core is written against
    net_posix.c    # BSD-socket impl of net.h (native test build only)
    ws_client.c    # RFC 6455 websocket client (handshake, masked frames)
    kiwi.c         # KiwiSDR protocol: SND (audio) + W/F (waterfall)
    adpcm.c        # IMA-ADPCM decoder (reset on new stream)
    jitter.c       # bounded audio jitter buffer, drop-oldest on overflow
  vita/            # Vita-specific layer (SCE calls live here only)
    net.c          # SceNet impl of net.h (matches the POSIX signatures)
    audio.c        # sceAudioOut output thread, fed from jitter buffer
    waterfall.c    # vita2d scrolling waterfall texture
    ui.c           # status bar, spectrum, freq ruler, sliders
    input.c        # button + analog stick polling
    config.c       # load/save config.ini from ux0:data/vitasdr/
    app.h          # shared app state + subsystem interfaces
    main.c         # entry point, threads (net/wf/audio), render loop
test/
  test_core.c      # native unit tests for the core
```

The core never calls sockets directly; it calls `net.h`. `net_posix.c`
(tests) and `vita/net.c` (device) both implement that interface, so protocol
code is platform-agnostic and testable on the host.

## Protocols

### KiwiSDR (implemented)

Corrected from the reference client (jks-prv/kiwiclient); the original brief's
"3-byte header" description was wrong.

- Two separate plain-WebSocket connections: `/<timestamp>/SND` (audio) and
  `/<timestamp>/W/F` (waterfall). The timestamp is only a cache-buster.
- All frames are binary and begin with a 3-byte ASCII tag: `MSG`, `SND`, `W/F`.
- Handshake (first client message): `SET auth t=kiwi p=<password>`
  (empty password for open receivers).
- Server pushes `MSG` control frames of space-separated `key=value` tokens:
  - `audio_rate=<N>`  → reply `SET AR OK in=<N> out=<N>` (N is usually 12000)
  - `sample_rate=<f>` → send rx params: `SET mod=<m> low_cut=<lc> high_cut=<hc>
    freq=<kHz>`, `SET compression=1`, `SET squelch=0 max=0`, `SET keepalive`
  - `audio_adpcm_state=<index>,<prev>` → resync the ADPCM decoder
- `SND` frame: `"SND"` + flags(u8) + seq(u32 LE) + smeter(u16 BE) + payload.
  Payload is IMA-ADPCM when the `0x10` (COMPRESSED) flag is set, else raw
  int16 LE. RSSI(dBm) = 0.1*smeter - 127.
- `W/F` frame: `"W/F"` + x_bin(u32 LE) + flags_zoom(u32 LE) + seq(u32 LE) +
  one power byte per FFT bin (1024 bins). Waterfall config: `SET wf_comp=0`
  (uncompressed), `SET zoom=<z> cf=<kHz>`, `SET wf_speed=1`.
- Keepalive: `SET keepalive` on each connection about once per second.
- Retune: `SET mod=... freq=<kHz>`; recenter waterfall: `SET zoom=z cf=<kHz>`.

### OpenWebRX/OpenWebRX+ (not yet implemented)

Single WebSocket to `/ws/`; server greets `CLIENT DE SERVER openwebrx.py`;
JSON control, binary frames (type byte 1=FFT, 2=audio), IMA-ADPCM audio.
Planned for a later milestone.

## UI layout (960x544)

- Status bar (top ~30px): frequency readout `M.kkk.hhh MHz`, mode, tuning step,
  S-meter bar, connection status dot (green=connected, amber=connecting,
  red=error, grey=off).
- Waterfall (~320px): scrolls downward; palettes hot / greyscale / classic.
- Spectrum (~100px): live FFT line from the current waterfall bins.
- Bottom (~94px): frequency ruler (left 2/3) and SQL/VOL sliders (right 1/3).

## Default hardware controls (implemented subset)

| Input             | Action                                     |
|-------------------|--------------------------------------------|
| Left stick L/R    | Tune (accelerated: gentle=fine, full=fast) |
| Right stick L/R   | Volume                                     |
| Right stick U/D   | Squelch                                    |
| D-pad L/R         | Cycle tuning step (1Hz..100kHz)            |
| L + R together    | Cycle demodulation mode                    |
| Start             | Connect / disconnect                       |
| Select            | Toggle spectrum overlay                    |
| Circle            | Exit                                       |

Touchscreen, bookmarks, band-plan jumps, per-button remapping, and the main
menu are specified for later milestones and not yet built.

## Data files (ux0:data/vitasdr/)

- `config.ini` — host, port, password, last freq/mode, step, zoom, volume,
  squelch, palette. Written with defaults on first run; edit it to point at
  your receiver.
- `bookmarks.txt`, `recent.txt` — planned, not yet used.

## Conventions

- C99 throughout.
- `core/` must compile cleanly with gcc on Linux with no VitaSDK headers.
- All Vita SCE calls go in `vita/` only. `core/` uses the `net.h` transport,
  stdlib, stdint.
- Check SCE return codes; fail gracefully rather than crashing.
- `adpcm.c` exposes `adpcm_reset()`; call it on a new stream (mode change).
- `jitter.c` is a fixed ring, drops oldest on overflow, never blocks.
- `waterfall.c` keeps one full-width texture, shifts rows down per frame; no
  per-frame allocation.
- No dynamic allocation after startup except the jitter buffer pool (once).

## Current state

Milestone 1 reached: a buildable VPK that connects to a KiwiSDR, decodes and
plays IMA-ADPCM audio, tunes, and renders a live RF waterfall + spectrum with
an S-meter.

**Verified on the host** (native `vitasdr_test`, 40 assertions): ADPCM decode,
jitter buffer, KiwiSDR SND/MSG/WF parsing, and a full websocket handshake +
frame round-trip against a loopback server.

**Not yet verified on hardware.** The `vita/` layer (SceNet, sceAudioOut,
vita2d rendering, threading) compiles cleanly with `-Wall -Wextra` but has not
been run on a real Vita or emulator. The KiwiSDR live wire behaviour has not
been exercised against a real receiver from this environment (no plain-ws
egress). See README "Testing status" for what to check first on device.

Next steps: on-device shakedown; then bookmarks, touchscreen, palette/zoom
controls, and the OpenWebRX protocol.
