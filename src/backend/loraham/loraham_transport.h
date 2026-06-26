// loraham_transport.h — abstraction over the LoRaHAM daemon's two local sockets
// (framed DATA + text CONF) for one band.
//
// Injected so LorahamBackend is unit-testable with an in-memory fake daemon (no
// sockets, no threads). The production implementation is PosixDaemonTransport.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_TRANSPORT_H
#define MEBRIDGE_BACKEND_LORAHAM_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "backend/loraham/loraham_config.h"  // Band

namespace mebridge {
namespace loraham {

// Progress of an in-flight non-blocking connect of the DATA + CONF sockets.
enum class ConnectState { Connecting, Connected, Failed };

// Fully NON-BLOCKING two-socket transport. No call may wait in a blocking
// syscall for daemon connect, writability, or input — every operation either
// makes immediate progress or reports "not yet". The bridge's single event loop
// drives progress by calling these from its per-iteration backend poll().
class DaemonTransport {
public:
    virtual ~DaemonTransport() = default;

    // Begin connecting the framed DATA and CONF sockets for the given band.
    // Replaces any existing connection. Non-blocking: returns false only on an
    // immediate hard failure (e.g. socket() or bad path); on true, progress is
    // observed via poll_connect(). The sockets are non-blocking from creation.
    virtual bool begin_connect(Band band) = 0;
    // Zero-timeout check of connect progress (getsockopt(SO_ERROR) after writable
    // readiness). Never blocks. Once Connected, send/recv may be used.
    virtual ConnectState poll_connect() = 0;

    // Non-blocking partial send. Returns bytes accepted (>= 0; 0 means the kernel
    // buffer is momentarily full — retry later) or -1 on a fatal error/close.
    virtual int conf_send_some(const uint8_t* data, size_t len) = 0;
    virtual int data_send_some(const uint8_t* data, size_t len) = 0;

    // Non-blocking read. Returns bytes read (> 0), 0 if nothing is available now,
    // or a negative value on close/error.
    virtual int conf_recv(uint8_t* buf, int cap) = 0;
    virtual int data_recv(uint8_t* buf, int cap) = 0;

    // Close both sockets (idempotent).
    virtual void close() = 0;
};

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_TRANSPORT_H
