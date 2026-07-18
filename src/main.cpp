// main.cpp — meshcom-loraham-bridge entry point.
//
// One process, one event loop, one active firmware client. In this milestone the
// only radio backend is the deterministic FakeBackend, so a connected firmware
// completes HELLO/AUTH/CONFIGURE and reaches READY (the fake applies the exact
// requested config) and is kept alive with PING/PONG. A real LoRaHAM-daemon
// adapter is a future module in this same process (see README).
//
// Security defaults: bind 127.0.0.1; open auth unless --password-file is given.
// Passwords are NEVER accepted on the command line.
//
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "auth/hmac_auth.h"
#include "backend/fake_backend.h"
#include "backend/loraham/loraham_backend.h"
#include "backend/loraham/loraham_posix_transport.h"
#include "util/clock.h"
#include "xr/xr_server.h"

#ifndef MEBRIDGE_VERSION
#define MEBRIDGE_VERSION "0.0.0-dev"  // overridden by the build (see CMakeLists.txt)
#endif

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

constexpr size_t kDefaultMaxOutbox = 64 * 1024;  // bounded per-client output

void usage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s [--bind ADDR] [--port N] [--password-file PATH] [--backend NAME]\n"
        "  --bind ADDR           bind address (default 127.0.0.1)\n"
        "  --port N              TCP port (default 7000; 0 = ephemeral)\n"
        "  --password-file PATH  enable one-way HMAC auth using the file contents\n"
        "                        as the password (passwords are never taken on argv)\n"
        "  --backend NAME        radio backend: 'fake' (default) or 'loraham'\n"
        "                        (loraham connects to the local LoRaHAM daemon sockets)\n"
        "  -v, --version         print version and exit\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    using namespace mebridge;

    std::string bind_addr = "127.0.0.1";
    uint16_t port = 7000;
    std::string password_file;
    std::string backend_name = "fake";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--bind") {
            bind_addr = next("--bind");
        } else if (a == "--port") {
            const char* v = next("--port");
            char* end = nullptr;
            errno = 0;
            const unsigned long p = std::strtoul(v, &end, 10);
            // Reject non-numeric, trailing garbage, overflow, or out-of-range so a
            // typo fails closed instead of silently truncating (99999 -> 33465) or
            // binding an ephemeral port. 0 stays valid (ephemeral, by design).
            if (end == v || *end != '\0' || errno != 0 || p > 65535) {
                std::fprintf(stderr, "error: --port must be an integer in [0, 65535]\n");
                return 2;
            }
            port = static_cast<uint16_t>(p);
        } else if (a == "--password-file") {
            password_file = next("--password-file");
        } else if (a == "--backend") {
            backend_name = next("--backend");
        } else if (a == "-v" || a == "--version") {
            std::printf("meshcom-loraham-bridge %s\n", MEBRIDGE_VERSION);
            return 0;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "error: unknown argument '%s'\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    AuthConfig auth;  // open mode by default
    if (!password_file.empty()) {
        std::string err;
        if (!AuthConfig::from_password_file(password_file, auth, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());  // never prints content
            return 1;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // Select the radio backend. The LoRaHAM transport must outlive the backend.
    SteadyClock clock;
    std::unique_ptr<loraham::PosixDaemonTransport> loraham_transport;
    std::unique_ptr<RadioBackend> backend;
    if (backend_name == "fake") {
        backend = std::make_unique<FakeBackend>();
    } else if (backend_name == "loraham") {
        loraham_transport = std::make_unique<loraham::PosixDaemonTransport>();
        backend = std::make_unique<LorahamBackend>(*loraham_transport, clock);
    } else {
        std::fprintf(stderr, "error: unknown backend '%s' (use 'fake' or 'loraham')\n",
                     backend_name.c_str());
        return 2;
    }

    XrServer server(*backend, std::move(auth), clock, XrSession::default_timeouts(),
                    kDefaultMaxOutbox);

    std::string err;
    if (!server.listen(bind_addr, port, err)) {
        std::fprintf(stderr, "error: listen failed: %s\n", err.c_str());
        return 1;
    }

    std::fprintf(stderr,
        "meshcom-loraham-bridge: listening on %s:%u, auth=%s, backend=%s\n",
        bind_addr.c_str(), static_cast<unsigned>(server.bound_port()),
        password_file.empty() ? "open" : "password", backend_name.c_str());

    server.run(g_stop);
    std::fprintf(stderr, "meshcom-loraham-bridge: shutting down\n");
    return 0;
}
