# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and this project adheres to
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Security
- Build with exploit-mitigation hardening by default: stack canaries
  (`-fstack-protector-strong`), hardened libstdc++ bounds assertions
  (`-D_GLIBCXX_ASSERTIONS`), a position-independent executable, full RELRO +
  immediate binding (`-Wl,-z,relro,-z,now`), a non-executable stack, and
  `-Wformat-security`; `_FORTIFY_SOURCE=2` is applied to optimised builds. Added
  an opt-in `-DMEBRIDGE_SANITIZE=ON` build (AddressSanitizer + UBSan) for
  tests/CI; the full suite passes clean under it.
- Bound the `--password-file` read to 4 KiB so a mistaken path (a huge file or a
  device such as `/dev/zero`) cannot exhaust memory; oversized files are rejected.

### Fixed
- Validate `--port`: a non-numeric, out-of-range, or overflowing value now fails
  closed (exit 2) instead of silently truncating (e.g. `99999` → `33465`) or
  binding an OS-assigned ephemeral port. `0` remains valid (ephemeral by design).
- Avoid a 100%-CPU busy-spin when `accept()` cannot consume a pending connection
  due to resource exhaustion (`EMFILE`/`ENFILE`/`ENOBUFS`/`ENOMEM`): the
  level-triggered listener stays readable, so the event loop now backs off briefly
  instead of re-polling immediately until an fd frees up.

### Documentation
- Record native validation of the real MeshCom `EXTERNAL_RADIO` firmware against
  this bridge under the Espressif ESP32 QEMU (connect + HMAC + control-plane
  configuration + live RX through native MeshCom ingress). Document payload
  neutrality, the three non-substitutable evidence levels (daemon `TX_RESULT` vs
  peer receipt vs MeshCom ACK), that `CHANNEL_BUSY` is opportunistic under managed
  CAD, and the safe bridge-process recovery rule after an uncertain pending TX (no
  automatic restart; daemon-owned TX may still transmit after the bridge exits).

### Fixed
- Parse the daemon v112 verbose CONF replies during configure. v112 acknowledges
  every `SET` on the CONF socket with a bare `OK` line (a rejected `SET` answers
  `ERR <reason>`); `GET STATUS` still answers with its data line and no trailing
  `OK`. The backend previously assumed v111's silent `SET`s and treated the first
  `OK` as unexpected input, failing and looping the configure even though the
  daemon had applied the config and the radio was ready. `pump_conf()` now consumes
  the `OK` acks while awaiting the terminal `STATUS`, fails the configure on any
  `SET` `ERR` (the backend must not reach a TX-capable `Ready` state on an
  unapplied config), and logs the offending line. The change is backward compatible
  with v111 (no `SET` ack → the OK-consume path simply never triggers).
- Preserve uncertain TX ownership across every teardown path. Previously only the
  bridge-side TX deadline entered draining; two paths lost ownership tracking while
  the daemon owned a complete TX frame: (1) a daemon link loss during `TxPending`
  reset to a reusable `Disconnected`, and (2) an XR client/session teardown
  (`~XrConnection`) reset any non-draining/faulted state via `stop()`. Now
  `XrConnection` teardown unconditionally calls `abandon_pending_tx()` before
  detaching the sink and stopping (a no-op unless a TX is owned), so any XR teardown
  during a daemon-owned TX enters draining; and `LorahamBackend::handle_disconnect()`
  faults (instead of resetting) when the daemon link drops while it owns a complete
  frame. Faulted is restart-only because daemon v111 provides no source-proven
  cancel/result recovery after link loss. A still-incomplete (`TxWriting`,
  bridge-owned) frame keeps resetting cleanly; normal terminal completion before any
  teardown still returns to `Ready` without draining or faulting.

