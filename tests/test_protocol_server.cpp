// test_protocol_server.cpp — server/connection-level policy tests.
//
// Covers: a second client does not displace the active one (real loopback
// sockets), and slow-client bounded-output handling (the outbox cap closes a
// client that never drains).
//
// SPDX-License-Identifier: MIT

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <memory>
#include <vector>

#include "auth/hmac_auth.h"
#include "backend/fake_backend.h"
#include "backend/loraham/loraham_backend.h"
#include "backend/loraham/loraham_framing.h"
#include "fake_daemon.h"
#include "test_helpers.h"
#include "util/clock.h"
#include "xr/xr_connection.h"
#include "xr/xr_server.h"
#include "xr/xr_session.h"

using namespace mebridge;
using namespace extradio;
using testutil::build_configure;
using testutil::build_hello;
using testutil::build_pong;
using testutil::build_tx_request;
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

// --- XrConnection lifecycle harness over a real socketpair (M12f) ----------
// Drives a single XrConnection backed by a LorahamBackend + in-memory fake
// daemon to TxPending, so teardown/disconnect ownership can be observed directly
// on the backend (Draining / Faulted), exactly as the real server would.

struct RecSink : BackendSink {
    int tx = 0, rx = 0, failures = 0, configs = 0;
    void on_rx(const RxEvent&) override { ++rx; }
    void on_tx_complete(mebridge::TxOutcome) override { ++tx; }
    void on_backend_failure() override { ++failures; }
    void on_configure_complete(uint32_t, const ConfigureResult&) override { ++configs; }
};

void nb(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    CHECK(fl >= 0);
    CHECK(::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0);
}

struct ConnFix {
    testutil::FakeDaemon ft;
    ManualClock clk{1000};
    AuthConfig auth;  // open mode
    LorahamBackend be{ft, clk};
    int sv[2] = {-1, -1};  // sv[0] -> XrConnection, sv[1] -> client
    std::unique_ptr<XrConnection> conn;

    explicit ConnFix(XrSession::Timeouts to = XrSession::default_timeouts()) {
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        nb(sv[0]);
        nb(sv[1]);
        conn = std::make_unique<XrConnection>(sv[0], be, auth, clk, to, 64 * 1024);
    }
    ~ConnFix() {
        conn.reset();
        if (sv[1] >= 0) ::close(sv[1]);
    }
    void client_send(const std::vector<uint8_t>& b) {
        CHECK(::send(sv[1], b.data(), b.size(), 0) == static_cast<ssize_t>(b.size()));
    }
    void pump(int iters = 3) {
        for (int i = 0; i < iters; ++i) {
            conn->service_read();
            conn->pump_backend();
            conn->tick();
            conn->service_write();
        }
    }
    // Reach Ready, then submit one TX so the daemon owns a complete frame.
    void to_tx_pending() {
        client_send(build_hello());
        pump();
        client_send(build_configure(default_config()));
        pump();
        CHECK(conn->session().ready());
        const uint8_t data[3] = {1, 2, 3};
        client_send(build_tx_request(7, data, sizeof(data)));
        pump();
        CHECK(conn->session().tx_in_flight());
        CHECK(be.tx_in_flight());
        CHECK(!be.tx_writing());   // full frame written -> daemon-owned (TxPending)
    }
    // Tear the connection down (destructor runs the ownership handoff).
    void destroy_conn() { conn.reset(); }
    void close_peer() { if (sv[1] >= 0) { ::close(sv[1]); sv[1] = -1; } }
};

