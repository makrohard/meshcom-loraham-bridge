// test_protocol_server.cpp — server/connection-level policy tests.
//
// Covers: a second client does not displace the active one (real loopback
// sockets), and slow-client bounded-output handling (the outbox cap closes a
// client that never drains).
//
// SPDX-License-Identifier: MIT

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <vector>

#include "auth/hmac_auth.h"
#include "backend/fake_backend.h"
#include "test_helpers.h"
#include "util/clock.h"
#include "xr/xr_server.h"
#include "xr/xr_session.h"

using namespace mebridge;
using namespace extradio;
using testutil::build_configure;
using testutil::build_hello;
using testutil::default_config;
using testutil::drain;
using testutil::find_type;
using testutil::parse_frames;

namespace {

int connect_client(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct timeval tv{1, 0};  // 1s recv timeout so tests never hang
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    CHECK(::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

// Pump the server several iterations so accept/read/write all make progress.
void pump(XrServer& srv, int iters = 20) {
    for (int i = 0; i < iters; ++i) srv.poll_once(10);
}

std::vector<uint8_t> recv_some(int fd) {
    uint8_t buf[2048];
    ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) return {};
    return std::vector<uint8_t>(buf, buf + r);
}

// 16. A second client does not displace the active client.
void test_second_client_does_not_displace() {
    FakeBackend backend;
    SteadyClock clock;
    XrServer srv(backend, AuthConfig{}, clock, XrSession::default_timeouts(), 64 * 1024);
    std::string err;
    CHECK(srv.listen("127.0.0.1", 0, err));
    const uint16_t port = srv.bound_port();
    CHECK(port != 0);

    int c1 = connect_client(port);
    auto hello = build_hello();
    CHECK(::send(c1, hello.data(), hello.size(), 0) == (ssize_t)hello.size());
    pump(srv);
    auto resp1 = recv_some(c1);
    auto f1 = parse_frames(resp1.data(), resp1.size());
    CHECK(find_type(f1, MSG_HELLO_ACK) != nullptr);
    CHECK(srv.has_active_connection());
    const XrConnection* active = srv.active_connection();

    // Second client connects: server must accept-and-close it without disturbing c1.
    int c2 = connect_client(port);
    auto hello2 = build_hello();
    ::send(c2, hello2.data(), hello2.size(), 0);
    pump(srv);
    auto resp2 = recv_some(c2);   // expect EOF (0 bytes) — connection refused/closed
    CHECK(resp2.empty());

    // c1 still active and functional: complete configuration.
    CHECK(srv.has_active_connection());
    CHECK(srv.active_connection() == active);
    auto cfg = build_configure(default_config());
    CHECK(::send(c1, cfg.data(), cfg.size(), 0) == (ssize_t)cfg.size());
    pump(srv);
    auto resp3 = recv_some(c1);
    auto f3 = parse_frames(resp3.data(), resp3.size());
    const Frame* cr = find_type(f3, MSG_CONFIG_RESULT);
    CHECK(cr != nullptr && cr->payload[0] == CFG_OK);
    CHECK(srv.active_connection()->session().ready());

    ::close(c1);
    ::close(c2);
}

// 17. Slow client: a bounded outbox closes a client that never drains output.
void test_slow_client_bounded_output() {
    FakeBackend be;
    ManualClock clk{1000};
    AuthConfig auth;  // open
    const size_t kSmallOutbox = 100;  // room for handshake, not a flood of RX
    XrSession s(be, auth, clk, XrSession::default_timeouts(), kSmallOutbox);
    be.set_sink(&s);
    s.open();

    // Handshake to Ready, draining as a healthy client would.
    s.feed(build_hello().data(), build_hello().size());
    (void)drain(s);
    s.feed(build_configure(default_config()).data(),
           build_configure(default_config()).size());
    be.poll();  // backend configuration completes asynchronously
    (void)drain(s);
    CHECK(s.ready());

    // Now the client stops reading: flood RX without draining the outbox.
    RxEvent e;
    e.rssi_cdbm = -5000;
    e.snr_cdb = 100;
    e.len = 20;
    std::memset(e.data, 0x5A, e.len);
    for (int i = 0; i < 10; ++i) be.script_rx(e);
    be.poll();  // delivers all RX; outbox overflows partway through

    CHECK(s.closed());
    CHECK(s.close_reason() == CloseReason::OutputOverflow);
}

}  // namespace

int main() {
    RUN(test_second_client_does_not_displace);
    RUN(test_slow_client_bounded_output);
    std::fprintf(stderr, "test_protocol_server: all passed\n");
    return 0;
}
