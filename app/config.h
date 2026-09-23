#pragma once

#include <istream>
#include <string>
#include <vector>

namespace od::app {

struct ReceiverConfig {
    int width = 1280;
    int height = 720;
    int bitrateKbps = 18000;
    bool fullscreen = false;
    bool adaptiveResolution = false;
};

struct DisplaySize {
    int width;
    int height;
};

// Use the actual client area (rounded down to even pixels for NV12).
DisplaySize AdaptiveDisplaySize(const ReceiverConfig& config,
                                int clientWidth, int clientHeight);

// Missing keys retain defaults. Invalid values are reported and ignored.
ReceiverConfig ParseConfig(std::istream& input, std::vector<std::string>& warnings);

} // namespace od::app
