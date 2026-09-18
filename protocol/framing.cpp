#include "framing.h"

#include <algorithm>
#include <cstring>

namespace od {

std::vector<uint8_t> EncodeFrame(std::span<const uint8_t> payload) {
    std::vector<uint8_t> out(kFrameHeaderSize + payload.size());
    WriteInt32BE(out.data(), uint32_t(payload.size()));
    std::memcpy(out.data() + kFrameHeaderSize, payload.data(), payload.size());
    return out;
}

std::vector<uint8_t> EncodeFrame(const std::string& payload) {
    return EncodeFrame(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
}

void FrameDecoder::Reset() {
    size_ = 0;
    cursor_ = 0;
}

std::vector<std::span<const uint8_t>> FrameDecoder::Feed(const uint8_t* data, std::size_t len, bool& valid) {
    std::vector<std::span<const uint8_t>> frames;
    valid = true;
    if (len == 0) return frames;

    if (size_ + len > buffer_.size()) {
        buffer_.resize(std::max(buffer_.size() * 2, size_ + len));
    }
    std::memcpy(buffer_.data() + size_, data, len);
    size_ += len;

    // Extract (payload offset, length) pairs first, then compact, then build
    // spans: the spans must point at post-compaction positions.
    std::vector<std::pair<std::size_t, std::size_t>> pending;
    while (size_ - cursor_ >= kFrameHeaderSize) {
        const uint32_t frameLen = ReadInt32BE(buffer_.data() + cursor_);
        if (frameLen > kMaxFrameSize) {
            // Corrupt/hostile length prefix: fail the stream. Frames already
            // extracted this call are still delivered.
            valid = false;
            break;
        }
        const std::size_t total = kFrameHeaderSize + frameLen; // 0 is harmless
        if (size_ - cursor_ < total) break; // incomplete, wait for more bytes
        pending.emplace_back(cursor_ + kFrameHeaderSize, frameLen);
        cursor_ += total;
    }

    if (!pending.empty()) {
        std::memmove(buffer_.data(), buffer_.data() + cursor_, size_ - cursor_);
        size_ -= cursor_;
        cursor_ = 0;
        for (auto& [offset, length] : pending) {
            frames.emplace_back(buffer_.data() + offset, length);
        }
    }
    return frames;
}

} // namespace od
