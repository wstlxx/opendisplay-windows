// One OpenDisplay receiver connection.
//
// Owns the read loop (Winsock -> FrameDecoder -> demux -> callbacks) and the
// outgoing control channel. Video is handed up as owned VideoSamples so the
// decode thread can consume them without sharing this object's buffers.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../protocol/annexb.h"
#include "../protocol/control.h"
#include "../protocol/demux.h"
#include "../protocol/framing.h"
#include "win32_compat.h"

namespace od::net {

// One video frame as received on the wire: Annex-B with 4-byte start codes,
// NALUs in wire order (SPS/PPS when present, SEI dropped). Owns its bytes.
struct VideoSample {
    std::vector<uint8_t> annexb;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    int64_t captureMs = -1;
    int64_t sendMs = -1;
    // Receiver-side arrival time (steady ms, OUR clock) -- set by the session
    // read loop. Skew-free basis for arrival->present latency stats.
    int64_t arrivalMs = 0;
    bool isKeyframe = false;
};

class Session {
public:
    struct Callbacks {
        // Runs on the session read thread. Must not block long: keep it to
        // parsing + queueing; decode work belongs on the decode thread.
        std::function<void(const ControlMessage&)> onControl;
        std::function<void(VideoSample&&)> onVideo;
        // Runs on the session read thread when the loop exits.
        std::function<void(bool cleanClose)> onClosed;
    };

    Session(SOCKET sock, std::string peer, Callbacks cb);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void Start();

    // Idempotent. Unblocks the read loop; the destructor joins the thread.
    void Close();

    bool SendControl(const std::string& json);
    void SendHello(int pixelsWide, int pixelsHigh, double scale,
                   const std::string& id, int bitrateKbps);
    void SendPing();
    void SendKeyframeRequest();
    void SendClosing();

    bool Alive() const { return !closed_.load(std::memory_order_acquire); }
    const std::string& Peer() const { return peer_; }
    uint64_t FramesReceived() const { return framesReceived_; }
    uint64_t BytesReceived() const { return bytesReceived_.load(); }

    // Liveness tuning (PROTOCOL.md 5.5).
    static constexpr int64_t kLivenessTimeoutMs = 5000;
    static constexpr int64_t kPingIntervalMs = 2000;

private:
    void ReadLoop();
    static int64_t NowMs();

    SOCKET sock_;
    std::string peer_;
    Callbacks cb_;

    FrameDecoder decoder_;
    std::thread readThread_;
    std::atomic<bool> closed_{false};

    std::mutex sendMutex_;  // one session == one TCP connection
    int64_t lastDataMs_ = 0;
    int64_t lastPingMs_ = 0;

    uint64_t framesReceived_ = 0;
    std::atomic<uint64_t> bytesReceived_{0};
};

} // namespace od::net
