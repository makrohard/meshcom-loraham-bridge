// loraham_framing.h — encode/decode of the LoRaHAM daemon framed DATA protocol
// (little-endian), plus a bounded streaming parser and the TX-status mapping.
//
// Pure: no sockets, no allocation beyond fixed buffers. Mirrors the daemon's
// framed_data.h on the wire; the daemon is unchanged.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_FRAMING_H
#define MEBRIDGE_BACKEND_LORAHAM_FRAMING_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "backend/loraham/loraham_protocol.h"
#include "backend/radio_backend.h"  // mebridge::TxOutcome

namespace mebridge {
namespace loraham {

// --- encoders --------------------------------------------------------------
// Build a framed TX_PACKET (client -> daemon). Returns total bytes, 0 on error.
size_t encode_tx_packet(uint8_t* out, size_t cap, const uint8_t* rf, uint16_t rf_len);
// Build a framed RX_PACKET (used by tests / a fake daemon). 0 on error.
size_t encode_rx_packet(uint8_t* out, size_t cap, int16_t rssi_cdbm,
                        int16_t snr_cdb, const uint8_t* rf, uint16_t rf_len);
// Build a framed TX_RESULT (used by tests / a fake daemon). 0 on error.
size_t encode_tx_result(uint8_t* out, size_t cap, uint8_t status, uint8_t flags,
                        uint16_t seq);

// --- one decoded frame (owns its payload) ----------------------------------
struct Frame {
    uint8_t  type = 0;
    uint16_t len = 0;
    uint8_t  payload[kRxPayloadMax] = {0};
};

// --- bounded streaming parser ----------------------------------------------
struct Parser {
    uint8_t buf[kRxFrameMax];
    size_t  have = 0;
};

enum PopResult { POP_NEED_MORE, POP_GOT_FRAME, POP_ERROR };

void parser_reset(Parser& p);
// Append up to n bytes; *consumed receives how many fit. Returns false only on a
// null/oversize-impossible input (caller bug). Drain with parser_pop between pushes.
bool parser_push(Parser& p, const uint8_t* data, size_t n, size_t& consumed);
// Extract one frame; payload copied into out. POP_ERROR => fail closed (disconnect).
PopResult parser_pop(Parser& p, Frame& out);

// --- payload decoders ------------------------------------------------------
// RX_PACKET -> signed centi rssi/snr (LE on the wire) + RF pointer/length.
bool decode_rx(const Frame& f, int16_t& rssi_cdbm, int16_t& snr_cdb,
               const uint8_t*& rf, uint16_t& rf_len);
// TX_RESULT -> status/flags/seq (daemon-allocated seq; not the XR sequence).
bool decode_tx_result(const Frame& f, uint8_t& status, uint8_t& flags, uint16_t& seq);

// Map a daemon TX status to the bridge's TxOutcome. Per the M12a contract:
// OK->Success, CHANNEL_BUSY->ChannelBusy, everything else (BUSY, RADIO_NOT_READY,
// RADIO_ERROR, INVALID_*) -> RadioError (never retry-eligible). Disconnect/missing
// result is handled out of band via BackendSink::on_backend_failure().
TxOutcome map_tx_status(uint8_t status);

// True if a daemon "GET STATUS" response line reports RADIO=READY.
bool status_line_radio_ready(const std::string& line);

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_FRAMING_H
