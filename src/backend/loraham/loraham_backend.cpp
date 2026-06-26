// loraham_backend.cpp — see loraham_backend.h.
//
// SPDX-License-Identifier: MIT

#include "backend/loraham/loraham_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "backend/loraham/loraham_config.h"

namespace mebridge {

using namespace loraham;

namespace {
bool starts_with(const std::string& s, const char* p) {
    return s.compare(0, std::strlen(p), p) == 0;
}
// Documented unsolicited daemon CONF broadcasts (see daemon_monitoring.cpp):
// TX state, CAD state, and live RSSI. These are ignored on the CONF stream.
bool is_broadcast_line(const std::string& s) {
    return starts_with(s, "TX=") || starts_with(s, "CAD=") || starts_with(s, "RSSI=");
}
}  // namespace

// --- configuration ---------------------------------------------------------

uint32_t LorahamBackend::begin_configure(const extradio::RadioConfig& requested) {
    // Refuse while ownership of a prior TX is unresolved, or while a config or TX
    // is already in flight: a fresh session must not reach a TX-capable state
    // until the daemon has provably cleared the previous operation.
    if (state_ == State::Faulted) {
        std::fprintf(stderr, "[loraham] configure refused: backend faulted (restart required)\n");
        return 0;
    }
    if (state_ == State::Draining) {
        std::fprintf(stderr, "[loraham] configure refused: draining a prior TX\n");
        return 0;
    }
    if (state_ == State::Connecting || state_ == State::Configuring) {
        std::fprintf(stderr, "[loraham] configure refused: a configuration is already in progress\n");
        return 0;
    }
    if (state_ == State::TxWriting || state_ == State::TxPending) {
        std::fprintf(stderr, "[loraham] configure refused: a TX is in flight\n");
        return 0;
    }

    Band band;
    ConfigError err = validate_config(requested, &band);
    if (err != ConfigError::Ok) {
        std::fprintf(stderr, "[loraham] config rejected: %s\n", config_error_name(err));
        return 0;
    }

    std::string set_cmd;
    if (!build_set_command(requested, &set_cmd)) return 0;  // validated above

    // Begin the non-blocking connect; the rest progresses in poll().
    if (!transport_.begin_connect(band)) {
        std::fprintf(stderr, "[loraham] cannot start daemon connect\n");
        return 0;
    }

    // Managed channel access + queued TX + per-TX result, then the radio config,
    // then a status query. The control SETs produce no CONF reply (daemon
    // v111: stdout only); only GET STATUS replies, so reply association is
    // unambiguous. Each line must be its own (the daemon matches whole lines).
    conf_out_  = "SET TXMODE=MANAGED\n";
    conf_out_ += "SET TXQUEUE=1\n";
    conf_out_ += "SET TXRESULT=1\n";
    conf_out_ += set_cmd;          // build_set_command already appends '\n'
    conf_out_ += "GET STATUS\n";
    conf_out_off_ = 0;
    awaiting_status_ = false;
    conf_line_.clear();
    parser_reset(parser_);
    pending_cfg_ = requested;
    state_ = State::Connecting;
    config_deadline_at_ = clock_.now_ms() + config_timeout_ms_;
    config_token_ = ++next_token_;  // nonzero
    return config_token_;
}

void LorahamBackend::advance_connect() {
    ConnectState cs = transport_.poll_connect();
    if (cs == ConnectState::Failed) { config_fail("daemon connect failed"); return; }
    if (cs == ConnectState::Connecting) return;  // still pending
    state_ = State::Configuring;
    advance_configure();  // start writing commands now
}

void LorahamBackend::advance_configure() {
    // 1) Write the queued CONF commands (resumable on partial writes).
    while (conf_out_off_ < conf_out_.size()) {
        int w = transport_.conf_send_some(
            reinterpret_cast<const uint8_t*>(conf_out_.data()) + conf_out_off_,
            conf_out_.size() - conf_out_off_);
        if (w < 0) { config_fail("CONF write failed during configure"); return; }
        if (w == 0) break;  // EAGAIN: resume next poll
        conf_out_off_ += static_cast<size_t>(w);
    }
    if (conf_out_off_ >= conf_out_.size()) awaiting_status_ = true;

    // 2) Read the single STATUS reply (ignoring broadcasts).
    if (!awaiting_status_) return;
    bool got = false, ready = false;
    if (!pump_conf(/*awaiting_status=*/true, &got, &ready)) {
        config_fail("daemon CONF error/unexpected input during configure");
        return;
    }
    if (got) {
        if (ready) config_succeed();
        else config_fail("daemon radio not ready after configure");
    }
}

void LorahamBackend::config_succeed() {
    state_ = State::Ready;
    ++tx_epoch_;  // new ownership generation for this (re)configuration
    conf_out_.clear();
    conf_out_off_ = 0;
    awaiting_status_ = false;
    std::fprintf(stderr, "[loraham] configured; radio ready (control-plane)\n");
    if (sink_) {
        ConfigureResult res;
        res.applied = true;
        res.effective = pending_cfg_;  // bridge is the authority; echo the request
        sink_->on_configure_complete(config_token_, res);
    }
}

void LorahamBackend::config_fail(const char* why) {
    std::fprintf(stderr, "[loraham] configuration failed: %s\n", why);
    reset_link_unconfigured();  // settle: close socket, back to Disconnected
    if (sink_) {
        ConfigureResult res;  // applied = false
        sink_->on_configure_complete(config_token_, res);
    }
}

// --- TX --------------------------------------------------------------------

LorahamBackend::TxFlush LorahamBackend::pump_tx_write() {
    while (tx_out_off_ < tx_out_.size()) {
        int w = transport_.data_send_some(tx_out_.data() + tx_out_off_,
                                          tx_out_.size() - tx_out_off_);
        if (w < 0) return TxFlush::Failed;
        if (w == 0) return TxFlush::Partial;  // EAGAIN: resume next poll
        tx_out_off_ += static_cast<size_t>(w);
    }
    return TxFlush::Complete;
}

bool LorahamBackend::submit_tx(const uint8_t* data, size_t len) {
    if (!started_ || state_ != State::Ready) return false;
    if (len == 0 || len > kMaxRfPayload) return false;
    uint8_t frame[kFramedHeaderLen + kMaxRfPayload];
    size_t n = encode_tx_packet(frame, sizeof(frame), data, static_cast<uint16_t>(len));
    if (n == 0) return false;

    tx_out_.assign(frame, frame + n);
    tx_out_off_ = 0;
    pending_epoch_ = ++tx_epoch_;
    state_ = State::TxWriting;  // bridge-owned until the whole frame is written

    TxFlush r = pump_tx_write();
    if (r == TxFlush::Failed) {
        // The daemon never received a complete frame, so it cannot transmit it.
        // Not daemon-owned: reset the link (the session will treat the refused
        // submit as uncertain and close). No sink callback here — the caller of
        // submit_tx drives the session on a false return.
        reset_link_unconfigured();
        return false;
    }
    if (r == TxFlush::Complete) state_ = State::TxPending;  // ownership -> daemon
    return true;  // accepted (TxWriting if partial, else TxPending)
}

void LorahamBackend::abandon_pending_tx() {
    if (state_ == State::TxPending) {
        // Daemon owns the frame; it may still queue or transmit it. Drain.
        state_ = State::Draining;
        drain_deadline_at_ = clock_.now_ms() + drain_timeout_ms_;
        std::fprintf(stderr,
            "[loraham] XR TX deadline reached with daemon result outstanding "
            "-> draining (ownership retained, no TIMEOUT fabricated)\n");
        return;
    }
    if (state_ == State::TxWriting) {
        // Only a partial/incomplete frame was written: the daemon cannot
        // transmit it. Discard and reset the link (conservative; not draining,
        // because the daemon never took ownership of a complete frame).
        std::fprintf(stderr,
            "[loraham] XR TX deadline during write: incomplete frame discarded "
            "(daemon never received a complete packet); resetting daemon link\n");
        reset_link_unconfigured();
        return;
    }
    // Nothing in flight: no-op.
}

// --- operational + recovery ------------------------------------------------

void LorahamBackend::dispatch(const Frame& f) {
    if (f.type == FRAMED_RX_PACKET) {
        // Forward RX only in receive-capable states; drop while draining (no
        // owning session) and otherwise. The sink also drops if its session is
        // not operational.
        const bool rx_ok = (state_ == State::Ready || state_ == State::TxWriting ||
                            state_ == State::TxPending);
        if (!rx_ok || !sink_) return;
        int16_t rssi = 0, snr = 0;
        const uint8_t* rf = nullptr;
        uint16_t rf_len = 0;
        if (!decode_rx(f, rssi, snr, rf, rf_len)) return;
        RxEvent ev;
        ev.rssi_cdbm = rssi;
        ev.snr_cdb = snr;
        ev.len = rf_len;
        if (rf_len) std::memcpy(ev.data, rf, rf_len);
        sink_->on_rx(ev);
        return;
    }

    if (f.type == FRAMED_TX_RESULT) {
        uint8_t status = 0, flags = 0;
        uint16_t seq = 0;
        if (!decode_tx_result(f, status, flags, seq)) return;
        if (state_ == State::TxPending) {
            state_ = State::Ready;
            tx_out_.clear();
            tx_out_off_ = 0;
            // Deliver only for the current transaction epoch (XR seq is not used
            // for correlation and may repeat across sessions).
            if (sink_ && pending_epoch_ == tx_epoch_)
                sink_->on_tx_complete(map_tx_status(status));
            return;
        }
        if (state_ == State::Draining) {
            resolve_drain("daemon delivered the outstanding TX result");
            return;
        }
        return;  // Ready/TxWriting/etc: stale/late, ignore
    }

    if (f.type == FRAMED_ERROR) {
        if (state_ == State::TxPending) {
            state_ = State::Ready;
            tx_out_.clear();
            tx_out_off_ = 0;
            if (sink_ && pending_epoch_ == tx_epoch_)
                sink_->on_tx_complete(TxOutcome::RadioError);
        } else if (state_ == State::Draining) {
            resolve_drain("daemon returned ERROR for the outstanding TX");
        }
        return;
    }
}

void LorahamBackend::advance_operational() {
    // CONF: strict — only documented broadcasts are tolerated; an unexpected
    // STATUS or other reply-like line fails the backend.
    bool got = false, ready = false;
    if (!pump_conf(/*awaiting_status=*/false, &got, &ready)) { handle_disconnect(); return; }

    // Resume an in-flight TX write (bridge-owned frame).
    if (state_ == State::TxWriting) {
        TxFlush r = pump_tx_write();
        if (r == TxFlush::Failed) {
            reset_link_unconfigured();
            if (sink_) sink_->on_backend_failure();
            return;
        }
        if (r == TxFlush::Complete) state_ = State::TxPending;
    }

    // DATA: RX frames, and the TX_RESULT/ERROR while TxPending.
    for (;;) {
        int r = transport_.data_recv(rbuf_, kReadChunk);
        if (r < 0) { handle_disconnect(); return; }
        if (r == 0) break;  // nothing available now

        size_t off = 0;
        for (;;) {
            Frame f;
            PopResult pr = parser_pop(parser_, f);
            if (pr == POP_GOT_FRAME) {
                dispatch(f);
                if (state_ == State::Disconnected || state_ == State::Faulted ||
                    state_ == State::Draining)
                    return;  // link torn down / handed to recovery
                continue;
            }
            if (pr == POP_ERROR) { handle_disconnect(); return; }
            if (off >= static_cast<size_t>(r)) break;  // need more from socket
            size_t took = 0;
            if (!parser_push(parser_, rbuf_ + off, static_cast<size_t>(r) - off, took)) {
                handle_disconnect();
                return;
            }
            off += took;
            if (took == 0) { handle_disconnect(); return; }  // impossible legal frame
        }

        if (r < kReadChunk) break;  // socket likely drained for now
    }
}

void LorahamBackend::advance_drain() {
    // Bounded drain: if the outstanding result never arrives, fault rather than
    // silently re-enabling TX.
    if (clock_.now_ms() >= drain_deadline_at_) {
        enter_fault("drain timeout: no daemon result for the outstanding TX");
        return;
    }

    drain_conf_input();  // lenient: discard broadcasts; closure detected via DATA

    for (;;) {
        int r = transport_.data_recv(rbuf_, kReadChunk);
        if (r < 0) { handle_disconnect(); return; }  // -> enter_fault (Draining)
        if (r == 0) break;

        size_t off = 0;
        for (;;) {
            Frame f;
            PopResult pr = parser_pop(parser_, f);
            if (pr == POP_GOT_FRAME) {
                dispatch(f);
                if (state_ != State::Draining) return;  // resolved/faulted
                continue;
            }
            if (pr == POP_ERROR) { handle_disconnect(); return; }
            if (off >= static_cast<size_t>(r)) break;
            size_t took = 0;
            if (!parser_push(parser_, rbuf_ + off, static_cast<size_t>(r) - off, took)) {
                handle_disconnect();
                return;
            }
            off += took;
            if (took == 0) { handle_disconnect(); return; }
        }
        if (r < kReadChunk) break;
    }
}

bool LorahamBackend::pump_conf(bool awaiting_status, bool* got_status, bool* status_ready) {
    *got_status = false;
    *status_ready = false;
    for (;;) {
        int r = transport_.conf_recv(rbuf_, kReadChunk);
        if (r < 0) return false;   // disconnect/error
        if (r == 0) break;         // nothing available now
        conf_line_.append(reinterpret_cast<const char*>(rbuf_), static_cast<size_t>(r));
        if (conf_line_.size() > kMaxConfLine) return false;  // bounded: runaway input

        size_t nl;
        while ((nl = conf_line_.find('\n')) != std::string::npos) {
            std::string line = conf_line_.substr(0, nl);
            conf_line_.erase(0, nl + 1);
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty()) continue;
            if (is_broadcast_line(line)) continue;  // documented unsolicited broadcast
            if (starts_with(line, "STATUS")) {
                if (!awaiting_status) return false;  // unsolicited STATUS: fatal
                *got_status = true;
                *status_ready = status_line_radio_ready(line);
                return true;  // exactly one status accepted; keep any trailing bytes
            }
            return false;  // any other non-broadcast reply-like line: fatal
        }
        if (r < kReadChunk) break;  // drained for now
    }
    return true;
}

