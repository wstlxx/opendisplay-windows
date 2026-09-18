# Phase 1 — Research summary: OpenDisplay pv3 receiver requirements

Sources (cloned to /tmp/od_research):

- `peetzweg/opendisplay` — canonical `PROTOCOL.md` (authoritative), Mac sender
  (`Mac/MacSender.swift`), `tools/fake-receiver.swift`, `Shared/StreamReceiver.swift`
- `josepacelli/opendisplay-android` — receiver reference: `net/Framing.kt`,
  `net/AnnexB.kt`, `net/PhoneReceiver.kt`, `video/VideoDecoder.kt`,
  `video/H264Sps.kt` + unit tests
- `gprot42/android-opendisplay` — second receiver reference (Kotlin,
  `ReceiverServer.kt`, `FrameCodec.kt`, `H264Decoder.kt`)
- `martinhoess/opendisplay-win` — Windows *sender* reference: Winsock
  techniques (`net/Connection.cpp`), JSON field scanner (`net/Protocol.cpp`),
  Annex-B encoder, Media Foundation encoder, CMake setup,
  `tools/mock_receiver.py` (useful test harness)

## Exact pv3 receiver requirements (normative, distilled from PROTOCOL.md)

### Transport
1. Receiver **listens** on TCP port **9000** (sender connects). Keep the role split.
2. One sender at a time. A new inbound connection **MUST** replace the active
   one; the old session is dropped and decoder state reset (spec §1).
3. Disable Nagle (`TCP_NODELAY`).
4. No TLS/auth at pv 3.
5. Silence: drop the connection after **> 5 s** without bytes from the sender
   (spec §8.2); send `ping` every **2 s** so both sides stay alive.

### Framing (spec §3)
- Every frame both directions: `[4-byte big-endian payload length][payload]`.
- Length counts payload only. Frames may be split across reads or packed
  together — buffer and reassemble.
- Receiver→sender payloads MUST be 1..2^20-1 bytes (we only send small JSON).
- Sender→receiver: no hard max; guard with a sane cap (Android uses 16 MiB).

### Demux (spec §4, deprecated heuristic — follow literally)
A sender→receiver payload is a **JSON control message** iff ALL hold:
1. length < 32768, 2. first byte is `{` (0x7B), 3. no NUL byte in payload.
Anything else is a **video frame**. Isolate this decision in one function
(the spec explicitly asks for cheap pv4 swap).

### Video (spec §5)
- H.264 Annex-B, one access unit (one picture) per wire frame.
- Layout: `[optional telemetry JSON prefix][00 00 00 01][NALU]...`
  - Telemetry prefix = everything before the first start code:
    `{"cap":<ms>,"snd":<ms>}` (Unix-epoch ms, sender clock). MUST tolerate
    absence, ignore unknown fields.
  - Start codes are **always 4 bytes** (`00 00 00 01`); sender MUST NOT emit
    3-byte codes, so split on the 4-byte pattern only. (Safe: emulation
    prevention makes `00 00 00` impossible inside a NALU, so the pattern can
    never occur mid-NALU.)
- Every IDR carries the current SPS+PPS NALUs in front of its slices.
  Non-keyframes carry only slices (+ optional SEI, NAL type 6 — may skip).
- All slices of one picture arrive in one wire frame → decode each wire frame
  as one sample.
- **No presentation timestamps.** Present in arrival order, as fast as
  arrival (no B-frames in the official sender).
- Video dimensions come **from the SPS, never from `hello`** (spec §5.2).
- On SPS/PPS change (rotation / quality change): detect, **rebuild decoder**,
  discard buffered frames. The sender only re-sends parameter sets when they
  actually change, so the receiver must remember the last-seen pair; after a
  decoder failure the remembered pair must be forgotten so a byte-identical
  keyframe still triggers a fresh rebuild (Android `VideoDecoder.signalError`).
- Decode failure (desync, dropped AU, codec error) → send `{"type":"kf"}`;
  sender MUST make the next frame an IDR. Debounce kf requests (Android:
  ≥ 1 s between).
- `streamConfig` (pv3 additive): sender announces `codec`/`width`/`height`/
  `framesPerSecond` before first video; ignore unknown fields; absence means
  legacy H.264. We log it and let the SPS be the source of truth.

### Control messages — receiver → sender (spec §6.1)
| msg | requirement |
|---|---|
| `hello` | **MUST be the first message** on every new connection. Fields: `pixelsWide`,`pixelsHigh` (panel px, current orientation), `scale`, optional `device`, `id` (stable UUID), `pv` (=3), optional `maxEncodeWide`/`maxEncodeHigh` (decode ceiling). Re-send when announced dimensions change (window resize), idempotent. Sender replies `welcome` to every hello. |
| `ping` | `t` = receiver clock ms-epoch; every ~2 s. |
| `kf` | request IDR on decode loss. |
| `stats` | free-form, every ~5 s (optional; we send fps/bytes basics). |
| `closing` | best-effort on app quit. |

