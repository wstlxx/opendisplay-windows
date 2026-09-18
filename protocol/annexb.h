// OpenDisplay video-frame parsing (PROTOCOL.md section 5.1).
//
// One wire frame (already deframed and already known not to be control JSON)
// is one H.264 Annex-B access unit:
//
//   [optional telemetry prefix: JSON, no start codes]
//   [00 00 00 01][NALU] [00 00 00 01][NALU] ...
//
// The telemetry prefix, if present, is {"cap":<ms>,"snd":<ms>} (sender
// clock, Unix epoch ms). Start codes are always 4 bytes; splitting on
// 00 00 00 01 is safe because emulation prevention makes 00 00 00
// impossible inside a NALU.
//
// NALUs are classified: 7 = SPS, 8 = PPS, 6 = SEI (skipped), everything
// else is a slice/VCL NALU. All slices of one picture travel in one wire
// frame and are fed to the decoder as one sample.
//
// Pure byte manipulation, unit-testable.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace od {

struct VideoFrame {
    // Bytes before the first start code, if any (telemetry prefix). Empty
    // when the frame begins with a start code.
    std::span<const uint8_t> telemetryPrefix;

    // Present only when this frame carries SPS/PPS (keyframes must, per
    // section 5.1). The decoder compares these against its last-seen pair
    // to decide whether to rebuild.
    std::span<const uint8_t> sps;
    std::span<const uint8_t> pps;

    // Slice NALUs in wire order (SEI dropped). One access unit.
    std::vector<std::span<const uint8_t>> vclNalus;

    // Sender timestamps from the telemetry prefix, Unix epoch ms, or -1.
    int64_t captureMs = -1;
    int64_t sendMs = -1;

    bool IsKeyframe() const; // carries SPS and an IDR slice
    bool IsEmpty() const { return vclNalus.empty() && sps.empty(); }
};

// Parses a non-JSON wire payload into a VideoFrame. Views point into
// `payload` and stay valid as long as the payload does. Never throws.
VideoFrame ParseAnnexB(std::span<const uint8_t> payload);

// Extracts "cap"/"snd" from a telemetry prefix without a JSON parser — the
// prefix shape is fixed ({"cap":<ms>,"snd":<ms>}). Unknown fields are
// ignored per spec; returns -1 when absent/unparsable.
void ParseTelemetry(std::span<const uint8_t> prefix, int64_t& capMs, int64_t& sndMs);

// H.264 NAL unit type from the NALU's header byte (low 5 bits).
inline int NaluType(std::span<const uint8_t> nalu) {
    return nalu.empty() ? -1 : (nalu[0] & 0x1F);
}

} // namespace od
