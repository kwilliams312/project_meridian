// SPDX-License-Identifier: Apache-2.0
//
// worldd — IF-3 session establishment implementation (issue #84). See
// world_session.h for the provenance + clean-room statement and the design of
// the grant consume + AEAD nonce scheme.

#include "world_session.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <cstring>

#include "meridian/core/log.hpp"

namespace meridian::worldd {
namespace {

namespace log = meridian::core::log;
constexpr const char* kCat = "worldd";

// HKDF-SHA256 info label (SAD §5.2 "meridian-world-v1"). The per-direction byte
// (0 = c2s, 1 = s2c) is appended so the two directions derive independent keys.
constexpr char kHkdfInfoBase[] = "meridian-world-v1";

// Derive one 32-byte AEAD key with HKDF-SHA256 (extract+expand) over the OpenSSL
// 3.x EVP_KDF interface. ikm = session_key, salt = "" (empty — the session_key
// is already high-entropy random from authd), info = "meridian-world-v1" ∥ dir.
bool hkdf_sha256(const Bytes& ikm, std::uint8_t direction,
                 std::array<std::uint8_t, kAeadKeyBytes>& out) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (kdf == nullptr) return false;
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (ctx == nullptr) return false;

    // info = base label ∥ direction byte.
    std::vector<std::uint8_t> info;
    info.reserve(sizeof(kHkdfInfoBase) - 1 + 1);
    info.insert(info.end(), kHkdfInfoBase, kHkdfInfoBase + (sizeof(kHkdfInfoBase) - 1));
    info.push_back(direction);

    char digest[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string("digest", digest, 0),
        OSSL_PARAM_construct_octet_string(
            "key", const_cast<std::uint8_t*>(ikm.data()), ikm.size()),
        OSSL_PARAM_construct_octet_string("info", info.data(), info.size()),
        // salt omitted -> HKDF uses an all-zero salt (RFC 5869 default).
        OSSL_PARAM_construct_end(),
    };

    int rc = EVP_KDF_derive(ctx, out.data(), out.size(), params);
    EVP_KDF_CTX_free(ctx);
    return rc == 1;
}

// Build the 96-bit ChaCha20-Poly1305 nonce for (direction, seq):
//   [ direction : 1 ][ 0 : 3 ][ seq : 8, big-endian ]  (SAD §5.2)
std::array<std::uint8_t, kAeadNonceBytes> make_nonce(Direction dir,
                                                     std::uint64_t seq) {
    std::array<std::uint8_t, kAeadNonceBytes> n{};
    n[0] = static_cast<std::uint8_t>(dir);
    // bytes 1..3 stay zero.
    for (int i = 0; i < 8; ++i) {
        n[4 + i] = static_cast<std::uint8_t>((seq >> (8 * (7 - i))) & 0xFF);
    }
    return n;
}

// One-shot ChaCha20-Poly1305 seal. Writes ciphertext (same length as plaintext)
// followed by the 16-byte tag into `out`. Returns false on any OpenSSL failure.
bool aead_seal(const std::array<std::uint8_t, kAeadKeyBytes>& key,
               const std::array<std::uint8_t, kAeadNonceBytes>& nonce,
               const Bytes& aad, const Bytes& plaintext, Bytes& out) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return false;
    bool ok = false;
    int len = 0;
    out.resize(plaintext.size() + kAeadTagBytes);

    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1)
        goto done;
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1)
        goto done;
    if (!aad.empty()) {
        if (EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(),
                              static_cast<int>(aad.size())) != 1)
            goto done;
    }
    if (EVP_EncryptUpdate(ctx, out.data(), &len,
                          plaintext.empty() ? reinterpret_cast<const std::uint8_t*>("")
                                            : plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1)
        goto done;
    {
        int final_len = 0;
        if (EVP_EncryptFinal_ex(ctx, out.data() + len, &final_len) != 1) goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kAeadTagBytes),
                            out.data() + plaintext.size()) != 1)
        goto done;
    ok = true;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

