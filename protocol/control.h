// OpenDisplay control messages (PROTOCOL.md section 6).
//
// Control messages are flat JSON objects, each in its own wire frame, with a
// "type" string discriminator. Rules that make the protocol evolvable:
//   - unknown "type" values MUST be ignored (log at most once per type);
//   - unknown fields on a known type MUST be ignored;
//   - an unparseable control payload MUST be ignored, not fatal.
//
// The protocol only ever carries flat single-level objects, so a tiny
// field scanner replaces a JSON dependency (same approach as the
// opendisplay-win sender). Pure C++, unit-testable.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace od {

// ---- Outgoing (receiver -> sender) message builders --------------------

struct HelloInfo {
    int pixelsWide = 0;
    int pixelsHigh = 0;
    double scale = 1.0;
    std::string device = "Windows";
    std::string id;                  // stable per-install UUID
    int pv = 3;
    std::optional<int> maxEncodeWide; // decode ceiling, omit if unknown
    std::optional<int> maxEncodeHigh;
};

std::string BuildHello(const HelloInfo& info);
// Receiver liveness/RTT ping, t = receiver wall-clock ms (PROTOCOL.md 8.1).
std::string BuildPing(int64_t tMs);
// NOTE: `pong` is a SENDER->receiver message (the sender replies to our
// ping). This builder is provided for simulators/tests that play sender.
// Echoes the ping's t unchanged and adds mt = sender-clock now.
std::string BuildPong(int64_t tMs, int64_t mtMs);
inline std::string BuildKeyframeRequest() { return "{\"type\":\"kf\"}"; }
inline std::string BuildClosing() { return "{\"type\":\"closing\"}"; }
inline std::string BuildSleeping() { return "{\"type\":\"sleeping\"}"; }
// Free-form receiver telemetry the sender only logs.
std::string BuildStats(int fps, int mbps, int rttMs, int e2eP50Ms, int e2eP95Ms);

// ---- Incoming (sender -> receiver) parsing ------------------------------

enum class ControlType {
    Pong, Ping, Welcome, UpdateRequired, StreamConfig,
    Cursor, CursorImg, // cursor features: parsed, ignored in initial scope
    Unknown,
};

struct ControlMessage {
    ControlType type = ControlType::Unknown;
    std::string typeString;  // for once-per-type logging of unknown types
    // pong / clock sync
    double t = 0, mt = 0;
    bool hasT = false, hasMt = false;
    // welcome
    int welcomePv = 1, welcomeMin = 1;
    // updateRequired
    std::string target, store, message;
    // streamConfig
    std::string codec;
    int width = 0, height = 0, fps = 0;
    // sender ping health counters (all optional, informational)
    int drops = 0, pending = 0;
    bool hasDrops = false, hasPending = false;
    double capFps = 0;
    bool hasCapFps = false;
};

// Parses a payload that already passed IsControlJson(). Returns nullopt when
// the payload is not valid JSON or has no "type" (spec: ignore, not fatal).
// Unrecognized types parse as ControlType::Unknown.
std::optional<ControlMessage> ParseControlMessage(std::span<const uint8_t> payload);

} // namespace od
