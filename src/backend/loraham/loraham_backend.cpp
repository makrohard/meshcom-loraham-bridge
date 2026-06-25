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
    tx_in_flight_ = false;
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

    // Confirm the radio is operational before claiming success. daemon v111 has
    // no per-field readback, so READY is the strongest available confirmation.
    std::string status;
    if (!transport_.conf_read_line(status, config_timeout_ms_) ||
        !status_line_radio_ready(status)) {
        std::fprintf(stderr, "[loraham] daemon radio not ready after configure\n");
        transport_.close();
        return res;
    }

    configured_ = true;
    res.applied = true;
    res.effective = requested;  // bridge is the authority; echo the requested config
    return res;
}

void LorahamBackend::stop() {
    transport_.close();
    started_ = false;
    configured_ = false;
    tx_in_flight_ = false;
}

bool LorahamBackend::submit_tx(const uint8_t* data, size_t len) {
    if (!ready() || tx_in_flight_) return false;
    if (len == 0 || len > kMaxRfPayload) return false;
    uint8_t frame[kFramedHeaderLen + kMaxRfPayload];
    size_t n = encode_tx_packet(frame, sizeof(frame), data, static_cast<uint16_t>(len));
    if (n == 0) return false;
    if (!transport_.data_send(frame, n)) return false;  // not pending on send failure
    tx_in_flight_ = true;
    return true;
}

void LorahamBackend::dispatch(const Frame& f) {
    if (f.type == FRAMED_RX_PACKET) {
        int16_t rssi = 0, snr = 0;
        const uint8_t* rf = nullptr;
        uint16_t rf_len = 0;
        if (!decode_rx(f, rssi, snr, rf, rf_len)) return;  // malformed: drop
        if (!sink_) return;
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
        if (!tx_in_flight_) return;  // stale/late completion: ignore
        tx_in_flight_ = false;
        if (sink_) sink_->on_tx_complete(map_tx_status(status));
        return;
    }

    if (f.type == FRAMED_ERROR) {
        // The daemon rejected a frame. If a TX is in flight, resolve it as a
        // (non-retry-eligible) radio error rather than leaving it stuck.
        if (tx_in_flight_) {
            tx_in_flight_ = false;
            if (sink_) sink_->on_tx_complete(TxOutcome::RadioError);
        }
        return;
    }
}

void LorahamBackend::handle_disconnect() {
    transport_.close();
    configured_ = false;
    tx_in_flight_ = false;
    parser_reset(parser_);
    std::fprintf(stderr, "[loraham] daemon link lost\n");
    if (sink_) sink_->on_backend_failure();
}

void LorahamBackend::poll() {
    if (!configured_) return;  // nothing to do until a config is applied/connected

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
                if (!configured_) return;  // a dispatch path tore the link down
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
