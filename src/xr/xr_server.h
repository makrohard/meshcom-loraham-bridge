// xr_server.h — non-blocking TCP listener with a single-active-client policy.
//
// Binds 127.0.0.1 by default, accepts exactly one firmware client at a time, and
// immediately closes any additional incoming connection WITHOUT disturbing the
// active one. Drives one XrConnection through a single poll()-based event loop:
// no threads.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_XR_XR_SERVER_H
#define MEBRIDGE_XR_XR_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "auth/hmac_auth.h"
#include "backend/radio_backend.h"
#include "util/clock.h"
#include "xr/xr_connection.h"
#include "xr/xr_session.h"

namespace mebridge {

class XrServer {
public:
    XrServer(RadioBackend& backend, AuthConfig auth, const Clock& clock,
             const XrSession::Timeouts& to, size_t max_outbox);
    ~XrServer();

    // Bind + listen. port 0 selects an ephemeral port (see bound_port()).
    // Returns false with a non-secret message in *err on failure.
    bool listen(const std::string& bind_addr, uint16_t port, std::string& err);

    uint16_t bound_port() const { return bound_port_; }
    int listener_fd() const { return listen_fd_; }
    bool has_active_connection() const { return conn_ != nullptr; }
    const XrConnection* active_connection() const { return conn_.get(); }

    // One event-loop iteration (poll up to timeout_ms, then service fds).
    void poll_once(int timeout_ms);

    // Run until *stop becomes true.
    void run(const std::atomic<bool>& stop);

private:
    void try_accept();
    void drop_connection_if_finished();
    // Poll timeout for the next loop iteration: capped at 100 ms and shortened to
    // the nearest active XR/backend deadline so deadlines fire promptly.
    int compute_timeout_ms() const;

    RadioBackend& backend_;
    AuthConfig auth_;
    const Clock& clock_;
    XrSession::Timeouts to_;
    size_t max_outbox_;

    int listen_fd_ = -1;
    uint16_t bound_port_ = 0;
    std::unique_ptr<XrConnection> conn_;
};

}  // namespace mebridge

#endif  // MEBRIDGE_XR_XR_SERVER_H
