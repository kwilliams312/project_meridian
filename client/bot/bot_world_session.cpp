// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — CLIENT-side IF-2 world session implementation. The HKDF +
// nonce + AEAD scheme is a byte-for-byte mirror of worldd #84
// (server/worldd/world_session.cpp); the frame codec mirrors #82/#83
// (server/worldd/world_dispatch.cpp encode_frame/decode_frame). Clean-room from
// those wire references + the OpenSSL EVP public API docs; no GPL source consulted
// (CONTRIBUTING.md).

#include "bot_world_session.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

namespace meridian::bot {
namespace {

// HKDF-SHA256 info label (SAD §5.2 "meridian-world-v1") — identical to worldd
// #84's kHkdfInfoBase. The per-direction byte (0=c2s, 1=s2c) is appended.
constexpr char kHkdfInfoBase[] = "meridian-world-v1";

// Derive one 32-byte AEAD key with HKDF-SHA256 over the OpenSSL 3.x EVP_KDF
// interface. ikm = session_key, salt = "" (empty; the session_key is already
// high-entropy from authd), info = "meridian-world-v1" ∥ dir. IDENTICAL to
// worldd #84 hkdf_sha256 so both ends derive the same key from the same key.
bool hkdf_sha256(const Bytes& ikm, std::uint8_t direction,
                 std::array<std::uint8_t, kAeadKeyBytes>& out) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    if (kdf == nullptr) return false;
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (ctx == nullptr) return false;

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
//   [ direction : 1 ][ 0 : 3 ][ seq : 8, big-endian ]  (SAD §5.2; worldd make_nonce).
std::array<std::uint8_t, kAeadNonceBytes> make_nonce(Direction dir, std::uint64_t seq) {
    std::array<std::uint8_t, kAeadNonceBytes> n{};
    n[0] = static_cast<std::uint8_t>(dir);
    for (int i = 0; i < 8; ++i) {
        n[4 + i] = static_cast<std::uint8_t>((seq >> (8 * (7 - i))) & 0xFF);
    }
    return n;
}

// One-shot ChaCha20-Poly1305 seal (mirror of worldd aead_seal). Writes ciphertext
// (== plaintext length) followed by the 16-byte tag into `out`. false on failure.
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

// One-shot ChaCha20-Poly1305 open (mirror of worldd aead_open). `in` is
// ciphertext‖tag. On a valid tag writes plaintext to `out` and returns true.
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
        if (EVP_DecryptFinal_ex(ctx, out.data() + len, &final_len) <= 0) goto done;
    }
    ok = true;
done:
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) out.clear();
    return ok;
}

// little-endian scalar writers/readers for the IF-2 frame header (matches
// world_dispatch.cpp put_u16/put_u64/get_u16/get_u64).
void put_u16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void put_u64(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
}
std::uint16_t get_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}
std::uint64_t get_u64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

Bytes encode_world_frame(std::uint16_t opcode, std::uint64_t seq, const Bytes& payload) {
    Bytes out;
    out.reserve(kFrameHeaderBytes + payload.size());
    put_u16(out, opcode);
    put_u64(out, seq);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<WorldFrame> decode_world_frame(const Bytes& frame) {
    if (frame.size() < kFrameHeaderBytes) return std::nullopt;
    WorldFrame f;
    f.opcode = get_u16(frame.data());
    f.seq = get_u64(frame.data() + sizeof(std::uint16_t));
    f.payload.assign(frame.begin() + kFrameHeaderBytes, frame.end());
    return f;
}

// ---------------------------------------------------------------------------
// ClientWorldSession
// ---------------------------------------------------------------------------

ClientWorldSession::ClientWorldSession(const Bytes& session_key) {
    if (session_key.size() != kAeadKeyBytes) return;  // ok_ stays false
    ok_ = hkdf_sha256(session_key, static_cast<std::uint8_t>(Direction::kClientToServer), k_c2s_) &&
          hkdf_sha256(session_key, static_cast<std::uint8_t>(Direction::kServerToClient), k_s2c_);
}

std::optional<Bytes> ClientWorldSession::seal(Direction dir, const Bytes& plaintext,
                                              const Bytes& aad, std::uint64_t& out_seq) {
    if (!ok_) return std::nullopt;
    std::uint64_t& counter = (dir == Direction::kClientToServer) ? seq_c2s_ : seq_s2c_;
    if (counter == UINT64_MAX) return std::nullopt;  // nonce would repeat — refuse
    const std::array<std::uint8_t, kAeadKeyBytes>& k =
        (dir == Direction::kClientToServer) ? k_c2s_ : k_s2c_;
    out_seq = counter;
    auto nonce = make_nonce(dir, counter);
    Bytes out;
    if (!aead_seal(k, nonce, aad, plaintext, out)) return std::nullopt;
    ++counter;
    return out;
}

std::optional<Bytes> ClientWorldSession::open(Direction dir, const Bytes& ciphertext_and_tag,
                                              std::uint64_t seq, const Bytes& aad) {
    if (!ok_) return std::nullopt;
    const std::array<std::uint8_t, kAeadKeyBytes>& k =
        (dir == Direction::kClientToServer) ? k_c2s_ : k_s2c_;
    auto nonce = make_nonce(dir, seq);
    Bytes out;
    if (!aead_open(k, nonce, aad, ciphertext_and_tag, out)) return std::nullopt;
    return out;
}

std::uint64_t ClientWorldSession::next_seq(Direction dir) const {
    return (dir == Direction::kClientToServer) ? seq_c2s_ : seq_s2c_;
}

const std::array<std::uint8_t, kAeadKeyBytes>& ClientWorldSession::key(Direction dir) const {
    return (dir == Direction::kClientToServer) ? k_c2s_ : k_s2c_;
}

}  // namespace meridian::bot