### Changed
- Make all LoRaHAM daemon I/O non-blocking and deadline-bounded. The daemon
  connect (`EINPROGRESS` + `getsockopt(SO_ERROR)`), CONF/DATA writes (partial,
  resumable), and reads (reassembled, broadcast-filtered) no longer block the
  single event loop. Configuration became an asynchronous progression driven by
  the backend's `poll()`: `RadioBackend::configure()` is now `begin_configure()`,
  with the result delivered via `BackendSink::on_configure_complete()` and fenced
  by an operation token so a completion from a disconnected or earlier session can
  never affect a new client; the XR session gains a `Configuring` phase. A
  distinct `TxWriting` state keeps a queued/partially-written TX frame bridge-owned
  until the whole frame is written (daemon ownership starts only then); a transport
  failure mid-write is a clean backend failure, not an uncertain TX. The event loop
  now shortens its poll timeout (≤ 100 ms) to the nearest XR/backend deadline. A
  daemon loss during pending or draining TX still never yields success or
  re-enables TX (M12d behavior preserved). No threads; one process, one event loop.
- Harden bridge TX-timeout ownership. On a bridge-side TX deadline the session no
  longer fabricates a `TIMEOUT`: it abandons the TX to the LoRaHAM backend and
  closes the XR session (firmware resolves UNKNOWN). The backend keeps the daemon
  socket open and drains the outstanding `TX_RESULT`, refusing new TX until
  ownership is provably clear; a mid-drain link loss or bounded drain timeout
  enters a faulted state (TX disabled until restart) instead of risking a
  duplicate transmission. Source analysis confirmed daemon v111 does not cancel a
  queued/in-flight TX on client disconnect and drops late results to closed slots.
- Clarify that LoRaHAM `ConfigureResult.applied` is control-plane acceptance, not
  a hardware-register read-back or on-air/RF confirmation (comments + README).

## [0.1.0] - 2026-06-26

First working release. Bridges MeshCom firmware (XR external-radio TCP protocol
v1, firmware = client) to a LoRaHAM Pi-HAT radio via the LoRaHAM daemon v111
(unchanged). One process, one event loop, one active firmware client.

### Added
- Generic XR TCP server with a server-side session state machine: HELLO/HELLO_ACK,
  optional one-way HMAC-SHA256 authentication, exact `CONFIG_RESULT` echo gate,
  RX delivery, single-in-flight TX with terminal `TX_RESULT`, and PING/PONG
  keepalive. Fail-closed on malformed frames, illegal transitions, missing PONG,
  or output overflow.
- One active client policy with bounded non-blocking I/O.
- `RadioBackend` abstraction with two backends, selectable via `--backend`:
  - `fake` (default): deterministic in-memory backend for tests and dry runs.
  - `loraham`: speaks the LoRaHAM daemon v111 local Unix sockets (framed DATA for
    RX/TX, CONF text for configuration); band auto-selected from frequency.
- Configuration authority model: validate the requested `RadioConfig` against the
  daemon's LoRa limits, apply via CONF, confirm `RADIO=READY`, and echo canonical
  integer-Hz values (insulating the firmware from the daemon's float rounding).
- HMAC auth via OpenSSL libcrypto: random 16-byte challenge, constant-time
  comparison, password from a file only (never on the command line); no secret,
  nonce, HMAC, or payload is logged.
- Operational logging on client connect/disconnect (with reason) and daemon
  configure/link events.
- `--version` flag; CTest host test suites (no hardware) and a CI workflow.

### Notes
- The vendored XR protocol codec under `third_party/external_radio_protocol/` is
  the author's own work, relicensed MIT for this repository (see
  `THIRD_PARTY_NOTICES.md`).
- Validated on real hardware against the MeshCom 433 profile (433.175 MHz / SF11 /
  BW250 / CR4-6 / sync 0x2B): configure, transmit, receive, and an on-air reply.
- The daemon caps TX power at 0-20 dBm; a requested power above 20 dBm is rejected.

[0.1.0]: https://github.com/makrohard/meshcom-loraham-bridge/releases/tag/v0.1.0