`touch`/`scroll`/`pencil`/`proximity`/`sleeping`/cursor-ack are input /
later-stage features (initial scope: none, or only `closing`).

### Control messages — sender → receiver (spec §6.2)
| msg | handling |
|---|---|
| `pong` | `t`,`mt` → RTT + clock offset (NTP-style; discard rtt<0 or ≥2000 ms; keep last 15; offset of min-RTT sample). Optional in scope; implement for diagnostics. |
| `ping` | liveness + health counters; informational, no reply. |
| `welcome` | `pv`,`min` → remember peer pv (absent welcome ⇒ peer pv 1). |
| `updateRequired` | surface message; do not depend on video stopping. |
| `cursor` / `cursorImg` | MAY ignore (no cursor rendering in initial scope). |
| `streamConfig` | log; assume H.264. |

Rules: **unknown `type` MUST be ignored** (log at most once per type),
unknown fields ignored, unparseable control JSON ignored (not fatal).

### Session lifecycle (spec §9)
connect → `hello` → `welcome` → video (IDR first) → steady state
(pings both ways, ~2 s) → on death: sender redials (1 s); receiver just
keeps listening. Receiver sends `sleeping`/`closing` best-effort before
closing; absence means nothing.

## Implementation traps found in the references

1. **Telemetry prefix + demux interplay**: video frames *start* with `{`
   (the prefix). The NUL check is what saves the heuristic — never classify
   by first byte alone. (`AnnexB.isControlJson`.)
2. **SPS-only-when-changed**: you cannot detect a resolution change by
   "does this frame carry an SPS"; you must byte-compare against the
   last-seen SPS/PPS. After a decoder reset, clear the remembered pair.
   (Android `VideoDecoder`.)
3. **Rebuild throttling**: Android throttles reconfigure to ≥ 1 s apart and
   keeps the new SPS/PPS pending. A well-behaved sender never triggers a
   rebuild per frame, but guard anyway.
4. **SPS dimensions**: parse the SPS itself (Exp-Golomb, emulation
   prevention removal, cropping). MF's reported output size is a fallback
   only. (`H264Sps.kt` — port to C++; unit-test with real SPS bytes.)
5. **No timestamps**: do not build PTS logic; present-on-arrival with a
   small (≤ 4 frames) drop-oldest buffer, like the Android receiver.
6. **Sender health on `ping`** is informational only; do not treat missing
   optional fields as errors.
7. **Frame-size guard**: corrupt length prefix → reject/drop session rather
   than allocate gigabytes (Android caps at 16 MiB; we use the same).
8. **Windows**: MF H.264 decode of a raw Annex-B stream is done via a media
   source over a custom `IMFByteStream` + source reader with
   `MF_LOW_LATENCY`, `MF_DECODE_TO_DISPLAY` (HW decoder), and an
   `IMFDXGIDeviceManager` so output samples hand us D3D11 textures directly
   (`MFGetD3DSurfaceFromSampleBuffer`). NV12 is directly samplable in D3D11
   → letterbox quad + pixel shader, zero CPU copies.

## Minimal receiver architecture

```
[Winsock listener :9000]          (net/tcp_listener)
        │ accept; new connection replaces old session
        ▼
[Session read thread]             (net/session)
   recv → FrameDecoder (4-byte BE deframe)
        ├── IsControlJson? → ControlParser → SessionController
        │     (pong/offset, welcome, streamConfig, cursor-ignored,
        │      updateRequired, liveness watchdog, 2 s ping timer)
        └── else → AnnexBParser → VideoFrame{sps?,pps?,vcl[],cap,snd}
                → VideoQueue (≤4, drop-oldest)
        ▼
[Decoder thread]                  (video/h264_decoder)
   drain queue → SPS/PPS change? rebuild MF source (byte stream)
   feed Annex-B AU → async source reader (MF_LOW_LATENCY, DXVA2 HW decode,
   DXGI device manager shared with renderer)
   output sample → D3D11Texture2D (NV12) → FrameSink (latest-frame slot)
   decode error → kf request (debounced 1 s)
        ▼
[Main/Win32 thread]               (render/*, app/main)
   window + swap chain; present latest texture on arrival or message
   letterbox + aspect; resize; borderless fullscreen toggle; clean shutdown
```

Modules `protocol/` (pure C++, no Win32 — unit-testable on Linux),
`net/`, `video/`, `render/` are separate translation units / directories so a
future pv4 typed frame header only touches `protocol/` + the demux call site.
