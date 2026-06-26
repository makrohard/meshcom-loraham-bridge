// loraham_backend.cpp — see loraham_backend.h.
//
// SPDX-License-Identifier: MIT

#include "backend/loraham/loraham_backend.h"

#include <cstdio>
#include <cstring>

#include "backend/loraham/loraham_config.h"

namespace mebridge {

using namespace loraham;

bool LorahamBackend::send_line(const std::string& s) {
    return transport_.conf_send(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

ConfigureResult LorahamBackend::configure(const extradio::RadioConfig& requested) {
    ConfigureResult res;  // applied=false by default

    // Refuse while ownership of a prior TX is unresolved: a fresh session must
    // not reach a TX-capable state until the daemon has provably cleared it.
    if (faulted_) {
        std::fprintf(stderr, "[loraham] configure refused: backend faulted (restart required)\n");
        return res;
    }
    if (tx_state_ == TxState::Draining) {
        std::fprintf(stderr, "[loraham] configure refused: draining a prior TX\n");
        return res;
    }

    Band band;
    ConfigError err = validate_config(requested, &band);
    if (err != ConfigError::Ok) {
        std::fprintf(stderr, "[loraham] config rejected: %s\n", config_error_name(err));
        return res;
    }

    // (Re)connect the band's sockets for this configuration.
    if (!transport_.connect(band)) {
        std::fprintf(stderr, "[loraham] cannot connect to daemon sockets\n");
        return res;
    }
    parser_reset(parser_);
    tx_state_ = TxState::Idle;
    configured_ = false;

    // Managed channel access + queued TX + per-TX result, then the radio config.
    // Each control SET must be its own line (the daemon matches whole lines).
    std::string set_cmd;
    if (!send_line("SET TXMODE=MANAGED\n") ||
        !send_line("SET TXQUEUE=1\n") ||
        !send_line("SET TXRESULT=1\n") ||
        !build_set_command(requested, &set_cmd) ||
        !send_line(set_cmd) ||
        !send_line("GET STATUS\n")) {
        std::fprintf(stderr, "[loraham] failed to send configuration to daemon\n");
        transport_.close();
        return res;
    }

    // Confirm the radio reports ready before claiming success. This is
    // control-plane acceptance; daemon v111 offers no register read-back.
    std::string status;
    if (!transport_.conf_read_line(status, config_timeout_ms_) ||
        !status_line_radio_ready(status)) {
        std::fprintf(stderr, "[loraham] daemon radio not ready after configure\n");
        transport_.close();
        return res;
    }

    configured_ = true;
    ++tx_epoch_;  // new ownership generation for this (re)configuration
    res.applied = true;
    res.effective = requested;  // bridge is the authority; echo the requested config
    return res;
}

void LorahamBackend::stop() {
    // Preserve recovery state across an XR connection teardown: keep the daemon
    // socket open (and ownership tracked) so an outstanding TX result can still
    // be drained headlessly by the server. Only started_ is dropped.
    if (tx_state_ == TxState::Draining || faulted_) {
        started_ = false;
        return;
    }
    transport_.close();
    started_ = false;
    configured_ = false;
    tx_state_ = TxState::Idle;
}

bool LorahamBackend::submit_tx(const uint8_t* data, size_t len) {
    if (!ready() || tx_state_ != TxState::Idle) return false;
    if (len == 0 || len > kMaxRfPayload) return false;
    uint8_t frame[kFramedHeaderLen + kMaxRfPayload];
    size_t n = encode_tx_packet(frame, sizeof(frame), data, static_cast<uint16_t>(len));
    if (n == 0) return false;
    if (!transport_.data_send(frame, n)) return false;  // not pending on send failure
    tx_state_ = TxState::Pending;
    pending_epoch_ = ++tx_epoch_;
    return true;
}

void LorahamBackend::abandon_pending_tx() {
    if (tx_state_ != TxState::Pending) return;
    tx_state_ = TxState::Draining;
    drain_deadline_at_ = clock_.now_ms() + drain_timeout_ms_;
    std::fprintf(stderr,
        "[loraham] XR TX deadline reached with daemon result outstanding "
        "-> draining (ownership retained, no TIMEOUT fabricated)\n");
}

void LorahamBackend::resolve_drain(const char* why) {
    tx_state_ = TxState::Idle;
    transport_.close();        // ownership proven clear; release the socket
    configured_ = false;
    parser_reset(parser_);
    std::fprintf(stderr, "[loraham] drain complete: %s; daemon TX ownership cleared\n", why);
}

void LorahamBackend::enter_fault(const char* why) {
    faulted_ = true;
    tx_state_ = TxState::Idle;
    transport_.close();
    configured_ = false;
    parser_reset(parser_);
    std::fprintf(stderr, "[loraham] FAULTED: %s; TX disabled until restart\n", why);
}

void LorahamBackend::dispatch(const Frame& f) {
    if (f.type == FRAMED_RX_PACKET) {
        // Drop RX while draining (no owning session); the sink also drops if its
        // session is not operational.
        if (tx_state_ == TxState::Draining || !sink_) return;
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
        if (tx_state_ == TxState::Pending) {
            tx_state_ = TxState::Idle;
            // Deliver only for the current transaction epoch (XR seq is not used
            // for correlation and may repeat across sessions).
            if (sink_ && pending_epoch_ == tx_epoch_)
                sink_->on_tx_complete(map_tx_status(status));
            return;
        }
        if (tx_state_ == TxState::Draining) {
            resolve_drain("daemon delivered the outstanding TX result");
            return;
        }
        return;  // Idle: stale/late, ignore
    }

    if (f.type == FRAMED_ERROR) {
        if (tx_state_ == TxState::Pending) {
            tx_state_ = TxState::Idle;
            if (sink_) sink_->on_tx_complete(TxOutcome::RadioError);
        } else if (tx_state_ == TxState::Draining) {
            resolve_drain("daemon returned ERROR for the outstanding TX");
        }
        return;
    }
}

void LorahamBackend::handle_disconnect() {
    if (tx_state_ == TxState::Draining) {
        // Daemon source shows a socket close does not cancel the TX, so a link
        // loss while draining cannot prove the packet will not transmit.
        enter_fault("daemon link lost during drain (cannot prove TX clearance)");
        return;
    }
    transport_.close();
    configured_ = false;
    tx_state_ = TxState::Idle;
    parser_reset(parser_);
    std::fprintf(stderr, "[loraham] daemon link lost\n");
    if (sink_) sink_->on_backend_failure();
}

void LorahamBackend::poll() {
    if (!configured_ && tx_state_ != TxState::Draining) return;

    // Bounded drain: if the outstanding result never arrives, fault rather than
    // silently re-enabling TX.
    if (tx_state_ == TxState::Draining && clock_.now_ms() >= drain_deadline_at_) {
        enter_fault("drain timeout: no daemon result for the outstanding TX");
        return;
    }

    transport_.conf_drain();  // keep the daemon's CONF output queue from filling

    for (;;) {
        int r = transport_.data_recv(rbuf_, kReadChunk);
        if (r < 0) { handle_disconnect(); return; }
        if (r == 0) break;  // nothing available now

        // Drain frames from this chunk, popping before pushing more (bounded).
        size_t off = 0;
        for (;;) {
            Frame f;
            PopResult pr = parser_pop(parser_, f);
            if (pr == POP_GOT_FRAME) {
                dispatch(f);
                if (!configured_ && tx_state_ != TxState::Draining) return;  // link torn down
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

}  // namespace mebridge
