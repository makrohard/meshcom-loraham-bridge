// loraham_posix_transport.cpp — see loraham_posix_transport.h.
//
// SPDX-License-Identifier: MIT

#include "backend/loraham/loraham_posix_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include "backend/loraham/loraham_protocol.h"

namespace mebridge {
namespace loraham {

DaemonPaths default_daemon_paths() {
    DaemonPaths p;
    p.data433 = kDataSocket433;
    p.conf433 = kConfSocket433;
    p.data868 = kDataSocket868;
    p.conf868 = kConfSocket868;
    return p;
}

PosixDaemonTransport::PosixDaemonTransport(DaemonPaths paths)
    : paths_(std::move(paths)) {}

PosixDaemonTransport::~PosixDaemonTransport() { close(); }

namespace {
// Upper bound for a single blocking send to the local daemon. Bounds the event
// loop against an unresponsive daemon rather than blocking indefinitely.
constexpr int kSendTimeoutMs = 2000;

bool set_nonblocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

// Blocking connect to an AF_UNIX stream socket; returns fd or -1.
int unix_connect(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(((struct sockaddr_un*)0)->sun_path))
        return -1;
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
}  // namespace

bool PosixDaemonTransport::send_all(int fd, const uint8_t* data, size_t len) {
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t w = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (w > 0) { off += static_cast<size_t>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Sockets are non-blocking; wait (bounded) for room rather than
            // failing a send on a momentarily full kernel buffer.
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (::poll(&pfd, 1, kSendTimeoutMs) <= 0) return false;  // timeout/error
            continue;
        }
        return false;
    }
    return true;
}

bool PosixDaemonTransport::connect(Band band) {
    close();
    const std::string& dpath = (band == Band::Band433) ? paths_.data433 : paths_.data868;
    const std::string& cpath = (band == Band::Band433) ? paths_.conf433 : paths_.conf868;

    int dfd = unix_connect(dpath);
    if (dfd < 0) return false;
    int cfd = unix_connect(cpath);
    if (cfd < 0) { ::close(dfd); return false; }

    // Both sockets are non-blocking: DATA is polled in poll(); CONF reads/sends
    // are bounded via poll() so an unresponsive daemon cannot stall the bridge.
    if (!set_nonblocking(dfd) || !set_nonblocking(cfd)) {
        ::close(dfd);
        ::close(cfd);
        return false;
    }

    data_fd_ = dfd;
    conf_fd_ = cfd;
    conf_linebuf_.clear();
    return true;
}

bool PosixDaemonTransport::conf_send(const uint8_t* data, size_t len) {
    return send_all(conf_fd_, data, len);
}

bool PosixDaemonTransport::conf_read_line(std::string& out, uint32_t timeout_ms) {
    if (conf_fd_ < 0) return false;

    for (;;) {
        // Serve a complete line already buffered.
        size_t nl = conf_linebuf_.find('\n');
        if (nl != std::string::npos) {
            out = conf_linebuf_.substr(0, nl);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            conf_linebuf_.erase(0, nl + 1);
            return true;
        }

        struct pollfd pfd;
        pfd.fd = conf_fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = ::poll(&pfd, 1, static_cast<int>(timeout_ms));
        if (pr <= 0) return false;  // timeout or error

        char buf[256];
        ssize_t r = ::recv(conf_fd_, buf, sizeof(buf), 0);
        if (r > 0) { conf_linebuf_.append(buf, static_cast<size_t>(r)); continue; }
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;  // non-blocking socket: re-poll
        return false;  // closed or error
    }
}

void PosixDaemonTransport::conf_drain() {
    if (conf_fd_ < 0) return;
    uint8_t buf[256];
    for (;;) {
        ssize_t r = ::recv(conf_fd_, buf, sizeof(buf), MSG_DONTWAIT);
        if (r > 0) continue;
        break;  // 0 (closed) or EAGAIN: stop; closure is detected via DATA recv
    }
}

bool PosixDaemonTransport::data_send(const uint8_t* data, size_t len) {
    return send_all(data_fd_, data, len);
}

int PosixDaemonTransport::data_recv(uint8_t* buf, int cap) {
    if (data_fd_ < 0 || cap <= 0) return -1;
    ssize_t r = ::recv(data_fd_, buf, static_cast<size_t>(cap), MSG_DONTWAIT);
    if (r > 0) return static_cast<int>(r);
    if (r == 0) return -1;  // peer closed
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (errno == EINTR) return 0;
    return -1;
}

void PosixDaemonTransport::close() {
    if (data_fd_ >= 0) { ::close(data_fd_); data_fd_ = -1; }
    if (conf_fd_ >= 0) { ::close(conf_fd_); conf_fd_ = -1; }
    conf_linebuf_.clear();
}

}  // namespace loraham
}  // namespace mebridge
