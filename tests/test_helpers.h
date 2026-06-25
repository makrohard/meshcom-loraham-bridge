// test_helpers.h — shared utilities for the host tests.
//
// Builds client-side XR frames (the firmware's side) and parses server output
// back into frames, so tests can exercise XrSession purely in memory. Uses the
// vendored codec for framing and the bridge's own HMAC for password tests.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_TESTS_TEST_HELPERS_H
#define MEBRIDGE_TESTS_TEST_HELPERS_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "external_radio_protocol.h"

// --- tiny assertion harness ------------------------------------------------
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s\n  at %s:%d\n", #cond,    \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

#define RUN(test)                                                            \
    do {                                                                     \
        std::fprintf(stderr, "[ run  ] %s\n", #test);                        \
        test();                                                              \
        std::fprintf(stderr, "[  ok  ] %s\n", #test);                        \
    } while (0)

namespace testutil {

using extradio::Frame;
using extradio::RadioConfig;

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}
inline uint16_t get16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline RadioConfig default_config() {
    RadioConfig c;
    c.freq_hz = 433900000u;
    c.bw_hz = 125000u;
    c.sf = 12;
    c.cr_denom = 5;
    c.sync_word = 0x12;
    c.preamble = 8;
    c.tx_power_dbm = 14;
    c.crc = 1;
    c.ldro = 1;
    return c;
}

inline void pack_config(uint8_t* p, const RadioConfig& c) {
    put32(p + 0, c.freq_hz);
    put32(p + 4, c.bw_hz);
    p[8] = c.sf;
    p[9] = c.cr_denom;
    put16(p + 10, c.sync_word);
    put16(p + 12, c.preamble);
    p[14] = static_cast<uint8_t>(c.tx_power_dbm);
    p[15] = c.crc;
    p[16] = c.ldro;
}

// --- client frame builders -------------------------------------------------
inline std::vector<uint8_t> encode_frame(uint8_t type, uint16_t seq,
                                         const uint8_t* payload, uint16_t len) {
    uint8_t buf[extradio::kMaxFrame];
    size_t n = extradio::encode(buf, sizeof(buf), type, seq, payload, len);
    return std::vector<uint8_t>(buf, buf + n);
}

inline std::vector<uint8_t> build_hello() {
    const uint8_t v[1] = { extradio::kVersion };
    return encode_frame(extradio::MSG_HELLO, 0, v, 1);
}
inline std::vector<uint8_t> build_pong() {
    return encode_frame(extradio::MSG_PONG, 0, nullptr, 0);
}
inline std::vector<uint8_t> build_auth_response(const uint8_t hmac32[32]) {
    return encode_frame(extradio::MSG_AUTH_RESPONSE, 0, hmac32, 32);
}
inline std::vector<uint8_t> build_configure(const RadioConfig& c) {
    uint8_t body[extradio::kConfigPayloadSize];
    pack_config(body, c);
    return encode_frame(extradio::MSG_CONFIGURE, 0, body, sizeof(body));
}
inline std::vector<uint8_t> build_tx_request(uint16_t seq, const uint8_t* data,
                                             uint16_t len) {
    return encode_frame(extradio::MSG_TX_REQUEST, seq, data, len);
}

// --- server output parsing -------------------------------------------------
// Parse a flat byte stream into frames (the server is well-formed).
inline std::vector<Frame> parse_frames(const uint8_t* data, size_t n) {
    std::vector<Frame> out;
    extradio::Parser p;
    extradio::parserReset(p);
    size_t off = 0;
    for (;;) {
        Frame f;
        uint8_t err = 0;
        extradio::PopResult r = extradio::parserPop(p, f, err);
        if (r == extradio::POP_GOT_FRAME) { out.push_back(f); continue; }
        if (r == extradio::POP_ERROR) { std::exit(2); }
        if (off >= n) break;
        size_t took = 0;
        extradio::parserPush(p, data + off, n - off, took);
        off += took;
        if (took == 0) break;
    }
    return out;
}

// Drain all output frames from a session and clear its outbox.
template <typename Session>
inline std::vector<Frame> drain(Session& s) {
    std::vector<uint8_t> bytes(s.out_data(), s.out_data() + s.out_size());
    s.out_consume(s.out_size());
    return parse_frames(bytes.data(), bytes.size());
}

// Count frames of a given type.
inline int count_type(const std::vector<Frame>& fs, uint8_t type) {
    int n = 0;
    for (const auto& f : fs) if (f.type == type) ++n;
    return n;
}
inline const Frame* find_type(const std::vector<Frame>& fs, uint8_t type) {
    for (const auto& f : fs) if (f.type == type) return &f;
    return nullptr;
}

}  // namespace testutil

#endif  // MEBRIDGE_TESTS_TEST_HELPERS_H
