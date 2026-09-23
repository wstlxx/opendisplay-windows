# opendisplay_windows

A native **Windows 10/11 receiver** for the [OpenDisplay](https://github.com/peetzweg/opendisplay)
protocol (pv 3). It receives an H.264 screen-cast from a Mac running the OpenDisplay
sender, decodes it with **Media Foundation** (hardware DXVA where available) and renders
it with **Direct3D 11** in a window or fullscreen.

No Electron, no Python, no VLC — one small C++20/Win32 executable.

```
 Mac sender (OpenDisplay)                    this receiver
 ┌──────────────┐   TCP :9000   ┌──────────────────────────────────────────────┐
 │ H.264 encode │ ─────────────▶ │ session read thread → VideoQueue → decode    │
 │ + JSON ctrl  │                │ thread → MF byte stream → reader thread →    │
 └──────────────┘                │ D3D11 texture → main-thread Renderer::Present│
                                 └──────────────────────────────────────────────┘
```

## Features

- **pv 3 protocol**: length-prefixed framing, JSON control demux, Annex-B parsing,
  SPS/PPS extraction, SPS dimension decode.
- **Bonjour/mDNS discovery**: advertises `_opensidecar._tcp` (TXT `id` = the stable
  per-install hello id, `pv` = 3) so a Mac sender finds it on the LAN; answers
  queries and sends periodic announcements (works even if the OS holds port 5353).
- **Real-time hardware decode** via Media Foundation (`MF_LOW_LATENCY`,
  `MF_DECODE_TO_DISPLAY`, shared D3D11 device through the DXGI device manager).
  Falls back to software decode + CPU upload when no DXVA device is present.
- **D3D11 presentation**: letterboxed, aspect-correct, `FLIP_DISCARD` swap chain,
  `MF_MT_DEFAULT_CROP` applied (correct 16:9 output from 1080p content).
- **Live resolution changes**: when the sender's SPS changes (screen resized on the
  Mac) the decode pipeline is torn down and rebuilt on the next keyframe.
- **Reliability**: bounded drop-oldest video queue, 5 s liveness timeout, 2 s
  receiver→sender RTT pings, keyframe requests on decode errors, new-connection
  replaces old.
- **Diagnostics**: periodic `stats` to the sender (fps/rtt), console log.

## Layout

```
protocol/   Platform-independent protocol core (framing, demux, Annex-B, SPS,
            control JSON). Pure C++20 — compiles and unit-tests on any OS.
net/        Winsock listener + per-connection session (read loop, control I/O).
video/      Media Foundation H.264 decoder (IMFByteStream + source reader).
render/     D3D11 letterbox renderer (fullscreen triangle + crop).
app/        main(): window, D3D device, threading wiring, fullscreen, stats.
tools/      sim_sender.py (Mac-sender simulator) + fake_receiver.cpp
            (cross-language integration harness over real TCP).
tests/      Protocol unit tests + a real 640x480 H.264 sample (tests/data).
docs/       RESEARCH.md — the distilled normative pv3 requirements + rationale.
```

## Build

### The receiver (Windows, Visual Studio 2022)

```powershell
cmake -B build
cmake --build build --config Release
build\Release\opendisplay_receiver.exe
```

The static CRT is used, so the `.exe` runs on machines without the VC++ redist.

> **CI.** The full app is built in CI on a `windows-2022` runner (MSVC): the
> Winsock `od_net`, Media Foundation `od_video`, D3D11 `od_render`, and
> `opendisplay_receiver` targets all compile, the protocol unit tests run, and
> the `.exe` is attached to the run as a downloadable artifact. The local build
> above produces the same binary.

### Protocol module + tests (any OS, no Windows SDK needed)

The protocol core has no platform dependency and is fully tested off-Windows:

```bash
cmake -B build && cmake --build build
./build/od_tests          # or: ctest --test-dir build
```

Or without CMake (just a compiler):

```bash
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -I. \
    protocol/*.cpp tests/test_protocol.cpp -o /tmp/test_protocol
/tmp/test_protocol
```

## Run

```
opendisplay_receiver.exe [--port N] [--log file] [--name S]
```

1. Start the receiver (defaults to `0.0.0.0:9000`). It advertises itself on the
   LAN via Bonjour (`_opensidecar._tcp`); the name defaults to the computer name
   or `--name S`.
