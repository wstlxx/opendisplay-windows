#include "tcp_listener.h"

#include <ws2tcpip.h>

#include <cstdio>

#include "../net/log.h"

namespace od::net {

bool TcpListener::Start(uint16_t port, AcceptHandler onAccept) {
    port_ = port;

    listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock_ == INVALID_SOCKET) {
        LOG_ERROR("listener: socket() failed: %d", WSAGetLastError());
        return false;
    }

    int yes = 1;
    setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    // Large receive buffer: a 1080p60 H.264 stream can burst several MB
    // between reader passes (e.g. while the app is blocked in a rebuild).
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(listenSock_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(listenSock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("listener: bind() :%u failed: %d (is another receiver running?)",
                  port, WSAGetLastError());
        ::closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
        return false;
    }
    if (::listen(listenSock_, SOMAXCONN) != 0) {
        LOG_ERROR("listener: listen() failed: %d", WSAGetLastError());
        ::closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
        return false;
    }

    running_ = true;
    thread_ = std::thread([this, onAccept = std::move(onAccept)]() mutable {
        while (running_) {
            sockaddr_in peer{};
            int peerLen = sizeof(peer);
            SOCKET client = ::accept(listenSock_,
                                     reinterpret_cast<sockaddr*>(&peer), &peerLen);
            if (!running_) {
                if (client != INVALID_SOCKET) ::closesocket(client);
                break;
            }
            if (client == INVALID_SOCKET) {
                if (WSAGetLastError() == WSAEINTR) continue; // closing
                LOG_ERROR("listener: accept() failed: %d", WSAGetLastError());
                continue;
            }
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            char peerName[64];
            std::snprintf(peerName, sizeof(peerName), "%s:%u", ip,
                          ntohs(peer.sin_port));
            LOG_INFO("listener: accepted connection from %s", peerName);
            onAccept(client, peerName);
        }
    });
    LOG_INFO("listener: listening on 0.0.0.0:%u", port);
    return true;
}

void TcpListener::Stop() {
    if (!running_ && listenSock_ == INVALID_SOCKET) return;
    running_ = false;
    if (listenSock_ != INVALID_SOCKET) {
        ::closesocket(listenSock_);  // unblocks accept()
        listenSock_ = INVALID_SOCKET;
    }
    if (thread_.joinable()) thread_.join();
}

} // namespace od::net
