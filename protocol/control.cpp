#include "control.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace od {

namespace {

std::string_view AsView(std::span<const uint8_t> payload) {
    return {reinterpret_cast<const char*>(payload.data()), payload.size()};
}

// Finds "key" : <value-start> in a flat JSON object and returns the offset
// of the first character of the value (after any whitespace), or npos.
size_t FindValueStart(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string_view::npos) return std::string_view::npos;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string_view::npos) return std::string_view::npos;
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    return pos;
}

std::optional<std::string> FindString(std::string_view json, std::string_view key) {
    const size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos || start >= json.size() || json[start] != '"')
        return std::nullopt;
    const size_t end = json.find('"', start + 1);
    if (end == std::string_view::npos) return std::nullopt;
    return std::string(json.substr(start + 1, end - start - 1));
}

std::optional<double> FindNumber(std::string_view json, std::string_view key) {
    const size_t start = FindValueStart(json, key);
    if (start == std::string_view::npos || start >= json.size()) return std::nullopt;
    const char* p = json.data() + start;
    const char* end = json.data() + json.size();
    double value = 0;
    auto [ptr, ec] = std::from_chars(p, end, value);
    if (ec != std::errc{} || ptr == p) return std::nullopt;
    return value;
}

std::optional<int> FindInt(std::string_view json, std::string_view key) {
    const auto v = FindNumber(json, key);
    if (!v) return std::nullopt;
    if (std::isfinite(*v) && std::abs(*v) < 1e9) return static_cast<int>(*v);
    return std::nullopt;
}

std::string FormatNumber(double v) {
    // Integral values print without a decimal point (the sender treats
    // numbers as numbers; both forms are legal JSON).
    if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

} // namespace

// ---- Outgoing -------------------------------------------------------------

std::string BuildHello(const HelloInfo& info) {
    std::string s = "{\"type\":\"hello\"";
    s += ",\"pixelsWide\":" + std::to_string(info.pixelsWide);
    s += ",\"pixelsHigh\":" + std::to_string(info.pixelsHigh);
    s += ",\"scale\":" + FormatNumber(info.scale);
    s += ",\"device\":\"" + info.device + "\"";
    if (!info.id.empty()) s += ",\"id\":\"" + info.id + "\"";
    s += ",\"pv\":" + std::to_string(info.pv);
    if (info.maxEncodeWide && info.maxEncodeHigh) {
        s += ",\"maxEncodeWide\":" + std::to_string(*info.maxEncodeWide);
        s += ",\"maxEncodeHigh\":" + std::to_string(*info.maxEncodeHigh);
    }
    if (info.bitrateKbps)
        s += ",\"bitrateKbps\":" + std::to_string(*info.bitrateKbps);
    s += "}";
    return s;
}

std::string BuildPing(int64_t tMs) {
    return "{\"type\":\"ping\",\"t\":" + std::to_string(tMs) + "}";
}

std::string BuildPong(int64_t tMs, int64_t mtMs) {
    return "{\"type\":\"pong\",\"t\":" + std::to_string(tMs) +
           ",\"mt\":" + std::to_string(mtMs) + "}";
}

std::string BuildTouch(const std::string& phase, double x, double y) {
    return "{\"type\":\"touch\",\"phase\":\"" + phase + "\"," +
           "\"x\":" + FormatNumber(x) + ",\"y\":" + FormatNumber(y) + "}";
}

std::string BuildScroll(double dx, double dy) {
    return "{\"type\":\"scroll\",\"dx\":" + FormatNumber(dx) +
           ",\"dy\":" + FormatNumber(dy) + "}";
}

std::string BuildStats(int fps, int mbps, int rttMs, int e2eP50Ms, int e2eP95Ms) {
    return "{\"type\":\"stats\",\"transport\":\"tcp\",\"fps\":" + std::to_string(fps) +
           ",\"mbps\":" + std::to_string(mbps) + ",\"rtt\":" + std::to_string(rttMs) +
           ",\"e2e50\":" + std::to_string(e2eP50Ms) + ",\"e2e95\":" + std::to_string(e2eP95Ms) +
           ",\"offsetKnown\":true}";
}

// ---- Incoming -------------------------------------------------------------

std::optional<ControlMessage> ParseControlMessage(std::span<const uint8_t> payload) {
    // Must be a top-level object with a "type" field.
    if (payload.empty() || payload[0] != '{' || payload.back() != '}') return std::nullopt;
    const std::string_view json = AsView(payload);

    const auto type = FindString(json, "type");
    if (!type) return std::nullopt;

    ControlMessage msg;
    msg.typeString = *type;
    if (*type == "pong") {
        msg.type = ControlType::Pong;
        msg.t = FindNumber(json, "t").value_or(0);
        msg.hasT = FindNumber(json, "t").has_value();
        msg.mt = FindNumber(json, "mt").value_or(0);
        msg.hasMt = FindNumber(json, "mt").has_value();
    } else if (*type == "ping") {
        msg.type = ControlType::Ping; // liveness/health, informational
        msg.drops = FindInt(json, "drops").value_or(0);
        msg.hasDrops = FindInt(json, "drops").has_value();
        msg.pending = FindInt(json, "pending").value_or(0);
        msg.hasPending = FindInt(json, "pending").has_value();
        msg.capFps = FindNumber(json, "capFps").value_or(0);
        msg.hasCapFps = FindNumber(json, "capFps").has_value();
    } else if (*type == "welcome") {
        msg.type = ControlType::Welcome;
        msg.welcomePv = FindInt(json, "pv").value_or(1);
        msg.welcomeMin = FindInt(json, "min").value_or(1);
    } else if (*type == "updateRequired") {
        msg.type = ControlType::UpdateRequired;
        msg.target = FindString(json, "target").value_or("");
        msg.store = FindString(json, "store").value_or("");
        msg.message = FindString(json, "message").value_or("");
    } else if (*type == "streamConfig") {
        msg.type = ControlType::StreamConfig;
        msg.codec = FindString(json, "codec").value_or("h264");
        msg.width = FindInt(json, "width").value_or(0);
        msg.height = FindInt(json, "height").value_or(0);
        msg.fps = FindInt(json, "framesPerSecond").value_or(0);
    } else if (*type == "cursor") {
        msg.type = ControlType::Cursor; // ignored in initial scope
    } else if (*type == "cursorImg") {
        msg.type = ControlType::CursorImg; // ignored in initial scope
    } else {
        msg.type = ControlType::Unknown;
    }
    return msg;
}

} // namespace od
