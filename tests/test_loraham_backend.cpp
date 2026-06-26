// test_loraham_backend.cpp — LorahamBackend over an in-memory fake daemon.
//
// Exercises the non-blocking connect/configure progression, the async config
// completion contract, partial CONF/DATA writes, fragmented/coalesced CONF and
// DATA reads, and the M12d TX-timeout draining/fault recovery — all without a
// real daemon, sockets, or threads.
//
// SPDX-License-Identifier: MIT

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "backend/loraham/loraham_backend.h"
#include "backend/loraham/loraham_framing.h"
#include "backend/loraham/loraham_transport.h"
#include "test_helpers.h"
#include "util/clock.h"

using namespace mebridge;
using mebridge::loraham::Band;
using mebridge::loraham::ConnectState;
using testutil::default_config;

namespace {

// In-memory NON-BLOCKING DaemonTransport. Scripts connect progress, CONF status
// replies, partial writes, and fragmented reads; captures what the backend sends.
class FakeDaemon : public loraham::DaemonTransport {
public:
    // --- connect scripting ---
    bool begin_connect_result = true;
    bool connect_stuck = false;                  // poll_connect always Connecting
    std::deque<ConnectState> connect_script;     // else popped per poll_connect

    // --- write scripting ---
    bool throttle_writes = false;                // one chunk per call, then EAGAIN
    size_t throttle_chunk = 4;
    bool data_send_fail = false;                 // data_send_some returns -1

    // --- CONF reply scripting ---
    std::string status_line = "STATUS RADIO=READY TX=0 CAD=0 TXMODE=MANAGED";
    std::string status_prefix;                   // broadcasts emitted before STATUS
    bool auto_status = true;                      // queue status after GET STATUS seen
    size_t conf_recv_chunk = 0;                  // 0 = serve all available
    bool conf_recv_disconnect = false;

    // --- DATA scripting ---
    bool recv_disconnect = false;
    size_t data_recv_chunk = 0;                  // 0 = serve all available

    // --- observed ---
    bool connected = false;
    Band band = Band::Band433;
    std::string conf_sent;
    std::vector<uint8_t> data_sent;
    std::vector<uint8_t> data_in;                // scripted inbound DATA
    std::string conf_in;                         // scripted inbound CONF

    void push_data(const uint8_t* p, size_t n) { data_in.insert(data_in.end(), p, p + n); }
    void deliver_status() { conf_in += status_prefix; conf_in += status_line; conf_in += "\n"; }

    bool begin_connect(Band b) override {
        if (!begin_connect_result) return false;
        connected = true;
        band = b;
        status_queued_ = false;
        conf_in.clear();
        conf_blocked_ = data_blocked_ = false;
        return true;
    }
    ConnectState poll_connect() override {
        if (connect_stuck) return ConnectState::Connecting;
        if (!connect_script.empty()) {
            ConnectState s = connect_script.front();
            connect_script.pop_front();
            return s;
        }
        return ConnectState::Connected;
    }
    int conf_send_some(const uint8_t* d, size_t len) override {
        size_t n = take_write(len, &conf_blocked_);
        if (n) conf_sent.append(reinterpret_cast<const char*>(d), n);
        maybe_queue_status();
        return static_cast<int>(n);
    }
    int data_send_some(const uint8_t* d, size_t len) override {
        if (data_send_fail) return -1;
        size_t n = take_write(len, &data_blocked_);
        if (n) data_sent.insert(data_sent.end(), d, d + n);
        return static_cast<int>(n);
    }
    int conf_recv(uint8_t* buf, int cap) override {
        if (conf_recv_disconnect) return -1;
        if (conf_in.empty()) return 0;
        size_t n = conf_in.size();
        if (conf_recv_chunk && n > conf_recv_chunk) n = conf_recv_chunk;
        if (n > static_cast<size_t>(cap)) n = static_cast<size_t>(cap);
        std::memcpy(buf, conf_in.data(), n);
        conf_in.erase(0, n);
        return static_cast<int>(n);
    }
    int data_recv(uint8_t* buf, int cap) override {
        if (recv_disconnect) return -1;
        if (data_in.empty()) return 0;
        size_t n = data_in.size();
        if (data_recv_chunk && n > data_recv_chunk) n = data_recv_chunk;
        if (n > static_cast<size_t>(cap)) n = static_cast<size_t>(cap);
        std::memcpy(buf, data_in.data(), n);
        data_in.erase(data_in.begin(), data_in.begin() + n);
        return static_cast<int>(n);
    }
    void close() override { connected = false; }

private:
    bool status_queued_ = false;
    bool conf_blocked_ = false;
    bool data_blocked_ = false;

