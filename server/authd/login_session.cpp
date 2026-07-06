// SPDX-License-Identifier: Apache-2.0
//
// authd — IF-1 login state machine implementation (server SAD §5.1, §3.1;
// issue #79). See login_session.h for the provenance + clean-room statement.
//
// Flow (SAD §5.1, wire contract schema/net/auth.fbs):
//   ClientHello{build,proto_ver}  -> ServerHello{build,proto_ver} | Error
//   SrpStart{account}             -> SrpChallenge{salt,B}
//   SrpProof{A,M1}                -> AuthResult{m2} | AuthResult{error}
//   RealmListRequest              -> RealmList{realms[]}
//   RealmSelect{realm_id}         -> SessionGrant{grant_id,session_key,rc} | Error
//   then authd closes (no long-lived auth connections — SAD §5.1).
//
// Each inbound frame is a FlatBuffer root table; auth.fbs declares no root_type,
// so we decode with the generic flatbuffers::GetRoot<T>/VerifyBuffer<T> (as the
// proto round-trip proof does) — the message type at each step is fixed by the
// strictly-sequenced handshake, so there is no opcode to dispatch on.

#include "login_session.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <ctime>

#include "auth_generated.h"

namespace meridian::authd {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;

using net::Bytes;

// ---- FlatBuffer encode helpers ---------------------------------------------
// Each helper finishes a builder on one message table and returns its bytes as
// a net::Bytes ready for Session::write_frame (the u32 LE length is prepended by
// the transport layer, so we hand it the bare FlatBuffer root).

Bytes finish(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

Bytes encode_server_hello(std::uint32_t build, std::uint16_t proto_ver) {
    fb::FlatBufferBuilder b;
    auto h = mn::CreateServerHello(b, build, proto_ver);
    b.Finish(h);
    return finish(b);
}

Bytes encode_error(mn::AuthErrorCode code, const char* message) {
    fb::FlatBufferBuilder b;
    auto msg = b.CreateString(message);
    auto e = mn::CreateError(b, code, msg);
    b.Finish(e);
    return finish(b);
}

Bytes encode_srp_challenge(const Bytes& salt, const Bytes& B) {
    fb::FlatBufferBuilder b;
    auto salt_v = b.CreateVector(salt);
    auto b_v = b.CreateVector(B);
    auto c = mn::CreateSrpChallenge(b, salt_v, b_v);
    b.Finish(c);
    return finish(b);
}

Bytes encode_auth_result_ok(const Bytes& M2) {
    fb::FlatBufferBuilder b;
    auto m2_v = b.CreateVector(M2);
    // success=true, m2 populated, no error (auth.fbs AuthResult contract).
    auto r = mn::CreateAuthResult(b, /*success=*/true, m2_v, /*error=*/0);
    b.Finish(r);
    return finish(b);
}

Bytes encode_auth_result_error(mn::AuthErrorCode code, const char* message) {
    fb::FlatBufferBuilder b;
    auto msg = b.CreateString(message);
    auto err = mn::CreateError(b, code, msg);
    // success=false, no m2, error populated.
    auto r = mn::CreateAuthResult(b, /*success=*/false, /*m2=*/0, err);
    b.Finish(r);
    return finish(b);
}

Bytes encode_session_grant(std::uint64_t grant_id, const Bytes& session_key,
                           std::uint32_t reconnect_window_ms) {
    fb::FlatBufferBuilder b;
    auto key_v = b.CreateVector(session_key);
    auto g = mn::CreateSessionGrant(b, grant_id, key_v, reconnect_window_ms);
    b.Finish(g);
    return finish(b);
}

// ---- FlatBuffer decode helpers ---------------------------------------------
// Verify then GetRoot; return nullptr on a malformed/oversized buffer so the
// caller can treat it as a protocol error (untrusted peer — never trust length
// or offsets without the Verifier).

template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

// Copy a FlatBuffer [ubyte] vector into a net::Bytes (empty if absent).
Bytes vec_to_bytes(const fb::Vector<std::uint8_t>* v) {
    if (v == nullptr) return {};
    return Bytes(v->data(), v->data() + v->size());
}

// ---- crypto / random helpers -----------------------------------------------

// 32 random bytes for a session_key (SAD §4.1 BINARY(32)). Throws on RNG
// failure — a login must never proceed with weak key material.
Bytes random_key32() {
    Bytes k(32);
    if (RAND_bytes(k.data(), static_cast<int>(k.size())) != 1) {
        throw net::TlsError("RAND_bytes failed for session_key");
    }
    return k;
}

// A random, non-zero u64 grant_id (SAD §4.1: random, NOT auto-increment, so it
// is unguessable / non-enumerable). Zero is avoided so callers can treat 0 as
// "no grant".
std::uint64_t random_grant_id() {
    std::uint64_t id = 0;
    do {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(&id), sizeof(id)) != 1) {
            throw net::TlsError("RAND_bytes failed for grant_id");
        }
    } while (id == 0);
    return id;
}

