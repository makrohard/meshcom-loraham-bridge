// xr_session.cpp — see xr_session.h.
//
// SPDX-License-Identifier: MIT

#include "xr/xr_session.h"

#include <cstring>

namespace mebridge {

using namespace extradio;

namespace {

// Big-endian writers (the XR wire is big-endian). Kept local so the vendored
// codec stays verbatim; layout matches external_radio_protocol.cpp::packConfig.
inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}
inline void put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

// Serialize RadioConfig into 17 big-endian bytes (mirrors the vendored codec).
void pack_config(uint8_t* p, const RadioConfig& c) {
    put32(p + 0, c.freq_hz);
    put32(p + 4, c.bw_hz);
    p[8] = c.sf;
    p[9] = c.cr_denom;
    put16(p + 10, c.sync_word);
    put16(p + 12, c.preamble);
    p[14] = static_cast<uint8_t>(c.tx_power_dbm);
    p[15] = c.crc;
    p[16] = c.ldro;
}

// Only these message types are legal from the firmware (client -> bridge).
bool is_client_message(uint8_t type) {
    switch (type) {
        case MSG_HELLO:
        case MSG_AUTH_RESPONSE:
        case MSG_CONFIGURE:
        case MSG_TX_REQUEST:
        case MSG_PONG:
        case MSG_ERROR:
            return true;
        default:
            return false;  // server-only types (incl. PING) are illegal inbound
    }
}

uint8_t tx_outcome_to_wire(TxOutcome o) {
    switch (o) {
        case TxOutcome::Success:     return TXR_SUCCESS;
        case TxOutcome::ChannelBusy: return TXR_CHANNEL_BUSY;
        case TxOutcome::Timeout:     return TXR_TIMEOUT;
        case TxOutcome::RadioError:  return TXR_RADIO_ERROR;
    }
    return TXR_RADIO_ERROR;
}

}  // namespace

const char* close_reason_name(CloseReason r) {
    switch (r) {
        case CloseReason::None:             return "none";
        case CloseReason::OutputOverflow:   return "output-overflow";
        case CloseReason::MalformedFrame:   return "malformed-frame";
        case CloseReason::ParserError:      return "parser-error";
        case CloseReason::BadState:         return "bad-state";
        case CloseReason::RemoteError:      return "remote-error";
        case CloseReason::AuthFailed:       return "auth-failed";
        case CloseReason::AuthError:        return "auth-error";
        case CloseReason::ConfigFailed:     return "config-failed";
        case CloseReason::BackendFailure:   return "backend-failure";
        case CloseReason::HandshakeTimeout: return "handshake-timeout";
        case CloseReason::AuthTimeout:      return "auth-timeout";
        case CloseReason::ConfigTimeout:    return "config-timeout";
        case CloseReason::PongTimeout:      return "pong-timeout";
        case CloseReason::TxTimeoutUncertain: return "tx-timeout-uncertain";
        case CloseReason::InternalError:    return "internal-error";
    }
    return "unknown";
}

XrSession::Timeouts XrSession::default_timeouts() {
    return Timeouts{
        /*handshake_ms*/ 5000,
        /*auth_ms*/ 5000,
        /*config_ms*/ 5000,
        /*tx_ms*/ 10000,
        /*ping_interval_ms*/ 15000,
        /*pong_timeout_ms*/ 10000,
    };
}

XrSession::XrSession(RadioBackend& backend, const AuthConfig& auth,
                     const Clock& clock, const Timeouts& to, size_t max_outbox)
    : backend_(backend), auth_(auth), clock_(clock), to_(to), outbox_(max_outbox) {
    parserReset(parser_);
}

void XrSession::open() {
    parserReset(parser_);
    outbox_.clear();
    phase_ = Phase::WaitHello;
    close_reason_ = CloseReason::None;
    tx_in_flight_ = false;
    awaiting_pong_ = false;
    backend_.start();
    arm(to_.handshake_ms, CloseReason::HandshakeTimeout);
}

void XrSession::arm(uint64_t timeout_ms, CloseReason on_expire) {
    deadline_active_ = true;
    deadline_at_ = clock_.now_ms() + timeout_ms;
    deadline_reason_ = on_expire;
}

void XrSession::close(CloseReason reason) {
    if (phase_ == Phase::Closed) return;
    // Record the first close reason; leave the outbox intact so the owner can
    // flush a final AUTH_FAIL / CONFIG_RESULT-failure frame before closing the fd.
    close_reason_ = reason;
    phase_ = Phase::Closed;
    deadline_active_ = false;
    tx_in_flight_ = false;
}

