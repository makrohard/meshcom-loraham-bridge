// hmac_auth.cpp — see hmac_auth.h.
//
// SPDX-License-Identifier: MIT

#include "auth/hmac_auth.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstdio>

namespace mebridge {

AuthConfig AuthConfig::from_password_bytes(const uint8_t* key, size_t len) {
    AuthConfig c;
    if (key && len) c.password_.assign(key, key + len);
    return c;
}

bool AuthConfig::from_password_file(const std::string& path, AuthConfig& out,
                                    std::string& err) {
    err.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open password file";  // path/content intentionally omitted
        return false;
    }
    // A pre-shared key / passphrase is small; cap the read so a mistaken path
    // (a huge file or a device like /dev/zero) cannot exhaust memory.
    constexpr size_t kMaxPasswordFileBytes = 4096;
    std::vector<uint8_t> data;
    uint8_t chunk[512];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (data.size() + n > kMaxPasswordFileBytes) {
            std::fclose(f);
            err = "password file too large";
            return false;
        }
        data.insert(data.end(), chunk, chunk + n);
    }
    const bool read_err = std::ferror(f) != 0;
    std::fclose(f);
    if (read_err) {
        err = "error reading password file";
        return false;
    }
    // Strip a single trailing newline (LF or CRLF) commonly added by editors.
    if (!data.empty() && data.back() == '\n') {
        data.pop_back();
        if (!data.empty() && data.back() == '\r') data.pop_back();
    }
    if (data.empty()) {
        err = "password file is empty";
        return false;
    }
    out.password_ = std::move(data);
    return true;
}

bool random_nonce(uint8_t out[extradio::kAuthNonceSize]) {
    return RAND_bytes(out, static_cast<int>(extradio::kAuthNonceSize)) == 1;
}

bool hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[extradio::kAuthResponseSize]) {
    unsigned int out_len = 0;
    const unsigned char* r = HMAC(EVP_sha256(), key, static_cast<int>(key_len),
                                  msg, msg_len, out, &out_len);
    return r != nullptr && out_len == extradio::kAuthResponseSize;
}

bool verify_auth_response(const AuthConfig& cfg,
                          const uint8_t nonce[extradio::kAuthNonceSize],
                          const uint8_t* response, size_t response_len) {
    if (!cfg.password_configured()) return false;  // never called in open mode
    if (!response || response_len != extradio::kAuthResponseSize) return false;
    uint8_t expected[extradio::kAuthResponseSize];
    if (!hmac_sha256(cfg.password().data(), cfg.password().size(),
                     nonce, extradio::kAuthNonceSize, expected)) {
        return false;
    }
    // Constant-time compare; CRYPTO_memcmp returns 0 on equality.
    return CRYPTO_memcmp(expected, response, extradio::kAuthResponseSize) == 0;
}

}  // namespace mebridge
