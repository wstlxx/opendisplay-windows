#!/usr/bin/env python3
"""OpenDisplay pv3 sender simulator.

Speaks the receiver side of the protocol well enough to exercise a receiver:

  * 4-byte big-endian length framing (PROTOCOL.md section 3)
  * sends `welcome`, `streamConfig`, periodic sender `ping`
  * streams an Annex-B .h264 file as access units, each prefixed with the
    `{"cap":<ms>,"snd":<ms>}` telemetry blob (PROTOCOL.md 5.1)
  * answers receiver `ping` with `pong` (echo t, add mt)
  * honours `kf` by jumping to the next IDR (with SPS/PPS if present)
  * logs receiver `stats` and `hello`

Run on any machine (macOS/Linux) pointed at the Windows receiver:

    python3 tools/sim_sender.py --file tests/data/sample.h264 --host <win-ip>

The sample file (tests/data/sample.h264) is a 640x480 H.264 capture.
"""

import argparse
import json
import socket
import struct
import sys
import time


def now_ms() -> int:
    return int(time.time() * 1000)


def encode_frame(payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + payload


class FrameReader:
    """Incremental reader for 4-byte-BE length-prefixed frames.

    Buffers partial frames across recv() calls, exactly like the C++
    FrameDecoder does, so split TCP segments never desync the stream.
    """

    MAX_FRAME = 16 * 1024 * 1024

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = b""

    def poll_frame(self, timeout: float) -> bytes | None:
        """Return one complete frame, or None on timeout. Raises
        ConnectionError if the peer closed or the stream is corrupt."""
        self.sock.settimeout(timeout)
        while True:
            if len(self.buf) >= 4:
                (length,) = struct.unpack(">I", self.buf[:4])
                if length > self.MAX_FRAME:
                    raise ConnectionError(f"corrupt frame length {length}")
                if len(self.buf) >= 4 + length:
                    frame = self.buf[4:4 + length]
                    self.buf = self.buf[4 + length:]
                    return frame
            try:
                chunk = self.sock.recv(1 << 20)
            except socket.timeout:
                return None
            if not chunk:
                raise ConnectionError("peer closed")
            self.buf += chunk
            if len(self.buf) > self.MAX_FRAME + 8:
                raise ConnectionError("frame buffer overrun")


def split_nalus(data: bytes):
    """Yield (start, end) offsets of NALUs in an Annex-B byte stream."""
    starts = []
    i = 0
    n = len(data)
    while i + 4 <= n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0 and data[i + 3] == 1:
            starts.append(i + 4)
            i += 4
        else:
            i += 1
    nalus = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 4 if k + 1 < len(starts) else n
        if e > s:
            nalus.append((s, e))
    return nalus


def nalu_type(data: bytes, start: int) -> int:
    return data[start] & 0x1F


VCL_TYPES = {1, 4, 5, 19, 20}  # non-IDR slice, IDRs, SEI-as-VCL not included


def build_access_units(data: bytes):
    """Split into access units: SPS/PPS/SEI ride with the following slice."""
    nalus = split_nalus(data)
    aus = []
    pending = []
    for s, e in nalus:
        t = nalu_type(data, s)
        if t in VCL_TYPES and t not in (6,):  # 6 = SEI (not in VCL_TYPES anyway)
            aus.append(pending + [(s, e)])
            pending = []
        else:
            pending.append((s, e))
    return aus


def nalu_is_idr(data: bytes, au) -> bool:
    for s, e in au:
        if nalu_type(data, s) == 5:
            return True
    return False


class SimSender:
    def __init__(self, args):
        self.args = args
        self.data = open(args.file, "rb").read()
        self.aus = build_access_units(self.data)
        self.idr_indexes = [i for i, au in enumerate(self.aus) if nalu_is_idr(self.data, au)]
        if not self.idr_indexes:
            sys.exit("no IDR frames in file; cannot start the stream")
        self.need_idr = True
        print(f"loaded {args.file}: {len(self.aus)} access units, "
              f"{len(self.idr_indexes)} IDRs", flush=True)

    def send_video(self, sock, au):
        # The Annex-B byte stream includes the 4-byte start codes: from the
        # first NALU's start code through the last NALU's final byte.
        s = au[0][0] - 4
        e = au[-1][1]
        blob = self.data[s:e]
        telem = json.dumps({"cap": now_ms(), "snd": now_ms()}, separators=(",", ":"))
        sock.sendall(encode_frame(telem.encode() + blob))

    def run(self):
        host, port = self.args.host, self.args.port
        sock = socket.create_connection((host, port), timeout=10)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"connected to {host}:{port}", flush=True)
        try:
            self.session(sock)
        except ConnectionError as e:
            print(f"connection closed: {e}", flush=True)
        finally:
            sock.close()

    def session(self, sock):
        sent_welcome = False
        last_sender_ping = 0.0
        start = time.time()

        au_index = None  # set after the first hello
        next_frame_at = 0.0

        reader = FrameReader(sock)
        while True:
            # --- control read (10 ms poll; keep frame pacing tight) ----
            payload = reader.poll_frame(0.01)
            if payload is not None:
                if payload[:1] == b"{":
                    try:
                        msg = json.loads(payload)
                    except json.JSONDecodeError:
                        continue
                    t = msg.get("type")
                    if t == "hello":
                        print(f"hello: {msg}", flush=True)
                        sent_welcome = True
                        sock.sendall(encode_frame(
                            json.dumps({"type": "welcome", "pv": 3, "min": 1}).encode()))
                        sc = {"type": "streamConfig", "codec": "h264",
                              "width": self.args.width, "height": self.args.height,
                              "framesPerSecond": self.args.fps}
                        sock.sendall(encode_frame(json.dumps(sc).encode()))
                        self.need_idr = True
                    elif t == "ping":
                        pong = {"type": "pong", "t": msg.get("t"), "mt": now_ms()}
                        sock.sendall(encode_frame(json.dumps(pong).encode()))
                    elif t == "kf":
                        print("kf request: jumping to next IDR", flush=True)
                        self.need_idr = True
                    elif t == "stats":
                        print(f"receiver stats: {msg}", flush=True)
                    elif t == "closing":
                        print("receiver closing; exiting", flush=True)
                        return
                    else:
                        print(f"control: {msg}", flush=True)
                else:
                    # A video frame from the receiver? Not expected.
                    print(f"unexpected video frame from receiver "
                          f"({len(payload)} bytes)", flush=True)

            # --- sender ping every 2 s ------------------------------------
            now = time.time()
            if sent_welcome and now - last_sender_ping >= 2.0:
                last_sender_ping = now
                sock.sendall(encode_frame(json.dumps(
                    {"type": "ping", "drops": 0, "pending": 0,
                     "capFps": self.args.fps}).encode()))

            # --- video ------------------------------------------------------
            if not sent_welcome:
                continue
            if self.need_idr:
                # (Re)start at a keyframe; for a looped file that is the
                # first IDR. Mirrors the real sender's kf handling.
                if not self.idr_indexes:
                    continue
                au_index = self.idr_indexes[0]
                self.need_idr = False
                next_frame_at = now

            frame_interval = 1.0 / self.args.fps
            if now < next_frame_at:
                time.sleep(min(0.005, next_frame_at - now))
                continue
            next_frame_at += frame_interval
            if next_frame_at < now:  # fell behind; catch up
                next_frame_at = now

            au = self.aus[au_index]
            if nalu_is_idr(self.data, au):
                self.need_idr = False
            self.send_video(sock, au)
            if not self.args.loop:
                au_index += 1
                if au_index >= len(self.aus):
                    print("end of file (use --loop to repeat)", flush=True)
                    au_index = None
                    self.need_idr = True
                    next_frame_at = time.time() + 0.5
            else:
                au_index += 1
                if au_index >= len(self.aus):
                    au_index = self.idr_indexes[0]
                    self.need_idr = False


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--file", default="tests/data/sample.h264")
    ap.add_argument("--fps", type=float, default=25.0)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--loop", action="store_true", default=True)
    args = ap.parse_args()

    SimSender(args).run()


if __name__ == "__main__":
    main()
