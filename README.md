# VitaSDR

A PS Vita homebrew client for web SDR receivers. Point it at a
[KiwiSDR](http://kiwisdr.com/) and it streams the audio, tunes it, and shows a
live RF waterfall — a shortwave/HF radio in your hands, using someone else's
antenna over the internet.

> Status: early. The radio core is tested on a host; the Vita front-end builds
> cleanly but has **not** yet been run on real hardware. See
> [Testing status](#testing-status).

## What works today

- KiwiSDR audio: IMA-ADPCM decode, jitter-buffered playback via `sceAudioOut`
- Tuning (accelerated analog stick), demodulation mode switching, keepalive
- Live RF waterfall (1024-bin) + spectrum line, S-meter from the RF signal
- Config persisted to `ux0:data/vitasdr/config.ini`

Not yet: OpenWebRX support, touchscreen, bookmarks, band-plan jumps, on-screen
menu, per-button remapping.

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

| Input            | Action                                     |
|------------------|--------------------------------------------|
| Left stick L/R   | Tune (gentle = fine, full deflection = fast) |
| Right stick L/R  | Volume                                     |
| Right stick U/D  | Squelch                                    |
| D-pad L/R        | Cycle tuning step (1 Hz … 100 kHz)         |
| L + R together   | Cycle mode (USB/LSB/AM/CW/NBFM)            |
| Start            | Connect / disconnect                       |
| Select           | Toggle spectrum                            |
| Circle           | Exit                                       |

## Testing status

Verified on the host by `vitasdr_test` (40 assertions):

- IMA-ADPCM decode against a hand-traced vector
- jitter buffer FIFO order and drop-oldest overflow
- KiwiSDR `SND`, `MSG`, and `W/F` frame parsing
- full RFC 6455 handshake (with `Sec-WebSocket-Accept` verification) and
  binary frame round-trip against a loopback server, including auto ping/pong

**Not yet verified**, and the first things to check on a real Vita:

1. Does it connect? (SceNet init, DNS resolve, ws handshake over real network)
2. Is there audio, at the right pitch? (12 kHz `sceAudioOut`, ADPCM continuity)
3. Does the waterfall render and scroll? (vita2d texture, `wf_comp=0` bins)
4. Do tuning and mode changes reach the server without stutter?

If audio is silent but the S-meter moves, suspect the audio thread / port
open. If the waterfall is noise, the receiver may be ignoring `wf_comp=0`
(compressed bins) — that path is not yet handled.

## License

See `LICENSE`.
