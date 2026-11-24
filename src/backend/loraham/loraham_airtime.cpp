// loraham_airtime.cpp — see loraham_airtime.h.
//
// SPDX-License-Identifier: MIT

#include "backend/loraham/loraham_airtime.h"

#include <algorithm>
#include <cmath>

namespace mebridge {
namespace loraham {

namespace {
constexpr double kCadProbeOverheadMs = 100.0;  // quiesce + SPI + RX restore per probe
constexpr double kMarginMs = 1000.0;

double symbol_ms(const extradio::RadioConfig& cfg) {
    return std::ldexp(1000.0, cfg.sf) / static_cast<double>(cfg.bw_hz);
}
}  // namespace

double airtime_ms(const extradio::RadioConfig& cfg, size_t len) {
    const double tsym = symbol_ms(cfg);
    const int sf = cfg.sf;
    const int de = cfg.ldro ? 1 : 0;
    const int cr = cfg.cr_denom - 4;  // 4/5 -> 1 .. 4/8 -> 4
    const double num = 8.0 * static_cast<double>(len) - 4.0 * sf + 28.0 + 16.0 * cfg.crc;
    const double den = 4.0 * (sf - 2 * de);
    const double payload_symbols = 8.0 + std::max(std::ceil(num / den) * (cr + 4), 0.0);
    return (cfg.preamble + 4.25) * tsym + payload_symbols * tsym;
}

double managed_wait_ms(const extradio::RadioConfig& cfg, uint32_t cadwait_ms,
                       uint32_t cadpoll_ms, uint32_t cad_symbols) {
    if (cadpoll_ms == 0) return 0.0;  // the daemon runs 0 ticks at interval 0
    const uint32_t ticks = (cadwait_ms + cadpoll_ms - 1u) / cadpoll_ms;
    if (ticks == 0) return 0.0;
    const double probe = cad_symbols * symbol_ms(cfg) + kCadProbeOverheadMs;
    return ticks * probe + (ticks - 1u) * static_cast<double>(cadpoll_ms);
}

uint64_t tx_ceiling_ms(const extradio::RadioConfig& cfg, size_t len, uint32_t cadwait_ms,
                       uint32_t cadpoll_ms, uint32_t cad_symbols) {
    const double ms = managed_wait_ms(cfg, cadwait_ms, cadpoll_ms, cad_symbols) +
                      airtime_ms(cfg, len) + kMarginMs;
    return static_cast<uint64_t>(std::ceil(ms));
}

}  // namespace loraham
}  // namespace mebridge
