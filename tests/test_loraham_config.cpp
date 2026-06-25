// test_loraham_config.cpp — XR RadioConfig -> LoRaHAM daemon CONF translation.
//
// Pure host tests for band selection, range validation against the daemon v111
// LoRa limits, bandwidth tokenization, and exact SET-command formatting.
//
// SPDX-License-Identifier: MIT

#include <string>

#include "backend/loraham/loraham_config.h"
#include "backend/loraham/loraham_protocol.h"
#include "test_helpers.h"

using namespace mebridge::loraham;
using extradio::RadioConfig;
using testutil::default_config;

namespace {

// A valid 868-band MeshCom-style config.
RadioConfig config_868() {
    RadioConfig c;
    c.freq_hz = 869525000u;
    c.bw_hz = 250000u;
    c.sf = 11;
    c.cr_denom = 5;
    c.sync_word = 0x2B;
    c.preamble = 16;
    c.tx_power_dbm = 10;
    c.crc = 1;
    c.ldro = 0;
    return c;
}

void test_validate_default_433() {
    Band band;
    CHECK(validate_config(default_config(), &band) == ConfigError::Ok);
    CHECK(band == Band::Band433);
}

void test_build_default_433_exact() {
    std::string cmd;
    CHECK(build_set_command(default_config(), &cmd));
    CHECK(cmd ==
          "SET MODE=LORA FREQ=433.900000 BW=125 SF=12 CR=5 CRC=1 PREAMBLE=8 "
          "SYNC=0x12 LDRO=1 POWER=14\n");
}

void test_build_868_exact() {
    Band band;
    CHECK(validate_config(config_868(), &band) == ConfigError::Ok);
    CHECK(band == Band::Band868);
    std::string cmd;
    CHECK(build_set_command(config_868(), &cmd));
    CHECK(cmd ==
          "SET MODE=LORA FREQ=869.525000 BW=250 SF=11 CR=5 CRC=1 PREAMBLE=16 "
          "SYNC=0x2B LDRO=0 POWER=10\n");
}

void test_bandwidth_tokens() {
    struct { uint32_t hz; const char* tok; } ok[] = {
        {7800,"7.8"},{10400,"10.4"},{15600,"15.6"},{20800,"20.8"},{31250,"31.25"},
        {41700,"41.7"},{62500,"62.5"},{125000,"125"},{250000,"250"},{500000,"500"},
    };
    for (auto& e : ok) {
        std::string t;
        CHECK(bandwidth_token(e.hz, &t));
        CHECK(t == e.tok);
    }
    std::string t;
    CHECK(!bandwidth_token(100000u, &t));   // not a daemon LoRa bandwidth
    CHECK(!bandwidth_token(0u, &t));
}

void test_reject_bandwidth() {
    RadioConfig c = default_config();
    c.bw_hz = 100000u;
    CHECK(validate_config(c, nullptr) == ConfigError::BandwidthUnsupported);
    std::string cmd;
    CHECK(!build_set_command(c, &cmd));     // build refuses invalid config
}

void test_reject_spreading_factor() {
    RadioConfig c = default_config();
    c.sf = 6;
    CHECK(validate_config(c, nullptr) == ConfigError::SpreadingFactor);
    c.sf = 13;
    CHECK(validate_config(c, nullptr) == ConfigError::SpreadingFactor);
}

void test_reject_coding_rate() {
    RadioConfig c = default_config();
    c.cr_denom = 4;
    CHECK(validate_config(c, nullptr) == ConfigError::CodingRate);
    c.cr_denom = 9;
    CHECK(validate_config(c, nullptr) == ConfigError::CodingRate);
}

void test_reject_sync_word() {
    RadioConfig c = default_config();
    c.sync_word = 0x2DD4;   // two-byte sync: not a single-byte LoRa sync
    CHECK(validate_config(c, nullptr) == ConfigError::SyncWord);
}

void test_reject_preamble() {
    RadioConfig c = default_config();
    c.preamble = 5;          // daemon requires >= 6
    CHECK(validate_config(c, nullptr) == ConfigError::Preamble);
}

void test_reject_power() {
    RadioConfig c = default_config();
    c.tx_power_dbm = -1;
    CHECK(validate_config(c, nullptr) == ConfigError::Power);
    c.tx_power_dbm = 21;
    CHECK(validate_config(c, nullptr) == ConfigError::Power);
}

void test_band_selection_and_out_of_band() {
    Band b;
    CHECK(select_band(433900000u, &b) && b == Band::Band433);
    CHECK(select_band(869525000u, &b) && b == Band::Band868);
    CHECK(!select_band(600000000u, &b));   // gap between bands
    RadioConfig c = default_config();
    c.freq_hz = 600000000u;
    CHECK(validate_config(c, nullptr) == ConfigError::FrequencyOutOfBand);
    std::string cmd;
    CHECK(!build_set_command(c, &cmd));
}

void test_protocol_constants_sane() {
    // Guard the daemon framed-protocol constants the adapter will rely on.
    CHECK(FRAMED_RX_PACKET == 0x01);
    CHECK(FRAMED_TX_PACKET == 0x02);
    CHECK(FRAMED_TX_RESULT == 0x04);
    CHECK(kFramedHeaderLen == 3);
    CHECK(kRxFrameMax == 262);
    CHECK(TX_STATUS_CHANNEL_BUSY == 2);
    CHECK(kSignalUnavailable == (int16_t)-32768);
}

}  // namespace

int main() {
    RUN(test_validate_default_433);
    RUN(test_build_default_433_exact);
    RUN(test_build_868_exact);
    RUN(test_bandwidth_tokens);
    RUN(test_reject_bandwidth);
    RUN(test_reject_spreading_factor);
    RUN(test_reject_coding_rate);
    RUN(test_reject_sync_word);
    RUN(test_reject_preamble);
    RUN(test_reject_power);
    RUN(test_band_selection_and_out_of_band);
    RUN(test_protocol_constants_sane);
    std::fprintf(stderr, "test_loraham_config: all passed\n");
    return 0;
}
