// loraham_airtime.h — worst-case time from a TX submit to the daemon's TX
// result, used as the bridge-side TX ceiling.
//
// The daemon sends in TXMODE=MANAGED: before the TX it may wait for a free
// channel for up to ceil(CADWAIT/CADPOLL) ticks. Each tick is one CAD probe
// (CADSYM symbols plus about 100 ms of radio quiesce/SPI/RX restore), and a
// CADPOLL sleep follows every tick but the last. Then the frame goes on air
// (Semtech LoRa time-on-air formula, explicit header). The ceiling is that sum
// plus 1 s of margin. This pure module performs no I/O.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_BACKEND_LORAHAM_AIRTIME_H
#define MEBRIDGE_BACKEND_LORAHAM_AIRTIME_H

#include <cstddef>
#include <cstdint>

#include "external_radio_protocol.h"  // extradio::RadioConfig

namespace mebridge {
namespace loraham {

// Daemon defaults (daemon_tx_policy.h) for a STATUS reply that does not name them.
constexpr uint32_t kDefaultCadWaitMs = 1500;
constexpr uint32_t kDefaultCadPollMs = 50;
constexpr uint32_t kDefaultCadSymbols = 4;

// Time on air of a `len`-byte frame with the config the bridge sent, in ms.
double airtime_ms(const extradio::RadioConfig& cfg, size_t len);

// Worst-case MANAGED channel wait before the TX starts, in ms.
double managed_wait_ms(const extradio::RadioConfig& cfg, uint32_t cadwait_ms,
                       uint32_t cadpoll_ms, uint32_t cad_symbols);

// managed_wait_ms + airtime_ms + 1000, rounded up to whole ms.
uint64_t tx_ceiling_ms(const extradio::RadioConfig& cfg, size_t len, uint32_t cadwait_ms,
                       uint32_t cadpoll_ms, uint32_t cad_symbols);

}  // namespace loraham
}  // namespace mebridge

#endif  // MEBRIDGE_BACKEND_LORAHAM_AIRTIME_H
