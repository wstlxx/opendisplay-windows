#include "session.h"

#include <ws2tcpip.h>

#include "log.h"

namespace od::net {

namespace {
int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

Session::Session(SOCKET sock, std::string peer, Callbacks cb)
    : sock_(sock), peer_(std::move(peer)), cb_(std::move(cb)) {}

Session::~Session() {
    Close();
    if (readThread_.joinable()) readThread_.join();
    if (sock_ != INVALID_SOCKET) {
        ::closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

void Session::Start() {
    lastDataMs_ = SteadyNowMs();
    lastPingMs_ = lastDataMs_;
    readThread_ = std::thread([this] { ReadLoop(); });
}

void Session::Close() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) return;
    if (sock_ != INVALID_SOCKET) {
        ::shutdown(sock_, SD_BOTH);
        // Do not closesocket here: the read thread may still be inside
        // recv(); the destructor closes the socket after join.
    }
}

bool Session::SendControl(const std::string& json) {
    const auto wire = od::EncodeFrame(
        std::span(reinterpret_cast<const uint8_t*>(json.data()), json.size()));
    std::lock_guard lock(sendMutex_);
    if (closed_.load()) return false;
    const char* p = reinterpret_cast<const char*>(wire.data());
    int total = 0;
    while (total < static_cast<int>(wire.size())) {
        const int n = ::send(sock_, p + total,
                             static_cast<int>(wire.size()) - total, 0);
        if (n <= 0) {
            LOG_WARN("session(%s): send() failed: %d", peer_.c_str(),
                     WSAGetLastError());
            Close();
            return false;
        }
        total += n;
    }
    return true;
}

void Session::SendHello(int pixelsWide, int pixelsHigh, double scale,
                        const std::string& id) {
    od::HelloInfo info;
    info.pixelsWide = pixelsWide;
    info.pixelsHigh = pixelsHigh;
    info.scale = scale;
    info.id = id;
    SendControl(od::BuildHello(info));
}

void Session::SendPing() { SendControl(od::BuildPing(SteadyNowMs())); }
void Session::SendKeyframeRequest() { SendControl(od::BuildKeyframeRequest()); }
void Session::SendClosing() { SendControl(od::BuildClosing()); }

int64_t Session::NowMs() { return SteadyNowMs(); }

void Session::ReadLoop() {
    bool cleanClose = true;
    {
        std::vector<uint8_t> buf(64 * 1024);
        while (!closed_) {
            // Poll with a short timeout so liveness + pings keep working
            // even when the sender is quiet.
            WSAPOLLFD pfd{};
            pfd.fd = sock_;
            pfd.events = POLLIN | POLLHUP;
            const int rc = ::WSAPoll(&pfd, 1, 250);
            if (rc == SOCKET_ERROR) {
                cleanClose = false;
                break;
            }

            const int64_t now = SteadyNowMs();
            if (now - lastDataMs_ > kLivenessTimeoutMs) {
                LOG_WARN("session(%s): no data for %lld ms, dropping "
                         "connection (sender assumed dead)",
                         peer_.c_str(), now - lastDataMs_);
                cleanClose = false;
                break;
            }
            if (now - lastPingMs_ >= kPingIntervalMs) {
                lastPingMs_ = now;
                SendPing();
            }

            if (rc == 0) continue; // timeout, nothing readable
            if (pfd.revents & (POLLERR | POLLNVAL)) break;
            if ((pfd.revents & POLLHUP) && !(pfd.revents & POLLIN)) break;

            const int n =
                ::recv(sock_, reinterpret_cast<char*>(buf.data()),
                       static_cast<int>(buf.size()), 0);
            if (n > 0) {
                lastDataMs_ = SteadyNowMs();
                bool valid = true;
                auto frames =
                    decoder_.Feed(buf.data(), static_cast<size_t>(n), valid);
                if (!valid) {
                    LOG_WARN("session(%s): corrupt frame length on wire, "
                             "closing", peer_.c_str());
                    cleanClose = false;
                    break;
                }
                for (const auto& frame : frames) {
                    if (od::IsControlJson(frame)) {
                        auto msg = od::ParseControlMessage(frame);
                        if (msg) {
                            if (cb_.onControl) cb_.onControl(*msg);
                        } else {
                            LOG_DEBUG("session(%s): unparseable JSON frame "
                                      "(len=%zu)", peer_.c_str(), frame.size());
                        }
                        continue;
                    }
                    const VideoFrame vf = od::ParseAnnexB(frame);
                    if (vf.IsEmpty()) {
                        LOG_DEBUG("session(%s): video frame without NALUs "
                                  "(len=%zu)", peer_.c_str(), frame.size());
                        continue;
                    }
                    framesReceived_++;
                    VideoSample sample;
                    sample.sps.assign(vf.sps.begin(), vf.sps.end());
                    sample.pps.assign(vf.pps.begin(), vf.pps.end());
                    sample.captureMs = vf.captureMs;
                    sample.sendMs = vf.sendMs;
                    sample.isKeyframe = vf.IsKeyframe();
                    // Re-serialize in wire order with 4-byte start codes so
                    // the MF byte stream sees a clean Annex-B stream.
                    if (!vf.sps.empty()) {
                        sample.annexb.insert(sample.annexb.end(), {0, 0, 0, 1});
                        sample.annexb.insert(sample.annexb.end(), vf.sps.begin(),
                                             vf.sps.end());
                    }
                    if (!vf.pps.empty()) {
                        sample.annexb.insert(sample.annexb.end(), {0, 0, 0, 1});
                        sample.annexb.insert(sample.annexb.end(), vf.pps.begin(),
                                             vf.pps.end());
                    }
                    for (const auto& nalu : vf.vclNalus) {
                        sample.annexb.insert(sample.annexb.end(), {0, 0, 0, 1});
                        sample.annexb.insert(sample.annexb.end(), nalu.begin(),
                                             nalu.end());
                    }
                    if (cb_.onVideo) cb_.onVideo(std::move(sample));
                }
            } else if (n == 0) {
                break; // peer closed
            } else {
                const int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) continue;
                cleanClose = false;
                LOG_WARN("session(%s): recv() failed: %d", peer_.c_str(), err);
                break;
            }
        }
    }
    if (cb_.onClosed) cb_.onClosed(cleanClose);
    LOG_INFO("session(%s): read loop ended (%s)", peer_.c_str(),
             cleanClose ? "peer closed" : "error/timeout");
}

} // namespace od::net