// Encode a framed daemon TX_RESULT(OK) for injection into the fake daemon.
std::vector<uint8_t> daemon_tx_result_ok() {
    uint8_t res[16];
    size_t n = loraham::encode_tx_result(res, sizeof(res), loraham::TX_STATUS_OK, 0, 1);
    return std::vector<uint8_t>(res, res + n);
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

// M12f-4: XR peer disconnect after a complete frame is written -> the backend
// enters Draining, keeps being polled headlessly, and a late daemon result is
// NOT delivered to a future XR session.
void test_peer_disconnect_during_pending_drains() {
    ConnFix f;
    f.to_tx_pending();

    f.close_peer();            // firmware vanished
    f.conn->service_read();    // observes EOF -> finished
    CHECK(f.conn->finished());
    f.destroy_conn();          // ~XrConnection runs the ownership handoff
    CHECK(f.be.draining());    // daemon may still transmit -> draining, not reset
    CHECK(!f.be.faulted());

    // A "future session" attaches to the same backend; the late result must not
    // surface to it.
    RecSink late;
    f.be.set_sink(&late);
    f.ft.push_data(daemon_tx_result_ok().data(), daemon_tx_result_ok().size());
    f.be.poll();               // headless drain consumes the late result
    CHECK(!f.be.draining());
    CHECK(!f.be.faulted());
    CHECK(late.tx == 0);       // never delivered to the new sink
    CHECK(f.be.begin_configure(default_config()) != 0);  // recovered, reusable
}

// M12f-5a: a session-owned close (illegal/malformed inbound frame) while the
// daemon owns the TX takes the same draining path as a peer disconnect.
void test_session_close_malformed_during_pending_drains() {
    ConnFix f;
    f.to_tx_pending();

    f.client_send(build_pong());   // unsolicited PONG in Ready -> BadState close
    f.pump();
    CHECK(f.conn->session().closed());
    CHECK(f.conn->session().close_reason() == CloseReason::BadState);
    CHECK(f.conn->finished());
    f.destroy_conn();
    CHECK(f.be.draining());         // ownership preserved
    CHECK(!f.be.faulted());
}

// M12f-5b: a PONG timeout (session-owned) while TxPending also drains. The TX
// ceiling is set well beyond the keepalive so the PONG timeout fires first.
void test_session_close_pong_timeout_during_pending_drains() {
    XrSession::Timeouts to = XrSession::default_timeouts();
    to.ping_interval_ms = 1000;
    to.pong_timeout_ms = 500;
    to.tx_ms = 100000;
    ConnFix f(to);
    f.to_tx_pending();

    f.clk.advance(to.ping_interval_ms + 1);   // server originates a PING
    f.pump();
    CHECK(f.conn->session().awaiting_pong());
    f.clk.advance(to.pong_timeout_ms + 1);     // no PONG -> session closes
    f.pump();
    CHECK(f.conn->session().closed());
    CHECK(f.conn->session().close_reason() == CloseReason::PongTimeout);
    f.destroy_conn();
    CHECK(f.be.draining());
    CHECK(!f.be.faulted());
}

// M12f-7: a NORMAL daemon terminal result followed by XR teardown does NOT enter
// draining or faulted.
void test_normal_completion_then_teardown_no_drain() {
    ConnFix f;
    f.to_tx_pending();

    f.ft.push_data(daemon_tx_result_ok().data(), daemon_tx_result_ok().size());
    f.pump();                                  // backend -> on_tx_complete -> Ready
    CHECK(!f.conn->session().tx_in_flight());
    CHECK(f.conn->session().ready());
    CHECK(!f.be.tx_in_flight());

    f.close_peer();
    f.conn->service_read();
    f.destroy_conn();
    CHECK(!f.be.draining());                    // nothing owed downstream
    CHECK(!f.be.faulted());
}

}  // namespace

int main() {
    RUN(test_second_client_does_not_displace);
    RUN(test_slow_client_bounded_output);
    RUN(test_peer_disconnect_during_pending_drains);
    RUN(test_session_close_malformed_during_pending_drains);
    RUN(test_session_close_pong_timeout_during_pending_drains);
    RUN(test_normal_completion_then_teardown_no_drain);
    std::fprintf(stderr, "test_protocol_server: all passed\n");
    return 0;
}
