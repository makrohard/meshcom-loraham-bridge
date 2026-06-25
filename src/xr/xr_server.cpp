// xr_server.cpp — see xr_server.h.
//
// SPDX-License-Identifier: MIT

#include "xr/xr_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace mebridge {

namespace {
bool set_nonblocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}
}  // namespace

XrServer::XrServer(RadioBackend& backend, AuthConfig auth, const Clock& clock,
                   const XrSession::Timeouts& to, size_t max_outbox)
    : backend_(backend), auth_(std::move(auth)), clock_(clock), to_(to),
      max_outbox_(max_outbox) {}

XrServer::~XrServer() {
    conn_.reset();
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

bool XrServer::listen(const std::string& bind_addr, uint16_t port, std::string& err) {
    err.clear();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = "socket() failed"; return false; }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1) {
        err = "invalid bind address";
        ::close(fd);
        return false;
    }
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind() failed";
        ::close(fd);
        return false;
    }
    if (::listen(fd, 4) != 0) {
        err = "listen() failed";
        ::close(fd);
        return false;
    }
    if (!set_nonblocking(fd)) {
        err = "fcntl(O_NONBLOCK) failed";
        ::close(fd);
        return false;
    }

    struct sockaddr_in actual;
    socklen_t alen = sizeof(actual);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr*>(&actual), &alen) == 0) {
        bound_port_ = ntohs(actual.sin_port);
    }
    listen_fd_ = fd;
    return true;
}

void XrServer::try_accept() {
    for (;;) {
        int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            return;  // transient accept error; try again next loop
        }
        if (conn_) {
            // One active client only: refuse the newcomer without disturbing it.
            ::close(cfd);
            continue;
        }
        if (!set_nonblocking(cfd)) { ::close(cfd); continue; }
        conn_ = std::make_unique<XrConnection>(cfd, backend_, auth_, clock_, to_,
                                               max_outbox_);
        std::fprintf(stderr, "[bridge] XR client connected\n");
    }
}

void XrServer::drop_connection_if_finished() {
    if (!conn_ || !conn_->finished()) return;
    // A session we closed carries a diagnostic reason; otherwise the peer closed.
    const XrSession& s = conn_->session();
    if (s.closed()) {
        std::fprintf(stderr, "[bridge] XR client disconnected: %s\n",
                     close_reason_name(s.close_reason()));
    } else {
        std::fprintf(stderr, "[bridge] XR client disconnected: peer closed\n");
    }
    conn_.reset();
}

void XrServer::poll_once(int timeout_ms) {
    struct pollfd fds[2];
    int n = 0;
    fds[n].fd = listen_fd_;
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    const int listen_idx = n;
    ++n;

    int conn_idx = -1;
    if (conn_) {
        conn_idx = n;
        fds[n].fd = conn_->fd();
        fds[n].events = POLLIN | (conn_->want_write() ? POLLOUT : 0);
        fds[n].revents = 0;
        ++n;
    }

    int pr = ::poll(fds, n, timeout_ms);
    if (pr < 0 && errno != EINTR) return;

    if (fds[listen_idx].revents & POLLIN) try_accept();

    if (conn_ && conn_idx >= 0) {
        const short re = fds[conn_idx].revents;
        if (re & (POLLIN | POLLHUP | POLLERR)) conn_->service_read();
        if (conn_ && (re & POLLOUT)) conn_->service_write();
    }

    // Always pump backend + time, even on a poll timeout (keepalive/deadlines).
    if (conn_) {
        conn_->pump_backend();
        conn_->tick();
        conn_->service_write();
        drop_connection_if_finished();
    }
}

void XrServer::run(const std::atomic<bool>& stop) {
    while (!stop.load()) {
        poll_once(100);
    }
}

}  // namespace mebridge
