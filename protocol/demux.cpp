#include "demux.h"

namespace od {

bool IsControlJson(std::span<const uint8_t> payload) {
    if (payload.empty() || payload.size() >= kJsonSniffMaxSize) return false;
    if (payload.front() != '{') return false;
    for (uint8_t b : payload)
        if (b == 0x00) return false;
    return true;
}

} // namespace od
