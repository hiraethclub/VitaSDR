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

On first launch VitaSDR writes a default `ux0:data/vitasdr/config.ini` with a
placeholder host and shows a reminder. Edit that file (in VitaShell) to point
at your receiver:

```ini
host=your-kiwi.example.com
port=8073
password=
freq_khz=7074.000
mode=usb
```

Relaunch; it auto-connects when a real host is configured. Press **Start** to
connect/disconnect manually.

## Controls

| Input            | Action                                              |
|------------------|-----------------------------------------------------|
| D-pad L/R        | Tune down/up by one step (hold to repeat)           |
| D-pad U/D        | Change tuning step (1 Hz … 100 kHz)                 |
| Left stick L/R   | Sweep tuning (rate scales with how far you push)    |
| Right stick L/R  | Volume                                              |
| Right stick U/D  | Squelch                                             |
| L + R together   | Cycle mode (USB/LSB/AM/CW/NBFM)                     |
| Start            | Connect / disconnect                                |
| Select           | Toggle spectrum                                     |
| Circle           | Exit                                                |

## Testing status

Verified on the host by `vitasdr_test` (55 assertions):

- IMA-ADPCM decode against a hand-traced vector
- jitter buffer FIFO order and drop-oldest overflow
- KiwiSDR `SND`, `MSG`, and `W/F` frame parsing, and band-plan lookup
- windowed-sinc resampler: image suppression, unity DC and mid-band gain
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

- volume/clipping behaviour at high gain could be gentler
- verbose diagnostics and audio captures still write to `ux0:data/vitasdr/`
- `wf_comp=0` (uncompressed bins) only; the compressed-waterfall path and
  OpenWebRX are not yet implemented

## License

See `LICENSE`.