    size_t take_write(size_t len, bool* blocked) {
        if (!throttle_writes) return len;
        if (*blocked) { *blocked = false; return 0; }  // EAGAIN this call
        *blocked = true;
        return len < throttle_chunk ? len : throttle_chunk;
    }
    void maybe_queue_status() {
        if (auto_status && !status_queued_ &&
            conf_sent.find("GET STATUS\n") != std::string::npos) {
            status_queued_ = true;
            deliver_status();
        }
    }
};

struct RecSink : BackendSink {
    std::vector<RxEvent> rx;
    std::vector<TxOutcome> tx;
    std::vector<ConfigureResult> cfgs;
    std::vector<uint32_t> cfg_tokens;
    int failures = 0;
    void on_rx(const RxEvent& e) override { rx.push_back(e); }
    void on_tx_complete(TxOutcome o) override { tx.push_back(o); }
    void on_backend_failure() override { ++failures; }
    void on_configure_complete(uint32_t t, const ConfigureResult& r) override {
        cfg_tokens.push_back(t);
        cfgs.push_back(r);
    }
};

// Begin a configuration and pump poll() until its deferred completion arrives.
ConfigureResult configure_now(LorahamBackend& be, RecSink& sink,
                              const extradio::RadioConfig& cfg, int max_polls = 80) {
    sink.cfgs.clear();
    sink.cfg_tokens.clear();
    uint32_t tok = be.begin_configure(cfg);
    if (tok == 0) { ConfigureResult r; return r; }  // immediate reject (applied=false)
    CHECK(sink.cfgs.empty());  // never delivered synchronously
    for (int i = 0; i < max_polls && sink.cfgs.empty(); ++i) be.poll();
    CHECK(sink.cfgs.size() == 1);
    CHECK(sink.cfg_tokens.back() == tok);
    return sink.cfgs.back();
}

void test_configure_success_and_conf_sequence() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();

    auto r = configure_now(be, sink, default_config());
    CHECK(r.applied);
    CHECK(extradio::configEqual(r.effective, default_config()));
    CHECK(be.ready());
    CHECK(ft.connected);
    CHECK(ft.band == Band::Band433);

    CHECK(ft.conf_sent.find("SET TXMODE=MANAGED\n") != std::string::npos);
    CHECK(ft.conf_sent.find("SET TXQUEUE=1\n") != std::string::npos);
    CHECK(ft.conf_sent.find("SET TXRESULT=1\n") != std::string::npos);
    CHECK(ft.conf_sent.find("SET MODE=LORA FREQ=433.900000 BW=125 SF=12") != std::string::npos);
    CHECK(ft.conf_sent.find("GET STATUS\n") != std::string::npos);
}

void test_configure_rejects_invalid_config() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    auto cfg = default_config();
    cfg.freq_hz = 600000000u;  // out of band
    CHECK(be.begin_configure(cfg) == 0);  // rejected outright, no connect
    CHECK(!be.ready());
    CHECK(!ft.connected);
    CHECK(ft.conf_sent.empty());
}

void test_configure_not_ready() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.status_line = "STATUS RADIO=FAILED TX=0";
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    auto r = configure_now(be, sink, default_config());
    CHECK(!r.applied);
    CHECK(!be.ready());
    CHECK(!ft.connected);  // link closed after a non-ready status
}

// connect: EINPROGRESS, then writable + SO_ERROR=0 completes the connect.
void test_connect_inprogress_then_completes() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.connect_script = {ConnectState::Connecting, ConnectState::Connected};
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    uint32_t tok = be.begin_configure(default_config());
    CHECK(tok != 0);
    be.poll();                    // still connecting
    CHECK(be.configuring());
    CHECK(sink.cfgs.empty());
    be.poll();                    // connected -> configure -> ready
    CHECK(sink.cfgs.size() == 1);
    CHECK(sink.cfgs.back().applied);
    CHECK(be.ready());
}

// connect: writable but SO_ERROR != 0 fails safely (no false ready).
void test_connect_failure_fails_config() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.connect_script = {ConnectState::Failed};
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    uint32_t tok = be.begin_configure(default_config());
    CHECK(tok != 0);
    be.poll();
    CHECK(sink.cfgs.size() == 1);
    CHECK(!sink.cfgs.back().applied);
    CHECK(!be.ready());
    CHECK(!be.configuring());
}

