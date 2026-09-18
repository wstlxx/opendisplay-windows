// Minimal H.264 SPS parser: extracts only the coded picture
// width/height (post-cropping). Ported from the Android receiver's
// H264Sps.kt, which exists because decoder-reported output sizes cannot be
// trusted to reflect the real bitstream dimensions on every platform.
//
// Pure byte/bit manipulation, unit-testable.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace od {

struct SpsDimensions {
    int width = 0;
    int height = 0;
};

// `sps` is one SPS NALU, header byte first (as produced by ParseAnnexB).
// Returns the coded frame size after cropping, or nullopt when the SPS is
// malformed or uses syntax this parser doesn't handle.
std::optional<SpsDimensions> ParseSpsDimensions(std::span<const uint8_t> sps);

} // namespace od
