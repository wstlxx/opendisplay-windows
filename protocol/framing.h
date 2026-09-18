// OpenDisplay wire framing (PROTOCOL.md section 3).
//
// Every message in both directions is length-prefixed:
//   [4-byte payload length, unsigned, big-endian][payload]
//
// TCP has no message boundaries: frames may arrive split across many reads
// or packed together in one read, so the decoder accumulates bytes and
// extracts complete payloads. Pure byte manipulation, no platform
// dependency, unit-testable.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace od {

inline constexpr std::size_t kFrameHeaderSize = 4;

// Defensive cap for sender->receiver frames. The wire protocol defines no
// hard maximum (a keyframe of a large panel is in the low megabytes); this
// bounds a corrupt/hostile length prefix from forcing a huge allocation.
// Same value as the Android receiver.
inline constexpr std::size_t kMaxFrameSize = 16 * 1024 * 1024;

// Hard cap for receiver->sender payloads (PROTOCOL.md section 3: the
// official sender treats length 0 or >= 2^20 as a protocol error).
inline constexpr std::size_t kMaxOutgoingFrameSize = (1u << 20) - 1;

// Reads a 4-byte big-endian unsigned integer. Requires size >= 4.
inline uint32_t ReadInt32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Writes a 4-byte big-endian unsigned integer.
inline void WriteInt32BE(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint32_t(v >> 16);
    p[2] = uint32_t(v >> 8);
    p[3] = uint8_t(v);
}

// Wraps payload with its 4-byte big-endian length prefix.
std::vector<uint8_t> EncodeFrame(std::span<const uint8_t> payload);

// Encodes a JSON/UTF-8 control message as one wire frame.
std::vector<uint8_t> EncodeFrame(const std::string& payload);

// Accumulates fed bytes and extracts complete [len][payload] frames.
// Feed from a single reader thread (the session read loop).
class FrameDecoder {
public:
    // Feeds bytes read off the wire and returns every complete payload now
    // available, in order. Payloads are views into internal storage valid
    // until the next Feed() call.
    //
    // If a frame's declared length is 0 or exceeds kMaxFrameSize (corrupt or
    // hostile prefix), *valid is set to false and the decoder must be
    // abandoned: the session treats this as fatal for the connection.
    std::vector<std::span<const uint8_t>> Feed(const uint8_t* data, std::size_t len, bool& valid);

    void Reset();
    std::size_t BufferedBytes() const { return size_ - cursor_; }

private:
    std::vector<uint8_t> buffer_;
    std::size_t size_ = 0;   // valid bytes in buffer_
    std::size_t cursor_ = 0; // consumed-but-not-yet-compacted prefix
};

} // namespace od
