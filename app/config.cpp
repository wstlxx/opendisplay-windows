#include "config.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>

namespace od::app {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.remove_suffix(1);
    return s;
}

bool ReadInt(std::string_view text, int& value) {
    const char* first = text.data();
    const char* last = first + text.size();
    auto [end, error] = std::from_chars(first, last, value);
    return error == std::errc{} && end == last;
}

bool ReadBool(std::string_view text, bool& value) {
    if (text == "true" || text == "1") { value = true; return true; }
    if (text == "false" || text == "0") { value = false; return true; }
    return false;
}

} // namespace

ReceiverConfig ParseConfig(std::istream& input, std::vector<std::string>& warnings) {
    ReceiverConfig config;
    std::string section;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        std::string_view text = Trim(line);
        if (text.empty() || text.front() == ';' || text.front() == '#') continue;
        if (text.front() == '[' && text.back() == ']') {
            section = std::string(Trim(text.substr(1, text.size() - 2)));
            continue;
        }
        const size_t equals = text.find('=');
        if (equals == std::string_view::npos) {
            warnings.push_back("line " + std::to_string(lineNumber) + ": expected key=value");
            continue;
        }
        const std::string_view key = Trim(text.substr(0, equals));
        const std::string_view value = Trim(text.substr(equals + 1));
        bool valid = false;
        int number = 0;
        bool boolean = false;
        if (section == "display" && key == "width") {
            valid = ReadInt(value, number) && number >= 320 && number <= 8192 && number % 2 == 0;
            if (valid) config.width = number;
        } else if (section == "display" && key == "height") {
            valid = ReadInt(value, number) && number >= 240 && number <= 8192 && number % 2 == 0;
            if (valid) config.height = number;
        } else if (section == "display" && key == "fullscreen") {
            valid = ReadBool(value, boolean);
            if (valid) config.fullscreen = boolean;
        } else if (section == "display" && key == "adaptive_resolution") {
            valid = ReadBool(value, boolean);
            if (valid) config.adaptiveResolution = boolean;
        } else if (section == "video" && key == "bitrate_kbps") {
            valid = ReadInt(value, number) && number >= 1000 && number <= 100000;
            if (valid) config.bitrateKbps = number;
        } else {
            warnings.push_back("line " + std::to_string(lineNumber) + ": unknown setting");
            continue;
        }
        if (!valid)
            warnings.push_back("line " + std::to_string(lineNumber) + ": invalid value");
    }
    return config;
}

DisplaySize AdaptiveDisplaySize(const ReceiverConfig& config,
                                int clientWidth, int clientHeight) {
    const DisplaySize base{config.width, config.height};
    if (clientWidth < 320 || clientHeight < 240) return base;
    // H.264 NV12 requires even dimensions. Match both client axes so changing
    // only the window width still changes the Mac virtual display width.
    return {std::min(clientWidth, 8192) & ~1,
            std::min(clientHeight, 8192) & ~1};
}

} // namespace od::app
