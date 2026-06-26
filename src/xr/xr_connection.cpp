// xr_connection.cpp — see xr_connection.h.
//
// SPDX-License-Identifier: MIT

#include "xr/xr_connection.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mebridge {

XrConnection::XrConnection(int fd, RadioBackend& backend, const AuthConfig& auth,
                           const Clock& clock, const XrSession::Timeouts& to,
                           size_t max_outbox)
    : fd_(fd), backend_(backend),
      session_(backend, auth, clock, to, max_outbox) {
    backend_.set_sink(&session_);
    session_.open();
}

XrConnection::~XrConnection() {
    // Preserve TX ownership across EVERY teardown path. If a complete TX_PACKET
    // is still daemon-owned (backend TxPending), abandon_pending_tx() moves the
    // backend to Draining so stop() keeps the daemon link open to drain the final
    // result — the daemon may still transmit after this client is gone. This must
    // run before detaching the sink and before stop(), and must not depend on the
    // session (which has already cleared its own tx_in_flight_ on close). It is a
    // no-op unless the backend currently owns a TX (see abandon_pending_tx()).
    backend_.abandon_pending_tx();
    backend_.set_sink(nullptr);
    backend_.stop();
    if (fd_ >= 0) ::close(fd_);
}

void XrConnection::service_read() {
    uint8_t buf[1024];
    for (;;) {
        ssize_t r = ::recv(fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (r > 0) {
            session_.feed(buf, static_cast<size_t>(r));
            if (session_.closed()) break;
            if (static_cast<size_t>(r) < sizeof(buf)) break;  // drained for now
            continue;
        }
        if (r == 0) { socket_error_ = true; break; }          // peer closed
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // nothing more now
        if (errno == EINTR) continue;
        socket_error_ = true;
        break;
    }
    flush();
}

void XrConnection::service_write() { flush(); }

void XrConnection::flush() {
    while (session_.out_size() > 0) {
        ssize_t w = ::send(fd_, session_.out_data(), session_.out_size(),
                           MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w > 0) { session_.out_consume(static_cast<size_t>(w)); continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // retry later
        if (w < 0 && errno == EINTR) continue;
        socket_error_ = true;
        break;
    }
}

bool XrConnection::finished() const {
    if (socket_error_) return true;
    return session_.closed() && session_.out_size() == 0;
}

}  // namespace mebridge
