#include "mdns_advertise.h"

#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include <vector>

#include "log.h"

#pragma comment(lib, "iphlpapi.lib")

namespace od::net {
namespace {

constexpr const char* kServiceType = "_opensidecar._tcp";
constexpr const char* kServiceDomain = "_opensidecar._tcp.local";
constexpr const char* kMulticastGroup = "224.0.0.251";
constexpr uint16_t kMulticastPort = 5353;
constexpr int kReannounceSecs = 10;

enum : uint16_t { kTypeA = 1, kTypePtr = 12, kTypeTxt = 16, kTypeSrv = 33 };

// IN class with the cache-flush bit set (RFC 6762 10): "I am the authority
// for this record; discard what you were told and use this."
constexpr uint16_t kClassCacheFlush = 0x8001;

// Two in_addr; the same layout as Winsock's ip_mreq. Hand-rolled so a reduced
// SDK that omits the named type can't break the build.
struct IpMreq {
    in_addr imr_multiaddr;
    in_addr imr_interface;
};

void PutU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v & 0xFF));
}

// Encodes a dotted DNS name ("a.b.c") as [1]a[1]b[1]c[0], root-terminated.
// The name is passed WITHOUT a trailing dot.
std::vector<uint8_t> EncodeName(const std::string& name) {
    std::vector<uint8_t> out;
    size_t start = 0;
    while (start <= name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        out.push_back(uint8_t(dot - start));
        out.insert(out.end(), name.begin() + start, name.begin() + dot);
        start = dot + 1;
    }
    out.push_back(0);
    return out;
}

// Appends one answer record: NAME TYPE CLASS TTL RDLENGTH RDATA.
void AppendRecord(std::vector<uint8_t>& out, const std::string& name,
                  uint16_t type, const std::vector<uint8_t>& rdata) {
    std::vector<uint8_t> enc = EncodeName(name);
    out.insert(out.end(), enc.begin(), enc.end());
    PutU16(out, type);
    PutU16(out, kClassCacheFlush);
    PutU16(out, 0);  // TTL hi (0: permanent)
    PutU16(out, 0);  // TTL lo
    PutU16(out, uint16_t(rdata.size()));
    out.insert(out.end(), rdata.begin(), rdata.end());
}

}  // namespace

MdnsAdvertiser::~MdnsAdvertiser() { Stop(); }

std::string MdnsAdvertiser::ComputerName() {
    // Prefer the DNS host name ("MyPC", no domain) as the .local base; fall
    // back to the NetBIOS name. COMPUTER_NAME_FORMAT values are CamelCase.
    const COMPUTER_NAME_FORMAT formats[] = {ComputerNameDnsHostname,
                                            ComputerNameNetBIOS};
    for (COMPUTER_NAME_FORMAT fmt : formats) {
        WCHAR name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD len = sizeof(name) / sizeof(name[0]);
        if (!GetComputerNameExW(fmt, name, &len)) continue;
        int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr,
                                    nullptr);
        std::string out;
        if (n > 1) {
            out.resize(n - 1);
            WideCharToMultiByte(CP_UTF8, 0, name, -1, &out[0], n, nullptr,
                                nullptr);
        }
        if (!out.empty()) return out;
    }
    return {};
}

std::string MdnsAdvertiser::FirstLanIPv4() {
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buf(size);
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (r != NO_ERROR) return {};
    for (auto* a = adapters; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp ||
            a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            char text[INET_ADDRSTRLEN] = {};
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) != nullptr)
                return text;
        }
    }
    return {};
}