bool XrSession::emit(uint8_t type, uint16_t seq, const uint8_t* payload, uint16_t len) {
    uint8_t buf[kMaxFrame];
    const size_t n = encode(buf, sizeof(buf), type, seq, payload, len);
    if (n == 0) { close(CloseReason::InternalError); return false; }
    if (!outbox_.append(buf, n)) { close(CloseReason::OutputOverflow); return false; }
    return true;
}

void XrSession::feed(const uint8_t* data, size_t n) {
    if (phase_ == Phase::Closed) return;

    size_t off = 0;
    for (;;) {
        Frame f;
        uint8_t err = ERR_NONE;
        PopResult r = parserPop(parser_, f, err);
        if (r == POP_GOT_FRAME) {
            handle_frame(f);
            if (phase_ == Phase::Closed) return;
            continue;
        }
        if (r == POP_ERROR) { close(CloseReason::ParserError); return; }
        // POP_NEED_MORE: feed more bytes from this read, if any remain.
        if (off >= n) break;
        size_t took = 0;
        ParserPushStatus st = parserPush(parser_, data + off, n - off, took);
        off += took;
        if (st == PARSER_PUSH_INVALID_INPUT) { close(CloseReason::ParserError); return; }
        if (st == PARSER_PUSH_NEED_DRAIN && took == 0) {
            // A full buffer that still cannot yield a frame is impossible for a
            // legal stream; fail closed.
            close(CloseReason::ParserError);
            return;
        }
    }
}

void XrSession::handle_frame(const extradio::Frame& f) {
    // 1) Strict structural validation (length/seq/fields) before anything else.
    const uint8_t verr = validate(f);
    if (verr != ERR_NONE) { close(CloseReason::MalformedFrame); return; }

    // 2) Direction check: reject any server-only message arriving from the client.
    if (!is_client_message(f.type)) { close(CloseReason::BadState); return; }

    // 3) Connection-wide control messages.
    if (f.type == MSG_ERROR) { close(CloseReason::RemoteError); return; }

    // 4) Phase-specific handling.
    switch (phase_) {
        case Phase::WaitHello:
            if (f.type != MSG_HELLO) { close(CloseReason::BadState); return; }
            // HELLO_ACK echoes the protocol version byte.
            {
                const uint8_t ver[1] = { kVersion };
                if (!emit(MSG_HELLO_ACK, 0, ver, 1)) return;
            }
            if (auth_.password_configured()) {
                if (!random_nonce(nonce_)) { close(CloseReason::AuthError); return; }
                if (!emit(MSG_AUTH_CHALLENGE, 0, nonce_, kAuthNonceSize)) return;
                phase_ = Phase::WaitAuth;
                arm(to_.auth_ms, CloseReason::AuthTimeout);
            } else {
                // Open mode: AUTH_OK immediately after HELLO_ACK.
                const uint8_t ok[1] = { AUTH_OK };
                if (!emit(MSG_AUTH_RESULT, 0, ok, 1)) return;
                phase_ = Phase::WaitConfigure;
                arm(to_.config_ms, CloseReason::ConfigTimeout);
            }
            return;

        case Phase::WaitAuth:
            if (f.type != MSG_AUTH_RESPONSE) { close(CloseReason::BadState); return; }
            if (verify_auth_response(auth_, nonce_, f.payload, f.len)) {
                const uint8_t ok[1] = { AUTH_OK };
                if (!emit(MSG_AUTH_RESULT, 0, ok, 1)) return;
                phase_ = Phase::WaitConfigure;
                arm(to_.config_ms, CloseReason::ConfigTimeout);
            } else {
                const uint8_t fail[1] = { AUTH_FAIL };
                emit(MSG_AUTH_RESULT, 0, fail, 1);  // best-effort: deliver then close
                close(CloseReason::AuthFailed);
            }
            return;

        case Phase::WaitConfigure: {
            if (f.type != MSG_CONFIGURE) { close(CloseReason::BadState); return; }
            RadioConfig requested;
            if (!decodeConfig(f, requested) || !radioConfigValid(requested)) {
                const uint8_t bad[1] = { CFG_INVALID };
                emit(MSG_CONFIG_RESULT, 0, bad, 1);
                close(CloseReason::ConfigFailed);
                return;
            }
            const ConfigureResult res = backend_.configure(requested);
            // CONFIG_RESULT success is sent ONLY when the backend applied the
            // request AND the effective config matches it exactly.
            if (!res.applied || !configEqual(res.effective, requested)) {
                const uint8_t bad[1] = { CFG_UNSUPPORTED };
                emit(MSG_CONFIG_RESULT, 0, bad, 1);
                close(CloseReason::ConfigFailed);
                return;
            }
            applied_cfg_ = res.effective;
            uint8_t body[1 + kConfigPayloadSize];
            body[0] = CFG_OK;
            pack_config(body + 1, res.effective);
            if (!emit(MSG_CONFIG_RESULT, 0, body, sizeof(body))) return;
            enter_ready();
            return;
        }

        case Phase::Ready:
            handle_ready_frame(f);
            return;

        case Phase::Closed:
            return;
    }
}

