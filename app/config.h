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

// Missing keys retain defaults. Invalid values are reported and ignored.
ReceiverConfig ParseConfig(std::istream& input, std::vector<std::string>& warnings);

} // namespace od::app
