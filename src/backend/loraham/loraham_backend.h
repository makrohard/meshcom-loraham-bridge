// loraham_backend.h — RadioBackend backed by the LoRaHAM daemon v111.
//
// Drives one band's framed DATA + CONF sockets through an injected
// DaemonTransport. The daemon is the XR configuration authority's downstream:
// the bridge validates the requested config, applies it via CONF `SET`, confirms
// the radio reports READY, and echoes the requested values as the effective
// config (control-plane acceptance, not register read-back — see README).
//
// TX ownership state machine (single TX in flight, never auto-resent):
//
//   Idle ──submit_tx──▶ Pending ──daemon TX_RESULT──▶ Idle        (normal)
//                          │
//                          └──abandon_pending_tx()──▶ Draining
//
//   Draining: the XR session's deadline expired before the daemon resolved the
//   TX. Source analysis of daemon v111 shows that closing/reconnecting the
//   framed socket neither cancels the TX nor lets us learn its outcome, and a
//   late TX_RESULT is only delivered on the SAME still-open slot. So draining
//   keeps the daemon socket open and reads it headlessly until the outstanding
//   TX_RESULT arrives (ownership cleared → back to a clean Idle) — never
//   forwarding that late result to any XR client. A bounded drain deadline, or a
//   daemon link loss during draining, transitions to FAULTED (faulted_): TX
//   stays disabled (no proof of clearance) until the process is restarted.
//
// While Draining or FAULTED the backend is not ready() and configure() is
// refused, so a fresh XR session cannot reach a TX-capable state until the prior
// TX's ownership is provably clear.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_BACKEND_H
#define MEBRIDGE_BACKEND_LORAHAM_BACKEND_H

#include <cstdint>
#include <string>

#include "backend/loraham/loraham_framing.h"
#include "backend/loraham/loraham_transport.h"
#include "backend/radio_backend.h"
#include "external_radio_protocol.h"
#include "util/clock.h"

namespace mebridge {

class LorahamBackend final : public RadioBackend {
public:
    LorahamBackend(loraham::DaemonTransport& transport, const Clock& clock,
                   uint32_t config_timeout_ms = 2000,
                   uint32_t drain_timeout_ms = 30000)
        : transport_(transport), clock_(clock),
          config_timeout_ms_(config_timeout_ms),
          drain_timeout_ms_(drain_timeout_ms) {}

    void set_sink(BackendSink* sink) override { sink_ = sink; }
    ConfigureResult configure(const extradio::RadioConfig& requested) override;
    void start() override { started_ = true; }
    void stop() override;
    bool ready() const override {
        return started_ && configured_ && tx_state_ != TxState::Draining && !faulted_;
    }
    bool submit_tx(const uint8_t* data, size_t len) override;
    void abandon_pending_tx() override;
    void poll() override;

    // Introspection for tests.
    bool tx_in_flight() const { return tx_state_ != TxState::Idle; }
    bool draining() const { return tx_state_ == TxState::Draining; }
    bool faulted() const { return faulted_; }

private:
    enum class TxState : uint8_t { Idle, Pending, Draining };
    static constexpr int kReadChunk = 512;

    bool send_line(const std::string& s);
    void dispatch(const loraham::Frame& f);
    void handle_disconnect();
    void resolve_drain(const char* why);   // ownership cleared while Draining
    void enter_fault(const char* why);     // unrecoverable: TX disabled

    loraham::DaemonTransport& transport_;
    const Clock& clock_;
    uint32_t config_timeout_ms_;
    uint32_t drain_timeout_ms_;

    BackendSink* sink_ = nullptr;
    bool started_ = false;
    bool configured_ = false;
    bool faulted_ = false;
    TxState tx_state_ = TxState::Idle;
    uint64_t drain_deadline_at_ = 0;

    // Per-TX ownership fence, independent of the XR sequence (which may repeat
    // across reconnects): a result is delivered to the sink only for the current
    // transaction epoch.
    uint32_t tx_epoch_ = 0;
    uint32_t pending_epoch_ = 0;

    loraham::Parser parser_{};
    uint8_t rbuf_[kReadChunk];
};

}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_BACKEND_H
