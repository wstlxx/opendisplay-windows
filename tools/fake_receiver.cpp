// Cross-language integration test: a minimal pv3 receiver that uses the REAL
// protocol module (framing + demux + annexb + SPS + control) over POSIX TCP.
//
// Run on Linux/macOS next to tools/sim_sender.py:
//   g++ -std=c++20 -O2 -I. protocol/*.cpp tools/fake_receiver.cpp
//       -o /tmp/fake_receiver   (run the two lines as one command)
//   /tmp/fake_receiver --port 9000 --seconds 15 &
//   python3 tools/sim_sender.py --file tests/data/sample.h264
//
// It validates the wire stream exactly the way the Windows receiver will:
// frame decoding, control/JSON demux, Annex-B parsing, SPS dimensions,
// keyframe tracking, pong replies.

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "protocol/annexb.h"
#include "protocol/control.h"
#include "protocol/demux.h"
#include "protocol/framing.h"
#include "protocol/sps.h"

namespace {

std::atomic<bool> g_running{true};
void OnSigint(int) { g_running = false; }

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void SendFrame(int fd, std::string_view frame) {
    const auto* p = frame.data();
    size_t off = 0;
    while (off < frame.size()) {
        ssize_t n = send(fd, p + off, frame.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

// Wraps a control JSON in the 4-byte-BE length frame, like the real
// Session::SendControl does.
void SendJson(int fd, const std::string& json) {
    std::vector<uint8_t> raw(json.begin(), json.end());
    auto frame = od::EncodeFrame(raw);
    SendFrame(fd, std::string_view(reinterpret_cast<const char*>(frame.data()),
                                   frame.size()));
}

} // namespace

int main(int argc, char** argv) {
    int port = 9000;
    double seconds = 15.0;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc)
            port = std::atoi(argv[++i]);
        else if (std::string(argv[i]) == "--seconds" && i + 1 < argc)
            seconds = std::atof(argv[++i]);
    }

    signal(SIGINT, OnSigint);
    signal(SIGTERM, OnSigint);

    int listenFd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror("bind");
        return 1;
    }
    listen(listenFd, 1);
    std::printf("fake receiver listening on :%d (for %.0f s)\n", port,
                seconds);
    std::fflush(stdout);

    const int64_t deadline = NowMs() + static_cast<int64_t>(seconds * 1000);
    int client = -1;
    int64_t lastPingAtMs = 0; // receiver ping for RTT (every 2 s)

    struct Stats {
        uint64_t videoFrames = 0;
        uint64_t keyframes = 0;
        uint64_t dropsSeen = 0;
        int64_t lastRttMs = -1;
        int streamW = 0, streamH = 0, streamFps = 0;
        uint64_t videoBytes = 0;
        bool sawWelcome = false;
    } st;

    od::FrameDecoder decoder;
    bool valid = true;

    auto processFrame = [&](std::span<const uint8_t> frame) {
        if (od::IsControlJson(frame)) {
            auto msg = od::ParseControlMessage(frame);
            if (!msg) {
                std::printf("[ctl] invalid JSON (first %zu bytes): \"", 
                            std::min<size_t>(frame.size(), 128));
                std::fwrite(frame.data(), 1, std::min<size_t>(frame.size(), 128),
                            stdout);
                std::printf("\"\n");
                return;
            }
            using T = od::ControlType;
            if (msg->type == T::Ping) {
                // Sender-side health ping (drops/pending/capFps). The
                // receiver does NOT pong this; only the sender pongs.
                std::printf("[ctl] sender ping: drops=%d pending=%d capFps=%.1f\n",
                            msg->drops, msg->pending, msg->capFps);
            } else if (msg->type == T::Pong) {
                if (msg->hasT) st.lastRttMs = NowMs() - static_cast<int64_t>(msg->t);
            } else if (msg->type == T::Welcome) {
                st.sawWelcome = true;
                std::printf("[ctl] welcome pv=%d min=%d\n", msg->welcomePv,
                            msg->welcomeMin);
            } else if (msg->type == T::StreamConfig) {
                st.streamW = msg->width;
                st.streamH = msg->height;
                st.streamFps = msg->fps;
                std::printf("[ctl] streamConfig: %s %dx%d @ %d fps\n",
                            msg->codec.c_str(), msg->width, msg->height,
                            msg->fps);
            } else {
                std::printf("[ctl] %s\n", msg->typeString.c_str());
            }
        } else {
            auto parsed = od::ParseAnnexB(frame);
            if (parsed.IsEmpty()) {
                std::printf("[vid] EMPTY video frame (%zu bytes) -- "
                            "dropped\n",
                            frame.size());
                ++st.dropsSeen;
                return;
            }
            ++st.videoFrames;
            st.videoBytes += frame.size();
            if (parsed.IsKeyframe()) {
                ++st.keyframes;
                if (!parsed.sps.empty()) {
                    auto dims = od::ParseSpsDimensions(parsed.sps);
                    if (dims) {
                        std::printf(
                            "[vid] keyframe: SPS %dx%d (%zu video AUs "
                            "in this frame)\n",
                            dims->width, dims->height, parsed.vclNalus.size());
                    } else {
                        std::printf("[vid] keyframe: SPS parse FAILED\n");
                    }
                }
            }
        }
    };

