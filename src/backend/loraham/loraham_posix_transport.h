// loraham_posix_transport.h — POSIX AF_UNIX implementation of DaemonTransport.
//
// Connects to the LoRaHAM daemon's per-band framed DATA + CONF sockets. Thin I/O
// only; all protocol logic lives in LorahamBackend / loraham_framing.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_POSIX_TRANSPORT_H
#define MEBRIDGE_BACKEND_LORAHAM_POSIX_TRANSPORT_H

#include <string>

#include "backend/loraham/loraham_transport.h"

namespace mebridge {
namespace loraham {

// Socket paths per band (defaults are the daemon v111 well-known paths; tests
// override them to point at a fake daemon).
struct DaemonPaths {
    std::string data433;
    std::string conf433;
    std::string data868;
    std::string conf868;
};
DaemonPaths default_daemon_paths();

class PosixDaemonTransport final : public DaemonTransport {
public:
    explicit PosixDaemonTransport(DaemonPaths paths = default_daemon_paths());
    ~PosixDaemonTransport() override;

    bool begin_connect(Band band) override;
    ConnectState poll_connect() override;
    int conf_send_some(const uint8_t* data, size_t len) override;
    int data_send_some(const uint8_t* data, size_t len) override;
    int conf_recv(uint8_t* buf, int cap) override;
    int data_recv(uint8_t* buf, int cap) override;
    void close() override;

private:
    static int send_some(int fd, const uint8_t* data, size_t len);
    static int recv_some(int fd, uint8_t* buf, int cap);
    // Start one non-blocking AF_UNIX connect. *connecting set true on EINPROGRESS.
    static int unix_begin_connect(const std::string& path, bool* connecting);
    // Zero-timeout SO_ERROR check on a connecting fd. Returns the ConnectState.
    static ConnectState check_one(int fd, bool* connecting);

    DaemonPaths paths_;
    int data_fd_ = -1;
    int conf_fd_ = -1;
    bool data_connecting_ = false;  // connect() still in progress (EINPROGRESS)
    bool conf_connecting_ = false;
};

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_POSIX_TRANSPORT_H
