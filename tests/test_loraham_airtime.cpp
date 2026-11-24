// test_loraham_airtime.cpp — TX ceiling = MANAGED channel wait + airtime + 1 s.
//
// Expected values are worked by hand from the Semtech time-on-air formula and
// the daemon's MANAGED tick formula (ticks = ceil(CADWAIT/CADPOLL)).
//
// SPDX-License-Identifier: MIT

#include <cmath>

#include "backend/loraham/loraham_airtime.h"
#include "test_helpers.h"

using namespace mebridge::loraham;
using extradio::RadioConfig;

namespace {

RadioConfig lora(uint8_t sf, uint32_t bw_hz, uint8_t ldro) {
    RadioConfig c = testutil::default_config();
    c.sf = sf;
    c.bw_hz = bw_hz;
    c.cr_denom = 6;
    c.preamble = 8;
    c.crc = 1;
    c.ldro = ldro;
    return c;
}

bool near(double got, double want, double tol) { return std::fabs(got - want) <= tol; }

// SF12/BW125, CR 4/6, LDRO 1, 100 B, daemon defaults:
// wait 30 x (4 x 32.768 + 100) + 29 x 50 = 8382.2 ms; airtime 12.25 x 32.768 +
// (8 + ceil(796 / 40) x 6) x 32.768 = 4595.7 ms; + 1000.
void test_sf12_bw125_100b_defaults() {
    RadioConfig c = lora(12, 125000u, 1);
    CHECK(near(managed_wait_ms(c, 1500, 50, 4), 8382.16, 0.01));
    CHECK(near(airtime_ms(c, 100), 4595.71, 0.01));
    CHECK(tx_ceiling_ms(c, 100, 1500, 50, 4) == 13978u);
}

// The default MeshCom config SF11/BW250 at the max frame: 5433.0 + 2476.1 + 1000.
// It stays under the session's 10 s floor.
void test_sf11_bw250_255b_under_floor() {
    RadioConfig c = lora(11, 250000u, 0);
    CHECK(near(airtime_ms(c, 255), 2476.03, 0.01));
    CHECK(tx_ceiling_ms(c, 255, 1500, 50, 4) == 8910u);
}

// SF12/BW125 at the max frame: the airtime alone is above 10 s.
void test_sf12_bw125_255b_airtime_above_10s() {
    RadioConfig c = lora(12, 125000u, 1);
    CHECK(airtime_ms(c, 255) > 10000.0);
    CHECK(near(airtime_ms(c, 255), 10691.0, 1.0));
}

// LDRO changes the payload symbol count (SF - 2 x DE in the divisor).
void test_ldro_counts() {
    CHECK(near(airtime_ms(lora(12, 125000u, 0), 100), 4005.89, 0.01));
    CHECK(near(airtime_ms(lora(12, 125000u, 1), 100), 4595.71, 0.01));
}

// CADSYM 8 instead of 4 adds 30 x 4 symbols = 3932.16 ms at SF12/BW125.
void test_cad_symbols() {
    RadioConfig c = lora(12, 125000u, 1);
    CHECK(near(managed_wait_ms(c, 1500, 50, 8) - managed_wait_ms(c, 1500, 50, 4), 3932.16, 0.01));
}

// ticks round up: CADWAIT 1501 / 50 is 31 ticks, one probe + one poll more.
void test_ticks_round_up() {
    RadioConfig c = lora(12, 125000u, 1);
    CHECK(near(managed_wait_ms(c, 1501, 50, 4) - managed_wait_ms(c, 1500, 50, 4),
               4 * 32.768 + 100 + 50, 0.01));
    CHECK(near(managed_wait_ms(c, 50, 50, 4), 4 * 32.768 + 100, 0.01));  // 1 tick, no sleep
}

// Interval 0 or wait 0: no ticks, no division by zero.
void test_zero_interval_and_wait() {
    RadioConfig c = lora(12, 125000u, 1);
    CHECK(managed_wait_ms(c, 1500, 0, 4) == 0.0);
    CHECK(managed_wait_ms(c, 0, 50, 4) == 0.0);
    CHECK(tx_ceiling_ms(c, 100, 1500, 0, 4) == static_cast<uint64_t>(std::ceil(4595.71 + 1000)));
}

}  // namespace

int main() {
    RUN(test_sf12_bw125_100b_defaults);
    RUN(test_sf11_bw250_255b_under_floor);
    RUN(test_sf12_bw125_255b_airtime_above_10s);
    RUN(test_ldro_counts);
    RUN(test_cad_symbols);
    RUN(test_ticks_round_up);
    RUN(test_zero_interval_and_wait);
    std::fprintf(stderr, "test_loraham_airtime: all passed\n");
    return 0;
}
