// loraham_protocol.h — LoRaHAM daemon v111 local wire definitions (client side).
//
// These mirror the daemon's framed DATA socket protocol and CONF text interface
// as the bridge's own client-side constants. The daemon is NOT modified; this is
// the bridge speaking the daemon's existing protocol.
//
// Reference: LoRaHAM_Daemon framed_data.h / daemon_protocol.h. Framed DATA
// integers are LITTLE-endian (note: the XR wire toward the firmware is
// big-endian — the adapter converts).
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_PROTOCOL_H
#define MEBRIDGE_BACKEND_LORAHAM_PROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace mebridge {
namespace loraham {

// --- Unix socket paths (per band) ------------------------------------------
// The daemon creates only the sockets for the radios it runs; the adapter picks
// the pair matching the configured band.
inline constexpr const char* kDataSocket433  = "/tmp/lora433f.sock";   // framed DATA
inline constexpr const char* kDataSocket868  = "/tmp/lora868f.sock";   // framed DATA
inline constexpr const char* kConfSocket433  = "/tmp/loraconf433.sock";
inline constexpr const char* kConfSocket868  = "/tmp/loraconf868.sock";

// --- Framed DATA socket protocol -------------------------------------------
// Frame: type[1] | length[2 little-endian] | payload[length]
inline constexpr size_t kFramedHeaderLen   = 3;
inline constexpr uint16_t kMaxRfPayload    = 255;

// RX metadata: int16 rssi (centi-dBm) + int16 snr (centi-dB), both LE, then RF.
inline constexpr size_t kRxMetaLen         = 4;
inline constexpr size_t kRxPayloadMax      = kRxMetaLen + kMaxRfPayload;   // 259
inline constexpr size_t kRxFrameMax        = kFramedHeaderLen + kRxPayloadMax; // 262
inline constexpr int16_t kSignalUnavailable = static_cast<int16_t>(-32768);

// Frame types.
enum FramedType : uint8_t {
    FRAMED_RX_PACKET = 0x01,   // daemon -> client
    FRAMED_TX_PACKET = 0x02,   // client -> daemon
    FRAMED_ERROR     = 0x03,   // daemon -> client (UTF-8 text)
    FRAMED_TX_RESULT = 0x04,   // daemon -> client (when SET TXRESULT=1)
};

// TX_RESULT payload: status[1] | flags[1] | seq[2 LE]. The daemon allocates its
// own seq (not the XR sequence) — the adapter correlates by single-in-flight
// ordering, not by this value.
inline constexpr size_t kTxResultPayloadLen = 4;

enum TxStatus : uint8_t {
    TX_STATUS_OK              = 0,
    TX_STATUS_BUSY            = 1,
    TX_STATUS_CHANNEL_BUSY    = 2,
    TX_STATUS_RADIO_NOT_READY = 3,
    TX_STATUS_RADIO_ERROR     = 4,
    TX_STATUS_INVALID_PACKET  = 5,
    TX_STATUS_INVALID_BAND    = 6,
};

// TX_RESULT flag bits (informational).
inline constexpr uint8_t TX_FLAG_MANAGED     = 0x01;
inline constexpr uint8_t TX_FLAG_DEFERRED    = 0x02;
inline constexpr uint8_t TX_FLAG_CAD_TIMEOUT = 0x04;

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_PROTOCOL_H
