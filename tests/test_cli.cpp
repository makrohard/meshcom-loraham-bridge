// test_cli.cpp — runs the built bridge binary (the public command line) and checks how it takes
// --data-socket / --conf-socket: accepted with a usable path, refused with exit 2 (the bridge's code for every
// argument error) for an empty, missing or too long path, or without --backend loraham.
//
// SPDX-License-Identifier: MIT

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_helpers.h"

#ifndef MEBRIDGE_BINARY
#error "MEBRIDGE_BINARY must name the built bridge binary"
#endif

extern char** environ;

namespace {

struct Run {
    int rc = -1;
    std::string out;   // stdout and stderr together
};

Run run(const std::vector<std::string>& args) {
    char tmpl[] = "/tmp/mebridge_cli_XXXXXX";
    int fd = ::mkstemp(tmpl);
    CHECK(fd >= 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd, 1);
    posix_spawn_file_actions_adddup2(&fa, fd, 2);
    std::vector<std::string> all = {MEBRIDGE_BINARY};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    CHECK(posix_spawn(&pid, MEBRIDGE_BINARY, &fa, nullptr, argv.data(), environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    Run r;
    r.rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    ::lseek(fd, 0, SEEK_SET);
    char buf[512];
    ssize_t n;
    while ((n = ::read(fd, buf, sizeof(buf))) > 0) r.out.append(buf, static_cast<size_t>(n));
    ::close(fd);
    ::unlink(tmpl);
    return r;
}

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// The options are taken together: with usable paths the parse goes on to --version, which prints and exits 0.
void test_the_socket_options_are_accepted() {
    Run r = run({"--backend", "loraham", "--data-socket", "/run/loraham/loraf-r2.sock",
                 "--conf-socket", "/run/loraham/loraconf-r2.sock", "--version"});
    CHECK(r.rc == 0);
    CHECK(has(r.out, "meshcom-loraham-bridge "));
}

// One alone would split the bridge across two daemons (that radio's and a per-band default): refused, naming the
// missing one.
void test_one_socket_option_alone_is_refused() {
    Run data_only = run({"--backend", "loraham", "--data-socket", "/run/loraham/loraf-r2.sock"});
    CHECK(data_only.rc == 2 && has(data_only.out, "--data-socket needs --conf-socket"));
    Run conf_only = run({"--backend", "loraham", "--conf-socket", "/run/loraham/loraconf-r2.sock"});
    CHECK(conf_only.rc == 2 && has(conf_only.out, "--conf-socket needs --data-socket"));
}

void test_a_bad_socket_path_is_refused_with_exit_2() {
    Run empty = run({"--backend", "loraham", "--data-socket", ""});
    CHECK(empty.rc == 2 && has(empty.out, "--data-socket is empty"));
    Run longp = run({"--backend", "loraham", "--conf-socket", std::string(108, 'a')});
    CHECK(longp.rc == 2 && has(longp.out, "--conf-socket is 108 bytes"));
    Run missing = run({"--backend", "loraham", "--data-socket"});
    CHECK(missing.rc == 2 && has(missing.out, "--data-socket requires a value"));
}

void test_the_socket_options_need_the_loraham_backend() {
    Run r = run({"--data-socket", "/run/loraham/loraf-r2.sock",
                 "--conf-socket", "/run/loraham/loraconf-r2.sock"});   // backend defaults to 'fake'
    CHECK(r.rc == 2 && has(r.out, "need --backend loraham"));
}

// --max-power is required: a launch without it refuses to start (no radio), never runs without a ceiling.
void test_max_power_is_required() {
    Run r = run({"--backend", "fake", "--port", "0"});
    CHECK(r.rc == 2 && has(r.out, "--max-power is required"));
}

// Strict: an integer 0..20 (dBm), nothing else.
void test_a_bad_max_power_is_refused_with_exit_2() {
    for (const char* bad : {"21", "-1", "abc", "", "14x", "1e1", " 5", "99999999999999999999"}) {
        Run r = run({"--max-power", bad, "--port", "0"});
        CHECK(r.rc == 2 && has(r.out, "--max-power must be an integer in [0, 20]"));
    }
    Run missing = run({"--max-power"});
    CHECK(missing.rc == 2 && has(missing.out, "--max-power requires a value"));
    Run ok = run({"--max-power", "17", "--version"});
    CHECK(ok.rc == 0 && has(ok.out, "meshcom-loraham-bridge "));
}

// The built bridge, end to end: started with --max-power 10, a firmware client asking for 14 dBm gets
// CFG_UNSUPPORTED and the session closes; one at 10 dBm gets its radio. The log names both, in the lines LoRaHAM Pi
// Control reads.
void test_a_configure_above_max_power_gets_no_radio() {
    int pfd[2];
    CHECK(::pipe(pfd) == 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pfd[1], 2);
    posix_spawn_file_actions_addclose(&fa, pfd[0]);
    std::vector<std::string> all = {MEBRIDGE_BINARY, "--port", "0", "--max-power", "10"};
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    CHECK(posix_spawn(&pid, MEBRIDGE_BINARY, &fa, nullptr, argv.data(), environ) == 0);
    posix_spawn_file_actions_destroy(&fa);
    ::close(pfd[1]);
    std::string log;
    auto read_until = [&](const char* part) {          // the bridge's output, up to 5 s for `part`
        for (int i = 0; i < 50 && !has(log, part); ++i) {
            pollfd p{pfd[0], POLLIN, 0};
            if (::poll(&p, 1, 100) > 0) {
                char buf[512];
                ssize_t n = ::read(pfd[0], buf, sizeof(buf));
                if (n > 0) log.append(buf, static_cast<size_t>(n));
            }
        }
        return has(log, part);
    };
    CHECK(read_until("listening on 127.0.0.1:"));
    CHECK(has(log, "max-power=10"));
    const unsigned port = static_cast<unsigned>(std::strtoul(log.substr(log.find("127.0.0.1:") + 10).c_str(), nullptr, 10));
    CHECK(port > 0 && port < 65536);
    auto configure = [&](int power) {                 // a firmware client: HELLO + CONFIGURE(power) -> its frames
        int c = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<uint16_t>(port));
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(c, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);
        auto out = testutil::build_hello();
        auto cfg = testutil::default_config();
        cfg.tx_power_dbm = static_cast<int8_t>(power);
        auto conf = testutil::build_configure(cfg);
        out.insert(out.end(), conf.begin(), conf.end());
        CHECK(::send(c, out.data(), out.size(), 0) == static_cast<ssize_t>(out.size()));
        std::vector<uint8_t> in;
        for (int i = 0; i < 50; ++i) {                 // to the CONFIG_RESULT or the bridge's close, up to 5 s
            auto fs = testutil::parse_frames(in.data(), in.size());
            if (testutil::find_type(fs, extradio::MSG_CONFIG_RESULT) != nullptr) break;
            pollfd p{c, POLLIN, 0};
            if (::poll(&p, 1, 100) <= 0) continue;
            uint8_t buf[512];
            ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            in.insert(in.end(), buf, buf + n);
        }
        ::close(c);
        return in;
    };
    std::vector<uint8_t> in = configure(14);
    auto frames = testutil::parse_frames(in.data(), in.size());
    const extradio::Frame* cr = testutil::find_type(frames, extradio::MSG_CONFIG_RESULT);
    CHECK(cr != nullptr && cr->len == 1 && cr->payload[0] == extradio::CFG_UNSUPPORTED);
    CHECK(read_until("XR client disconnected: config-failed"));
    CHECK(has(log, "XR CONFIGURE refused: power 14 dBm is above --max-power 10 dBm"));
    // the node at the ceiling: its next session gets the radio, and the log says so
    std::vector<uint8_t> ok = configure(10);
    auto okf = testutil::parse_frames(ok.data(), ok.size());
    const extradio::Frame* okr = testutil::find_type(okf, extradio::MSG_CONFIG_RESULT);
    CHECK(okr != nullptr && okr->payload[0] == extradio::CFG_OK);
    CHECK(read_until("XR configured: power 10 dBm (--max-power 10 dBm)"));
    ::kill(pid, SIGTERM);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    ::close(pfd[0]);
}

void test_usage_names_the_options() {
    Run r = run({"--help"});
    CHECK(r.rc == 0 && has(r.out, "--data-socket PATH") && has(r.out, "--conf-socket PATH")
          && has(r.out, "--max-power N"));
}

}  // namespace

// One test by name (`test_cli <name>`), so each case can be run on its own (its red-before); none: all.
#define RUN_SELECTED(test) \
    do { if (only.empty() || only == #test) { RUN(test); ++ran; } } while (0)

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    int ran = 0;
    RUN_SELECTED(test_the_socket_options_are_accepted);
    RUN_SELECTED(test_one_socket_option_alone_is_refused);
    RUN_SELECTED(test_a_bad_socket_path_is_refused_with_exit_2);
    RUN_SELECTED(test_the_socket_options_need_the_loraham_backend);
    RUN_SELECTED(test_max_power_is_required);
    RUN_SELECTED(test_a_bad_max_power_is_refused_with_exit_2);
    RUN_SELECTED(test_a_configure_above_max_power_gets_no_radio);
    RUN_SELECTED(test_usage_names_the_options);
    CHECK(ran > 0);                                  // a misspelt name runs nothing: fail
    std::fprintf(stderr, "test_cli: all passed\n");
    return 0;
}