// Anti-enumeration fake salt for an unknown username (SAD §2.1). Deterministic
// per {username, server secret} so a repeat SrpStart for the same missing user
// yields the SAME salt (a real account's salt is stable too — a varying salt
// would itself be an oracle). We derive SHA-256(secret || username) and take 32
// bytes. The secret is process-random: it need not persist (the goal is only to
// deny an attacker a "user exists?" signal within a run), and a fresh secret per
// process is strictly safer.
const std::array<std::uint8_t, 32>& enumeration_secret() {
    static const std::array<std::uint8_t, 32> secret = [] {
        std::array<std::uint8_t, 32> s{};
        if (RAND_bytes(s.data(), static_cast<int>(s.size())) != 1) {
            // Extremely unlikely; fall back to a fixed value rather than crash —
            // the anti-enumeration property degrades but login still works.
            s.fill(0x5A);
        }
        return s;
    }();
    return secret;
}

Bytes fake_salt_for(std::string_view username) {
    // SHA-256(secret || username) via the non-deprecated EVP one-shot digest.
    const auto& secret = enumeration_secret();
    Bytes input;
    input.reserve(secret.size() + username.size());
    input.insert(input.end(), secret.begin(), secret.end());
    input.insert(input.end(), username.begin(), username.end());

    Bytes salt(32);
    unsigned int out_len = 0;
    if (EVP_Digest(input.data(), input.size(), salt.data(), &out_len,
                   EVP_sha256(), nullptr) != 1 ||
        out_len != 32) {
        throw net::TlsError("EVP_Digest(SHA-256) failed for anti-enumeration salt");
    }
    return salt;
}

// Format a UTC DATETIME string "YYYY-MM-DD HH:MM:SS" `ttl` seconds from now, for
// the session_grant.expires_at column (SAD §4.1 — server writes UTC).
std::string utc_expires_in(std::uint32_t ttl_seconds) {
    std::time_t now = std::time(nullptr);
    std::time_t exp = now + static_cast<std::time_t>(ttl_seconds);
    std::tm tm_utc{};
    gmtime_r(&exp, &tm_utc);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_utc);
    return std::string(buf);
}

// Parse a decimal string cell to u64 (0 on absent/garbage — callers validate).
std::uint64_t cell_u64(const db::Cell& c) {
    if (!c.has_value()) return 0;
    return std::strtoull(c->c_str(), nullptr, 10);
}

// Bind a Bytes as a DB blob param.
db::Param blob(const Bytes& b) {
    return db::Param{db::Bytes_t(b.begin(), b.end())};
}

}  // namespace

