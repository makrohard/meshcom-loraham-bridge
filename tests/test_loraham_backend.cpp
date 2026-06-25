// test_loraham_backend.cpp — LorahamBackend over an in-memory fake daemon.
//
// SPDX-License-Identifier: MIT

#include <cstring>
#include <string>
#include <vector>

#include "backend/loraham/loraham_backend.h"
#include "backend/loraham/loraham_framing.h"
#include "backend/loraham/loraham_transport.h"
#include "test_helpers.h"

using namespace mebridge;
using mebridge::loraham::Band;
using testutil::default_config;

namespace {

// In-memory DaemonTransport: scripts CONF status + inbound DATA frames, captures
// what the backend sends. No sockets, no threads.
class FakeDaemon : public loraham::DaemonTransport {
public:
    // knobs
    bool connect_result = true;
    bool conf_read_result = true;
    std::string status_line = "STATUS RADIO=READY TX=0 CAD=0 TXMODE=MANAGED";
    bool recv_disconnect = false;

    // observed
    bool connected = false;
    Band band = Band::Band433;
    std::string conf_sent;
    std::vector<uint8_t> data_sent;

    // scripted inbound DATA bytes
    std::vector<uint8_t> data_in;

    void push_data(const uint8_t* p, size_t n) { data_in.insert(data_in.end(), p, p + n); }

    bool connect(Band b) override {
        if (!connect_result) return false;
        connected = true;
        band = b;
        return true;
    }
    bool conf_send(const uint8_t* data, size_t len) override {
        conf_sent.append(reinterpret_cast<const char*>(data), len);
        return true;
    }
    bool conf_read_line(std::string& out, uint32_t) override {
        if (!conf_read_result) return false;
        out = status_line;
        return true;
    }
    void conf_drain() override {}
    bool data_send(const uint8_t* data, size_t len) override {
        data_sent.insert(data_sent.end(), data, data + len);
        return true;
    }
    int data_recv(uint8_t* buf, int cap) override {
        if (recv_disconnect) return -1;
        if (data_in.empty()) return 0;
        int n = static_cast<int>(data_in.size());
        if (n > cap) n = cap;
        std::memcpy(buf, data_in.data(), static_cast<size_t>(n));
        data_in.erase(data_in.begin(), data_in.begin() + n);
        return n;
    }
    void close() override { connected = false; }
};

struct RecSink : BackendSink {
    std::vector<RxEvent> rx;
    std::vector<TxOutcome> tx;
    int failures = 0;
    void on_rx(const RxEvent& e) override { rx.push_back(e); }
    void on_tx_complete(TxOutcome o) override { tx.push_back(o); }
    void on_backend_failure() override { ++failures; }
};

void test_configure_success_and_conf_sequence() {
    FakeDaemon ft;
    RecSink sink;
    LorahamBackend be(ft);
    be.set_sink(&sink);
    be.start();

    auto r = be.configure(default_config());
    CHECK(r.applied);
    CHECK(extradio::configEqual(r.effective, default_config()));
    CHECK(be.ready());
    CHECK(ft.connected);
    CHECK(ft.band == Band::Band433);

    // The control SETs and the radio config + GET STATUS were all sent on CONF.
    CHECK(ft.conf_sent.find("SET TXMODE=MANAGED\n") != std::string::npos);
    CHECK(ft.conf_sent.find("SET TXQUEUE=1\n") != std::string::npos);
    CHECK(ft.conf_sent.find("SET TXRESULT=1\n") != std::string::npos);
    CHECK(ft.conf_sent.find("SET MODE=LORA FREQ=433.900000 BW=125 SF=12") != std::string::npos);
    CHECK(ft.conf_sent.find("GET STATUS\n") != std::string::npos);
}

void test_configure_rejects_invalid_config() {
    FakeDaemon ft;
    LorahamBackend be(ft);
    be.start();
    auto cfg = default_config();
    cfg.freq_hz = 600000000u;  // out of band
    auto r = be.configure(cfg);
    CHECK(!r.applied);
    CHECK(!be.ready());
    CHECK(!ft.connected);             // never connected on a validation failure
    CHECK(ft.conf_sent.empty());
}

void test_configure_not_ready() {
    FakeDaemon ft;
    ft.status_line = "STATUS RADIO=FAILED TX=0";
    LorahamBackend be(ft);
    be.start();
    auto r = be.configure(default_config());
    CHECK(!r.applied);
    CHECK(!be.ready());
    CHECK(!ft.connected);             // transport closed after a non-ready status
}

void test_rx_forwarding() {
    FakeDaemon ft;
    RecSink sink;
    LorahamBackend be(ft);
    be.set_sink(&sink);
    be.start();
    CHECK(be.configure(default_config()).applied);

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

void test_tx_success_and_one_in_flight() {
    FakeDaemon ft;
    RecSink sink;
    LorahamBackend be(ft);
    be.set_sink(&sink);
    be.start();
    CHECK(be.configure(default_config()).applied);

    const uint8_t payload[3] = {1, 2, 3};
    CHECK(be.submit_tx(payload, 3));
    CHECK(be.tx_in_flight());
    CHECK(!be.submit_tx(payload, 3));   // one in flight

    // What we sent must be a framed TX_PACKET carrying the payload.
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
        FakeDaemon ft;
        RecSink sink;
        LorahamBackend be(ft);
        be.set_sink(&sink);
        be.start();
        CHECK(be.configure(default_config()).applied);
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

void test_stale_tx_result_ignored() {
    FakeDaemon ft;
    RecSink sink;
    LorahamBackend be(ft);
    be.set_sink(&sink);
    be.start();
    CHECK(be.configure(default_config()).applied);

    uint8_t res[16];
    size_t n = loraham::encode_tx_result(res, sizeof(res), loraham::TX_STATUS_OK, 0, 1);
    ft.push_data(res, n);
    be.poll();                          // no TX in flight
    CHECK(sink.tx.empty());
}

void test_disconnect_is_backend_failure() {
    FakeDaemon ft;
    RecSink sink;
    LorahamBackend be(ft);
    be.set_sink(&sink);
    be.start();
    CHECK(be.configure(default_config()).applied);
    CHECK(be.submit_tx((const uint8_t*)"x", 1));

    ft.recv_disconnect = true;
    be.poll();
    CHECK(sink.failures == 1);
    CHECK(!be.ready());
    CHECK(sink.tx.empty());             // never a (false) success on disconnect
}

}  // namespace

int main() {
    RUN(test_configure_success_and_conf_sequence);
    RUN(test_configure_rejects_invalid_config);
    RUN(test_configure_not_ready);
    RUN(test_rx_forwarding);
    RUN(test_tx_success_and_one_in_flight);
    RUN(test_tx_outcome_mapping);
    RUN(test_stale_tx_result_ignored);
    RUN(test_disconnect_is_backend_failure);
    std::fprintf(stderr, "test_loraham_backend: all passed\n");
    return 0;
}
