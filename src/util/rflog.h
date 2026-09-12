// rflog.h — RF log: what the radio heard and sent, one persistent line per frame.
//
// The controller names the file (`--rflog on --rflog-path <absolute>`); this
// process derives nothing. Line contract, shared with the other LoRaHAM writers:
//
//   <utc> RX rssi=<dBm> snr=<dB> len=<n> hex=<..> ascii="<..>"
//   <utc> TX rssi=- snr=- len=<n> outcome=<ok|unconfirmed> hex=<..> ascii="<..>"
//
// RSSI/SNR are receive metadata; a TX line carries payload and outcome. The
// payload is the raw RF frame at the daemon boundary (a MeshCom frame is
// binary, so there is no text summary). One writer, one radio, one process.
//
// Retention: copy-truncate at kMaxBytes into <path>.1; the inode never changes,
// so an external truncate (the controller's Clear) is tolerated (O_APPEND).
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_UTIL_RFLOG_H
#define MEBRIDGE_UTIL_RFLOG_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace mebridge {
namespace rflog {

inline constexpr size_t kMaxBytes = 5u * 1024u * 1024u;

// Open for append. Absolute path only; err names the reason on failure.
bool open(const std::string& path, std::string& err);
void close();
bool active();

// No-ops while not active. A lost line is never worth blocking the event loop.
void rx(int16_t rssi_cdbm, int16_t snr_cdb, const uint8_t* rf, size_t len);
void tx(const char* outcome, const uint8_t* rf, size_t len);

// Pure helpers, exposed for the unit test. Return strlen(out); bounded.
size_t format_rx(char* out, size_t cap, const char* utc, int16_t rssi_cdbm,
                 int16_t snr_cdb, const uint8_t* rf, size_t len);
size_t format_tx(char* out, size_t cap, const char* utc, const char* outcome,
                 const uint8_t* rf, size_t len);
void set_max_bytes(size_t max_bytes);  // test helper; close() restores kMaxBytes

}  // namespace rflog
}  // namespace mebridge

#endif  // MEBRIDGE_UTIL_RFLOG_H
