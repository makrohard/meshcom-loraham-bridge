// hmac_auth.h — one-way connection authentication for the XR server.
//
// Mirrors the firmware NetConsole model: the bridge (server) optionally
// authenticates the firmware (client). When a password is configured the server
// sends a fresh random 16-byte nonce and expects raw HMAC-SHA256(password,
// nonce). Comparison is constant-time. In open mode the server skips the
// challenge and accepts immediately (operator-controlled private link only).
//
// Crypto is delegated to OpenSSL libcrypto (HMAC, RAND_bytes, CRYPTO_memcmp);
// no crypto is hand-rolled. No secret, nonce, HMAC, or raw payload is ever
// logged or returned in error text. Passwords are never read from argv: only an
// AuthConfig built from a file (or empty for open mode) is accepted.
//
// SPDX-License-Identifier: MIT

#ifndef MEBRIDGE_AUTH_HMAC_AUTH_H
#define MEBRIDGE_AUTH_HMAC_AUTH_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "external_radio_protocol.h"  // kAuthNonceSize, kAuthResponseSize

namespace mebridge {

class AuthConfig {
public:
    // Open mode: no password, no challenge.
    AuthConfig() = default;

    // Password mode from raw key bytes (used by tests).
    static AuthConfig from_password_bytes(const uint8_t* key, size_t len);

    // Password mode from a file. A single trailing '\n' or "\r\n" is stripped so
    // a normally-edited password file matches the firmware's stored password.
    // Returns false (and leaves *err set to a NON-secret message) on read error
    // or empty password. Never logs file contents.
    static bool from_password_file(const std::string& path, AuthConfig& out,
                                   std::string& err);

    bool password_configured() const { return !password_.empty(); }
    const std::vector<uint8_t>& password() const { return password_; }

private:
    std::vector<uint8_t> password_;
};

// Fill out[16] with cryptographically secure random bytes. Returns false on
// failure (the caller must then fail closed and not start auth).
bool random_nonce(uint8_t out[extradio::kAuthNonceSize]);

// Compute raw HMAC-SHA256(key, msg) into out[32]. Returns false on backend
// failure (caller fails closed). Length is exactly kAuthResponseSize.
bool hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[extradio::kAuthResponseSize]);

// Constant-time verification of a client AUTH_RESPONSE against the expected HMAC
// of (password, nonce). Returns true only on an exact match. Never logs.
bool verify_auth_response(const AuthConfig& cfg,
                          const uint8_t nonce[extradio::kAuthNonceSize],
                          const uint8_t* response, size_t response_len);

}  // namespace mebridge

#endif  // MEBRIDGE_AUTH_HMAC_AUTH_H
