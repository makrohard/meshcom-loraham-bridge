# meshcom-extradio-bridge

A small Raspberry-Pi bridge that lets MeshCom firmware use a remote radio over
the **XR external-radio TCP protocol v1**. The firmware is the TCP **client**;
this bridge is the **server**. It is one process with one event loop and one
active firmware client at a time.

```text
MeshCom firmware (ESP32, or later ESP32/QEMU)
        |
        |  XR external-radio TCP protocol v1   (firmware = client, bridge = server)
        v
meshcom-extradio-bridge   (this repository, one process)
  ├─ generic XR TCP server / session / authentication
  ├─ generic RadioBackend interface
  ├─ FakeBackend (host tests + runnable default)   <-- the only backend in this milestone
  └─ future LoRaHAM daemon adapter module          <-- deferred (see below)
        |
        v
LoRaHAM daemon  ->  LoRaHAM Pi-HAT radio
```

The future daemon adapter is a **module in this same executable**, not a separate
helper process.

## Status (milestone M11b)

Implemented: the generic XR server, the server-side session state machine,
optional one-way HMAC authentication, a generic `RadioBackend` interface, a
deterministic `FakeBackend`, bounded I/O, and keepalive — all host-tested.

**The LoRaHAM daemon adapter is intentionally deferred.** The daemon's `CONF`
`SET` path currently provides no stable acknowledgement and its status does not
read back every XR `RadioConfig` field, so a real adapter cannot yet prove that a
requested configuration was applied exactly. This bridge never reports a
`CONFIG_RESULT` success unless the backend confirms it applied the exact
effective configuration; until the daemon can confirm-and-read-back a full
configuration, only the `FakeBackend` satisfies that contract. Adding that
daemon capability is the next milestone (M12a).

## Build

Requirements: a C++17 compiler, CMake ≥ 3.16, and OpenSSL `libcrypto`
development headers.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Run

```bash
./build/meshcom-extradio-bridge --bind 127.0.0.1 --port 7000
# optional one-way authentication using a password file:
./build/meshcom-extradio-bridge --port 7000 --password-file /path/to/secret
```

With the `FakeBackend` a connecting firmware completes the handshake, reaches the
operational state (the fake applies the exact requested configuration), and is
kept alive with PING/PONG. The fake performs no RF and no daemon I/O.

## Security defaults

- Binds **`127.0.0.1`** by default; expose it only on a trusted private interface.
- Authentication is **open by default**. With `--password-file`, the bridge sends
  a fresh random 16-byte challenge and requires a raw HMAC-SHA256 response,
  compared in constant time.
- **Passwords are never accepted on the command line** — only via a file whose
  contents are the password (a single trailing newline is ignored).
- No secret, nonce, HMAC, or raw payload is ever logged.
- Output to a client is **bounded**; a client that stops reading is disconnected
  rather than allowed to grow memory without limit.

## Fake backend scope

`FakeBackend` is a deterministic in-memory test double. It does **not** talk to a
radio or a daemon. Tests script its configuration result, its terminal TX
outcome (`SUCCESS` / `CHANNEL_BUSY` / `TIMEOUT` / `RADIO_ERROR`), injected RX
events, stale completions, and backend failures. It must never simulate an
unverified "daemon SET succeeded" path.

## Layout

```text
src/
  main.cpp              CLI + event loop wiring
  xr/                   XR server, session FSM, connection
  backend/              RadioBackend interface + FakeBackend
  auth/                 HMAC-SHA256 one-way auth (OpenSSL libcrypto)
  util/                 injectable clock, bounded byte buffer
third_party/
  external_radio_protocol/   vendored XR codec (see THIRD_PARTY_NOTICES.md)
tests/                  CTest host tests (no hardware)
```

## License

MIT. The reused XR protocol codec under `third_party/external_radio_protocol/` is
the bridge author's own work, relicensed MIT for this repository; see `LICENSE`
and `THIRD_PARTY_NOTICES.md`.
