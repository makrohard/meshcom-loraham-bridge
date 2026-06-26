// test_fake_backend.cpp — unit tests for the deterministic test double itself.
//
// SPDX-License-Identifier: MIT

#include <vector>

#include "backend/fake_backend.h"
#include "test_helpers.h"

using namespace mebridge;
using testutil::default_config;

namespace {

// Minimal recording sink.
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

// Drive a begin_configure() to its deferred completion and return the result.
ConfigureResult run_configure(FakeBackend& be, RecSink& sink,
                              const extradio::RadioConfig& cfg) {
    sink.cfgs.clear();
    sink.cfg_tokens.clear();
    uint32_t tok = be.begin_configure(cfg);
    CHECK(tok != 0);
    CHECK(sink.cfgs.empty());  // never delivered synchronously
    be.poll();
    CHECK(sink.cfgs.size() == 1);
    CHECK(sink.cfg_tokens[0] == tok);
    return sink.cfgs[0];
}

void test_configure_exact_echo() {
    FakeBackend be;
    RecSink sink;
    be.set_sink(&sink);
    auto cfg = default_config();
    auto r = run_configure(be, sink, cfg);
    CHECK(r.applied);
    CHECK(extradio::configEqual(r.effective, cfg));
    CHECK(be.ready() == false);  // not started yet
    be.start();
    CHECK(be.ready());
}

void test_configure_failure() {
    FakeBackend be;
    RecSink sink;
    be.set_sink(&sink);
    be.script_config_failure();
    auto r = run_configure(be, sink, default_config());
    CHECK(!r.applied);
}

void test_configure_mismatch() {
    FakeBackend be;
    RecSink sink;
    be.set_sink(&sink);
    auto eff = default_config();
    eff.sf = 7;  // different from what a caller requests with sf=12
    be.script_config_effective(eff);
    auto r = run_configure(be, sink, default_config());
    CHECK(r.applied);
    CHECK(!extradio::configEqual(r.effective, default_config()));
}

void test_tx_outcome_arming_and_one_in_flight() {
    FakeBackend be;
    RecSink sink;
    be.set_sink(&sink);
    run_configure(be, sink, default_config());
    be.start();

    be.script_tx_outcome(TxOutcome::ChannelBusy);
    const uint8_t data[3] = {1, 2, 3};
    CHECK(be.submit_tx(data, sizeof(data)));
    CHECK(be.tx_in_flight());
    CHECK(!be.submit_tx(data, sizeof(data)));  // one in flight at backend level

    be.poll();
    CHECK(sink.tx.size() == 1);
    CHECK(sink.tx[0] == TxOutcome::ChannelBusy);
    CHECK(!be.tx_in_flight());
}

void test_rx_order() {
    FakeBackend be;
    RecSink sink;
    be.set_sink(&sink);
    RxEvent a; a.len = 1; a.data[0] = 0xAA;
    RxEvent b; b.len = 1; b.data[0] = 0xBB;
    be.script_rx(a);
    be.script_rx(b);
    be.poll();
    CHECK(sink.rx.size() == 2);
    CHECK(sink.rx[0].data[0] == 0xAA);
    CHECK(sink.rx[1].data[0] == 0xBB);
}

void test_stale_completion_and_failure() {
    FakeBackend be;
    RecSink sink;
    be.set_sink(&sink);
    be.script_stale_tx_complete(TxOutcome::Success);
    be.poll();
    CHECK(sink.tx.size() == 1);   // delivered even with no TX in flight (stale)

    be.script_backend_failure();
    be.poll();
    CHECK(sink.failures == 1);
}

}  // namespace

int main() {
    RUN(test_configure_exact_echo);
    RUN(test_configure_failure);
    RUN(test_configure_mismatch);
    RUN(test_tx_outcome_arming_and_one_in_flight);
    RUN(test_rx_order);
    RUN(test_stale_completion_and_failure);
    std::fprintf(stderr, "test_fake_backend: all passed\n");
    return 0;
}
