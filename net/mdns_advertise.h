// mDNS/DNS-SD advertiser for the OpenDisplay receiver (PROTOCOL.md 2.1).
//
// The receiver listens on TCP :9000; this advertises it as the Bonjour
// service `_opensidecar._tcp` so a Mac sender's NWBrowser can discover it on
// the LAN and dial us. We are the authority for our own records, so they are
// sent with the cache-flush class bit and TTL 0 (RFC 6762).
//
// Discovery is served two ways, both from one UDP socket:
//   (a) answer incoming PTR/SRV/TXT/A queries for our service (fast path), and
//   (b) send periodic unsolicited announcements (works even if the OS's own
//       mDNS holds port 5353 and we cannot receive queries — mDNS clients
//       process unsolicited announcements).
//
// The TXT `id` MUST equal the `id` later sent in `hello` (PROTOCOL.md 2.1);
// the caller passes the same stable value to both.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "win32_compat.h"

namespace od::net {

class MdnsAdvertiser {
public:
    struct Config {
        std::string instanceName;  // display name; empty => computer name
        std::string id;            // stable UUID; MUST equal the hello id
        int pv = 3;                // protocol version advertised in TXT
        uint16_t port = 9000;      // TCP port senders dial
    };

    MdnsAdvertiser() = default;
    ~MdnsAdvertiser();

    MdnsAdvertiser(const MdnsAdvertiser&) = delete;
    MdnsAdvertiser& operator=(const MdnsAdvertiser&) = delete;

    // Builds the packet, opens the socket, spawns the thread.
    bool Start(const Config& cfg);
    void Stop();  // idempotent; joins the thread, closes the socket

private:
    void ThreadMain();
    void SendAnnouncement(const std::vector<uint8_t>& pkt);
    static std::string ComputerName();
    static std::string FirstLanIPv4();

    Config cfg_;
    std::string instanceName_;  // resolved (may come from the computer name)
    std::string hostName_;      // instance name (sans spaces) + ".local"
    std::string ipv4_;          // A record payload / multicast egress interface
    std::vector<uint8_t> announcement_;

#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#endif
    bool canReceive_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int sendCount_ = 0;    // worker-thread only; rate-limited diagnostics
    int queryCount_ = 0;   // worker-thread only; rate-limited diagnostics
    int anyRecvCount_ = 0; // worker-thread only; rate-limited diagnostics
};

}  // namespace od::net
