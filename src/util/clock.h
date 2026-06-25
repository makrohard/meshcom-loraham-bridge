// clock.h — injectable monotonic millisecond clock.
//
// All time-dependent logic (keepalive, handshake/auth/config deadlines, TX
// timeouts) reads time only through this interface so host tests can drive time
// deterministically with ManualClock and never sleep.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_UTIL_CLOCK_H
#define MEBRIDGE_UTIL_CLOCK_H

#include <chrono>
#include <cstdint>

namespace mebridge {

// Monotonic clock in milliseconds. now_ms() must never go backwards.
class Clock {
public:
    virtual ~Clock() = default;
    virtual uint64_t now_ms() const = 0;
};

// Real steady-clock backed implementation for the running bridge.
class SteadyClock final : public Clock {
public:
    uint64_t now_ms() const override {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
};

// Test clock: time only advances when the test explicitly advances it.
class ManualClock final : public Clock {
public:
    explicit ManualClock(uint64_t start_ms = 0) : now_(start_ms) {}
    uint64_t now_ms() const override { return now_; }
    void advance(uint64_t delta_ms) { now_ += delta_ms; }
    void set(uint64_t ms) { now_ = ms; }
private:
    uint64_t now_;
};

}  // namespace mebridge

#endif  // MEBRIDGE_UTIL_CLOCK_H