2. On the Mac, the OpenDisplay sender discovers it automatically (or you can dial
   the machine's IP:port directly). It connects, sends `hello` → you reply
   `welcome`, then streams H.264 + control JSON.
3. Place `config.ini` beside the `.exe` and edit it before starting the receiver.
   Windowed 1280x720 is the default. In fullscreen, **Ctrl+Shift+Alt+Q** quits;
   F1, F11 and Esc are not receiver shortcuts.

```ini
[display]
width = 1280
height = 720
fullscreen = false
adaptive_resolution = false

[video]
bitrate_kbps = 18000
```

`width` and `height` set the initial window client size and the Mac virtual
display size. With `adaptive_resolution = false`, resizing the window scales
the existing stream. With `true`, the receiver sends a new `hello` after the
window size has been stable for 500 ms. The requested size follows both client
dimensions, rounded down to even pixels; widening a 1280×720 window to
1412×720 requests a 1412×720 stream. At most one size request is sent
every 20 seconds to avoid overlapping Mac rebuilds. A new size request makes
the current Mac sender rebuild its virtual display and encoder, which can pause
video. Fullscreen startup uses the monitor size when adaptation is on.
The resolution values must be even (width 320-8192, height 240-8192).

`bitrate_kbps` is sent as an optional `hello.bitrateKbps` request. The current
official Mac sender ignores it and uses the bitrate of its selected quality
preset; setting this value alone does not change the encoded bitrate. A future
sender can honor this additive field. The allowed range is 1000-100000 kbps.

The console prints the connection, the decoded stream size, the negotiated RTT,
and a periodic `fps`/`rtt` line.

## Testing without a Mac

`tools/sim_sender.py` re-encodes nothing — it replays a recorded Annex-B stream
(`tests/data/sample.h264`, 640×480@25) with the exact pv 3 framing and control
messages a real sender produces. `tools/fake_receiver.cpp` links the real
protocol module over a POSIX socket, so you get a genuine cross-language
integration test:

```bash
g++ -std=c++20 -O2 -Wall -Wextra -I. protocol/*.cpp \
    tools/fake_receiver.cpp -o /tmp/fake_receiver
/tmp/fake_receiver --port 9127 --seconds 8 &
python3 tools/sim_sender.py --port 9127 --file tests/data/sample.h264
```

Expect: `hello`/`welcome` handshake, ~25 fps video, `pong` RTT, and
`RESULT: PASS` when the run completes.

## Architecture & threading

```
listener thread        accept() → new Session (replaces any old one)
  └─ session read thread   recv → FrameDecoder → demux
        ├─ JSON control  → onControl   (main-thread-safe: stats, welcome, ping)
        └─ video (Annex-B) → onVideo → VideoQueue (cap 4, drop-oldest)
decode thread          VideoQueue.PopWait → H264Decoder::Submit
  └─ MF parser/reader thread   ReadSample → onFrame (publishes DecodedFrame)
main thread (Win32)    message loop → Renderer::Present(latest DecodedFrame)
```

Key invariants (see `docs/RESEARCH.md` for the full reasoning):

- **Video samples are owned end-to-end**: the session re-serializes each frame
  into Annex-B with 4-byte start codes and hands a `VideoSample` (its own
  `std::vector`) to the queue, so no buffer is shared across threads.
- **SPS/PPS byte-compare drives pipeline rebuilds** (the sender re-encodes on a
  new SPS); a decode error drops to "await keyframe" and asks for one.
- **The session's callbacks capture a `weak_ptr` box, not the session itself** —
  a strong self-reference would leak the session and its read thread.
- The D3D11 device is created once and shared with Media Foundation through
  `IMFDXGIDeviceManager`, so decoded frames land directly on the render device.

## Protocol summary (pv 3)

Every message both directions is `[4-byte big-endian length][payload]`.

- A payload is **JSON control** iff `len < 32768` **and** first byte is `{`
  (0x7B) **and** it contains no NUL byte; otherwise it is a **video frame**.
- **Sender → receiver**: `welcome`, `streamConfig`, `pong`, `ping` (health),
  `updateRequired`, `cursor`.
- **Receiver → sender**: `hello` (first message, carries virtual display size/scale/id),
  `ping` (RTT, every 2 s), `kf` (keyframe request), `stats` (every ~5 s),
  `closing`.
- Video is Annex-B; when an SPS/PPS is present the sender inlines it with the
  keyframe.

Full normative detail is in `docs/RESEARCH.md`.

## Limitations / known issues

- Single sender at a time (new connection replaces the old one) — per spec.
- Cursor rendering is parsed but not drawn (out of initial scope).
- With adaptive resolution disabled, resizing or maximizing the window scales
  the configured virtual display without restarting Mac capture. Incoming
  streams at other sizes are letterboxed in the window.
- The Windows-only layer (`net/`, `video/`, `render/`, `app/`) needs the
  Windows SDK, so it is built in CI on a `windows-2022` runner (not on Linux).
  It is additionally cross-checked by the protocol-level integration test.
- The CI runner ships a reduced Windows SDK: `ID3D11Texture2D::GetDesc`, no
  `MF_MT_DEFAULT_CROP`/`MF_ATTRIBUTE_VALUE_TYPE`, and no
  `IMFDXGIDeviceManager::GetDevice`. The code is written against that surface
  (and works against the full SDK too).