void XrSession::enter_ready() {
    phase_ = Phase::Ready;
    disarm();
    awaiting_pong_ = false;
    next_ping_at_ = clock_.now_ms() + to_.ping_interval_ms;
}

void XrSession::handle_ready_frame(const extradio::Frame& f) {
    switch (f.type) {
        case MSG_PONG:
            if (!awaiting_pong_) { close(CloseReason::BadState); return; }  // unsolicited
            awaiting_pong_ = false;
            next_ping_at_ = clock_.now_ms() + to_.ping_interval_ms;
            return;

        case MSG_TX_REQUEST:
            if (tx_in_flight_) { close(CloseReason::BadState); return; }  // one in flight
            tx_seq_ = f.seq;  // validate() guaranteed seq != 0; echoed verbatim later
            if (!backend_.submit_tx(f.payload, f.len)) {
                // Backend refused: uncertain. Close so the firmware resolves the
                // TX as UNKNOWN — never a false success.
                close(CloseReason::BackendFailure);
                return;
            }
            tx_in_flight_ = true;
            tx_deadline_at_ = clock_.now_ms() + to_.tx_ms;
            return;

        case MSG_HELLO:
        case MSG_CONFIGURE:
        case MSG_AUTH_RESPONSE:
            // Legal client types, but not in Ready: illegal transition.
            close(CloseReason::BadState);
            return;

        default:
            close(CloseReason::BadState);
            return;
    }
}

void XrSession::map_and_send_tx_result(TxOutcome outcome) {
    const uint8_t code[1] = { tx_outcome_to_wire(outcome) };
    tx_in_flight_ = false;
    emit(MSG_TX_RESULT, tx_seq_, code, 1);
}

void XrSession::on_rx(const RxEvent& rx) {
    if (phase_ != Phase::Ready) return;             // never before exact config
    if (!rxPayloadAcceptable(rx.len)) return;       // drop zero-length / oversized
    uint8_t payload[6 + kMaxLoraPayload];
    put16(payload + 0, static_cast<uint16_t>(rx.rssi_cdbm));
    put16(payload + 2, static_cast<uint16_t>(rx.snr_cdb));
    put16(payload + 4, rx.len);
    std::memcpy(payload + 6, rx.data, rx.len);
    emit(MSG_RX_PACKET, 0, payload, static_cast<uint16_t>(6 + rx.len));
}

void XrSession::on_tx_complete(TxOutcome outcome) {
    if (!tx_in_flight_) return;   // stale/late completion: ignore (no second result)
    map_and_send_tx_result(outcome);
}

void XrSession::on_backend_failure() {
    // Fatal for this client: close without a TX_RESULT so any in-flight TX is
    // resolved as uncertain/unknown on the firmware, never as success.
    close(CloseReason::BackendFailure);
}

void XrSession::tick() {
    if (phase_ == Phase::Closed) return;
    const uint64_t now = clock_.now_ms();

    // Phase deadlines (handshake/auth/config).
    if (deadline_active_ && now >= deadline_at_) {
        close(deadline_reason_);
        return;
    }

    if (phase_ != Phase::Ready) return;

    // Bridge-side TX ceiling. We do NOT know whether the backend/daemon
    // transmitted, so we never fabricate a terminal TX_RESULT here. Hand
    // ownership to the backend's recovery (it keeps tracking the TX downstream)
    // and close the session — the firmware then resolves this TX as UNKNOWN
    // (uncertain, never resent), and the backend refuses new TX until the
    // outstanding one is provably clear.
    if (tx_in_flight_ && now >= tx_deadline_at_) {
        backend_.abandon_pending_tx();
        close(CloseReason::TxTimeoutUncertain);
        return;
    }

    // Keepalive: originate PING only in Ready; missing PONG closes.
    if (awaiting_pong_) {
        if (now >= pong_deadline_at_) { close(CloseReason::PongTimeout); return; }
    } else if (now >= next_ping_at_) {
        if (!emit(MSG_PING, 0, nullptr, 0)) return;
        awaiting_pong_ = true;
        pong_deadline_at_ = now + to_.pong_timeout_ms;
    }
}

}  // namespace mebridge
