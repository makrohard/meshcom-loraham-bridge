# Provenance and third-party notices

## external_radio_protocol (reused from MeshCom-Firmware)

Location in this repository: `third_party/external_radio_protocol/`

This is the generic, hardware- and transport-independent XR external-radio
companion-protocol codec (binary frame codec, bounded streaming parser, strict
per-message validation, session state machine, and the normalized `RadioConfig`
type). The `.h`/`.cpp` are copied byte-for-byte from the source repository so the
bridge speaks exactly the same wire protocol the firmware was tested against.

### Provenance

- Source project: MeshCom-Firmware (fork `makrohard/MeshCom-Firmware`)
- Source path: `lib/external_radio_protocol/`
- Local source tree scanned: `/home/makro/src/MeshCom-Firmware`
- Branch: `feature/external-radio-tcp-draft`
- Repository HEAD at copy time: `302e3aaff46cbefa29ac651640559b920dd3b713`
- Commit that last modified these files: `34dd638f175df6c1fa74035d781d25b2a20139e2`
- Verbatim source SHA-256 (`.h` / `.cpp` unchanged from source):
  - `external_radio_protocol.h`:
    `a09bb4bf0931781229af5be780cc760ba9142461c8da9d50a416caa1d8220f36`
  - `external_radio_protocol.cpp`:
    `7b67a80120f18b3eb401a1770ba7978a84fb090d6281911b96f12a55d939176f`

### Authorship and license

These files are the **original work of the bridge author, Johannes Loose**
(git author handle `makrohard <410733@gmail.com>`). Verified from the source
repository: every commit touching `lib/external_radio_protocol/` and every line
of both files is authored by `makrohard` — there are no other contributors and no
inherited lines from the upstream icssw-org MeshCom-Firmware tree (the files were
added new and include only standard C++ headers).

In MeshCom-Firmware these files were published with `"license":
"GPL-3.0-or-later"` (in their `library.json`). As the sole copyright holder, the
author relicenses this reused copy under the **MIT License** for use in this
repository. Accordingly, the vendored `library.json` here has been updated to
`"license": "MIT"` (the only change to the vendored files; the `.h`/`.cpp` remain
byte-verbatim as recorded by the SHA-256 above).

The whole repository is therefore MIT-licensed; see `LICENSE`.

## OpenSSL (libcrypto) — linked, not vendored

The bridge links the system OpenSSL `libcrypto` for HMAC-SHA256, secure random
nonces (`RAND_bytes`), and constant-time comparison (`CRYPTO_memcmp`). OpenSSL
3.x is distributed under the Apache License 2.0, which is compatible with MIT.
OpenSSL is a separate system library and is not redistributed in this repository.
