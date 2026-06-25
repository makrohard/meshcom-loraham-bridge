// loraham_config.cpp — see loraham_config.h.
//
// Ranges mirror LoRaHAM_Daemon config_policy.cpp (v111): SF 7..12, CR 5..8,
// PREAMBLE 6..65535, SYNC <=0xFF, POWER 0..20, and the discrete LoRa bandwidth
// set. Band ranges cover the daemon's SX1278 (433) and RFM95/SX1276 (868) radios.
//
// SPDX-License-Identifier: MIT

#include "backend/loraham/loraham_config.h"

#include <cstdio>

namespace mebridge {
namespace loraham {

namespace {

// Daemon-served sub-GHz bands (Hz). Frequencies outside both are rejected rather
// than guessed.
constexpr uint32_t k433Lo = 137000000u;
constexpr uint32_t k433Hi = 525000000u;
constexpr uint32_t k868Lo = 779000000u;
constexpr uint32_t k868Hi = 960000000u;

struct BwMap { uint32_t hz; const char* token; };

// The daemon's allowed LoRa bandwidths, kHz tokens chosen to round-trip exactly
// through the firmware's bw_khz<->bw_hz normalization and the daemon's float
// parse + 0.0001 tolerance compare.
constexpr BwMap kBwTable[] = {
    {   7800u, "7.8"   },
    {  10400u, "10.4"  },
    {  15600u, "15.6"  },
    {  20800u, "20.8"  },
    {  31250u, "31.25" },
    {  41700u, "41.7"  },
    {  62500u, "62.5"  },
    { 125000u, "125"   },
    { 250000u, "250"   },
    { 500000u, "500"   },
};

}  // namespace

const char* config_error_name(ConfigError e) {
    switch (e) {
        case ConfigError::Ok:                  return "OK";
        case ConfigError::FrequencyOutOfBand:  return "FREQUENCY_OUT_OF_BAND";
        case ConfigError::BandwidthUnsupported:return "BANDWIDTH_UNSUPPORTED";
        case ConfigError::SpreadingFactor:     return "SPREADING_FACTOR";
        case ConfigError::CodingRate:          return "CODING_RATE";
        case ConfigError::SyncWord:            return "SYNC_WORD";
        case ConfigError::Preamble:            return "PREAMBLE";
        case ConfigError::Power:               return "POWER";
        case ConfigError::Crc:                 return "CRC";
        case ConfigError::Ldro:                return "LDRO";
    }
    return "OK";
}

bool select_band(uint32_t freq_hz, Band* out) {
    if (freq_hz >= k433Lo && freq_hz <= k433Hi) { if (out) *out = Band::Band433; return true; }
    if (freq_hz >= k868Lo && freq_hz <= k868Hi) { if (out) *out = Band::Band868; return true; }
    return false;
}

bool bandwidth_token(uint32_t bw_hz, std::string* token_out) {
    for (const BwMap& m : kBwTable) {
        if (m.hz == bw_hz) { if (token_out) *token_out = m.token; return true; }
    }
    return false;
}

ConfigError validate_config(const extradio::RadioConfig& cfg, Band* band_out) {
    Band band;
    if (!select_band(cfg.freq_hz, &band))               return ConfigError::FrequencyOutOfBand;
    std::string bw;
    if (!bandwidth_token(cfg.bw_hz, &bw))               return ConfigError::BandwidthUnsupported;
    if (cfg.sf < 7 || cfg.sf > 12)                      return ConfigError::SpreadingFactor;
    if (cfg.cr_denom < 5 || cfg.cr_denom > 8)           return ConfigError::CodingRate;
    if (cfg.sync_word > 0xFF)                           return ConfigError::SyncWord;
    if (cfg.preamble < 6)                               return ConfigError::Preamble; // u16: <=65535 always
    if (cfg.tx_power_dbm < 0 || cfg.tx_power_dbm > 20)  return ConfigError::Power;
    if (cfg.crc > 1)                                    return ConfigError::Crc;
    if (cfg.ldro > 1)                                   return ConfigError::Ldro;
    if (band_out) *band_out = band;
    return ConfigError::Ok;
}

bool build_set_command(const extradio::RadioConfig& cfg, std::string* out) {
    if (!out) return false;
    if (validate_config(cfg, nullptr) != ConfigError::Ok) return false;

    std::string bw;
    bandwidth_token(cfg.bw_hz, &bw);  // validated above

    char buf[256];
    const double freq_mhz = static_cast<double>(cfg.freq_hz) / 1000000.0;
    int n = std::snprintf(
        buf, sizeof(buf),
        "SET MODE=LORA FREQ=%.6f BW=%s SF=%u CR=%u CRC=%u PREAMBLE=%u "
        "SYNC=0x%02X LDRO=%u POWER=%d\n",
        freq_mhz,
        bw.c_str(),
        static_cast<unsigned>(cfg.sf),
        static_cast<unsigned>(cfg.cr_denom),
        static_cast<unsigned>(cfg.crc),
        static_cast<unsigned>(cfg.preamble),
        static_cast<unsigned>(cfg.sync_word) & 0xFFu,
        static_cast<unsigned>(cfg.ldro),
        static_cast<int>(cfg.tx_power_dbm));
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) return false;
    out->assign(buf, static_cast<size_t>(n));
    return true;
}

}  // namespace loraham
}  // namespace mebridge
