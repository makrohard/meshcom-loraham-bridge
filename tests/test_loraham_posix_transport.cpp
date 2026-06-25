// test_loraham_posix_transport.cpp — exercises the real POSIX AF_UNIX transport
// against an in-process fake daemon over loopback Unix sockets (no LoRaHAM daemon,
// no hardware). This covers the one module the other tests stub out.
//
// SPDX-License-Identifier: MIT

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "backend/loraham/loraham_framing.h"
#include "backend/loraham/loraham_posix_transport.h"
#include "test_helpers.h"

using namespace mebridge::loraham;

namespace {

// AF_UNIX stream listener at path. connect() to a listening socket succeeds and
// is queued, so the transport's blocking connect returns before we accept().
int make_listener(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_un a;
    std::memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
    CHECK(::bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof(a)) == 0);
    CHECK(::listen(fd, 1) == 0);
    return fd;
}

int accept_one(int lfd) {
    int c = ::accept(lfd, nullptr, nullptr);
    CHECK(c >= 0);
    return c;
}

void send_str(int fd, const std::string& s) {
    CHECK(::send(fd, s.data(), s.size(), 0) == static_cast<ssize_t>(s.size()));
}

std::string recv_str(int fd, size_t n) {
    std::string out;
    while (out.size() < n) {
        char buf[256];
        ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
        CHECK(r > 0);
        out.append(buf, static_cast<size_t>(r));
    }
    return out;
}

// data_recv is non-blocking; retry briefly so the test isn't racy on delivery.
int data_recv_retry(PosixDaemonTransport& t, uint8_t* buf, int cap) {
    for (int i = 0; i < 200; ++i) {
        int r = t.data_recv(buf, cap);
        if (r != 0) return r;
        ::usleep(1000);
    }
    return 0;
}

struct TempPaths {
    char dir[64];
    std::string data, conf;
    TempPaths() {
        std::strcpy(dir, "/tmp/mebridge_txp_XXXXXX");
        CHECK(::mkdtemp(dir) != nullptr);
        data = std::string(dir) + "/data.sock";
        conf = std::string(dir) + "/conf.sock";
    }
    ~TempPaths() {
        ::unlink(data.c_str());
        ::unlink(conf.c_str());
        ::rmdir(dir);
    }
    DaemonPaths as_daemon_paths() const {
        DaemonPaths p;
        p.data433 = data;  // band 433 routes here
        p.conf433 = conf;
        p.data868 = data;
        p.conf868 = conf;
        return p;
    }
};

void test_conf_roundtrip_and_fragmented_line() {
    TempPaths tp;
    int ldata = make_listener(tp.data);
    int lconf = make_listener(tp.conf);

    PosixDaemonTransport t(tp.as_daemon_paths());
    CHECK(t.connect(Band::Band433));
    int sdata = accept_one(ldata);
    int sconf = accept_one(lconf);

    // transport -> daemon (CONF)
    const char* cmd = "GET STATUS\n";
    CHECK(t.conf_send(reinterpret_cast<const uint8_t*>(cmd), std::strlen(cmd)));
    CHECK(recv_str(sconf, std::strlen(cmd)) == cmd);

    // daemon -> transport (CONF), delivered in two fragments
    send_str(sconf, "STATUS RADIO=");
    send_str(sconf, "READY TX=0\n");
    std::string line;
    CHECK(t.conf_read_line(line, 1000));
    CHECK(line == "STATUS RADIO=READY TX=0");

    // timeout path: nothing to read
    CHECK(!t.conf_read_line(line, 50));

    ::close(sdata);
    ::close(sconf);
    ::close(ldata);
    ::close(lconf);
}

void test_data_tx_and_rx() {
    TempPaths tp;
    int ldata = make_listener(tp.data);
    int lconf = make_listener(tp.conf);

    PosixDaemonTransport t(tp.as_daemon_paths());
    CHECK(t.connect(Band::Band433));
    int sdata = accept_one(ldata);
    int sconf = accept_one(lconf);

    // TX: transport sends a framed TX_PACKET; daemon side receives the bytes.
    const uint8_t rf[3] = {0x11, 0x22, 0x33};
    uint8_t txf[16];
    size_t txn = encode_tx_packet(txf, sizeof(txf), rf, 3);
    CHECK(t.data_send(txf, txn));
    std::string got = recv_str(sdata, txn);
    CHECK(got.size() == txn);
    CHECK(static_cast<uint8_t>(got[0]) == FRAMED_TX_PACKET);
    CHECK(std::memcmp(got.data() + kFramedHeaderLen, rf, 3) == 0);

    // RX: daemon sends a framed RX_PACKET; transport receives + parses it.
    uint8_t rxf[64];
    size_t rxn = encode_rx_packet(rxf, sizeof(rxf), -8000, 250, rf, 3);
    CHECK(::send(sdata, rxf, rxn, 0) == static_cast<ssize_t>(rxn));

    uint8_t buf[256];
    int r = data_recv_retry(t, buf, sizeof(buf));
    CHECK(r > 0);
    Parser p;
    parser_reset(p);
    size_t took = 0;
    CHECK(parser_push(p, buf, static_cast<size_t>(r), took));
    Frame f;
    CHECK(parser_pop(p, f) == POP_GOT_FRAME);
    int16_t rssi = 0, snr = 0;
    const uint8_t* prf = nullptr;
    uint16_t plen = 0;
    CHECK(decode_rx(f, rssi, snr, prf, plen));
    CHECK(rssi == -8000 && snr == 250 && plen == 3);
    CHECK(std::memcmp(prf, rf, 3) == 0);

    ::close(sdata);
    ::close(sconf);
    ::close(ldata);
    ::close(lconf);
}

void test_disconnect_detected() {
    TempPaths tp;
    int ldata = make_listener(tp.data);
    int lconf = make_listener(tp.conf);

    PosixDaemonTransport t(tp.as_daemon_paths());
    CHECK(t.connect(Band::Band433));
    int sdata = accept_one(ldata);
    int sconf = accept_one(lconf);

    ::close(sdata);  // daemon closed the DATA socket
    uint8_t buf[64];
    int r = 0;
    for (int i = 0; i < 200; ++i) {
        r = t.data_recv(buf, sizeof(buf));
        if (r < 0) break;
        ::usleep(1000);
    }
    CHECK(r < 0);  // EOF reported as disconnect

    ::close(sconf);
    ::close(ldata);
    ::close(lconf);
}

}  // namespace

int main() {
    RUN(test_conf_roundtrip_and_fragmented_line);
    RUN(test_data_tx_and_rx);
    RUN(test_disconnect_detected);
    std::fprintf(stderr, "test_loraham_posix_transport: all passed\n");
    return 0;
}
