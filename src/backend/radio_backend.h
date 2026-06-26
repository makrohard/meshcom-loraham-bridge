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
//
// What `applied == true` means is backend-defined CONTROL-PLANE acceptance, not
// physical proof:
//   * FakeBackend returns the exact effective config it accepted (deterministic).
//   * The LoRaHAM backend means: the request passed bridge validation, was
//     submitted through the daemon's existing CONF interface, and the daemon then
//     reported the radio ready. It is NOT a hardware-register read-back and NOT
//     on-air/RF confirmation (daemon v111 exposes neither).
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
    // Terminal result of an asynchronous begin_configure(). Delivered from poll(),
    // NEVER synchronously from begin_configure(). `op_token` echoes the token that
    // begin_configure() returned: a session must ignore a completion whose token
    // does not match its own in-flight configuration (it belongs to an earlier or
    // a different session). See begin_configure().
    virtual void on_configure_complete(uint32_t op_token, const ConfigureResult& res) = 0;
};

class RadioBackend {
public:
    virtual ~RadioBackend() = default;

    // Where asynchronous events are delivered. Must be set before start().
    virtual void set_sink(BackendSink* sink) = 0;

    // Begin applying a configuration. NON-BLOCKING: it starts a deadline-bounded
    // progression (connect → submit settings → confirm radio ready) that is
    // driven by poll(); the terminal result is delivered later via
    // BackendSink::on_configure_complete(). It MUST NOT call the sink
    // synchronously. Returns a non-zero operation token on a successful start, or
    // 0 if the request is rejected outright (invalid config, or the backend is
    // busy/faulted/draining and cannot start a new configuration). The token
    // fences a stale completion from reaching a later session.
    virtual uint32_t begin_configure(const extradio::RadioConfig& requested) = 0;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool ready() const = 0;

    // Absolute time (clock ms) of this backend's nearest internal deadline
    // (connect/configure/drain), or UINT64_MAX if none is pending. The event loop
    // uses it to shorten its poll timeout so a backend deadline is serviced
    // promptly without busy-waiting.
    virtual uint64_t next_deadline_ms() const = 0;

    // Submit one packet for transmission. Returns false if the backend could not
    // accept it (e.g. not ready, or one already in flight). A true return is NOT
    // RF success — the terminal result arrives later via on_tx_complete().
    virtual bool submit_tx(const uint8_t* data, size_t len) = 0;

    // The XR session is abandoning the in-flight TX because its bridge-side
    // deadline expired before any terminal result arrived. The backend MUST NOT
    // treat the packet as a known success or failure: it may still be queued or
    // transmitting downstream. The backend retains ownership and runs its own
    // recovery so a later result cannot leak to a future session. No terminal
    // TX_RESULT is fabricated. Safe to call when nothing is in flight (no-op).
    virtual void abandon_pending_tx() = 0;

    // Drive backend work and deliver any pending async events to the sink. Called
    // once per event-loop iteration. Non-blocking.
    virtual void poll() = 0;
};

}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_RADIO_BACKEND_H
