// TCP listener on 0.0.0.0:9000 (OpenDisplay protocol default port).
// Accepts one connection at a time; a new connection replaces the old one
// (PROTOCOL.md: the receiver MAY close the previous session).

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "win32_compat.h"

namespace od::net {

// Called for each accepted connection with the socket and a printable peer
// name (ip:port). The handler is invoked on the listener thread and must
// return promptly (it should spawn its own work).
using AcceptHandler = std::function<void(SOCKET, const std::string&)>;

class TcpListener {
public:
    bool Start(uint16_t port, AcceptHandler onAccept);
    void Stop();

    uint16_t Port() const { return port_; }

private:
    uint16_t port_ = 9000;
    SOCKET listenSock_ = INVALID_SOCKET;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace od::net