LoginResult run_login(net::Session& sess, db::Connection& db,
                      const LoginConfig& cfg) {
    LoginResult result;

    // ---- 1. ClientHello -> ServerHello | Error ------------------------------
    Bytes frame;
    try {
        frame = sess.read_frame();
    } catch (const net::ConnectionClosed&) {
        result.outcome = LoginOutcome::kTransportClosed;
        result.detail = "peer closed before ClientHello";
        return result;
    }

    const mn::ClientHello* hello = decode<mn::ClientHello>(frame);
    if (hello == nullptr) {
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed ClientHello"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable ClientHello";
        return result;
    }

    const std::uint32_t client_build = hello->build();
    if (hello->proto_ver() != cfg.proto_ver) {
        sess.write_frame(encode_error(mn::AuthErrorCode::PROTOCOL_MISMATCH,
                                      "unsupported protocol version"));
        result.outcome = LoginOutcome::kRejectedHello;
        result.detail = "proto_ver mismatch";
        return result;
    }
    if (cfg.build_floor != 0 && client_build < cfg.build_floor) {
        sess.write_frame(encode_error(mn::AuthErrorCode::BUILD_REJECTED,
                                      "client build below floor"));
        result.outcome = LoginOutcome::kRejectedHello;
        result.detail = "build below floor";
        return result;
    }
    sess.write_frame(encode_server_hello(cfg.server_build, cfg.proto_ver));

    // ---- 2. SrpStart{account} -> SrpChallenge{salt,B} -----------------------
    frame = sess.read_frame();
    const mn::SrpStart* start = decode<mn::SrpStart>(frame);
    if (start == nullptr || start->account() == nullptr) {
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed SrpStart"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable SrpStart";
        return result;
    }
    const std::string username = start->account()->str();

    // Load {salt, verifier} for the account (SAD §5.1 "load {salt, verifier}").
    // Parameterized — the username binds through a prepared-statement parameter,
    // never concatenated (backend standards; meridian-db contract).
    db::Result acct = db.execute(
        "SELECT id, srp_salt, srp_verifier FROM account WHERE username = ?",
        {db::Param{username}});

    Bytes salt, verifier;
    std::uint64_t account_id = 0;
    bool known_user = (acct.rows.size() == 1);
    if (known_user) {
        const db::Row& r = acct.rows[0];
        account_id = cell_u64(r[0]);
        if (r[1].has_value()) salt.assign(r[1]->begin(), r[1]->end());
        if (r[2].has_value()) verifier.assign(r[2]->begin(), r[2]->end());
    } else {
        // Anti-enumeration: fabricate a stable fake salt + a throwaway verifier
        // so an unknown user is indistinguishable from a wrong password. We
        // derive a verifier from a random password over the fake salt — the
        // client can never produce a matching M1, so verify() always rejects.
        salt = fake_salt_for(username);
        Bytes fake_pw = random_key32();
        std::string pw_hex;
        pw_hex.reserve(fake_pw.size() * 2);
        static const char* hexd = "0123456789abcdef";
        for (std::uint8_t byte : fake_pw) {
            pw_hex.push_back(hexd[byte >> 4]);
            pw_hex.push_back(hexd[byte & 0x0F]);
        }
        srp::Verifier v =
            srp::make_verifier(username, pw_hex, cfg.srp_params, salt);
        verifier = v.verifier;
    }
    result.account_id = account_id;

    srp::ServerSession srp(username, salt, verifier, cfg.srp_params);
    sess.write_frame(encode_srp_challenge(salt, srp.B()));

    // ---- 3. SrpProof{A,M1} -> AuthResult{m2} | AuthResult{error} ------------
    frame = sess.read_frame();
    const mn::SrpProof* proof = decode<mn::SrpProof>(frame);
    if (proof == nullptr || proof->a() == nullptr || proof->m1() == nullptr) {
        sess.write_frame(encode_auth_result_error(mn::AuthErrorCode::INTERNAL,
                                                  "malformed SrpProof"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable SrpProof";
        return result;
    }
    Bytes A = vec_to_bytes(proof->a());
    Bytes M1 = vec_to_bytes(proof->m1());

    std::optional<Bytes> M2 = srp.verify(A, M1);
    if (!M2.has_value() || !known_user) {
        // Wrong proof, or an unknown user (whose throwaway verifier can never
        // match): same BAD_CREDENTIALS outcome, no grant written (SAD §5.1).
        sess.write_frame(encode_auth_result_error(
            mn::AuthErrorCode::BAD_CREDENTIALS, "authentication failed"));
        result.outcome = LoginOutcome::kRejectedAuth;
        result.detail = known_user ? "bad SRP proof" : "unknown user";
        return result;
    }
    sess.write_frame(encode_auth_result_ok(*M2));

    // ---- 4. RealmListRequest -> RealmList -----------------------------------
    frame = sess.read_frame();
    const mn::RealmListRequest* rlr = decode<mn::RealmListRequest>(frame);
    if (rlr == nullptr) {
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed RealmListRequest"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable RealmListRequest";
        return result;
    }

    // Serve the realm table (SAD §4.1). Build the RealmList FlatBuffer directly
    // from the DB rows.
    db::Result realms = db.execute(
        "SELECT id, name, address, port, population, build_min, build_max, "
        "flags FROM realm ORDER BY id");
    {
        fb::FlatBufferBuilder b;
        std::vector<fb::Offset<mn::RealmRow>> rows;
        rows.reserve(realms.rows.size());
        for (const db::Row& r : realms.rows) {
            auto name = b.CreateString(r[1].has_value() ? *r[1] : "");
            auto addr = b.CreateString(r[2].has_value() ? *r[2] : "");
            rows.push_back(mn::CreateRealmRow(
                b,
                static_cast<std::uint32_t>(cell_u64(r[0])),
                name, addr,
                static_cast<std::uint16_t>(cell_u64(r[3])),
                static_cast<std::uint16_t>(cell_u64(r[4])),  // population bucket
                static_cast<std::uint32_t>(cell_u64(r[5])),
                static_cast<std::uint32_t>(cell_u64(r[6])),
                static_cast<std::uint32_t>(cell_u64(r[7]))));
        }
        auto vec = b.CreateVector(rows);
        auto list = mn::CreateRealmList(b, vec);
        b.Finish(list);
        sess.write_frame(finish(b));
    }

    // ---- 5. RealmSelect{realm_id} -> SessionGrant | Error -------------------
    frame = sess.read_frame();
    const mn::RealmSelect* select = decode<mn::RealmSelect>(frame);
    if (select == nullptr) {
        sess.write_frame(encode_error(mn::AuthErrorCode::INTERNAL,
                                      "malformed RealmSelect"));
        result.outcome = LoginOutcome::kProtocolError;
        result.detail = "undecodable RealmSelect";
        return result;
    }
    const std::uint32_t realm_id = select->realm_id();

    // Validate the realm exists and accepts this client_build (SAD §5.1
    // "REALM_UNAVAILABLE / build range").
    db::Result realm = db.execute(
        "SELECT build_min, build_max FROM realm WHERE id = ?",
        {db::Param{static_cast<std::int64_t>(realm_id)}});
    if (realm.rows.size() != 1) {
        sess.write_frame(encode_error(mn::AuthErrorCode::REALM_UNAVAILABLE,
                                      "no such realm"));
        result.outcome = LoginOutcome::kRejectedRealm;
        result.detail = "unknown realm_id";
        return result;
    }
    {
        const db::Row& r = realm.rows[0];
        std::uint32_t bmin = static_cast<std::uint32_t>(cell_u64(r[0]));
        std::uint32_t bmax = static_cast<std::uint32_t>(cell_u64(r[1]));
        if (client_build < bmin || (bmax != 0 && client_build > bmax)) {
            sess.write_frame(encode_error(mn::AuthErrorCode::BUILD_REJECTED,
                                          "client build outside realm range"));
            result.outcome = LoginOutcome::kRejectedRealm;
            result.detail = "build outside realm range";
            return result;
        }
    }

    // Write the single-use session_grant (SAD §3.1, §4.1): random u64 grant_id,
    // 32-byte random session_key, expires_at = now + ttl, consumed_at NULL,
    // bound to {account, realm, client_build}. Parameterized INSERT.
    std::uint64_t grant_id = random_grant_id();
    Bytes session_key = random_key32();
    std::string expires_at = utc_expires_in(cfg.grant_ttl_seconds);

    // grant_id is a full-range random u64 (SAD §4.1). meridian-db binds an
    // int64 as a SIGNED LONGLONG, which MariaDB rejects ("out of range") for a
    // BIGINT UNSIGNED column once the value exceeds INT64_MAX. Bind it (and
    // account_id, also BIGINT UNSIGNED) as a DECIMAL STRING instead: MariaDB
    // parses the string into the column's unsigned range losslessly, and the
    // prepared statement keeps it injection-proof (it is a bound value, not
    // concatenated SQL).
    db.execute(
        "INSERT INTO session_grant "
        "(grant_id, account_id, realm_id, session_key, client_build, "
        " expires_at) VALUES (?, ?, ?, ?, ?, ?)",
        {db::Param{std::to_string(grant_id)},
         db::Param{std::to_string(account_id)},
         db::Param{static_cast<std::int64_t>(realm_id)},
         blob(session_key),
         db::Param{static_cast<std::int64_t>(client_build)},
         db::Param{expires_at}});

    sess.write_frame(
        encode_session_grant(grant_id, session_key, cfg.reconnect_window_ms));

    // authd closes: no long-lived auth connections (SAD §5.1).
    result.outcome = LoginOutcome::kGranted;
    result.grant_id = grant_id;
    result.detail = "grant issued";
    return result;
}

}  // namespace meridian::authd
