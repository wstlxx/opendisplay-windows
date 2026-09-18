// OpenDisplay channel demux (PROTOCOL.md section 4, "deprecated heuristic").
//
// Sender-to-receiver frames carry both H.264 video and JSON control messages
// on one connection. At pv <= 3 a frame is a JSON control message if and
// only if all three hold:
//   1. payload length < 32768 bytes
//   2. first byte is '{' (0x7B)
//   3. payload contains no NUL byte (0x00)
//
// Anything else is a video frame. This works because Annex-B start codes
// (00 00 00 01) guarantee NUL bytes in every video frame, including video
// frames that begin with '{' (the telemetry prefix, section 5.1).
//
// The spec explicitly asks implementations to isolate this decision in one
// place so the pv4 typed frame header is a cheap swap.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace od {

inline constexpr std::size_t kJsonSniffMaxSize = 32768;

bool IsControlJson(std::span<const uint8_t> payload);

} // namespace od
