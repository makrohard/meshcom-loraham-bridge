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

class DaemonTransport {
public:
    virtual ~DaemonTransport() = default;

    // Open the framed DATA and CONF sockets for the given band. Replaces any
    // existing connection. Returns true only if both are connected.
    virtual bool connect(Band band) = 0;

    // Send bytes on the CONF socket (all-or-fail).
    virtual bool conf_send(const uint8_t* data, size_t len) = 0;
    // Read one newline-terminated line from CONF within timeout_ms (newline
    // stripped). Returns false on timeout/closed/error.
    virtual bool conf_read_line(std::string& out, uint32_t timeout_ms) = 0;
    // Discard any pending CONF input (the daemon broadcasts TX=1/0 etc.); keeps
    // the daemon's per-client output queue from filling. Non-blocking.
    virtual void conf_drain() = 0;

    // Send bytes on the framed DATA socket (all-or-fail).
    virtual bool data_send(const uint8_t* data, size_t len) = 0;
    // Non-blocking read from the framed DATA socket. Returns bytes read (0 if
    // nothing available now) or a negative value on close/error.
    virtual int data_recv(uint8_t* buf, int cap) = 0;

    // Close both sockets (idempotent).
    virtual void close() = 0;
};

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_TRANSPORT_H
