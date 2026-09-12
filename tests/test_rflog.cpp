// test_rflog.cpp — RF log: line contract, open rules, copy-truncate retention
// on one inode, tolerance of an external truncate.
//
// SPDX-License-Identifier: MIT

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "test_helpers.h"
#include "util/rflog.h"

using namespace mebridge;

namespace {

long file_inode(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long>(st.st_ino) : -1;
}

long file_size(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : -1;
}

long count_lines(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return -1;
    long n = 0;
    int c;
    while ((c = std::fgetc(f)) != EOF)
        if (c == '\n') ++n;
    std::fclose(f);
    return n;
}

void test_line_format() {
    char line[512];
    const uint8_t rf[] = {0x41, 0x22, 0x5c, 0x7f, 0x00};
    size_t n = rflog::format_rx(line, sizeof(line), "2026-09-12T16:03:47.412Z",
                                -10450, 725, rf, sizeof(rf));
    CHECK(std::string(line) ==
          "2026-09-12T16:03:47.412Z RX rssi=-104.50 snr=7.25 len=5"
          " hex=41225c7f00 ascii=\"A....\"\n");
    CHECK(n == std::strlen(line));

    n = rflog::format_tx(line, sizeof(line), "2026-09-12T16:03:51.006Z", "unconfirmed",
                         rf, 2);
    CHECK(std::string(line) ==
          "2026-09-12T16:03:51.006Z TX rssi=- snr=- len=2 outcome=unconfirmed"
          " hex=4122 ascii=\"A.\"\n");
    CHECK(n == std::strlen(line));
}

void test_line_is_bounded() {
    uint8_t rf[255];
    std::memset(rf, 'A', sizeof(rf));
    char line[96];
    size_t n = rflog::format_rx(line, sizeof(line), "t", 0, 0, rf, sizeof(rf));
    CHECK(n == sizeof(line) - 1);
    CHECK(line[n - 1] == '\n');
    CHECK(line[n] == '\0');
}

void test_open_rules() {
    std::string err;
    CHECK(!rflog::open("logs/rf.log", err));
    CHECK(err.find("absolute") != std::string::npos);
    CHECK(!rflog::open("", err));
    CHECK(!rflog::open("/nonexistent-dir-x/rf.log", err));
    CHECK(!rflog::active());
}

void test_rollover_keeps_inode() {
    char dir[] = "/tmp/mebridge-rflog-XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/rf-meshcom.log";
    const std::string prev = path + ".1";
    const uint8_t rf[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

    std::string err;
    CHECK(rflog::open(path, err));
    CHECK(rflog::active());
    rflog::set_max_bytes(2000);
    const long inode_before = file_inode(path);

    // ~100 bytes per line; 30 lines cross a 2000-byte cap once.
    for (int i = 0; i < 30; ++i) rflog::rx(-9000, 500, rf, sizeof(rf));
    CHECK(file_size(prev) > 0);
    CHECK(file_inode(path) == inode_before);   // truncated in place, not renamed
    CHECK(file_size(path) < 2000);
    CHECK(count_lines(prev) + count_lines(path) == 30);  // nothing lost

    // External truncate (the controller's Clear): the next line lands at the
    // new end of the same inode.
    int fd = ::open(path.c_str(), O_WRONLY);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, 0) == 0);
    ::close(fd);
    rflog::tx("ok", rf, 4);
    CHECK(count_lines(path) == 1);
    CHECK(file_inode(path) == inode_before);

    rflog::close();
    CHECK(!rflog::active());
    rflog::rx(0, 0, rf, 2);                     // silent no-op
    CHECK(count_lines(path) == 1);
    ::unlink(path.c_str());
    ::unlink(prev.c_str());
    ::rmdir(dir);
}

}  // namespace

int main() {
    RUN(test_line_format);
    RUN(test_line_is_bounded);
    RUN(test_open_rules);
    RUN(test_rollover_keeps_inode);
    std::fprintf(stderr, "test_rflog: all passed\n");
    return 0;
}
