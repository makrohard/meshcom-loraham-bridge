// xr_connection.h — one accepted firmware TCP connection.
//
// Binds a non-blocking client socket fd to an XrSession and the shared
// RadioBackend: pumps socket reads into the session, drains session output to
// the socket, runs the backend poll, and ticks time. It owns no protocol logic
// (that is XrSession) and no accept logic (that is XrServer).
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_XR_XR_CONNECTION_H
#define MEBRIDGE_XR_XR_CONNECTION_H

#include "auth/hmac_auth.h"
#include "backend/radio_backend.h"
#include "util/clock.h"
#include "xr/xr_session.h"

namespace mebridge {

class XrConnection {
public:
    XrConnection(int fd, RadioBackend& backend, const AuthConfig& auth,
                 const Clock& clock, const XrSession::Timeouts& to,
                 size_t max_outbox);
    ~XrConnection();

    XrConnection(const XrConnection&) = delete;
    XrConnection& operator=(const XrConnection&) = delete;

    int fd() const { return fd_; }

    // Read available bytes (non-blocking) and feed the session.
    void service_read();
    // Flush queued output (non-blocking).
    void service_write();
    // Deliver backend async events into the session.
    void pump_backend() { backend_.poll(); }
    // Time-driven session work.
    void tick() { session_.tick(); }

    bool want_write() const { return session_.out_size() > 0; }

    // True once the session has closed AND all output has been flushed, or the
    // socket failed hard. The owner should then destroy this connection.
    bool finished() const;

    const XrSession& session() const { return session_; }

private:
    void flush();

    int fd_;
    RadioBackend& backend_;
    XrSession session_;
    bool socket_error_ = false;
};

}  // namespace mebridge

#endif  // MEBRIDGE_XR_XR_CONNECTION_H
