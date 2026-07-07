// SPDX-License-Identifier: Apache-2.0
//
// meridian client SRP-6a CLIENT core implementation. Clean-room from RFC 5054 +
// RFC 2945, mirroring this repo's server-side meridian-srp (server/libs/srp/src/
// srp.cpp) so the two interoperate exactly. Modular arithmetic via OpenSSL
// BIGNUM; hashing via EVP.
//
// Notation (identical to server/libs/srp/src/srp.cpp):
//   N, g   group prime and generator (RFC 5054 Appendix A); g = 2
//   H      hash function; PAD(x) left-pads x to the byte length of N
//   k = H(N | PAD(g))
//   x = H(s | H(I ":" P))                               (client derives from pw)
//   A = g^a mod N (client, random a)
//   B = (k*v + g^b) mod N (server, received)
//   u = H(PAD(A) | PAD(B))
//   S_client = (B - k*g^x)^(a + u*x) mod N              K = H(S)
//   M1 = H(H(N) XOR H(g) | H(I) | s | A | B | K)        (client proof, sent)
//   M2 = H(A | M1 | K)                                  (server proof, verified)

#include "srp_client_core.h"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdexcept>

namespace meridian::login {
namespace {

// ---- RFC 5054 Appendix A groups (hex, big-endian) — byte-identical to the
// server's srp.cpp so client and server compute the same k, x, u, S. -----------

constexpr const char* kN1024 =
    "EEAF0AB9ADB38DD69C33F80AFA8FC5E86072618775FF3C0B9EA2314C9C256576"
    "D674DF7496EA81D3383B4813D692C6E0E0D5D8E250B98BE48E495C1D6089DAD1"
    "5DC7D7B46154D6B6CE8EF4AD69B15D4982559B297BCF1885C529F566660E57EC"
    "68EDBC3C05726CC02FD4CBF4976EAA9AFD5138FE8376435B9FC61D2FC0EB06E3";

constexpr const char* kN2048 =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050"
    "A37329CBB4A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50"
    "E8083969EDB767B0CF6095179A163AB3661A05FBD5FAAAE82918A9962F0B93B8"
    "55F97993EC975EEAA80D740ADBF4FF747359D041D5C33EA71D281E446B14773B"
    "CA97B43A23FB801676BD207A436C6481F1D2B9078717461A5B9D32E688F87748"
    "544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB3786160279004E57AE6"
    "AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DBFBB6"
    "94B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";

const char* group_hex(SrpGroup g) {
    return g == SrpGroup::Rfc5054_1024 ? kN1024 : kN2048;
}

const EVP_MD* md_for(SrpHash h) {
    return h == SrpHash::Sha1 ? EVP_sha1() : EVP_sha256();
}

Bytes hash(SrpHash h, std::initializer_list<Bytes> parts) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("srp-client: EVP_MD_CTX_new");
    const EVP_MD* md = md_for(h);
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("srp-client: DigestInit");
    }
    for (const auto& p : parts) {
        if (!p.empty() && EVP_DigestUpdate(ctx, p.data(), p.size()) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("srp-client: DigestUpdate");
        }
    }
    Bytes out(EVP_MD_get_size(md));
    unsigned len = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("srp-client: DigestFinal");
    }
    EVP_MD_CTX_free(ctx);
    out.resize(len);
    return out;
}

Bytes to_bytes(std::string_view s) { return Bytes(s.begin(), s.end()); }

// ---- BIGNUM helpers (mirrors srp.cpp's BN RAII wrapper) --------------------

struct BN {
    BIGNUM* p;
    BN() : p(BN_new()) {}
    explicit BN(BIGNUM* n) : p(n) {}
    BN(BN&& o) noexcept : p(o.p) { o.p = nullptr; }
    BN& operator=(BN&& o) noexcept {
        if (this != &o) { BN_free(p); p = o.p; o.p = nullptr; }
        return *this;
    }
    ~BN() { BN_free(p); }  // BN_free(nullptr) is safe
    BN(const BN&) = delete;
    BN& operator=(const BN&) = delete;
    operator BIGNUM*() const { return p; }
};

BN from_bytes(const Bytes& b) {
    BN n;
    BN_bin2bn(b.data(), static_cast<int>(b.size()), n.p);
    return n;
}

BN from_hex(const char* hex) {
    BIGNUM* n = nullptr;
    BN_hex2bn(&n, hex);
    return BN(n);
}

Bytes bn_bytes(const BIGNUM* n) {
    Bytes b(BN_num_bytes(n));
    BN_bn2bin(n, b.data());
    return b;
}

// PAD(x): left-pad the big-endian encoding of x to `width` bytes (RFC 5054).
Bytes pad(const BIGNUM* n, int width) {
    Bytes b(width, 0);
    if (BN_bn2binpad(n, b.data(), width) < 0)
        throw std::runtime_error("srp-client: value wider than N");
    return b;
}

}  // namespace

// ---- Per-exchange context (group, hash, scratch) — mirrors srp.cpp Ctx. -----

struct SrpClientSession::Impl {
    SrpParams params;
    std::string username;
    std::string password;
    BN N;
    BN g;
    int width;      // byte length of N (PAD target)
    BN_CTX* bnctx;
    BN a;           // client ephemeral private (random or injected)