// One-shot ChaCha20-Poly1305 open. `in` is ciphertext ∥ tag. On a valid tag,
// writes the plaintext into `out` and returns true; on any tag mismatch or
// OpenSSL failure returns false (out is left cleared).
bool aead_open(const std::array<std::uint8_t, kAeadKeyBytes>& key,
               const std::array<std::uint8_t, kAeadNonceBytes>& nonce,
               const Bytes& aad, const Bytes& in, Bytes& out) {
    if (in.size() < kAeadTagBytes) return false;
    const std::size_t ct_len = in.size() - kAeadTagBytes;
    const std::uint8_t* tag = in.data() + ct_len;

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) return false;
    bool ok = false;
    int len = 0;
    out.assign(ct_len, 0);

    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1)
        goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN,
                            static_cast<int>(nonce.size()), nullptr) != 1)
        goto done;
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce.data()) != 1)
        goto done;
    if (!aad.empty()) {
        if (EVP_DecryptUpdate(ctx, nullptr, &len, aad.data(),
                              static_cast<int>(aad.size())) != 1)
            goto done;
    }
    if (ct_len > 0) {
        if (EVP_DecryptUpdate(ctx, out.data(), &len, in.data(),
                              static_cast<int>(ct_len)) != 1)
            goto done;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kAeadTagBytes),
                            const_cast<std::uint8_t*>(tag)) != 1)
        goto done;
    {
        int final_len = 0;
        // Returns > 0 only if the tag verified.
        if (EVP_DecryptFinal_ex(ctx, out.data() + len, &final_len) <= 0) goto done;
    }
    ok = true;
done:
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) out.clear();
    return ok;
}

// Parse a decimal string cell to u64 (0 on absent/garbage — callers validate).
std::uint64_t cell_u64(const db::Cell& c) {
    if (!c.has_value()) return 0;
    return std::strtoull(c->c_str(), nullptr, 10);
}

}  // namespace

// ---------------------------------------------------------------------------
// WorldSession
// ---------------------------------------------------------------------------

WorldSession::WorldSession(const Bytes& session_key) {
    if (session_key.size() != kAeadKeyBytes) {
        throw net::TlsError("WorldSession: session_key must be 32 bytes, got " +
                            std::to_string(session_key.size()));
    }
    if (!hkdf_sha256(session_key, static_cast<std::uint8_t>(Direction::kClientToServer), k_c2s_) ||
        !hkdf_sha256(session_key, static_cast<std::uint8_t>(Direction::kServerToClient), k_s2c_)) {
        throw net::TlsError("WorldSession: HKDF-SHA256 key derivation failed");
    }
}

Bytes WorldSession::seal(Direction dir, const Bytes& plaintext, const Bytes& aad,
                         std::uint64_t& out_seq) {
    std::uint64_t& counter = (dir == Direction::kClientToServer) ? seq_c2s_ : seq_s2c_;
    if (counter == UINT64_MAX) {
        // A nonce would repeat on the next frame — refuse rather than reuse.
        throw net::TlsError("WorldSession: nonce counter exhausted (rekey required)");
    }
    const std::array<std::uint8_t, kAeadKeyBytes>& key =
        (dir == Direction::kClientToServer) ? k_c2s_ : k_s2c_;
    out_seq = counter;
    auto nonce = make_nonce(dir, counter);
    Bytes out;
    if (!aead_seal(key, nonce, aad, plaintext, out)) {
        throw net::TlsError("WorldSession: ChaCha20-Poly1305 seal failed");
    }
    ++counter;
    return out;
}

std::optional<Bytes> WorldSession::open(Direction dir, const Bytes& ciphertext_and_tag,
                                        std::uint64_t seq, const Bytes& aad) {
    const std::array<std::uint8_t, kAeadKeyBytes>& key =
        (dir == Direction::kClientToServer) ? k_c2s_ : k_s2c_;
    auto nonce = make_nonce(dir, seq);
    Bytes out;
    if (!aead_open(key, nonce, aad, ciphertext_and_tag, out)) {
        return std::nullopt;  // tampered / wrong seq / wrong aad — authentication failed
    }
    return out;
}

std::uint64_t WorldSession::next_seq(Direction dir) const {
    return (dir == Direction::kClientToServer) ? seq_c2s_ : seq_s2c_;
}

const std::array<std::uint8_t, kAeadKeyBytes>& WorldSession::key(Direction dir) const {
    return (dir == Direction::kClientToServer) ? k_c2s_ : k_s2c_;
}

// ---------------------------------------------------------------------------
// Grant validation + single-use consume
// ---------------------------------------------------------------------------

