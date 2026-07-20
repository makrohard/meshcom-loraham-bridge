// log.cpp — see log.h.
//
// SPDX-License-Identifier: MIT

#include "util/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace mebridge {

void logf(const char* fmt, ...) {
    struct timespec ts;
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    struct tm tm_local;
    ::localtime_r(&ts.tv_sec, &tm_local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);
    std::fprintf(stderr, "%s.%03ld ", stamp, ts.tv_nsec / 1000000L);

    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

}  // namespace mebridge