    explicit Impl(const SrpParams& p)
        : params(p), N(from_hex(group_hex(p.group))), g(from_hex("2")),
          width(BN_num_bytes(N)), bnctx(BN_CTX_new()) {}
    ~Impl() { BN_CTX_free(bnctx); }

    Bytes H(std::initializer_list<Bytes> parts) const {
        return hash(params.hash, parts);
    }
    // k = H(N | PAD(g))
    Bytes k() const { return H({pad(N, width), pad(g, width)}); }

    // x = H(s | H(I ":" P))
    Bytes compute_x(const Bytes& salt) const {
        Bytes ip = to_bytes(username);
        ip.push_back(':');
        ip.insert(ip.end(), password.begin(), password.end());
        Bytes inner = H({ip});
        return H({salt, inner});
    }

    // u = H(PAD(A) | PAD(B))
    Bytes compute_u(const BIGNUM* A, const BIGNUM* B) const {
        return H({pad(A, width), pad(B, width)});
    }
};

// Shared constructor body: install `a` (random or fixed) into impl and compute A.
// Free-function form kept local to this TU; takes the concrete Impl (fully defined
// above) so it needs no access to the private nested type from the header.
static void install_a_and_compute_A(SrpClientSession::Impl& impl,
                                    const Bytes* fixed_a, Bytes& out_a_pub) {
    if (fixed_a != nullptr) {
        BN_copy(impl.a.p, from_bytes(*fixed_a).p);
        // a must be non-zero mod N (SRP requires a != 0).
        BN amod;
        BN_mod(amod.p, impl.a.p, impl.N, impl.bnctx);
        if (BN_is_zero(amod.p)) {
            throw std::invalid_argument("srp-client: fixed a is zero mod N");
        }
    } else {
        // Random a in [1, N-1]. BN_rand_range yields [0, N-1); bump off 0.
        BN_rand_range(impl.a.p, impl.N);
        if (BN_is_zero(impl.a.p)) BN_one(impl.a.p);
    }
    // A = g^a mod N.
    BN A;
    BN_mod_exp(A.p, impl.g, impl.a.p, impl.N, impl.bnctx);
    out_a_pub = bn_bytes(A.p);
}

SrpClientSession::SrpClientSession(std::string_view username,
                                   std::string_view password,
                                   const SrpParams& params) {
    impl_ = new Impl(params);
    impl_->username = std::string(username);
    impl_->password = std::string(password);
    install_a_and_compute_A(*impl_, /*fixed_a=*/nullptr, a_pub_);
}

SrpClientSession::SrpClientSession(std::string_view username,
                                   std::string_view password,
                                   const SrpParams& params, const Bytes& fixed_a) {
    impl_ = new Impl(params);
    impl_->username = std::string(username);
    impl_->password = std::string(password);
    install_a_and_compute_A(*impl_, &fixed_a, a_pub_);
}

SrpClientSession::~SrpClientSession() { delete impl_; }

Bytes SrpClientSession::compute_proof(const Bytes& salt, const Bytes& B_in) {
    Impl& c = *impl_;
    BN B = from_bytes(B_in);

    // Reject B ≡ 0 (mod N) — mandatory SRP-6a safety check on the server value.
    BN Bmod;
    BN_mod(Bmod.p, B.p, c.N, c.bnctx);
    if (BN_is_zero(Bmod.p)) {
        throw std::invalid_argument("srp-client: server B is zero mod N");
    }

    BN A = from_bytes(a_pub_);
    BN u = from_bytes(c.compute_u(A.p, B.p));
    BN x = from_bytes(c.compute_x(salt));

    // S_client = (B - k*g^x)^(a + u*x) mod N
    BN kbn = from_bytes(c.k());
    BN gx;
    BN_mod_exp(gx.p, c.g, x.p, c.N, c.bnctx);
    BN kgx;
    BN_mod_mul(kgx.p, kbn.p, gx.p, c.N, c.bnctx);
    BN base;
    BN_mod_sub(base.p, B.p, kgx.p, c.N, c.bnctx);
    BN ux;
    BN_mul(ux.p, u.p, x.p, c.bnctx);
    BN exp;
    BN_add(exp.p, c.a.p, ux.p);
    BN S;
    BN_mod_exp(S.p, base.p, exp.p, c.N, c.bnctx);

    K_ = c.H({bn_bytes(S.p)});

    // M1 = H(H(N) XOR H(g) | H(I) | s | A | B | K)
    Bytes hN = c.H({pad(c.N, c.width)});
    Bytes hg = c.H({pad(c.g, c.width)});
    Bytes hNg(hN.size());
    for (size_t i = 0; i < hN.size(); ++i) hNg[i] = hN[i] ^ hg[i];
    Bytes hI = c.H({to_bytes(c.username)});
    Bytes M1 = c.H({hNg, hI, salt, a_pub_, B_in, K_});

    // M2 = H(A | M1 | K) — what we expect the server to send back.
    expected_m2_ = c.H({a_pub_, M1, K_});
    return M1;
}

bool SrpClientSession::verify_server(const Bytes& m2) const {
    if (expected_m2_.empty()) return false;  // compute_proof() not called yet
    if (m2.size() != expected_m2_.size()) return false;
    return CRYPTO_memcmp(m2.data(), expected_m2_.data(), expected_m2_.size()) == 0;
}

}  // namespace meridian::login