void LorahamBackend::drain_conf_input() {
    for (;;) {
        int r = transport_.conf_recv(rbuf_, kReadChunk);
        if (r <= 0) break;  // 0 / closed / error: stop; DATA recv owns closure here
        if (r < kReadChunk) break;
    }
}

void LorahamBackend::handle_disconnect() {
    if (state_ == State::Draining) {
        // Daemon source shows a socket close does not cancel the TX, so a link
        // loss while draining cannot prove the packet will not transmit.
        enter_fault("daemon link lost during drain (cannot prove TX clearance)");
        return;
    }
    const bool was_tx = (state_ == State::TxPending || state_ == State::TxWriting);
    reset_link_unconfigured();
    std::fprintf(stderr, "[loraham] daemon link lost%s\n",
                 was_tx ? " (TX in flight -> uncertain)" : "");
    if (sink_) sink_->on_backend_failure();
}

void LorahamBackend::resolve_drain(const char* why) {
    reset_link_unconfigured();  // ownership proven clear; release the socket
    std::fprintf(stderr, "[loraham] drain complete: %s; daemon TX ownership cleared\n", why);
}

void LorahamBackend::enter_fault(const char* why) {
    transport_.close();
    state_ = State::Faulted;
    parser_reset(parser_);
    conf_line_.clear();
    conf_out_.clear();
    conf_out_off_ = 0;
    tx_out_.clear();
    tx_out_off_ = 0;
    awaiting_status_ = false;
    std::fprintf(stderr, "[loraham] FAULTED: %s; TX disabled until restart\n", why);
}

