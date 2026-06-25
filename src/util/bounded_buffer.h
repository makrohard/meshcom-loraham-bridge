// bounded_buffer.h — fixed-capacity FIFO byte buffer.
//
// Used as the per-client outbound queue. append() fails closed (returns false)
// when the data would exceed the cap, which the session turns into a disconnect
// of a slow/stuck client rather than unbounded memory growth.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_UTIL_BOUNDED_BUFFER_H
#define MEBRIDGE_UTIL_BOUNDED_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mebridge {

class BoundedBuffer {
public:
    explicit BoundedBuffer(size_t cap) : cap_(cap) {}

    size_t capacity() const { return cap_; }
    size_t size() const { return buf_.size() - head_; }
    bool empty() const { return size() == 0; }

    // Append len bytes. Returns false (and appends nothing) if that would exceed
    // the capacity — the caller must treat this as a fatal overflow.
    bool append(const uint8_t* data, size_t len) {
        if (len == 0) return true;
        if (size() + len > cap_) return false;
        compact_if_needed(len);
        buf_.insert(buf_.end(), data, data + len);
        return true;
    }

    // Pointer to the first unsent byte and the number of unsent bytes.
    const uint8_t* data() const { return buf_.data() + head_; }

    // Drop the first n sent bytes from the front.
    void consume(size_t n) {
        if (n >= size()) { clear(); return; }
        head_ += n;
    }

    void clear() { buf_.clear(); head_ = 0; }

private:
    // Reclaim already-consumed front space before growing, so a long-lived buffer
    // does not grow without bound as bytes are appended and consumed.
    void compact_if_needed(size_t incoming) {
        if (head_ == 0) return;
        const size_t live = size();
        if (head_ >= live || head_ + incoming + live > buf_.capacity()) {
            std::vector<uint8_t> next;
            next.reserve(live + incoming);
            next.insert(next.end(), buf_.begin() + head_, buf_.end());
            buf_.swap(next);
            head_ = 0;
        }
    }

    size_t cap_;
    std::vector<uint8_t> buf_;
    size_t head_ = 0;
};

}  // namespace mebridge

#endif  // MEBRIDGE_UTIL_BOUNDED_BUFFER_H
