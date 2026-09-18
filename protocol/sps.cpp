#include "sps.h"

#include <vector>

namespace od {

namespace {

const int kProfilesWithChromaInfo[] = {100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};

bool HasChromaInfo(int profileIdc) {
    for (int p : kProfilesWithChromaInfo)
        if (p == profileIdc) return true;
    return false;
}

// Removes emulation_prevention_three_byte (0x03 after a run of >= 2 zeros)
// and the leading NAL header byte, leaving the raw RBSP bit sequence.
std::vector<uint8_t> StripEmulationPrevention(std::span<const uint8_t> nalu) {
    std::vector<uint8_t> out;
    out.reserve(nalu.size() > 1 ? nalu.size() - 1 : 0);
    int zeroRun = 0;
    for (size_t i = 1; i < nalu.size(); ++i) {
        const uint8_t b = nalu[i];
        if (zeroRun >= 2 && b == 0x03) {
            zeroRun = 0;
            continue;
        }
        out.push_back(b);
        zeroRun = (b == 0) ? zeroRun + 1 : 0;
    }
    return out;
}

class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> data) : data_(data) {}

    int u(int n) {
        int value = 0;
        for (int i = 0; i < n; ++i) value = (value << 1) | bit();
        return value;
    }
    int ue() {
        int leadingZeros = 0;
        while (bit() == 0)
            if (++leadingZeros > 32) throw 0;
        int value = 1;
        for (int i = 0; i < leadingZeros; ++i) value = (value << 1) | bit();
        return value - 1;
    }
    int se() {
        const int codeNum = ue();
        return (codeNum % 2 == 0) ? -(codeNum / 2) : (codeNum + 1) / 2;
    }

private:
    int bit() {
        const size_t byteIndex = pos_ / 8;
        if (byteIndex >= data_.size()) throw 0;
        const int bitIndex = 7 - int(pos_ % 8);
        ++pos_;
        return (int(data_[byteIndex]) >> bitIndex) & 1;
    }
    std::span<const uint8_t> data_;
    size_t pos_ = 0;
};

void SkipScalingList(BitReader& r, int size) {
    int lastScale = 8;
    int nextScale = 8;
    for (int i = 0; i < size; ++i) {
        if (nextScale != 0) nextScale = (lastScale + r.se() + 256) % 256;
        if (nextScale != 0) lastScale = nextScale;
    }
}

} // namespace

std::optional<SpsDimensions> ParseSpsDimensions(std::span<const uint8_t> sps) {
    if (sps.size() < 4) return std::nullopt;
    try {
        const auto bits = StripEmulationPrevention(sps);
        BitReader r(bits);
        const int profileIdc = r.u(8);
        r.u(8); // constraint flags + reserved
        r.u(8); // level_idc
        r.ue(); // seq_parameter_set_id

        int chromaFormatIdc = 1;
        if (HasChromaInfo(profileIdc)) {
            chromaFormatIdc = r.ue();
            if (chromaFormatIdc == 3) r.u(1); // separate_colour_plane_flag
            r.ue(); // bit_depth_luma_minus8
            r.ue(); // bit_depth_chroma_minus8
            r.u(1); // qpprime_y_zero_transform_bypass_flag
            if (r.u(1) == 1) { // seq_scaling_matrix_present_flag
                const int count = (chromaFormatIdc != 3) ? 8 : 12;
                for (int i = 0; i < count; ++i)
                    if (r.u(1) == 1) SkipScalingList(r, i < 6 ? 16 : 64);
            }
        }

        r.ue(); // log2_max_frame_num_minus4
        switch (r.ue()) { // pic_order_cnt_type
            case 0:
                r.ue();
                break;
            case 1:
                r.u(1);
                r.se();
                r.se();
                for (int i = 0, n = r.ue(); i < n; ++i) r.se();
                break;
            default:
                break;
        }

        r.ue(); // max_num_ref_frames
        r.u(1); // gaps_in_frame_num_value_allowed_flag
        const int picWidthInMbsMinus1 = r.ue();
        const int picHeightInMapUnitsMinus1 = r.ue();
        const int frameMbsOnlyFlag = r.u(1);
        if (frameMbsOnlyFlag == 0) r.u(1); // mb_adaptive_frame_field_flag
        r.u(1); // direct_8x8_inference_flag

        int cropLeft = 0, cropRight = 0, cropTop = 0, cropBottom = 0;
        if (r.u(1) == 1) { // frame_cropping_flag
            cropLeft = r.ue();
            cropRight = r.ue();
            cropTop = r.ue();
            cropBottom = r.ue();
        }

        const int subWidthC = (chromaFormatIdc == 1 || chromaFormatIdc == 2) ? 2 : 1;
        const int subHeightC = (chromaFormatIdc == 1) ? 2 : 1;
        const int cropUnitX = (chromaFormatIdc == 0) ? 1 : subWidthC;
        const int cropUnitY = (chromaFormatIdc == 0) ? 2 - frameMbsOnlyFlag
                                                     : subHeightC * (2 - frameMbsOnlyFlag);

        const int width = (picWidthInMbsMinus1 + 1) * 16 - cropUnitX * (cropLeft + cropRight);
        const int height = (2 - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1) * 16 -
                           cropUnitY * (cropTop + cropBottom);

        if (width <= 0 || height <= 0) return std::nullopt;
        return SpsDimensions{width, height};
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace od
