# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/), and this project adheres to
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed
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
