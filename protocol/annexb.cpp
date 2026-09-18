#include "annexb.h"

#include <cstring>
#include <string>

namespace od {

namespace {

// Scans "key" : <number> in the flat telemetry JSON and returns the value,
// or -1 when absent/unparsable. The prefix shape is fixed
// ({"cap":<ms>,"snd":<ms>}) so a small scanner needs no JSON library.
int64_t FindNumber(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return -1;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return -1;
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n'))
        ++pos;
    size_t end = pos;
    while (end < json.size() &&
           (json[end] == '-' || (json[end] >= '0' && json[end] <= '9')))
        ++end;
    if (end == pos) return -1;
    try {
        return std::stoll(std::string(json.substr(pos, end - pos)));
    } catch (...) {
        return -1;
    }
}

// H.264 slice_type (0..9) from the first slice NALU: first_mb_in_slice (UE)
// then slice_type (UE). Returns -1 when undecodable.
int SliceType(std::span<const uint8_t> nalu) {
    struct BitReader {
        std::span<const uint8_t> b;
        size_t pos = 0;
        int bit() {
            if (pos / 8 >= b.size()) return -1;
            const int v = (b[pos / 8] >> (7 - pos % 8)) & 1;
            ++pos;
            return v;
        }
        int ue() {
            int zeros = 0;
            while (bit() == 0)
                if (++zeros > 24) return -1;
            int v = 1;
            for (int i = 0; i < zeros; ++i) v = (v << 1) | bit();
            return v - 1;
        }
    } r{nalu};
    if (r.ue() < 0) return -1; // first_mb_in_slice
    const int t = r.ue();
    return (t >= 0 && t <= 9) ? t : -1;
}

} // namespace

void ParseTelemetry(std::span<const uint8_t> prefix, int64_t& capMs, int64_t& sndMs) {
    capMs = -1;
    sndMs = -1;
    if (prefix.empty()) return;
    const std::string_view json(reinterpret_cast<const char*>(prefix.data()), prefix.size());
    capMs = FindNumber(json, "cap");
    sndMs = FindNumber(json, "snd");
}

VideoFrame ParseAnnexB(std::span<const uint8_t> payload) {
    VideoFrame frame;

    constexpr size_t kStartCodeLen = 4;

    // Split on 4-byte start codes only (senders MUST NOT emit 3-byte codes,
    // PROTOCOL.md section 5.1).
    std::vector<size_t> starts;
    size_t i = 0;
    while (i + kStartCodeLen <= payload.size()) {
        if (payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 0 && payload[i + 3] == 1) {
            starts.push_back(i + kStartCodeLen);
            i += kStartCodeLen;
        } else {
            ++i;
        }
    }
    if (starts.empty()) {
        // No start codes at all: not a usable video frame. If there are
        // leading bytes they are an unparsable telemetry prefix; ignore.
        return frame;
    }
    if (starts[0] > kStartCodeLen) { // bytes before the first start code
        frame.telemetryPrefix = payload.subspan(0, starts[0] - kStartCodeLen);
        ParseTelemetry(frame.telemetryPrefix, frame.captureMs, frame.sendMs);
    }
    for (size_t k = 0; k < starts.size(); ++k) {
        const size_t naluBegin = starts[k];
        const size_t naluEnd = (k + 1 < starts.size()) ? starts[k + 1] - kStartCodeLen : payload.size();
        if (naluBegin >= naluEnd) continue; // empty NALU between start codes
        const auto nalu = payload.subspan(naluBegin, naluEnd - naluBegin);
        switch (NaluType(nalu)) {
            case 7: frame.sps = nalu; break; // SPS
            case 8: frame.pps = nalu; break; // PPS
            case 6: break;                   // SEI — skipped per spec
            default: frame.vclNalus.push_back(nalu); break;
        }
    }
    return frame;
}

bool VideoFrame::IsKeyframe() const {
    if (sps.empty() || vclNalus.empty()) return false;
    // IDR slice NAL (type 5) or any slice with slice_type 7 (I).
    for (const auto& n : vclNalus) {
        const int t = NaluType(n);
        if (t == 5 || (t == 1 && SliceType(n) == 7)) return true;
    }
    return false;
}

} // namespace od
