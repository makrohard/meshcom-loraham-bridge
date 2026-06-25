// test_loraham_framing.cpp — LoRaHAM daemon framed-protocol codec tests.
//
// SPDX-License-Identifier: MIT

#include <cstring>
#include <vector>

#include "backend/loraham/loraham_framing.h"
#include "test_helpers.h"

using namespace mebridge::loraham;

namespace {

// Run a byte stream through the parser, collecting frames.
std::vector<Frame> run_parser(const std::vector<uint8_t>& bytes, size_t chunk) {
    std::vector<Frame> out;
    Parser p;
    parser_reset(p);
    size_t off = 0;
    for (;;) {
        Frame f;
        PopResult pr = parser_pop(p, f);
        if (pr == POP_GOT_FRAME) { out.push_back(f); continue; }
        if (pr == POP_ERROR) { std::exit(3); }
        if (off >= bytes.size()) break;
        size_t want = bytes.size() - off;
        if (want > chunk) want = chunk;
        size_t took = 0;
        CHECK(parser_push(p, bytes.data() + off, want, took));
        off += took;
        if (took == 0) break;
    }
    return out;
}

void test_tx_packet_encoding() {
    const uint8_t rf[3] = {0xAA, 0xBB, 0xCC};
    uint8_t buf[16];
    size_t n = encode_tx_packet(buf, sizeof(buf), rf, 3);
    CHECK(n == kFramedHeaderLen + 3);
    CHECK(buf[0] == FRAMED_TX_PACKET);
    CHECK(buf[1] == 3 && buf[2] == 0);          // length little-endian
    CHECK(std::memcmp(buf + 3, rf, 3) == 0);
    CHECK(encode_tx_packet(buf, sizeof(buf), rf, 256) == 0);  // oversize rejected
}

void test_rx_roundtrip() {
    const uint8_t rf[5] = {'h', 'e', 'l', 'l', 'o'};
    uint8_t buf[64];
    size_t n = encode_rx_packet(buf, sizeof(buf), -12050, -275, rf, 5);
    CHECK(n == kFramedHeaderLen + kRxMetaLen + 5);

    auto frames = run_parser(std::vector<uint8_t>(buf, buf + n), 64);
    CHECK(frames.size() == 1);
    int16_t rssi = 0, snr = 0;
    const uint8_t* p = nullptr;
    uint16_t len = 0;
    CHECK(decode_rx(frames[0], rssi, snr, p, len));
    CHECK(rssi == -12050);
    CHECK(snr == -275);
    CHECK(len == 5);
    CHECK(std::memcmp(p, rf, 5) == 0);
}

void test_tx_result_roundtrip() {
    uint8_t buf[16];
    size_t n = encode_tx_result(buf, sizeof(buf), TX_STATUS_CHANNEL_BUSY,
                                TX_FLAG_MANAGED, 0x1234);
    CHECK(n == kFramedHeaderLen + kTxResultPayloadLen);
    auto frames = run_parser(std::vector<uint8_t>(buf, buf + n), 64);
    CHECK(frames.size() == 1);
    uint8_t status = 0, flags = 0;
    uint16_t seq = 0;
    CHECK(decode_tx_result(frames[0], status, flags, seq));
    CHECK(status == TX_STATUS_CHANNEL_BUSY);
    CHECK(flags == TX_FLAG_MANAGED);
    CHECK(seq == 0x1234);                          // little-endian
}

void test_fragmented_and_coalesced() {
    const uint8_t rf[2] = {1, 2};
    uint8_t a[64], b[16];
    size_t na = encode_rx_packet(a, sizeof(a), -100, 50, rf, 2);
    size_t nb = encode_tx_result(b, sizeof(b), TX_STATUS_OK, 0, 7);
    std::vector<uint8_t> both(a, a + na);
    both.insert(both.end(), b, b + nb);

    // byte-at-a-time (fragmented)
    CHECK(run_parser(both, 1).size() == 2);
    // single push (coalesced)
    CHECK(run_parser(both, both.size()).size() == 2);
}

void test_parser_fail_closed() {
    // Unknown frame type -> fail closed.
    {
        Parser p; parser_reset(p);
        uint8_t bad[3] = {0x09, 0x00, 0x00};
        size_t took = 0;
        CHECK(parser_push(p, bad, 3, took));
        Frame f;
        CHECK(parser_pop(p, f) == POP_ERROR);
    }
    // Oversize length -> fail closed.
    {
        Parser p; parser_reset(p);
        uint8_t hdr[3] = {FRAMED_RX_PACKET, 0xFF, 0xFF};  // len 65535 > max payload
        size_t took = 0;
        CHECK(parser_push(p, hdr, 3, took));
        Frame f;
        CHECK(parser_pop(p, f) == POP_ERROR);
    }
}

void test_tx_status_mapping() {
    using mebridge::TxOutcome;
    CHECK(map_tx_status(TX_STATUS_OK) == TxOutcome::Success);
    CHECK(map_tx_status(TX_STATUS_CHANNEL_BUSY) == TxOutcome::ChannelBusy);
    CHECK(map_tx_status(TX_STATUS_BUSY) == TxOutcome::RadioError);
    CHECK(map_tx_status(TX_STATUS_RADIO_NOT_READY) == TxOutcome::RadioError);
    CHECK(map_tx_status(TX_STATUS_RADIO_ERROR) == TxOutcome::RadioError);
    CHECK(map_tx_status(TX_STATUS_INVALID_PACKET) == TxOutcome::RadioError);
    CHECK(map_tx_status(TX_STATUS_INVALID_BAND) == TxOutcome::RadioError);
}

void test_status_line_ready() {
    CHECK(status_line_radio_ready("STATUS RADIO=READY TX=0 CAD=0"));
    CHECK(status_line_radio_ready("STATUS RADIO=READY"));
    CHECK(!status_line_radio_ready("STATUS RADIO=FAILED TX=0"));
    CHECK(!status_line_radio_ready("STATUS RADIO=READYX TX=0"));  // value must be exact
    CHECK(!status_line_radio_ready("STATUS TX=0"));
}

}  // namespace

int main() {
    RUN(test_tx_packet_encoding);
    RUN(test_rx_roundtrip);
    RUN(test_tx_result_roundtrip);
    RUN(test_fragmented_and_coalesced);
    RUN(test_parser_fail_closed);
    RUN(test_tx_status_mapping);
    RUN(test_status_line_ready);
    std::fprintf(stderr, "test_loraham_framing: all passed\n");
    return 0;
}
