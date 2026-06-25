// loraham_backend.h — RadioBackend backed by the LoRaHAM daemon v111.
//
// Drives one band's framed DATA + CONF sockets through an injected
// DaemonTransport. The daemon is the XR configuration authority's downstream:
// the bridge validates the requested config, applies it via CONF `SET`, confirms
// the radio reports READY, and echoes the requested values as the effective
// config (daemon v111 has no per-field readback — see README). RX/TX flow over
// the framed DATA socket; one TX is in flight at a time; a lost daemon link is a
// backend failure (the XR session then resolves any in-flight TX as uncertain).
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

namespace mebridge {

class LorahamBackend final : public RadioBackend {
public:
    explicit LorahamBackend(loraham::DaemonTransport& transport,
                            uint32_t config_timeout_ms = 2000)
        : transport_(transport), config_timeout_ms_(config_timeout_ms) {}

    void set_sink(BackendSink* sink) override { sink_ = sink; }
    ConfigureResult configure(const extradio::RadioConfig& requested) override;
    void start() override { started_ = true; }
    void stop() override;
    bool ready() const override { return started_ && configured_; }
    bool submit_tx(const uint8_t* data, size_t len) override;
    void poll() override;

    // Introspection for tests.
    bool tx_in_flight() const { return tx_in_flight_; }

private:
    static constexpr int kReadChunk = 512;

    bool send_line(const std::string& s);
    void dispatch(const loraham::Frame& f);
    void handle_disconnect();

    loraham::DaemonTransport& transport_;
    BackendSink* sink_ = nullptr;
    uint32_t config_timeout_ms_;

    bool started_ = false;
    bool configured_ = false;
    bool tx_in_flight_ = false;

    loraham::Parser parser_{};
    uint8_t rbuf_[kReadChunk];
};

}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_BACKEND_H
