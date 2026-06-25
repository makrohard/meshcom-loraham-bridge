// fake_backend.cpp — see fake_backend.h.
//
// SPDX-License-Identifier: MIT

#include "backend/fake_backend.h"

namespace mebridge {

ConfigureResult FakeBackend::configure(const extradio::RadioConfig& requested) {
    ConfigureResult r;
    switch (config_mode_) {
        case ConfigMode::Failure:
            r.applied = false;
            configured_ = false;
            break;
        case ConfigMode::Mismatch:
            // Backend "applied" something, but not what was asked for. The
            // session must NOT treat this as success.
            r.applied = true;
            r.effective = forced_effective_;
            configured_ = true;
            break;
        case ConfigMode::Exact:
        default:
            r.applied = true;
            r.effective = requested;  // honored exactly
            configured_ = true;
            break;
    }
    return r;
}

bool FakeBackend::submit_tx(const uint8_t* data, size_t len) {
    (void)data;
    if (!ready()) return false;
    if (tx_pending_) return false;  // one in flight at the backend level too
    if (len > extradio::kMaxLoraPayload) return false;
    tx_pending_ = true;
    ++submit_count_;
    // Arm the terminal result to be delivered on the next poll(). Defaults to
    // Success if no outcome was scripted.
    pending_tx_result_ = next_tx_outcome_.value_or(TxOutcome::Success);
    next_tx_outcome_.reset();
    return true;
}

void FakeBackend::poll() {
    if (!sink_) return;

    // 1) Backend failure takes precedence and is terminal for the session.
    if (fail_pending_) {
        fail_pending_ = false;
        tx_pending_ = false;
        pending_tx_result_.reset();
        sink_->on_backend_failure();
        return;
    }

    // 2) Deliver received packets in arrival order.
    while (!rx_queue_.empty()) {
        RxEvent e = rx_queue_.front();
        rx_queue_.pop_front();
        sink_->on_rx(e);
    }

    // 3) Deliver the terminal result of an in-flight TX exactly once.
    if (tx_pending_ && pending_tx_result_) {
        TxOutcome o = *pending_tx_result_;
        pending_tx_result_.reset();
        tx_pending_ = false;
        sink_->on_tx_complete(o);
    }

    // 4) Deliver any scripted stale/late completions (no TX in flight). The
    //    session is expected to ignore these.
    while (!stale_completions_.empty()) {
        TxOutcome o = stale_completions_.front();
        stale_completions_.pop_front();
        sink_->on_tx_complete(o);
    }
}

}  // namespace mebridge
