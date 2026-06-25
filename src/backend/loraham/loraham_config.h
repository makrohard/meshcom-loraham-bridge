// loraham_config.h — translate an XR RadioConfig into a LoRaHAM daemon CONF
// command, validating it against the daemon v111 LoRa limits first.
//
// The bridge is the XR configuration authority: it validates the requested
// config against the daemon's known-accepted ranges (mirrored here from the
// daemon's config_policy), and only then drives the daemon CONF socket. Because
// daemon v111 CONF `SET` has no per-field acknowledgement, the adapter treats a
// validated+sent config (with the radio reporting ready) as the effective
// configuration and echoes the requested values back to the firmware. This pure
// module performs no I/O and is fully host-testable.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_CONFIG_H
#define MEBRIDGE_BACKEND_LORAHAM_CONFIG_H

#include <cstdint>
#include <string>

#include "external_radio_protocol.h"  // extradio::RadioConfig

namespace mebridge {
namespace loraham {

enum class Band { Band433, Band868 };

// Why a requested config cannot be applied by the daemon. Each maps to a CONFIG
// failure on the XR side (the bridge never reports false success).
enum class ConfigError {
    Ok = 0,
    FrequencyOutOfBand,   // not within a band the daemon serves
    BandwidthUnsupported, // not one of the daemon's discrete LoRa bandwidths
    SpreadingFactor,      // not 7..12
    CodingRate,           // not 5..8
    SyncWord,             // not a single byte (0..255)
    Preamble,             // not 6..65535
    Power,                // not 0..20 dBm
    Crc,                  // not 0/1
    Ldro,                 // not 0/1
};

const char* config_error_name(ConfigError e);

// Map a carrier frequency to the daemon band that serves it. Returns false if no
// band serves it.
bool select_band(uint32_t freq_hz, Band* out);

// Map an XR bandwidth in Hz to the daemon's canonical kHz token (e.g. 125000 ->
// "125", 62500 -> "62.5"). Returns false for an unsupported bandwidth.
bool bandwidth_token(uint32_t bw_hz, std::string* token_out);

// Validate the requested config against the daemon LoRa limits. On success
// returns ConfigError::Ok and (if band_out) the selected band.
ConfigError validate_config(const extradio::RadioConfig& cfg, Band* band_out);

// Build the daemon CONF line that applies the whole config. Example output:
//   SET MODE=LORA FREQ=433.900000 BW=125 SF=12 CR=5 CRC=1 PREAMBLE=8 SYNC=0x12 LDRO=1 POWER=14
// followed by a newline (the daemon is line-framed). Returns false
// (and leaves out unchanged) if the config is invalid — call validate first.
bool build_set_command(const extradio::RadioConfig& cfg, std::string* out);

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_CONFIG_H
