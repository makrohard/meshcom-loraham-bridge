# meshcom-loraham-bridge

A small Raspberry-Pi bridge that lets MeshCom firmware use a LoRaHAM Pi-HAT radio
(driven by the LoRaHAM daemon) over the **XR external-radio TCP protocol v1**. The
firmware is the TCP **client**; this bridge is the **server**. It is one process
with one event loop and one active firmware client at a time.

```text
MeshCom firmware (ESP32, or later ESP32/QEMU)
        |
        |  XR external-radio TCP protocol v1   (firmware = client, bridge = server)
        v
meshcom-loraham-bridge   (this repository, one process)
  ├─ generic XR TCP server / session / authentication
  ├─ generic RadioBackend interface
  ├─ FakeBackend (host tests + runnable default)
  └─ LoRaHAM daemon adapter (configure + RX/TX over the daemon's Unix sockets)
        |
        | local-only daemon sockets (Unix)
        v
LoRaHAM daemon v111  ->  LoRaHAM Pi-HAT radio
```

The LoRaHAM daemon adapter is a **module in this same executable**, not a
separate helper process.

## Protocol

The XR external-radio TCP protocol v1 — framing, handshake, optional auth, the
normalized `RadioConfig`, and RX/TX semantics — is specified in the MeshCom
firmware repository at `docs/external-radio-protocol.md`. Its hardware- and
transport-independent codec is vendored here under
`third_party/external_radio_protocol/` (see `THIRD_PARTY_NOTICES.md`).

## Status

The generic XR bridge core and the LoRaHAM daemon adapter are implemented and
host-tested with CTest (no hardware required). The LoRaHAM path has additionally
been validated on real hardware against the MeshCom 433 profile (433.175 MHz /
SF11 / BW250 / CR4-6 / sync 0x2B): configuration, transmit, and receive of live
MeshCom packets, including an on-air reply.

Backends (`--backend`):

- `fake` (default) — a deterministic in-memory `RadioBackend` for tests and dry
  runs; no RF, no daemon I/O.
- `loraham` — speaks the LoRaHAM daemon v111 local Unix sockets (framed DATA for
  RX/TX, CONF text for configuration). The daemon runs **unchanged**. All daemon
  connection, configuration, and I/O is **non-blocking and deadline-bounded**:
  connect uses `EINPROGRESS` + `getsockopt(SO_ERROR)`, sends are partial/resumable,
  reads are reassembled, and every phase has a deadline driven by the injected
  clock — a slow, missing, restarting, or congested daemon can never stall XR
  parsing, keepalive, timeout recovery, or client-disconnect handling.

Configuration model: the bridge is the XR configuration authority. It validates a
requested `RadioConfig` against the daemon's known LoRa limits, then runs an
**asynchronous, deadline-bounded** progression — connect the daemon sockets,
submit the managed-TX and radio settings, query status, and confirm the radio
reports ready — before echoing the requested values as the effective
configuration. The XR session stays responsive throughout (it is in a
`Configuring` phase); the `CONFIG_RESULT` is sent only once the whole sequence and
the readiness check complete. A `CONFIG_RESULT` success is **control-plane
acceptance** — the request was validated, submitted through the daemon's existing
`CONF` interface, and the daemon reported the radio ready. It is **not** a
hardware-register read-back and **not** on-air/RF confirmation (daemon v111
exposes neither). This is a deliberate, documented trade-off that keeps the daemon
untouched. Any daemon error, disconnect, malformed reply, or deadline during
configuration fails the configuration (never a false success).

TX ownership boundary: a queued or only partially written daemon TX frame is
still **bridge-owned** — daemon ownership begins only once the complete framed
packet has been written. A transport failure while the frame is still being
written means the daemon never received a complete packet (so it cannot transmit
it): that is a clean backend failure, not an uncertain one.

TX-timeout ownership: a TX has a single owner at a time. If the bridge's TX
deadline expires before the daemon delivers a result (after the daemon already
owns the frame), the bridge does **not** fabricate a `TIMEOUT` (it cannot know
whether the packet transmitted). It closes
the XR session (so the firmware resolves the TX as uncertain/UNKNOWN, never
resent) and keeps the daemon socket open to *drain* the outstanding result; no new
TX is accepted until that ownership is provably clear. If the daemon link dies
mid-drain or a bounded drain deadline elapses without a result, the backend enters
a logged faulted state (TX disabled until restart) rather than risk a duplicate
transmission.

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
./build/meshcom-loraham-bridge --bind 127.0.0.1 --port 7000
# optional one-way authentication using a password file:
./build/meshcom-loraham-bridge --port 7000 --password-file /path/to/secret
```

With the default `FakeBackend` a connecting firmware completes the handshake,
reaches the operational state (the fake applies the exact requested
configuration), and is kept alive with PING/PONG. The fake performs no RF and no
daemon I/O.

### With the LoRaHAM daemon (real radio)

Start the LoRaHAM daemon (v111) first so its sockets exist, then:

```bash
./build/meshcom-loraham-bridge --port 7000 --backend loraham
```

The band (433/868) is selected automatically from the frequency the firmware
requests. Note: the daemon caps TX power at 0–20 dBm, so a requested power above
20 dBm is rejected — keep the firmware's configured power ≤ 20 dBm.

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
  main.cpp              CLI + backend selection + event loop wiring
  xr/                   XR server, session FSM, connection
  backend/              RadioBackend interface + FakeBackend
  backend/loraham/      LoRaHAM daemon adapter: config translate, framing,
                        POSIX transport, backend
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
