// log.h — timestamped stderr logging.
//
// logf() prefixes each line with local wall-clock time "YYYY-MM-DDTHH:MM:SS.mmm "
// and writes to stderr. It never appends a newline: call sites keep their own
// trailing "\n" (exactly like the fprintf sites they replace). Dependency-free;
// not async-signal-safe (only called from normal code paths).
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_UTIL_LOG_H
#define MEBRIDGE_UTIL_LOG_H

namespace mebridge {

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace mebridge

#endif  // MEBRIDGE_UTIL_LOG_H
