// test_cli.cpp — runs the built bridge binary (the public command line) and checks how it takes
// --data-socket / --conf-socket: accepted with a usable path, refused with exit 2 (the bridge's code for every
// argument error) for an empty, missing or too long path, or without --backend loraham.
//
// SPDX-License-Identifier: MIT

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
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

void test_usage_names_the_options() {
    Run r = run({"--help"});
    CHECK(r.rc == 0 && has(r.out, "--data-socket PATH") && has(r.out, "--conf-socket PATH"));
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
    RUN_SELECTED(test_usage_names_the_options);
    CHECK(ran > 0);                                  // a misspelt name runs nothing: fail
    std::fprintf(stderr, "test_cli: all passed\n");
    return 0;
}
