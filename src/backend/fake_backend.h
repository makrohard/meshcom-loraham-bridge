// fake_backend.h — deterministic in-memory backend for host tests and a runnable
// default. It performs no I/O, no radio, no daemon. Tests script its behavior;
// poll() then delivers the scripted events to the sink in order.
//
// This is the ONLY backend in M11b. It must never simulate an unverified
// "daemon SET succeeded" configuration path: configure() succeeds only when it
// is told it applied the config exactly.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_FAKE_BACKEND_H
#define MEBRIDGE_BACKEND_FAKE_BACKEND_H

#include <deque>
#include <optional>

#include "backend/radio_backend.h"

namespace mebridge {

class FakeBackend final : public RadioBackend {
public:
    // --- scripting: configuration ----------------------------------------
    // Default behavior: configure() applies the requested config exactly
    // (applied = true, effective = requested) -> firmware reaches READY.

    // Make the next configure() report total failure (applied = false).
    void script_config_failure() { config_mode_ = ConfigMode::Failure; }
    // Make the next configure() report applied = true but with a DIFFERENT
    // effective config, modeling a backend that could not honor the request
    // exactly. The session must reject this (no false success).
    void script_config_effective(const extradio::RadioConfig& eff) {
        config_mode_ = ConfigMode::Mismatch;
        forced_effective_ = eff;
    }
    void script_config_exact() { config_mode_ = ConfigMode::Exact; }

    // --- scripting: TX ----------------------------------------------------
    // Outcome delivered for the next submit_tx() on the following poll().
    void script_tx_outcome(TxOutcome o) { next_tx_outcome_ = o; }
    // Emit an unsolicited (stale) completion on the next poll(), even though no
    // TX is in flight. The session must ignore it.
    void script_stale_tx_complete(TxOutcome o) { stale_completions_.push_back(o); }

    // --- scripting: RX ----------------------------------------------------
    void script_rx(const RxEvent& e) { rx_queue_.push_back(e); }

    // --- scripting: backend failure --------------------------------------
    void script_backend_failure() { fail_pending_ = true; }

    // --- RadioBackend -----------------------------------------------------
    void set_sink(BackendSink* sink) override { sink_ = sink; }
    ConfigureResult configure(const extradio::RadioConfig& requested) override;
    void start() override { started_ = true; }
    void stop() override { started_ = false; }
    bool ready() const override { return started_ && configured_; }
    bool submit_tx(const uint8_t* data, size_t len) override;
    // The fake has no real downstream owner, so abandoning simply forgets the
    // in-flight TX (no draining/recovery is meaningful here).
    void abandon_pending_tx() override { tx_pending_ = false; pending_tx_result_.reset(); }
    void poll() override;

    // --- introspection for tests -----------------------------------------
    bool tx_in_flight() const { return tx_pending_; }
    size_t submit_count() const { return submit_count_; }

private:
    enum class ConfigMode { Exact, Failure, Mismatch };

    BackendSink* sink_ = nullptr;
    bool started_ = false;
    bool configured_ = false;

    ConfigMode config_mode_ = ConfigMode::Exact;
    extradio::RadioConfig forced_effective_{};

    bool tx_pending_ = false;
    size_t submit_count_ = 0;
    std::optional<TxOutcome> next_tx_outcome_;
    std::optional<TxOutcome> pending_tx_result_;  // armed by submit, drained by poll
    std::deque<TxOutcome> stale_completions_;

    std::deque<RxEvent> rx_queue_;
    bool fail_pending_ = false;
};

}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_FAKE_BACKEND_H