bool MdnsAdvertiser::Start(const Config& cfg) {
    if (running_.exchange(true)) return true;  // already started
    cfg_ = cfg;

    ipv4_ = FirstLanIPv4();
    if (ipv4_.empty()) {
        LOG_WARN("mdns: no LAN IPv4 found; not advertising");
        return false;
    }

    instanceName_ = cfg_.instanceName;
    if (instanceName_.empty()) instanceName_ = ComputerName();
    if (instanceName_.empty()) instanceName_ = "OpenDisplay";

    // Host name is internal (SRV target + A record name); keep it space-free.
    std::string hn = instanceName_;
    hn.erase(std::remove(hn.begin(), hn.end(), ' '), hn.end());
    if (hn.empty()) hn = "opendisplay";
    hostName_ = hn + ".local";

    // Build the announcement once: PTR + SRV + TXT + A in a single packet.
    const std::string fullInstance = instanceName_ + "." + kServiceType + ".local";
    std::vector<uint8_t>& pkt = announcement_;
    PutU16(pkt, 0);        // transaction id (mDNS ignores it)
    PutU16(pkt, 0x8400);   // flags: response (QR) + authoritative (AA)
    PutU16(pkt, 0);        // questions
    PutU16(pkt, 4);        // answers
    PutU16(pkt, 0);        // authority
    PutU16(pkt, 0);        // additional

    // 1. PTR: service type -> instance name.
    AppendRecord(pkt, kServiceDomain, kTypePtr, EncodeName(fullInstance));

    // 2. SRV: instance -> priority/weight/port + target host.
    std::vector<uint8_t> srv;
    PutU16(srv, 0);  // priority
    PutU16(srv, 0);  // weight
    PutU16(srv, cfg_.port);
    std::vector<uint8_t> hostEnc = EncodeName(hostName_);
    srv.insert(srv.end(), hostEnc.begin(), hostEnc.end());
    AppendRecord(pkt, fullInstance, kTypeSrv, srv);

    // 3. TXT: instance -> id + pv (length-prefixed strings).
    std::vector<uint8_t> txt;
    auto addTxt = [&](const std::string& s) {
        txt.push_back(uint8_t(s.size()));
        txt.insert(txt.end(), s.begin(), s.end());
    };
    addTxt("id=" + cfg_.id);
    addTxt("pv=" + std::to_string(cfg_.pv));
    AppendRecord(pkt, fullInstance, kTypeTxt, txt);

    // 4. A: host -> IPv4.
    in_addr addr{};
    if (inet_pton(AF_INET, ipv4_.c_str(), &addr) != 1) {
        LOG_WARN("mdns: bad IPv4 '%s'; not advertising", ipv4_.c_str());
        running_ = false;
        return false;
    }
    const uint8_t* ab = reinterpret_cast<const uint8_t*>(&addr);
    AppendRecord(pkt, hostName_, kTypeA, std::vector<uint8_t>(ab, ab + 4));

    // UDP socket.
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        LOG_WARN("mdns: socket() failed (%d)", WSAGetLastError());
        running_ = false;
        return false;
    }
    int one = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&one), sizeof(one));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(kMulticastPort);
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    canReceive_ = (bind(sock_, reinterpret_cast<sockaddr*>(&bindAddr),
                         sizeof(bindAddr)) == 0);
    if (canReceive_) {
        IpMreq mreq{};
        inet_pton(AF_INET, kMulticastGroup, &mreq.imr_multiaddr);
        inet_pton(AF_INET, ipv4_.c_str(), &mreq.imr_interface);
        setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    }
    in_addr ifAddr{};
    inet_pton(AF_INET, ipv4_.c_str(), &ifAddr);
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_IF,
               reinterpret_cast<const char*>(&ifAddr), sizeof(ifAddr));
    DWORD ttl = 2;
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    LOG_INFO("mdns: advertising '%s' (%s) as %s port %u (recv=%s)",
             instanceName_.c_str(), ipv4_.c_str(), kServiceType,
             unsigned(cfg_.port), canReceive_ ? "yes" : "no");

    thread_ = std::thread([this] { ThreadMain(); });
    return true;
}

void MdnsAdvertiser::SendAnnouncement(const std::vector<uint8_t>& pkt) {
    if (sock_ == INVALID_SOCKET || pkt.empty()) return;
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(kMulticastPort);
    inet_pton(AF_INET, kMulticastGroup, &group.sin_addr);
    sendto(sock_, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0,
           reinterpret_cast<sockaddr*>(&group), sizeof(group));
}

void MdnsAdvertiser::ThreadMain() {
    using clock = std::chrono::steady_clock;

    // Initial burst: two announcements ~250 ms apart (RFC 6762 8).
    SendAnnouncement(announcement_);
    auto last = clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (!running_) return;
    SendAnnouncement(announcement_);
    last = clock::now();

    const std::string marker = kServiceType;
    while (running_) {
        if (canReceive_) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(sock_, &rs);
            timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            int r = select(0, &rs, nullptr, nullptr, &tv);
            if (r < 0) break;
            if (r > 0 && FD_ISSET(sock_, &rs)) {
                uint8_t buf[2048];
                int n = recv(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                if (n > 0) {
                    std::string_view sv(reinterpret_cast<const char*>(buf),
                                        size_t(n));
                    if (sv.find(marker) != std::string_view::npos) {
                        SendAnnouncement(announcement_);
                        last = clock::now();
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        auto now = clock::now();
        if (now - last >= std::chrono::seconds(kReannounceSecs)) {
            SendAnnouncement(announcement_);
            last = now;
        }
    }
}

void MdnsAdvertiser::Stop() {
    if (!running_.exchange(false)) {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        return;
    }
    if (thread_.joinable()) thread_.join();
    if (sock_ != INVALID_SOCKET) {
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

}  // namespace od::net