    while (g_running) {
        int64_t remaining = deadline - NowMs();
        if (remaining <= 0) break;

        if (client < 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listenFd, &rfds);
            timeval tv{0, 100 * 1000};
            select(listenFd + 1, &rfds, nullptr, nullptr, &tv);
            if (FD_ISSET(listenFd, &rfds)) {
                client = accept(listenFd, nullptr, nullptr);
                std::printf("accepted connection\n");
                // hello first (PROTOCOL.md 6.1)
                od::HelloInfo hi;
                hi.pixelsWide = 1280;
                hi.pixelsHigh = 720;
                hi.id = "fake0001";
                SendJson(client, od::BuildHello(hi));
                lastPingAtMs = NowMs();
            }
            continue;
        }

        // Receiver-side RTT ping every 2 s (the sender pongs it).
        if (NowMs() - lastPingAtMs >= 2000) {
            lastPingAtMs = NowMs();
            SendJson(client, od::BuildPing(NowMs()));
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client, &rfds);
        timeval tv{0, 100 * 1000};
        int r = select(client + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;
        uint8_t buf[65536];
        ssize_t n = recv(client, buf, sizeof(buf), 0);
        if (n <= 0) {
            std::printf("peer closed\n");
            close(client);
            client = -1;
            continue;
        }
        auto frames = decoder.Feed(buf, static_cast<size_t>(n), valid);
        if (!valid) {
            std::printf("FRAMING INVALID (oversize frame) -- resetting\n");
            decoder.Reset();
            valid = true;
        }
        for (auto f : frames) processFrame(f);
        std::fflush(stdout);

        if (st.videoFrames > 0 && (st.videoFrames % 128) == 0) {
            std::printf("[..] %llu frames, %llu bytes, %llu keyframes, "
                        "rtt=%dms\n",
                        (unsigned long long)st.videoFrames,
                        (unsigned long long)st.videoBytes,
                        (unsigned long long)st.keyframes,
                        static_cast<int>(st.lastRttMs));
            std::fflush(stdout);
        }
    }

    if (client >= 0) {
        SendJson(client, od::BuildClosing());
        close(client);
    }
    close(listenFd);

    const int64_t span = std::max<int64_t>(1, NowMs() - (deadline - seconds * 1000));
    std::printf("\n=== summary ===\n");
    std::printf("welcome: %s\n", st.sawWelcome ? "yes" : "NO (FAIL)");
    std::printf("video frames: %llu (%.1f fps over %lld ms)\n",
                (unsigned long long)st.videoFrames,
                st.videoFrames ? 1000.0 * st.videoFrames / span : 0.0,
                static_cast<long long>(span));
    std::printf("video bytes:  %llu\n", (unsigned long long)st.videoBytes);
    std::printf("keyframes:    %llu\n", (unsigned long long)st.keyframes);
    std::printf("streamConfig: %dx%d @ %d fps\n", st.streamW, st.streamH,
                st.streamFps);
    std::printf("last rtt:     %d ms\n", static_cast<int>(st.lastRttMs));
    const bool ok = st.sawWelcome && st.videoFrames > 10;
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
