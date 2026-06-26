// xr_session.h — server-side XR protocol state machine (one firmware client).
//
// This is the mirror of the firmware's client-side Session: the firmware drives
// HELLO/AUTH/CONFIGURE/TX_REQUEST/PONG; the bridge answers HELLO_ACK/
// AUTH_CHALLENGE/AUTH_RESULT/CONFIG_RESULT and originates RX_PACKET/TX_RESULT/
// PING. It owns the bounded input parser, the bounded output queue, auth, and
// keepalive. It performs NO socket I/O: bytes are fed in via feed() and drained
// out via out_data()/out_consume(), so it is fully host-testable with a manual
// clock. It is the BackendSink for its RadioBackend.
//
// Invariants enforced here (fail closed on any violation):
//   * RX and TX are forbidden until an exact CONFIG_RESULT echo (Ready);
//   * at most one TX is in flight; a second TX_REQUEST while pending closes;
//   * a socket write is never RF success — only a backend terminal result (or a
//     bridge-side TX timeout) produces a TX_RESULT;
//   * backend failure while a TX is pending closes the session (no false
//     success) so the firmware resolves the TX as uncertain/unknown;
//   * malformed frames, illegal transitions, client-sent server-only messages,
//     missing PONG, or output overflow all close the session.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_XR_XR_SESSION_H
#define MEBRIDGE_XR_XR_SESSION_H

#include <cstddef>
#include <cstdint>

#include "auth/hmac_auth.h"
#include "backend/radio_backend.h"
#include "external_radio_protocol.h"
#include "util/bounded_buffer.h"
#include "util/clock.h"

namespace mebridge {

enum class Phase : uint8_t {
    WaitHello,
    WaitAuth,
    WaitConfigure,
    Configuring,    // CONFIGURE accepted; awaiting the backend's async result
    Ready,
    Closed,
};

// Why a session closed. Diagnostic only (never put on the wire as success).
enum class CloseReason : uint8_t {
    None = 0,
    OutputOverflow,     // slow/stuck client exceeded the bounded outbox
    MalformedFrame,     // codec validate() rejected a frame
    ParserError,        // streaming parser fail-closed (bad magic/len/etc.)
    BadState,           // illegal transition / client sent a server-only message
    RemoteError,        // client sent an ERROR frame
    AuthFailed,         // HMAC mismatch
    AuthError,          // crypto/nonce backend failure
    ConfigFailed,       // backend did not apply the exact requested config
    BackendFailure,     // backend link failed / refused a TX
    HandshakeTimeout,
    AuthTimeout,
    ConfigTimeout,
    PongTimeout,
    TxTimeoutUncertain, // bridge TX deadline; daemon outcome unknown (no fabricated result)
    InternalError,      // encode failure (should not happen)
};

// Human-readable name for a CloseReason (diagnostics/logging only).
const char* close_reason_name(CloseReason r);

class XrSession final : public BackendSink {
public:
    struct Timeouts {
        uint64_t handshake_ms;
        uint64_t auth_ms;
        uint64_t config_ms;
        uint64_t tx_ms;            // bridge-side ceiling for an in-flight TX
        uint64_t ping_interval_ms; // gap between keepalive PINGs when Ready
        uint64_t pong_timeout_ms;  // max wait for a PONG after a PING
    };
    static Timeouts default_timeouts();

    XrSession(RadioBackend& backend, const AuthConfig& auth, const Clock& clock,
              const Timeouts& to, size_t max_outbox);

    // Begin a fresh connection: arm the handshake deadline and start the backend.
    void open();

    // Feed inbound socket bytes. May enqueue output and/or close the session.
    void feed(const uint8_t* data, size_t n);

    // Periodic time-driven work: phase deadlines, keepalive, TX timeout.
    void tick();

    // Absolute time (clock ms) of the nearest active session deadline, or
    // UINT64_MAX if none. The event loop uses it to shorten its poll timeout.
    uint64_t next_deadline_ms() const;

    // BackendSink (called from RadioBackend::poll()).
    void on_rx(const RxEvent& rx) override;
    void on_tx_complete(TxOutcome outcome) override;
    void on_backend_failure() override;
    void on_configure_complete(uint32_t op_token, const ConfigureResult& res) override;

    // Outbound byte queue (to be written to the socket by the owner).
    const uint8_t* out_data() const { return outbox_.data(); }
    size_t out_size() const { return outbox_.size(); }
    void out_consume(size_t n) { outbox_.consume(n); }

    bool closed() const { return phase_ == Phase::Closed; }
    CloseReason close_reason() const { return close_reason_; }

    // Introspection for tests.
    Phase phase() const { return phase_; }
    bool ready() const { return phase_ == Phase::Ready; }
    bool tx_in_flight() const { return tx_in_flight_; }
    bool awaiting_pong() const { return awaiting_pong_; }

private:
    void handle_frame(const extradio::Frame& f);
    void handle_ready_frame(const extradio::Frame& f);

    bool emit(uint8_t type, uint16_t seq, const uint8_t* payload, uint16_t len);
    void close(CloseReason reason);

    void arm(uint64_t timeout_ms, CloseReason on_expire);
    void disarm() { deadline_active_ = false; }
    void enter_ready();
    void map_and_send_tx_result(TxOutcome outcome);

    RadioBackend& backend_;
    AuthConfig auth_;
    const Clock& clock_;
    Timeouts to_;

    extradio::Parser parser_;
    BoundedBuffer outbox_;

    Phase phase_ = Phase::Closed;
    CloseReason close_reason_ = CloseReason::None;

    // phase deadline (handshake/auth/config)
    bool deadline_active_ = false;
    uint64_t deadline_at_ = 0;
    CloseReason deadline_reason_ = CloseReason::None;

    // auth
    uint8_t nonce_[extradio::kAuthNonceSize] = {0};

    // configuration (the exact config we echoed; used for invariant checks)
    extradio::RadioConfig applied_cfg_{};
    // the config requested by the client, awaiting the backend's async result
    extradio::RadioConfig requested_cfg_{};
    // token of the in-flight begin_configure(); fences a stale completion from an
    // earlier/different session (which may share the backend) out of this one.
    uint32_t config_op_token_ = 0;

    // TX
    bool tx_in_flight_ = false;
    uint16_t tx_seq_ = 0;       // firmware's XR sequence, echoed verbatim
    uint64_t tx_deadline_at_ = 0;

    // keepalive
    bool awaiting_pong_ = false;
    uint64_t next_ping_at_ = 0;
    uint64_t pong_deadline_at_ = 0;
};

}  // namespace mebridge

#endif  // MEBRIDGE_XR_XR_SESSION_H