void LorahamBackend::reset_link_unconfigured() {
    transport_.close();
    state_ = State::Disconnected;
    parser_reset(parser_);
    conf_line_.clear();
    conf_out_.clear();
    conf_out_off_ = 0;
    tx_out_.clear();
    tx_out_off_ = 0;
    awaiting_status_ = false;
}

void LorahamBackend::stop() {
    // Preserve recovery state across an XR connection teardown: keep the daemon
    // socket open (and ownership tracked) so an outstanding TX result can still
    // be drained headlessly by the server. Only started_ is dropped.
    if (state_ == State::Draining || state_ == State::Faulted) {
        started_ = false;
        return;
    }
    // Any other state (incl. an in-progress configuration or a bridge-owned
    // TxWriting frame) is safely settled by closing the link: a fresh session
    // must run a brand-new begin_configure() before it can reach Ready.
    reset_link_unconfigured();
    started_ = false;
}

uint64_t LorahamBackend::next_deadline_ms() const {
    if (state_ == State::Connecting || state_ == State::Configuring)
        return config_deadline_at_;
    if (state_ == State::Draining) return drain_deadline_at_;
    return UINT64_MAX;
}

void LorahamBackend::poll() {
    switch (state_) {
        case State::Disconnected:
        case State::Faulted:
            return;
        case State::Connecting:
            if (clock_.now_ms() >= config_deadline_at_) {
                config_fail("connect deadline");
                return;
            }
            advance_connect();
            return;
        case State::Configuring:
            if (clock_.now_ms() >= config_deadline_at_) {
                config_fail("configuration deadline");
                return;
            }
            advance_configure();
            return;
        case State::Ready:
        case State::TxWriting:
        case State::TxPending:
            advance_operational();
            return;
        case State::Draining:
            advance_drain();
            return;
    }
}

}  // namespace mebridge
