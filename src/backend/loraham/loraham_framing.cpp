// loraham_framing.cpp — see loraham_framing.h.
//
// SPDX-License-Identifier: MIT

#include "backend/loraham/loraham_framing.h"

#include <cstring>

namespace mebridge {
namespace loraham {

namespace {
inline void put16le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline uint16_t get16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
}  // namespace

size_t encode_tx_packet(uint8_t* out, size_t cap, const uint8_t* rf, uint16_t rf_len) {
    if (!out || rf_len > kMaxRfPayload) return 0;
    if (rf_len > 0 && !rf) return 0;
    const size_t total = kFramedHeaderLen + rf_len;
    if (cap < total) return 0;
    out[0] = FRAMED_TX_PACKET;
    put16le(out + 1, rf_len);
    if (rf_len) std::memcpy(out + kFramedHeaderLen, rf, rf_len);
    return total;
}

size_t encode_rx_packet(uint8_t* out, size_t cap, int16_t rssi_cdbm,
                        int16_t snr_cdb, const uint8_t* rf, uint16_t rf_len) {
    if (!out || rf_len > kMaxRfPayload) return 0;
    if (rf_len > 0 && !rf) return 0;
    const uint16_t plen = static_cast<uint16_t>(kRxMetaLen + rf_len);
    const size_t total = kFramedHeaderLen + plen;
    if (cap < total) return 0;
    out[0] = FRAMED_RX_PACKET;
    put16le(out + 1, plen);
    put16le(out + kFramedHeaderLen + 0, static_cast<uint16_t>(rssi_cdbm));
    put16le(out + kFramedHeaderLen + 2, static_cast<uint16_t>(snr_cdb));
    if (rf_len) std::memcpy(out + kFramedHeaderLen + kRxMetaLen, rf, rf_len);
    return total;
}

size_t encode_tx_result(uint8_t* out, size_t cap, uint8_t status, uint8_t flags,
                        uint16_t seq) {
    const size_t total = kFramedHeaderLen + kTxResultPayloadLen;
    if (!out || cap < total) return 0;
    out[0] = FRAMED_TX_RESULT;
    put16le(out + 1, static_cast<uint16_t>(kTxResultPayloadLen));
    out[kFramedHeaderLen + 0] = status;
    out[kFramedHeaderLen + 1] = flags;
    put16le(out + kFramedHeaderLen + 2, seq);
    return total;
}

void parser_reset(Parser& p) { p.have = 0; }

bool parser_push(Parser& p, const uint8_t* data, size_t n, size_t& consumed) {
    consumed = 0;
    if (n == 0) return true;
    if (!data) return false;
    const size_t space = sizeof(p.buf) - p.have;
    const size_t take = (n < space) ? n : space;
    if (take) {
        std::memcpy(p.buf + p.have, data, take);
        p.have += take;
        consumed = take;
    }
    return true;
}

PopResult parser_pop(Parser& p, Frame& out) {
    if (p.have < kFramedHeaderLen) return POP_NEED_MORE;

    const uint8_t type = p.buf[0];
    const uint16_t len = get16le(p.buf + 1);

    // Only daemon->client frame types are expected; bound the length to the
    // maximum legal payload and fail closed otherwise.
    if (type != FRAMED_RX_PACKET && type != FRAMED_TX_RESULT &&
        type != FRAMED_ERROR) {
        return POP_ERROR;
    }
    if (len > kRxPayloadMax) return POP_ERROR;

    const size_t total = kFramedHeaderLen + len;
    if (p.have < total) return POP_NEED_MORE;

    out.type = type;
    out.len = len;
    if (len) std::memcpy(out.payload, p.buf + kFramedHeaderLen, len);

    const size_t rest = p.have - total;
    if (rest) std::memmove(p.buf, p.buf + total, rest);
    p.have = rest;
    return POP_GOT_FRAME;
}

bool decode_rx(const Frame& f, int16_t& rssi_cdbm, int16_t& snr_cdb,
               const uint8_t*& rf, uint16_t& rf_len) {
    if (f.type != FRAMED_RX_PACKET) return false;
    if (f.len < kRxMetaLen) return false;
    const uint16_t n = static_cast<uint16_t>(f.len - kRxMetaLen);
    if (n > kMaxRfPayload) return false;
    rssi_cdbm = static_cast<int16_t>(get16le(f.payload + 0));
    snr_cdb = static_cast<int16_t>(get16le(f.payload + 2));
    rf = f.payload + kRxMetaLen;
    rf_len = n;
    return true;
}

bool decode_tx_result(const Frame& f, uint8_t& status, uint8_t& flags, uint16_t& seq) {
    if (f.type != FRAMED_TX_RESULT) return false;
    if (f.len != kTxResultPayloadLen) return false;
    status = f.payload[0];
    flags = f.payload[1];
    seq = get16le(f.payload + 2);
    return true;
}

TxOutcome map_tx_status(uint8_t status) {
    switch (status) {
        case TX_STATUS_OK:           return TxOutcome::Success;
        case TX_STATUS_CHANNEL_BUSY: return TxOutcome::ChannelBusy;
        default:                     return TxOutcome::RadioError;  // BUSY/NOT_READY/ERROR/INVALID_*
    }
}

bool status_line_radio_ready(const std::string& line) {
    const std::string key = "RADIO=";
    size_t pos = line.find(key);
    if (pos == std::string::npos) return false;
    size_t v = pos + key.size();
    size_t end = line.find_first_of(" \t\r\n", v);
    std::string value = line.substr(v, end == std::string::npos ? std::string::npos : end - v);
    return value == "READY";
}

}  // namespace loraham
}  // namespace mebridge
