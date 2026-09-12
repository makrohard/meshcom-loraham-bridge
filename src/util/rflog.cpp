// rflog.cpp — see rflog.h.
//
// SPDX-License-Identifier: MIT

#include "util/rflog.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mebridge {
namespace rflog {

namespace {

int g_fd = -1;
std::string g_path;
size_t g_max_bytes = kMaxBytes;

// 255-byte frame: 510 hex + 255 ascii + the fixed fields fit well within this.
constexpr size_t kLineMax = 1024;

void utc_now(char* out, size_t cap) {
    struct timespec ts;
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }
    struct tm tm_utc;
    ::gmtime_r(&ts.tv_sec, &tm_utc);
    char base[32];
    std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    std::snprintf(out, cap, "%s.%03dZ", base, static_cast<int>(ts.tv_nsec / 1000000L));
}

size_t put(char* out, size_t cap, size_t pos, const char* text) {
    int n = std::snprintf(out + pos, pos < cap ? cap - pos : 0, "%s", text);
    return n < 0 ? pos : pos + static_cast<size_t>(n);
}

// hex=<..> ascii="<..>"\n for the payload; bounded, never overruns `out`.
size_t append_payload(char* out, size_t cap, size_t pos, const uint8_t* rf, size_t len) {
    static const char digits[] = "0123456789abcdef";
    pos = put(out, cap, pos, " hex=");
    for (size_t i = 0; i < len && pos + 2 < cap; ++i) {
        out[pos++] = digits[rf[i] >> 4];
        out[pos++] = digits[rf[i] & 0x0f];
    }
    pos = put(out, cap, pos, " ascii=\"");
    for (size_t i = 0; i < len && pos + 1 < cap; ++i) {
        const uint8_t c = rf[i];
        // Printable ASCII only; the quote and the backslash would break the
        // line's own quoting, so they are dots like every other non-printable.
        out[pos++] = (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') ? static_cast<char>(c) : '.';
    }
    pos = put(out, cap, pos, "\"\n");
    if (pos >= cap) {
        // Truncated: still end the record, so the file stays one per line.
        out[cap - 2] = '\n';
        out[cap - 1] = '\0';
        return cap - 1;
    }
    out[pos] = '\0';
    return pos;
}

// --- retention: copy-truncate, same inode ---------------------------------
bool copy_to_previous() {
    const std::string prev = g_path + ".1";
    int pfd = ::open(prev.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (pfd < 0) return false;
    uint8_t chunk[65536];
    off_t off = 0;
    bool ok = true;
    for (;;) {
        ssize_t n = ::pread(g_fd, chunk, sizeof(chunk), off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) break;
        ssize_t done = 0;
        while (done < n) {
            ssize_t w = ::write(pfd, chunk + done, static_cast<size_t>(n - done));
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            done += w;
        }
        if (!ok) break;
        off += n;
    }
    ::close(pfd);
    return ok;
}

void write_line(const char* line, size_t n) {
    struct stat st;
    if (::fstat(g_fd, &st) == 0 && static_cast<size_t>(st.st_size) + n > g_max_bytes) {
        // The previous segment is replaced only when the copy succeeded; a failed
        // copy keeps the live file intact and merely lets it grow past the cap.
        if (copy_to_previous() && ::ftruncate(g_fd, 0) != 0) {
            // Copied but not truncated: the live file keeps growing and the next
            // roll overwrites <path>.1 again. Nothing is lost either way.
        }
    }
    size_t done = 0;
    while (done < n) {
        ssize_t w = ::write(g_fd, line + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;  // a lost line is not worth blocking the event loop
        }
        done += static_cast<size_t>(w);
    }
}

}  // namespace

bool open(const std::string& path, std::string& err) {
    // Absolute only: the controller names the file; this process never resolves
    // a relative path against a directory it did not choose.
    if (path.empty() || path[0] != '/') {
        err = "--rflog-path must be an absolute path";
        return false;
    }
    // O_RDWR, not O_WRONLY: the rollover reads this same descriptor to copy the
    // tail out. O_APPEND still lands every write at the current end.
    int fd = ::open(path.c_str(), O_RDWR | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        err = "cannot open RF log " + path + ": " + std::strerror(errno);
        return false;
    }
    if (g_fd >= 0) ::close(g_fd);
    g_fd = fd;
    g_path = path;
    return true;
}

void close() {
    if (g_fd >= 0) {
        ::close(g_fd);
        g_fd = -1;
    }
    g_path.clear();
    g_max_bytes = kMaxBytes;
}

bool active() { return g_fd >= 0; }

void set_max_bytes(size_t max_bytes) { g_max_bytes = max_bytes; }

size_t format_rx(char* out, size_t cap, const char* utc, int16_t rssi_cdbm,
                 int16_t snr_cdb, const uint8_t* rf, size_t len) {
    if (!out || cap < 8) return 0;
    int n = std::snprintf(out, cap, "%s RX rssi=%.2f snr=%.2f len=%zu",
                          utc ? utc : "-", rssi_cdbm / 100.0, snr_cdb / 100.0, len);
    if (n < 0) return 0;
    size_t pos = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
    return append_payload(out, cap, pos, rf, len);
}

size_t format_tx(char* out, size_t cap, const char* utc, const char* outcome,
                 const uint8_t* rf, size_t len) {
    if (!out || cap < 8) return 0;
    int n = std::snprintf(out, cap, "%s TX rssi=- snr=- len=%zu outcome=%s",
                          utc ? utc : "-", len, outcome ? outcome : "-");
    if (n < 0) return 0;
    size_t pos = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
    return append_payload(out, cap, pos, rf, len);
}

void rx(int16_t rssi_cdbm, int16_t snr_cdb, const uint8_t* rf, size_t len) {
    if (g_fd < 0) return;
    char utc[40];
    char line[kLineMax];
    utc_now(utc, sizeof(utc));
    write_line(line, format_rx(line, sizeof(line), utc, rssi_cdbm, snr_cdb, rf, len));
}

void tx(const char* outcome, const uint8_t* rf, size_t len) {
    if (g_fd < 0) return;
    char utc[40];
    char line[kLineMax];
    utc_now(utc, sizeof(utc));
    write_line(line, format_tx(line, sizeof(line), utc, outcome, rf, len));
}

}  // namespace rflog
}  // namespace mebridge
