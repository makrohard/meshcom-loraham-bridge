// fake_daemon.h — in-memory, fully NON-BLOCKING DaemonTransport for host tests.
//
// Scripts connect progress, CONF status replies, partial writes, and fragmented
// reads; captures what the backend sends. No sockets, no threads. Shared by the
// backend unit tests and the server/connection lifecycle tests.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_TESTS_FAKE_DAEMON_H
#define MEBRIDGE_TESTS_FAKE_DAEMON_H

#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "backend/loraham/loraham_transport.h"

namespace testutil {

class FakeDaemon : public mebridge::loraham::DaemonTransport {
public:
    using Band = mebridge::loraham::Band;
    using ConnectState = mebridge::loraham::ConnectState;

    // --- connect scripting ---
    bool begin_connect_result = true;
    bool connect_stuck = false;                  // poll_connect always Connecting
    std::deque<ConnectState> connect_script;     // else popped per poll_connect

    // --- write scripting ---
    bool throttle_writes = false;                // one chunk per call, then EAGAIN
    size_t throttle_chunk = 4;
    bool data_send_fail = false;                 // data_send_some returns -1

    // --- CONF reply scripting ---
    std::string status_line = "STATUS RADIO=READY TX=0 CAD=0 TXMODE=MANAGED";
    std::string status_prefix;                   // broadcasts emitted before STATUS
    bool auto_status = true;                      // queue status after GET STATUS seen
    size_t conf_recv_chunk = 0;                  // 0 = serve all available
    bool conf_recv_disconnect = false;

    // --- DATA scripting ---
    bool recv_disconnect = false;
    size_t data_recv_chunk = 0;                  // 0 = serve all available

    // --- observed ---
    bool connected = false;
    Band band = Band::Band433;
    std::string conf_sent;
    std::vector<uint8_t> data_sent;
    std::vector<uint8_t> data_in;                // scripted inbound DATA
    std::string conf_in;                         // scripted inbound CONF

    void push_data(const uint8_t* p, size_t n) { data_in.insert(data_in.end(), p, p + n); }
    void deliver_status() { conf_in += status_prefix; conf_in += status_line; conf_in += "\n"; }

    bool begin_connect(Band b) override {
        if (!begin_connect_result) return false;
        connected = true;
        band = b;
        status_queued_ = false;
        conf_in.clear();
        conf_blocked_ = data_blocked_ = false;
        return true;
    }
    ConnectState poll_connect() override {
        if (connect_stuck) return ConnectState::Connecting;
        if (!connect_script.empty()) {
            ConnectState s = connect_script.front();
            connect_script.pop_front();
            return s;
        }
        return ConnectState::Connected;
    }
    int conf_send_some(const uint8_t* d, size_t len) override {
        size_t n = take_write(len, &conf_blocked_);
        if (n) conf_sent.append(reinterpret_cast<const char*>(d), n);
        maybe_queue_status();
        return static_cast<int>(n);
    }
    int data_send_some(const uint8_t* d, size_t len) override {
        if (data_send_fail) return -1;
        size_t n = take_write(len, &data_blocked_);
        if (n) data_sent.insert(data_sent.end(), d, d + n);
        return static_cast<int>(n);
    }
    int conf_recv(uint8_t* buf, int cap) override {
        if (conf_recv_disconnect) return -1;
        if (conf_in.empty()) return 0;
        size_t n = conf_in.size();
        if (conf_recv_chunk && n > conf_recv_chunk) n = conf_recv_chunk;
        if (n > static_cast<size_t>(cap)) n = static_cast<size_t>(cap);
        std::memcpy(buf, conf_in.data(), n);
        conf_in.erase(0, n);
        return static_cast<int>(n);
    }
    int data_recv(uint8_t* buf, int cap) override {
        if (recv_disconnect) return -1;
        if (data_in.empty()) return 0;
        size_t n = data_in.size();
        if (data_recv_chunk && n > data_recv_chunk) n = data_recv_chunk;
        if (n > static_cast<size_t>(cap)) n = static_cast<size_t>(cap);
        std::memcpy(buf, data_in.data(), n);
        data_in.erase(data_in.begin(), data_in.begin() + n);
        return static_cast<int>(n);
    }
    void close() override { connected = false; }

private:
    bool status_queued_ = false;
    bool conf_blocked_ = false;
    bool data_blocked_ = false;

    size_t take_write(size_t len, bool* blocked) {
        if (!throttle_writes) return len;
        if (*blocked) { *blocked = false; return 0; }  // EAGAIN this call
        *blocked = true;
        return len < throttle_chunk ? len : throttle_chunk;
    }
    void maybe_queue_status() {
        if (auto_status && !status_queued_ &&
            conf_sent.find("GET STATUS\n") != std::string::npos) {
            status_queued_ = true;
            deliver_status();
        }
    }
};

}  // namespace testutil

#endif  // MEBRIDGE_TESTS_FAKE_DAEMON_H
