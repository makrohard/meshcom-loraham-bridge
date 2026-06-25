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

    bool connect(Band band) override;
    bool conf_send(const uint8_t* data, size_t len) override;
    bool conf_read_line(std::string& out, uint32_t timeout_ms) override;
    void conf_drain() override;
    bool data_send(const uint8_t* data, size_t len) override;
    int data_recv(uint8_t* buf, int cap) override;
    void close() override;

private:
    static bool send_all(int fd, const uint8_t* data, size_t len);

    DaemonPaths paths_;
    int data_fd_ = -1;
    int conf_fd_ = -1;
    std::string conf_linebuf_;  // CONF read carry-over between calls
};

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_POSIX_TRANSPORT_H
