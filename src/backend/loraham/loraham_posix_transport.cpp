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
bool set_nonblocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}
}  // namespace

// Create a non-blocking AF_UNIX socket and start connecting. Returns the fd, or
// -1 on a hard failure. On success *connecting is true iff the connect is still
// in progress (EINPROGRESS); false means it completed immediately.
int PosixDaemonTransport::unix_begin_connect(const std::string& path, bool* connecting) {
    *connecting = false;
    if (path.empty() || path.size() >= sizeof(((struct sockaddr_un*)0)->sun_path))
        return -1;
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (!set_nonblocking(fd)) { ::close(fd); return -1; }  // non-blocking BEFORE connect

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) return fd;                          // connected immediately
    if (errno == EINPROGRESS) { *connecting = true; return fd; }
    ::close(fd);
    return -1;                                        // hard failure
}

ConnectState PosixDaemonTransport::check_one(int fd, bool* connecting) {
    if (fd < 0) return ConnectState::Failed;
    if (!*connecting) return ConnectState::Connected;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    int pr = ::poll(&pfd, 1, 0);                       // zero-timeout: never blocks
    if (pr <= 0) return ConnectState::Connecting;      // not writable yet
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return ConnectState::Failed;

    int err = 0;
    socklen_t elen = sizeof(err);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0) return ConnectState::Failed;
    if (err != 0) return ConnectState::Failed;          // a writable fd is NOT proof
    *connecting = false;
    return ConnectState::Connected;
}

int PosixDaemonTransport::send_some(int fd, const uint8_t* data, size_t len) {
    if (fd < 0) return -1;
    if (len == 0) return 0;
    ssize_t w = ::send(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (w > 0) return static_cast<int>(w);
    if (w == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

int PosixDaemonTransport::recv_some(int fd, uint8_t* buf, int cap) {
    if (fd < 0 || cap <= 0) return -1;
    ssize_t r = ::recv(fd, buf, static_cast<size_t>(cap), MSG_DONTWAIT);
    if (r > 0) return static_cast<int>(r);
    if (r == 0) return -1;  // peer closed
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

bool PosixDaemonTransport::begin_connect(Band band) {
    close();
    const std::string& dpath = (band == Band::Band433) ? paths_.data433 : paths_.data868;
    const std::string& cpath = (band == Band::Band433) ? paths_.conf433 : paths_.conf868;

    int dfd = unix_begin_connect(dpath, &data_connecting_);
    if (dfd < 0) return false;
    int cfd = unix_begin_connect(cpath, &conf_connecting_);
    if (cfd < 0) { ::close(dfd); data_connecting_ = false; return false; }

    data_fd_ = dfd;
    conf_fd_ = cfd;
    return true;
}

ConnectState PosixDaemonTransport::poll_connect() {
    ConnectState d = check_one(data_fd_, &data_connecting_);
    ConnectState c = check_one(conf_fd_, &conf_connecting_);
    if (d == ConnectState::Failed || c == ConnectState::Failed) return ConnectState::Failed;
    if (d == ConnectState::Connected && c == ConnectState::Connected)
        return ConnectState::Connected;
    return ConnectState::Connecting;
}

int PosixDaemonTransport::conf_send_some(const uint8_t* data, size_t len) {
    return send_some(conf_fd_, data, len);
}

int PosixDaemonTransport::data_send_some(const uint8_t* data, size_t len) {
    return send_some(data_fd_, data, len);
}

int PosixDaemonTransport::conf_recv(uint8_t* buf, int cap) {
    return recv_some(conf_fd_, buf, cap);
}

int PosixDaemonTransport::data_recv(uint8_t* buf, int cap) {
    return recv_some(data_fd_, buf, cap);
}

void PosixDaemonTransport::close() {
    if (data_fd_ >= 0) { ::close(data_fd_); data_fd_ = -1; }
    if (conf_fd_ >= 0) { ::close(conf_fd_); conf_fd_ = -1; }
    data_connecting_ = false;
    conf_connecting_ = false;
}

}  // namespace loraham
}  // namespace mebridge