std::optional<GrantConsumed> validate_and_consume_grant(
    db::Connection& db, std::uint64_t grant_id, std::uint32_t expected_realm_id,
    GrantReject& out_reject) {
    out_reject = GrantReject::kUnknown;
    const std::string grant_str = std::to_string(grant_id);

    try {
        // THE single-use guard: one atomic UPDATE. affected_rows == 1 means this
        // caller won the row (it was unconsumed AND unexpired at the instant the
        // row lock was taken). A replay / expired / unknown grant affects 0 rows.
        db::Result upd = db.execute(
            "UPDATE session_grant SET consumed_at = UTC_TIMESTAMP() "
            "WHERE grant_id = ? AND consumed_at IS NULL "
            "AND expires_at > UTC_TIMESTAMP()",
            {db::Param{grant_str}});

        if (upd.affected_rows != 1) {
            // Rejected. Read the row (if any) to classify WHY for logging — this
            // read is NOT the guard (the UPDATE already lost the race / found
            // nothing); it only distinguishes unknown / expired / already-consumed
            // so the server log is useful. The wire reason is GRANT_INVALID for
            // all three (no oracle to the client).
            db::Result sel = db.execute(
                "SELECT consumed_at, (expires_at <= UTC_TIMESTAMP()) AS expired "
                "FROM session_grant WHERE grant_id = ?",
                {db::Param{grant_str}});
            if (sel.rows.empty()) {
                out_reject = GrantReject::kUnknown;
            } else {
                const db::Row& r = sel.rows[0];
                bool consumed = r[0].has_value();
                bool expired = cell_u64(r[1]) != 0;
                out_reject = consumed ? GrantReject::kAlreadyConsumed
                                      : (expired ? GrantReject::kExpired
                                                 : GrantReject::kUnknown);
            }
            return std::nullopt;
        }

        // Won the row: fetch the bound {account, realm, session_key} plus the
        // account's GM level (account.gm_level) via a JOIN — the session learns
        // its GM permission level here, at the handshake, from the same read
        // (OPS-02a, #417). The FK guarantees the account row exists (the grant
        // could not have been written without it), so the JOIN never drops the
        // row. Parameterized — grant_id binds as a decimal string (full u64).
        db::Result sel = db.execute(
            "SELECT g.account_id, g.realm_id, g.session_key, a.gm_level "
            "FROM session_grant g JOIN account a ON a.id = g.account_id "
            "WHERE g.grant_id = ?",
            {db::Param{grant_str}});
        if (sel.rows.size() != 1) {
            // Extremely unlikely (we just UPDATEd it) — treat as a DB error.
            out_reject = GrantReject::kDbError;
            return std::nullopt;
        }
        const db::Row& r = sel.rows[0];
        GrantConsumed g;
        g.account_id = cell_u64(r[0]);
        g.realm_id = static_cast<std::uint32_t>(cell_u64(r[1]));
        if (r[2].has_value()) g.session_key.assign(r[2]->begin(), r[2]->end());
        g.gm_level = static_cast<std::uint8_t>(cell_u64(r[3]));

        // Realm binding (SAD §5.3). The grant is already consumed at this point;
        // a wrong-realm grant is still a reject (this worldd is not its realm),
        // but the single-use property is preserved — the grant is spent and
        // cannot be replayed against the correct realm either.
        if (expected_realm_id != 0 && g.realm_id != expected_realm_id) {
            out_reject = GrantReject::kWrongRealm;
            return std::nullopt;
        }
        if (g.session_key.size() != kAeadKeyBytes) {
            out_reject = GrantReject::kDbError;
            return std::nullopt;
        }
        return g;
    } catch (const db::DbError& e) {
        log::warn(kCat, std::string("grant consume DbError: ") + e.what());
        out_reject = GrantReject::kDbError;
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Enter-world: server-authoritative character load (D-35 / #341)
// ---------------------------------------------------------------------------

std::optional<LoadedCharacter> load_owned_character(db::Connection& char_db,
                                                    std::uint64_t account_id,
                                                    std::uint64_t character_id) {
    // Ownership is the WHERE predicate: a row comes back ONLY when this exact
    // character id belongs to this account. A nonexistent id, or one owned by a
    // different account, matches zero rows -> nullopt -> the caller rejects entry.
    // The server never fabricates a character (the D-11 placeholder is gone).
    // A db::DbError propagates to the caller (mapped to ENTER_WORLD INTERNAL).
    db::Result r = char_db.execute(
        "SELECT id, name, class, level FROM `character` "
        "WHERE id = ? AND account_id = ? LIMIT 1",
        {db::Param{std::to_string(character_id)},
         db::Param{std::to_string(account_id)}});
    if (r.rows.size() != 1) return std::nullopt;

    const db::Row& row = r.rows[0];
    LoadedCharacter c;
    c.char_guid = cell_u64(row[0]);
    if (row[1].has_value()) c.name = *row[1];
    c.class_id = static_cast<std::uint8_t>(cell_u64(row[2]));
    c.level = static_cast<std::uint16_t>(cell_u64(row[3]));
    return c;
}

}  // namespace meridian::worldd