// connect deadline expiry fails without a stalled loop.
void test_connect_deadline_fails() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.connect_stuck = true;
    LorahamBackend be(ft, clk, /*config_timeout_ms=*/2000, /*drain_timeout_ms=*/30000);
    be.set_sink(&sink);
    be.start();
    uint32_t tok = be.begin_configure(default_config());
    CHECK(tok != 0);
    be.poll();
    CHECK(be.configuring());
    CHECK(sink.cfgs.empty());
    clk.advance(2001);
    be.poll();
    CHECK(sink.cfgs.size() == 1);
    CHECK(!sink.cfgs.back().applied);
    CHECK(!be.configuring());
}

// partial CONF writes are resumed across polls without loss or duplication.
void test_partial_conf_write_resumes() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.throttle_writes = true;  // one small chunk of CONF per poll
    ft.throttle_chunk = 4;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    auto r = configure_now(be, sink, default_config(), /*max_polls=*/200);
    CHECK(r.applied);
    CHECK(ft.conf_sent.find("SET TXMODE=MANAGED\n") != std::string::npos);
    CHECK(ft.conf_sent.find("GET STATUS\n") != std::string::npos);
}

// fragmented CONF status reply is reassembled correctly.
void test_fragmented_conf_status_reassembled() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.conf_recv_chunk = 3;  // status dribbles in three bytes at a time
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    auto r = configure_now(be, sink, default_config(), /*max_polls=*/200);
    CHECK(r.applied);
    CHECK(be.ready());
}

// coalesced CONF input (broadcasts before the STATUS reply) is parsed in order.
void test_coalesced_conf_broadcasts_then_status() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.status_prefix = "TX=0\nCAD=1\nRSSI=-92.50\n";  // unsolicited noise first
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    auto r = configure_now(be, sink, default_config());
    CHECK(r.applied);   // broadcasts ignored; STATUS still recognized
    CHECK(be.ready());
}

// configuration is not reported successful until the readiness condition is met.
void test_no_success_until_status_received() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.auto_status = false;  // daemon withholds the STATUS reply
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    uint32_t tok = be.begin_configure(default_config());
    CHECK(tok != 0);
    for (int i = 0; i < 5; ++i) be.poll();
    CHECK(sink.cfgs.empty());     // commands written, but no readiness yet
    CHECK(be.configuring());
    ft.deliver_status();          // now the daemon answers
    be.poll();
    CHECK(sink.cfgs.size() == 1);
    CHECK(sink.cfgs.back().applied);
    CHECK(be.ready());
}

// daemon disconnect during configuration produces no success.
void test_disconnect_during_configure_fails() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.auto_status = false;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    uint32_t tok = be.begin_configure(default_config());
    CHECK(tok != 0);
    be.poll();                    // connect + write commands; awaiting status
    CHECK(be.configuring());
    ft.conf_recv_disconnect = true;
    be.poll();
    CHECK(sink.cfgs.size() == 1);
    CHECK(!sink.cfgs.back().applied);
    CHECK(!be.ready());
}

void test_rx_forwarding() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);

    const uint8_t rf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint8_t frame[64];
    size_t n = loraham::encode_rx_packet(frame, sizeof(frame), -9000, 125, rf, 4);
    ft.push_data(frame, n);
    be.poll();

    CHECK(sink.rx.size() == 1);
    CHECK(sink.rx[0].rssi_cdbm == -9000);
    CHECK(sink.rx[0].snr_cdb == 125);
    CHECK(sink.rx[0].len == 4);
    CHECK(std::memcmp(sink.rx[0].data, rf, 4) == 0);
}

// fragmented inbound DATA is reassembled by the parser across polls.
void test_rx_forwarding_fragmented() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    ft.data_recv_chunk = 3;  // RX frame arrives a few bytes at a time
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);

    const uint8_t rf[5] = {1, 2, 3, 4, 5};
    uint8_t frame[64];
    size_t n = loraham::encode_rx_packet(frame, sizeof(frame), -7000, 50, rf, 5);
    ft.push_data(frame, n);
    for (int i = 0; i < 40 && sink.rx.empty(); ++i) be.poll();
    CHECK(sink.rx.size() == 1);
    CHECK(sink.rx[0].len == 5);
    CHECK(std::memcmp(sink.rx[0].data, rf, 5) == 0);
}

