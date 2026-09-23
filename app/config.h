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

// Choose a stream raster that fits the client area while preserving the
// configured desktop aspect ratio. Quarter-scale steps avoid rebuilding the
// Mac virtual display for small window changes or letterbox-only growth.
DisplaySize AdaptiveDisplaySize(const ReceiverConfig& config,
                                int clientWidth, int clientHeight);

// Missing keys retain defaults. Invalid values are reported and ignored.
ReceiverConfig ParseConfig(std::istream& input, std::vector<std::string>& warnings);

} // namespace od::app
