// test_xr_session.cpp — server-side protocol state machine tests.
//
// Covers: fragmented/coalesced input, HELLO/HELLO_ACK, open + password auth,
// malformed/illegal-state close, exact-config gating, RX endian/metadata, one
// TX in flight, TX outcome mapping, backend failure while pending, stale
// completion, and PING/PONG keepalive.
//
// SPDX-License-Identifier: MIT

#include <vector>

#include "auth/hmac_auth.h"
#include "backend/fake_backend.h"
#include "test_helpers.h"
#include "util/clock.h"
#include "xr/xr_session.h"

using namespace mebridge;
using namespace extradio;
// NOTE: both namespaces declare a TxOutcome, so it is always written qualified
// as mebridge::TxOutcome below to avoid an ambiguous-lookup error.
using testutil::build_auth_response;
using testutil::build_configure;
using testutil::build_hello;
using testutil::build_pong;
using testutil::build_tx_request;
using testutil::count_type;
using testutil::default_config;
using testutil::drain;
using testutil::find_type;
using testutil::get16;

namespace {

constexpr size_t kBigOutbox = 1u << 20;

struct Fix {
    FakeBackend be;
    ManualClock clk{1000};
    AuthConfig auth;  // open mode
    XrSession s;
    explicit Fix(XrSession::Timeouts to = XrSession::default_timeouts())
        : s(be, auth, clk, to, kBigOutbox) {
        be.set_sink(&s);
        s.open();
    }
    void feed(const std::vector<uint8_t>& bytes) { s.feed(bytes.data(), bytes.size()); }
};

// Drive an open-mode session to Ready and assert the handshake frames.
void bring_ready_open(Fix& f) {
    f.feed(build_hello());
    auto hs = drain(f.s);
    CHECK(find_type(hs, MSG_HELLO_ACK) != nullptr);
    const Frame* ar = find_type(hs, MSG_AUTH_RESULT);
    CHECK(ar != nullptr && ar->payload[0] == AUTH_OK);

    f.feed(build_configure(default_config()));
    auto cr = drain(f.s);
    const Frame* cfg = find_type(cr, MSG_CONFIG_RESULT);
    CHECK(cfg != nullptr);
    CHECK(cfg->len == 1 + kConfigPayloadSize);
    CHECK(cfg->payload[0] == CFG_OK);
    CHECK(f.s.ready());
}

// 1. fragmented and coalesced input
void test_fragmented_and_coalesced() {
    // Fragmented: feed HELLO one byte at a time.
    {
        Fix f;
        auto hello = build_hello();
        for (uint8_t b : hello) f.s.feed(&b, 1);
        auto out = drain(f.s);
        CHECK(find_type(out, MSG_HELLO_ACK) != nullptr);
        CHECK(find_type(out, MSG_AUTH_RESULT) != nullptr);
    }
    // Coalesced: HELLO + CONFIGURE in a single feed reaches Ready.
    {
        Fix f;
        std::vector<uint8_t> both = build_hello();
        auto cfg = build_configure(default_config());
        both.insert(both.end(), cfg.begin(), cfg.end());
        f.feed(both);
        CHECK(f.s.ready());
        auto out = drain(f.s);
        CHECK(find_type(out, MSG_HELLO_ACK) != nullptr);
        CHECK(find_type(out, MSG_AUTH_RESULT) != nullptr);
        CHECK(find_type(out, MSG_CONFIG_RESULT) != nullptr);
    }
}

// 2. HELLO -> HELLO_ACK
void test_hello_ack() {
    Fix f;
    f.feed(build_hello());
    auto out = drain(f.s);
    const Frame* ack = find_type(out, MSG_HELLO_ACK);
    CHECK(ack != nullptr);
    CHECK(ack->len == 1 && ack->payload[0] == kVersion);
    CHECK(ack->seq == 0);
}

// 3. open authentication
void test_open_auth() {
    Fix f;
    f.feed(build_hello());
    auto out = drain(f.s);
    const Frame* ar = find_type(out, MSG_AUTH_RESULT);
    CHECK(ar != nullptr && ar->payload[0] == AUTH_OK);
    CHECK(find_type(out, MSG_AUTH_CHALLENGE) == nullptr);  // no challenge in open mode
}

// 4. password authentication: success and failure
void test_password_auth() {
    const uint8_t pw[] = {'s', 'e', 'c', 'r', 'e', 't'};

    // success
    {
        FakeBackend be;
        ManualClock clk{1000};
        AuthConfig auth = AuthConfig::from_password_bytes(pw, sizeof(pw));
        XrSession s(be, auth, clk, XrSession::default_timeouts(), kBigOutbox);
        be.set_sink(&s);
        s.open();
        s.feed(build_hello().data(), build_hello().size());
        auto out = drain(s);
        const Frame* ch = find_type(out, MSG_AUTH_CHALLENGE);
        CHECK(ch != nullptr && ch->len == kAuthNonceSize);
        uint8_t resp[32];
        CHECK(hmac_sha256(pw, sizeof(pw), ch->payload, kAuthNonceSize, resp));
        auto ar = build_auth_response(resp);
        s.feed(ar.data(), ar.size());
        auto out2 = drain(s);
        const Frame* res = find_type(out2, MSG_AUTH_RESULT);
        CHECK(res != nullptr && res->payload[0] == AUTH_OK);
        CHECK(!s.closed());
    }

    // failure: wrong response -> AUTH_FAIL then closed
    {
        FakeBackend be;
        ManualClock clk{1000};
        AuthConfig auth = AuthConfig::from_password_bytes(pw, sizeof(pw));
        XrSession s(be, auth, clk, XrSession::default_timeouts(), kBigOutbox);
        be.set_sink(&s);
        s.open();
        s.feed(build_hello().data(), build_hello().size());
        (void)drain(s);
        uint8_t bad[32];
        std::memset(bad, 0xAB, sizeof(bad));
        auto ar = build_auth_response(bad);
        s.feed(ar.data(), ar.size());
        auto out = drain(s);
        const Frame* res = find_type(out, MSG_AUTH_RESULT);
        CHECK(res != nullptr && res->payload[0] == AUTH_FAIL);
        CHECK(s.closed());
        CHECK(s.close_reason() == CloseReason::AuthFailed);
    }
}

// 5. malformed frame / illegal state both close the session
void test_malformed_and_bad_state() {
    // bad version byte in the header -> parser fails closed
    {
        Fix f;
        uint8_t bad[8] = {0x58, 0x52, 0x99 /*version*/, MSG_HELLO, 0, 1, 0, 0};
        f.s.feed(bad, sizeof(bad));
        CHECK(f.s.closed());
        CHECK(f.s.close_reason() == CloseReason::ParserError);
    }
    // CONFIGURE before HELLO -> illegal transition
    {
        Fix f;
        f.feed(build_configure(default_config()));
        CHECK(f.s.closed());
        CHECK(f.s.close_reason() == CloseReason::BadState);
    }
    // client sends a server-only message (RX_PACKET) -> illegal
    {
        Fix f;
        uint8_t payload[6] = {0, 0, 0, 0, 0, 0};
        auto rx = testutil::encode_frame(MSG_RX_PACKET, 0, payload, 6);
        f.feed(rx);
        CHECK(f.s.closed());
        CHECK(f.s.close_reason() == CloseReason::BadState);
    }
}

// 6. CONFIGURE reaches ready only after an exact echo
void test_config_exact_reaches_ready() {
    Fix f;
    bring_ready_open(f);
    CHECK(f.s.phase() == Phase::Ready);
}

// 7. mismatched effective config is rejected (no false success)
void test_config_mismatch_rejected() {
    Fix f;
    f.feed(build_hello());
    (void)drain(f.s);
    auto eff = default_config();
    eff.sf = 7;  // backend cannot honor sf=12 exactly
    f.be.script_config_effective(eff);
    f.feed(build_configure(default_config()));
    auto out = drain(f.s);
    const Frame* cr = find_type(out, MSG_CONFIG_RESULT);
    CHECK(cr != nullptr);
    CHECK(cr->len == 1);            // failure form (single status byte)
    CHECK(cr->payload[0] != CFG_OK);
    CHECK(f.s.closed());
    CHECK(f.s.close_reason() == CloseReason::ConfigFailed);
}

// 8. RX forwarding endian + metadata correctness
void test_rx_forwarding() {
    Fix f;
    bring_ready_open(f);
    RxEvent e;
    e.rssi_cdbm = -12050;  // -120.50 dBm
    e.snr_cdb = -275;      // -2.75 dB
    const uint8_t msg[5] = {'h', 'e', 'l', 'l', 'o'};
    e.len = 5;
    std::memcpy(e.data, msg, 5);
    f.be.script_rx(e);
    f.be.poll();
    auto out = drain(f.s);
    const Frame* rx = find_type(out, MSG_RX_PACKET);
    CHECK(rx != nullptr);
    CHECK(rx->seq == 0);
    CHECK(rx->len == 6 + 5);
    CHECK(static_cast<int16_t>(get16(rx->payload + 0)) == -12050);  // big-endian
    CHECK(static_cast<int16_t>(get16(rx->payload + 2)) == -275);
    CHECK(get16(rx->payload + 4) == 5);
    CHECK(std::memcmp(rx->payload + 6, msg, 5) == 0);
}

// 9. only one TX in flight
void test_one_tx_in_flight() {
    Fix f;
    bring_ready_open(f);
    const uint8_t data[3] = {1, 2, 3};
    f.feed(build_tx_request(5, data, sizeof(data)));  // accepted, pending
    CHECK(f.s.tx_in_flight());
    f.feed(build_tx_request(6, data, sizeof(data)));  // second while pending
    CHECK(f.s.closed());
    CHECK(f.s.close_reason() == CloseReason::BadState);
}

// Helper to submit one TX and return the resulting TX_RESULT after a backend poll.
Frame submit_and_complete(Fix& f, uint16_t seq, mebridge::TxOutcome outcome) {
    const uint8_t data[4] = {9, 8, 7, 6};
    f.be.script_tx_outcome(outcome);
    f.feed(build_tx_request(seq, data, sizeof(data)));
    CHECK(f.s.tx_in_flight());
    f.be.poll();
    auto out = drain(f.s);
    const Frame* r = find_type(out, MSG_TX_RESULT);
    CHECK(r != nullptr);
    return *r;
}

// 10. TX success maps to SUCCESS with the firmware's seq echoed
void test_tx_success() {
    Fix f;
    bring_ready_open(f);
    Frame r = submit_and_complete(f, 42, mebridge::TxOutcome::Success);
    CHECK(r.seq == 42);
    CHECK(r.len == 1 && r.payload[0] == TXR_SUCCESS);
    CHECK(!f.s.tx_in_flight());
    CHECK(f.s.ready());
}

// 11. CHANNEL_BUSY
void test_tx_channel_busy() {
    Fix f;
    bring_ready_open(f);
    Frame r = submit_and_complete(f, 7, mebridge::TxOutcome::ChannelBusy);
    CHECK(r.seq == 7 && r.payload[0] == TXR_CHANNEL_BUSY);
}

// 12. timeout and radio error
void test_tx_timeout_and_radio_error() {
    {
        Fix f;
        bring_ready_open(f);
        Frame r = submit_and_complete(f, 11, mebridge::TxOutcome::Timeout);
        CHECK(r.seq == 11 && r.payload[0] == TXR_TIMEOUT);
    }
    {
        Fix f;
        bring_ready_open(f);
        Frame r = submit_and_complete(f, 12, mebridge::TxOutcome::RadioError);
        CHECK(r.seq == 12 && r.payload[0] == TXR_RADIO_ERROR);
    }
}

// 13. backend failure while a TX is pending closes the session without success
void test_backend_failure_while_pending() {
    Fix f;
    bring_ready_open(f);
    const uint8_t data[2] = {1, 2};
    f.feed(build_tx_request(99, data, sizeof(data)));
    CHECK(f.s.tx_in_flight());
    f.be.script_backend_failure();
    f.be.poll();
    auto out = drain(f.s);
    CHECK(count_type(out, MSG_TX_RESULT) == 0);  // never a (false) success/result
    CHECK(f.s.closed());
    CHECK(f.s.close_reason() == CloseReason::BackendFailure);
}

// 14. stale backend completion is ignored
void test_stale_completion_ignored() {
    Fix f;
    bring_ready_open(f);
    Frame r = submit_and_complete(f, 21, mebridge::TxOutcome::Success);
    CHECK(r.seq == 21);
    CHECK(!f.s.tx_in_flight());
    // A late/duplicate completion arrives with no TX in flight.
    f.be.script_stale_tx_complete(mebridge::TxOutcome::Success);
    f.be.poll();
    auto out = drain(f.s);
    CHECK(count_type(out, MSG_TX_RESULT) == 0);  // ignored, no second result
    CHECK(f.s.ready());
}

// 15. PING/PONG keepalive and timeout
void test_ping_pong() {
    XrSession::Timeouts to = XrSession::default_timeouts();
    to.ping_interval_ms = 1000;
    to.pong_timeout_ms = 500;
    Fix f(to);
    bring_ready_open(f);

    // No PING before the interval elapses.
    f.clk.advance(900);
    f.s.tick();
    CHECK(count_type(drain(f.s), MSG_PING) == 0);

    // PING after the interval; replying PONG clears the wait.
    f.clk.advance(200);  // now 1100 since ready baseline
    f.s.tick();
    auto out = drain(f.s);
    CHECK(count_type(out, MSG_PING) == 1);
    CHECK(f.s.awaiting_pong());
    f.feed(build_pong());
    CHECK(!f.s.awaiting_pong());
    CHECK(!f.s.closed());

    // Next PING with no reply -> PONG timeout closes the session.
    f.clk.advance(1000);
    f.s.tick();
    CHECK(count_type(drain(f.s), MSG_PING) == 1);
    CHECK(f.s.awaiting_pong());
    f.clk.advance(600);
    f.s.tick();
    CHECK(f.s.closed());
    CHECK(f.s.close_reason() == CloseReason::PongTimeout);
}

// bonus: an unsolicited PONG (not awaiting) closes the session
void test_unsolicited_pong() {
    Fix f;
    bring_ready_open(f);
    f.feed(build_pong());
    CHECK(f.s.closed());
    CHECK(f.s.close_reason() == CloseReason::BadState);
}

}  // namespace

int main() {
    RUN(test_fragmented_and_coalesced);
    RUN(test_hello_ack);
    RUN(test_open_auth);
    RUN(test_password_auth);
    RUN(test_malformed_and_bad_state);
    RUN(test_config_exact_reaches_ready);
    RUN(test_config_mismatch_rejected);
    RUN(test_rx_forwarding);
    RUN(test_one_tx_in_flight);
    RUN(test_tx_success);
    RUN(test_tx_channel_busy);
    RUN(test_tx_timeout_and_radio_error);
    RUN(test_backend_failure_while_pending);
    RUN(test_stale_completion_ignored);
    RUN(test_ping_pong);
    RUN(test_unsolicited_pong);
    std::fprintf(stderr, "test_xr_session: all passed\n");
    return 0;
}