void test_tx_success_and_one_in_flight() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);

    const uint8_t payload[3] = {1, 2, 3};
    CHECK(be.submit_tx(payload, 3));
    CHECK(be.tx_in_flight());
    CHECK(!be.submit_tx(payload, 3));   // one in flight

    // A full immediate write hands ownership to the daemon (TxPending).
    CHECK(!be.tx_writing());
    CHECK(ft.data_sent.size() == loraham::kFramedHeaderLen + 3);
    CHECK(ft.data_sent[0] == loraham::FRAMED_TX_PACKET);
    CHECK(std::memcmp(ft.data_sent.data() + 3, payload, 3) == 0);

    uint8_t res[16];
    size_t n = loraham::encode_tx_result(res, sizeof(res), loraham::TX_STATUS_OK, 0, 1);
    ft.push_data(res, n);
    be.poll();
    CHECK(sink.tx.size() == 1);
    CHECK(sink.tx[0] == TxOutcome::Success);
    CHECK(!be.tx_in_flight());
}

void test_tx_outcome_mapping() {
    struct { uint8_t status; TxOutcome want; } cases[] = {
        {loraham::TX_STATUS_CHANNEL_BUSY, TxOutcome::ChannelBusy},
        {loraham::TX_STATUS_RADIO_ERROR,  TxOutcome::RadioError},
        {loraham::TX_STATUS_BUSY,         TxOutcome::RadioError},
    };
    for (auto& c : cases) {
        FakeDaemon ft; RecSink sink; ManualClock clk;
        LorahamBackend be(ft, clk);
        be.set_sink(&sink);
        be.start();
        CHECK(configure_now(be, sink, default_config()).applied);
        const uint8_t payload[1] = {9};
        CHECK(be.submit_tx(payload, 1));
        uint8_t res[16];
        size_t n = loraham::encode_tx_result(res, sizeof(res), c.status, 0, 1);
        ft.push_data(res, n);
        be.poll();
        CHECK(sink.tx.size() == 1);
        CHECK(sink.tx[0] == c.want);
    }
}

// partial DATA-frame write: the frame stays bridge-owned (TxWriting) until fully
// written, then transitions to daemon ownership and completes normally.
void test_partial_data_write_resumes() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);

    ft.throttle_writes = true;  // dribble the DATA frame out
    ft.throttle_chunk = 2;
    const uint8_t payload[5] = {5, 4, 3, 2, 1};
    CHECK(be.submit_tx(payload, 5));
    CHECK(be.tx_writing());     // not yet daemon-owned
    CHECK(be.tx_in_flight());

    for (int i = 0; i < 40 && be.tx_writing(); ++i) be.poll();
    CHECK(!be.tx_writing());    // full frame written -> TxPending
    CHECK(ft.data_sent.size() == loraham::kFramedHeaderLen + 5);

    uint8_t res[16];
    size_t n = loraham::encode_tx_result(res, sizeof(res), loraham::TX_STATUS_OK, 0, 1);
    ft.push_data(res, n);
    be.poll();
    CHECK(sink.tx.size() == 1);
    CHECK(sink.tx[0] == TxOutcome::Success);
}

// transport failure after a partial DATA write: conservative recovery, never a
// false success, TX not re-enabled.
void test_data_write_failure_after_partial() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);

    ft.throttle_writes = true;
    ft.throttle_chunk = 2;
    const uint8_t payload[5] = {1, 1, 1, 1, 1};
    CHECK(be.submit_tx(payload, 5));   // partial -> TxWriting
    CHECK(be.tx_writing());

    ft.data_send_fail = true;          // the rest of the frame can't be written
    be.poll();
    CHECK(sink.failures == 1);         // surfaced as backend failure (uncertain)
    CHECK(sink.tx.empty());            // never a (false) terminal result
    CHECK(!be.tx_in_flight());
    CHECK(!be.ready());
}

void test_stale_tx_result_ignored() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);

    uint8_t res[16];
    size_t n = loraham::encode_tx_result(res, sizeof(res), loraham::TX_STATUS_OK, 0, 1);
    ft.push_data(res, n);
    be.poll();                          // no TX in flight
    CHECK(sink.tx.empty());
}

void test_disconnect_is_backend_failure() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);
    CHECK(be.submit_tx((const uint8_t*)"x", 1));

    ft.recv_disconnect = true;
    be.poll();
    CHECK(sink.failures == 1);
    CHECK(!be.ready());
    CHECK(sink.tx.empty());             // never a (false) success on disconnect
}

