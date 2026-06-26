// loraham_backend.h — RadioBackend backed by the LoRaHAM daemon v111.
//
// Drives one band's framed DATA + CONF sockets through an injected, fully
// NON-BLOCKING DaemonTransport. The daemon is the XR configuration authority's
// downstream: the bridge validates the requested config, submits it via CONF
// `SET`, confirms the radio reports READY, and echoes the requested values as the
// effective config (control-plane acceptance, not register read-back — see
// README). All daemon I/O is bounded and deadline-driven; nothing here ever
// blocks the bridge's single event loop.
//
// One state machine owns the whole daemon lifecycle:
//
//   Disconnected ─begin_configure─▶ Connecting ─connected─▶ Configuring
//        ▲                                                       │
//        │                                  STATUS RADIO=READY   │
//        │                                                       ▼
//        └───────────────────────────────────────────────────  Ready
//                                                              │   ▲
//                                            submit_tx (write) │   │ TX_RESULT
//                                                              ▼   │
//                                   TxWriting ─frame fully written─▶ TxPending
//
// Ownership boundary (refinement): a TX frame that is queued or only partially
// written is still BRIDGE-owned (TxWriting). Daemon ownership begins only once
// the complete framed TX_PACKET has been written (TxPending). A transport
// failure during TxWriting means the daemon never received a complete frame, so
// it cannot transmit it — that is a clean backend failure (not "uncertain").
//
// TX timeout recovery (M12d, preserved exactly):
//
//   TxPending ─abandon_pending_tx()─▶ Draining ─daemon TX_RESULT─▶ Disconnected
//                                         │
//                       link loss / drain timeout │
//                                         ▼
//                                      Faulted (TX disabled until restart)
//
//   Draining keeps the daemon socket open and reads it headlessly until the
//   outstanding TX_RESULT arrives (ownership cleared) — never forwarding that
//   late result to any XR client. Closing/reconnecting daemon v111 neither
//   cancels the TX nor lets us learn its outcome, and a late TX_RESULT is only
//   delivered on the SAME still-open slot, so we must drain. A bounded drain
//   deadline, or a link loss during draining, transitions to Faulted.
//
// While not in the Ready state the backend is not ready() and begin_configure()
// is refused (Draining/Faulted) or only restarts from Disconnected, so a fresh
// XR session cannot reach a TX-capable state until any prior TX's ownership is
// provably clear.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_BACKEND_H
#define MEBRIDGE_BACKEND_LORAHAM_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>

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
    uint32_t begin_configure(const extradio::RadioConfig& requested) override;
    void start() override { started_ = true; }
    void stop() override;
    bool ready() const override { return started_ && state_ == State::Ready; }
    uint64_t next_deadline_ms() const override;
    bool submit_tx(const uint8_t* data, size_t len) override;
    void abandon_pending_tx() override;
    void poll() override;

    // Introspection for tests.
    bool tx_in_flight() const {
        return state_ == State::TxWriting || state_ == State::TxPending ||
               state_ == State::Draining;
    }
    bool tx_writing() const { return state_ == State::TxWriting; }
    bool draining() const { return state_ == State::Draining; }
    bool faulted() const { return state_ == State::Faulted; }
    bool configuring() const {
        return state_ == State::Connecting || state_ == State::Configuring;
    }

private:
    enum class State : uint8_t {
        Disconnected,  // no daemon connection; not configured
        Connecting,    // begin_connect issued; awaiting poll_connect() == Connected
        Configuring,   // connected; writing SET/GET STATUS, awaiting the STATUS reply
        Ready,         // configured; RX flowing; TX idle
        TxWriting,     // TX frame queued/partly written (still bridge-owned)
        TxPending,     // full TX frame written (daemon-owned); awaiting TX_RESULT
        Draining,      // M12d: XR abandoned a pending TX; clearing daemon ownership
        Faulted,       // unrecoverable; TX disabled until restart
    };
    enum class TxFlush : uint8_t { Complete, Partial, Failed };
    static constexpr int kReadChunk = 512;
    static constexpr size_t kMaxConfLine = 1024;  // bounded CONF reassembly

    // configuration progression
    void advance_connect();
    void advance_configure();
    void config_succeed();
    void config_fail(const char* why);
    // operational + recovery
    void advance_operational();
    void advance_drain();
    void dispatch(const loraham::Frame& f);
    void handle_disconnect();
    void resolve_drain(const char* why);  // ownership cleared while Draining
    void enter_fault(const char* why);    // unrecoverable: TX disabled
    void reset_link_unconfigured();       // close + back to Disconnected
    // TX write pump (bridge-owned until the whole frame is out)
    TxFlush pump_tx_write();
    // CONF input: reassemble lines, drop documented broadcasts. While awaiting a
    // STATUS reply, *got_status / *status_ready report it; otherwise an
    // unexpected reply-like line is fatal. Returns false on disconnect/fatal.
    bool pump_conf(bool awaiting_status, bool* got_status, bool* status_ready);
    void drain_conf_input();  // lenient discard (used while Draining)

    loraham::DaemonTransport& transport_;
    const Clock& clock_;
    uint32_t config_timeout_ms_;
    uint32_t drain_timeout_ms_;

    BackendSink* sink_ = nullptr;
    bool started_ = false;
    State state_ = State::Disconnected;

    // configuration operation
    uint32_t config_token_ = 0;       // current op token (echoed to the sink)
    uint32_t next_token_ = 0;
    uint64_t config_deadline_at_ = 0;
    extradio::RadioConfig pending_cfg_{};  // config being applied; echoed on success
    std::string conf_out_;            // pending CONF command bytes
    size_t conf_out_off_ = 0;
    bool awaiting_status_ = false;     // GET STATUS sent; one STATUS reply expected
    std::string conf_line_;           // bounded CONF line reassembly

    // recovery
    uint64_t drain_deadline_at_ = 0;

    // TX
    std::vector<uint8_t> tx_out_;     // framed TX_PACKET being written
    size_t tx_out_off_ = 0;
    // Per-TX ownership fence, independent of the XR sequence (which may repeat
    // across reconnects): a result is delivered to the sink only for the current
    // transaction epoch. configure() and submit_tx() both advance the epoch.
    uint32_t tx_epoch_ = 0;
    uint32_t pending_epoch_ = 0;

    loraham::Parser parser_{};
    uint8_t rbuf_[kReadChunk];
};

}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_BACKEND_H
