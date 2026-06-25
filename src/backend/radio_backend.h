// radio_backend.h — generic radio backend abstraction.
//
// The XR session never talks to a radio (or a daemon) directly: it drives a
// RadioBackend and receives asynchronous RX / TX-completion / failure events
// through a BackendSink. M11b ships exactly one backend — FakeBackend — used by
// the host tests. A real LoRaHAM-daemon adapter is a future, separate module in
// this same process (see README); it is deliberately NOT implemented here.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_RADIO_BACKEND_H
#define MEBRIDGE_BACKEND_RADIO_BACKEND_H

#include <cstddef>
#include <cstdint>

#include "external_radio_protocol.h"  // extradio::RadioConfig, kMaxLoraPayload

namespace mebridge {

// Terminal outcome of a single submitted TX. Mirrors the XR wire codes but is
// backend-facing: the backend reports one of these; the session maps it to the
// XR TX_RESULT code. There is intentionally no "unknown/uncertain" value here —
// uncertainty is expressed by on_backend_failure(), which closes the session so
// the firmware resolves the in-flight TX as UNKNOWN on its side.
enum class TxOutcome : uint8_t {
    Success = 0,
    ChannelBusy,
    Timeout,
    RadioError,
};

// One received RF packet handed up from the backend. RSSI/SNR are already in the
// signed centi-units the XR wire uses (centi-dBm / centi-dB), so no scaling is
// required — only byte-order conversion to big-endian on the wire.
struct RxEvent {
    int16_t rssi_cdbm = 0;
    int16_t snr_cdb = 0;
    uint16_t len = 0;
    uint8_t data[extradio::kMaxLoraPayload] = {0};
};

// Result of a configure() request. A CONFIG_RESULT success is sent to the
// firmware ONLY when applied == true AND effective exactly equals the requested
// config. Anything else is a configuration failure (fail closed). The bridge
// never reports success merely because it forwarded a configuration downstream.
struct ConfigureResult {
    bool applied = false;
    extradio::RadioConfig effective{};
};

// Asynchronous events delivered by the backend during poll(). The session
// implements this; the backend holds a pointer to it.
class BackendSink {
public:
    virtual ~BackendSink() = default;
    // One validated received packet, in arrival order.
    virtual void on_rx(const RxEvent& rx) = 0;
    // Terminal result of the single in-flight TX.
    virtual void on_tx_complete(TxOutcome outcome) = 0;
    // The backend link failed/closed. The bridge treats this as fatal for the
    // current client session (any in-flight TX becomes uncertain on the firmware).
    virtual void on_backend_failure() = 0;
};

class RadioBackend {
public:
    virtual ~RadioBackend() = default;

    // Where asynchronous events are delivered. Must be set before start().
    virtual void set_sink(BackendSink* sink) = 0;

    // Apply a configuration. Synchronous and authoritative: the returned
    // ConfigureResult decides whether the firmware reaches READY.
    virtual ConfigureResult configure(const extradio::RadioConfig& requested) = 0;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool ready() const = 0;

    // Submit one packet for transmission. Returns false if the backend could not
    // accept it (e.g. not ready, or one already in flight). A true return is NOT
    // RF success — the terminal result arrives later via on_tx_complete().
    virtual bool submit_tx(const uint8_t* data, size_t len) = 0;

    // Drive backend work and deliver any pending async events to the sink. Called
    // once per event-loop iteration. Non-blocking.
    virtual void poll() = 0;
};

}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_RADIO_BACKEND_H