// --- M12d: TX-timeout ownership recovery (preserved exactly) ----------------

static void to_draining(LorahamBackend& be, RecSink& sink) {
    be.set_sink(&sink);
    be.start();
    CHECK(configure_now(be, sink, default_config()).applied);
    CHECK(be.submit_tx((const uint8_t*)"hi", 2));
    CHECK(be.tx_in_flight());
    be.abandon_pending_tx();
    CHECK(be.draining());
    CHECK(be.tx_in_flight());     // still owned downstream
    CHECK(!be.ready());           // not TX-capable while draining
}

void test_abandon_enters_draining() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    to_draining(be, sink);
    CHECK(sink.tx.empty());       // no fabricated terminal result
    CHECK(!be.faulted());
}

void test_configure_refused_while_draining() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    to_draining(be, sink);
    CHECK(be.begin_configure(default_config()) == 0);  // refused while draining
    CHECK(be.draining());
}

void test_drain_resolves_on_late_result_then_recovers() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    to_draining(be, sink);

    // The outstanding daemon result finally arrives during draining: it clears
    // ownership and is NOT delivered to any client.
    uint8_t res[16];
    size_t n = loraham::encode_tx_result(res, sizeof(res), loraham::TX_STATUS_OK, 0, 1);
    ft.push_data(res, n);
    be.poll();
    CHECK(!be.draining());
    CHECK(!be.tx_in_flight());
    CHECK(!be.faulted());
    CHECK(sink.tx.empty());       // late result never surfaced to a client

    // A fresh configuration + TX now works normally, and its result is delivered.
    CHECK(configure_now(be, sink, default_config()).applied);
    CHECK(be.submit_tx((const uint8_t*)"yo", 2));
    uint8_t ok[16];
    size_t m = loraham::encode_tx_result(ok, sizeof(ok), loraham::TX_STATUS_OK, 0, 9);
    ft.push_data(ok, m);
    be.poll();
    CHECK(sink.tx.size() == 1);   // exactly the new transaction's result
    CHECK(sink.tx[0] == TxOutcome::Success);
}

void test_drain_transport_death_faults() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk);
    to_draining(be, sink);
    ft.recv_disconnect = true;    // daemon link dies mid-drain
    be.poll();
    CHECK(be.faulted());
    CHECK(!be.ready());
    CHECK(sink.tx.empty());
    CHECK(be.begin_configure(default_config()) == 0);  // stays unavailable
}

void test_drain_timeout_faults() {
    FakeDaemon ft; RecSink sink; ManualClock clk;
    LorahamBackend be(ft, clk, /*config_timeout_ms=*/2000, /*drain_timeout_ms=*/30000);
    to_draining(be, sink);
    clk.advance(30001);           // bounded drain elapses with no result
    be.poll();
    CHECK(be.faulted());
    CHECK(!be.ready());
    CHECK(sink.tx.empty());
    CHECK(be.begin_configure(default_config()) == 0);
}

}  // namespace

int main() {
    RUN(test_configure_success_and_conf_sequence);
    RUN(test_configure_rejects_invalid_config);
    RUN(test_configure_not_ready);
    RUN(test_connect_inprogress_then_completes);
    RUN(test_connect_failure_fails_config);
    RUN(test_connect_deadline_fails);
    RUN(test_partial_conf_write_resumes);
    RUN(test_fragmented_conf_status_reassembled);
    RUN(test_coalesced_conf_broadcasts_then_status);
    RUN(test_no_success_until_status_received);
    RUN(test_disconnect_during_configure_fails);
    RUN(test_rx_forwarding);
    RUN(test_rx_forwarding_fragmented);
    RUN(test_tx_success_and_one_in_flight);
    RUN(test_tx_outcome_mapping);
    RUN(test_partial_data_write_resumes);
    RUN(test_data_write_failure_after_partial);
    RUN(test_stale_tx_result_ignored);
    RUN(test_disconnect_is_backend_failure);
    RUN(test_abandon_enters_draining);
    RUN(test_configure_refused_while_draining);
    RUN(test_drain_resolves_on_late_result_then_recovers);
    RUN(test_drain_transport_death_faults);
    RUN(test_drain_timeout_faults);
    std::fprintf(stderr, "test_loraham_backend: all passed\n");
    return 0;
}
