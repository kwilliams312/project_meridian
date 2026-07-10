// SPDX-License-Identifier: Apache-2.0
//
// meridian-srp implementation. Clean-room from RFC 5054 + RFC 2945; no GPL
// source consulted. Modular arithmetic via OpenSSL BIGNUM; hashing via EVP.
//
// Notation (RFC 2945 / 5054):
//   N, g   group prime and generator (RFC 5054 Appendix A)
//   H      hash function; PAD(x) left-pads x to the byte length of N
//   k = H(N | PAD(g))
//   x = H(s | H(I ":" P))          v = g^x mod N            (registration)
//   A = g^a mod N (client)         B = (k*v + g^b) mod N (server)
//   u = H(PAD(A) | PAD(B))
//   S_server = (A * v^u)^b mod N   K = H(S)
//   M1 = H(H(N) XOR H(g) | H(I) | s | A | B | K)   (client proof)
//   M2 = H(A | M1 | K)                              (server proof)

#include "meridian/srp/srp.h"

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>
#include <stdexcept>

namespace meridian::srp {
namespace {

// ---- RFC 5054 Appendix A groups (hex, big-endian) --------------------------

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

const char* group_hex(Group g) {
    return g == Group::Rfc5054_1024 ? kN1024 : kN2048;
}

// ---- Hashing (EVP; SHA-1 for RFC vectors, SHA-256 for production) ----------

const EVP_MD* md_for(Hash h) {
    return h == Hash::Sha1 ? EVP_sha1() : EVP_sha256();
}

Bytes hash(Hash h, std::initializer_list<Bytes> parts) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("srp: EVP_MD_CTX_new");
    const EVP_MD* md = md_for(h);
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("srp: DigestInit");
    }
    for (const auto& p : parts) {
        if (!p.empty() && EVP_DigestUpdate(ctx, p.data(), p.size()) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error("srp: DigestUpdate");
        }
    }
    Bytes out(EVP_MD_get_size(md));
    unsigned len = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("srp: DigestFinal");
    }
    EVP_MD_CTX_free(ctx);
    out.resize(len);
    return out;
}

Bytes to_bytes(std::string_view s) {
    return Bytes(s.begin(), s.end());
}

// ---- BIGNUM helpers --------------------------------------------------------

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

// Serialize to big-endian bytes with no padding (minimal length).
Bytes bn_bytes(const BIGNUM* n) {
    Bytes b(BN_num_bytes(n));
    BN_bn2bin(n, b.data());
    return b;
}

// PAD(x): left-pad the big-endian encoding of x to `width` bytes.
Bytes pad(const BIGNUM* n, int width) {
    Bytes b(width, 0);
    // BN_bn2binpad writes exactly `width` bytes, left-padded with zeros.
    if (BN_bn2binpad(n, b.data(), width) < 0)
        throw std::runtime_error("srp: value wider than N");
    return b;
}

// Per-exchange context bundling the group, hash and scratch.
struct Ctx {
    Parameters params;
    BN N;
    BN g;
    int width;  // byte length of N (PAD target)
    BN_CTX* bnctx;

    explicit Ctx(const Parameters& p)
        : params(p), N(from_hex(group_hex(p.group))), g(from_hex("2")),
          width(BN_num_bytes(N)), bnctx(BN_CTX_new()) {}
    ~Ctx() { BN_CTX_free(bnctx); }

    Bytes H(std::initializer_list<Bytes> parts) const {
        return hash(params.hash, parts);
    }
    // k = H(N | PAD(g))
    Bytes k() const { return H({pad(N, width), pad(g, width)}); }
};

// x = H(s | H(I ":" P))
Bytes compute_x_impl(const Ctx& c, std::string_view I, std::string_view P,
                     const Bytes& salt) {
    Bytes ip = to_bytes(I);
    ip.push_back(':');
    ip.insert(ip.end(), P.begin(), P.end());
    Bytes inner = c.H({ip});
    return c.H({salt, inner});
}

// u = H(PAD(A) | PAD(B))
Bytes compute_u_impl(const Ctx& c, const BIGNUM* A, const BIGNUM* B) {
    return c.H({pad(A, c.width), pad(B, c.width)});
}

}  // namespace

// ---- Public API ------------------------------------------------------------

Verifier make_verifier(std::string_view username, std::string_view password,
                       const Parameters& params, const Bytes& salt_in) {
    Ctx c(params);
    Bytes salt = salt_in;
    if (salt.empty()) {
        salt.resize(32);
        if (RAND_bytes(salt.data(), 32) != 1)
            throw std::runtime_error("srp: RAND_bytes");
    }
    BN x = from_bytes(compute_x_impl(c, username, password, salt));
    BN v;  // v = g^x mod N
    BN_mod_exp(v.p, c.g, x.p, c.N, c.bnctx);
    // Left-pad to N's byte width so the stored credential is a deterministic
    // width. v is an integer in [0, N); its minimal encoding is a byte short
    // whenever the top byte is zero (~1 in 256 for a 2048-bit N), which made the
    // account test's fixed-width assertion flaky. Leading zeros are semantically
    // insignificant — every reader parses the verifier back through from_bytes.
    return Verifier{salt, pad(v.p, c.width)};
}

struct ServerSession::Impl {
    Ctx c;
    std::string username;
    Bytes salt;
    BN v;
    BN b;      // server ephemeral private
    BN B;      // server public
    explicit Impl(const Parameters& p) : c(p) {}
};

ServerSession::ServerSession(std::string_view username, Bytes salt,
                             Bytes verifier, const Parameters& params,
                             const Bytes& fixed_b) {
    impl_ = new Impl(params);
    impl_->username = std::string(username);
    impl_->salt = std::move(salt);
    BN_copy(impl_->v.p, from_bytes(verifier).p);

    Ctx& c = impl_->c;
    if (!fixed_b.empty()) {
        BN_copy(impl_->b.p, from_bytes(fixed_b).p);
    } else {
        // random b in [1, N-1]
        BN_rand_range(impl_->b.p, c.N);
        if (BN_is_zero(impl_->b.p)) BN_one(impl_->b.p);
    }
    // B = (k*v + g^b) mod N
    BN kbn = from_bytes(c.k());
    BN kv;
    BN_mod_mul(kv.p, kbn.p, impl_->v.p, c.N, c.bnctx);
    BN gb;
    BN_mod_exp(gb.p, c.g, impl_->b.p, c.N, c.bnctx);
    BN_mod_add(impl_->B.p, kv.p, gb.p, c.N, c.bnctx);
    b_pub_ = bn_bytes(impl_->B.p);
}

ServerSession::~ServerSession() { delete impl_; }

std::optional<Bytes> ServerSession::verify(const Bytes& A_in,
                                           const Bytes& M1_client) {
    Ctx& c = impl_->c;
    BN A = from_bytes(A_in);

    // Reject A ≡ 0 (mod N) — a mandatory SRP-6a safety check.
    BN Amod;
    BN_mod(Amod.p, A.p, c.N, c.bnctx);
    if (BN_is_zero(Amod.p)) return std::nullopt;

    // u = H(PAD(A) | PAD(B)); S = (A * v^u)^b mod N; K = H(S)
    BN u = from_bytes(compute_u_impl(c, A.p, impl_->B.p));
    BN vu;
    BN_mod_exp(vu.p, impl_->v.p, u.p, c.N, c.bnctx);
    BN Avu;
    BN_mod_mul(Avu.p, A.p, vu.p, c.N, c.bnctx);
    BN S;
    BN_mod_exp(S.p, Avu.p, impl_->b.p, c.N, c.bnctx);
    Bytes Sbytes = bn_bytes(S.p);
    K_ = c.H({Sbytes});

    // M1 = H(H(N) XOR H(g) | H(I) | s | A | B | K)
    Bytes hN = c.H({pad(c.N, c.width)});
    Bytes hg = c.H({pad(c.g, c.width)});
    Bytes hNg(hN.size());
    for (size_t i = 0; i < hN.size(); ++i) hNg[i] = hN[i] ^ hg[i];
    Bytes hI = c.H({to_bytes(impl_->username)});
    Bytes M1_server =
        c.H({hNg, hI, impl_->salt, A_in, b_pub_, K_});

    // Constant-time compare of the client proof against ours.
    if (M1_client.size() != M1_server.size()) return std::nullopt;
    if (CRYPTO_memcmp(M1_client.data(), M1_server.data(), M1_server.size()) != 0)
        return std::nullopt;

    // M2 = H(A | M1 | K)
    return c.H({A_in, M1_server, K_});
}

// ---- Test-only intermediates (assert against RFC 5054 Appendix B) ----------

namespace testing {

Bytes compute_k(const Parameters& params) { return Ctx(params).k(); }

Bytes compute_x(std::string_view username, std::string_view password,
                const Bytes& salt, const Parameters& params) {
    Ctx c(params);
    return compute_x_impl(c, username, password, salt);
}

Bytes compute_u(const Bytes& A, const Bytes& B, const Parameters& params) {
    Ctx c(params);
    return compute_u_impl(c, from_bytes(A).p, from_bytes(B).p);
}

Bytes server_premaster(std::string_view /*username*/, const Bytes& /*salt*/,
                       const Bytes& verifier, const Bytes& A_in,
                       const Bytes& fixed_b, const Parameters& params) {
    Ctx c(params);
    BN v = from_bytes(verifier);
    BN b = from_bytes(fixed_b);
    // B needed for u
    BN kbn = from_bytes(c.k());
    BN kv;
    BN_mod_mul(kv.p, kbn.p, v.p, c.N, c.bnctx);
    BN gb;
    BN_mod_exp(gb.p, c.g, b.p, c.N, c.bnctx);
    BN B;
    BN_mod_add(B.p, kv.p, gb.p, c.N, c.bnctx);
    BN A = from_bytes(A_in);
    BN u = from_bytes(compute_u_impl(c, A.p, B.p));
    BN vu;
    BN_mod_exp(vu.p, v.p, u.p, c.N, c.bnctx);
    BN Avu;
    BN_mod_mul(Avu.p, A.p, vu.p, c.N, c.bnctx);
    BN S;
    BN_mod_exp(S.p, Avu.p, b.p, c.N, c.bnctx);
    return bn_bytes(S.p);
}

ClientProof client_side(std::string_view username, std::string_view password,
                        const Bytes& salt, const Bytes& fixed_a, const Bytes& B_in,
                        const Parameters& params) {
    Ctx c(params);
    BN a = from_bytes(fixed_a);
    BN A;  // A = g^a mod N
    BN_mod_exp(A.p, c.g, a.p, c.N, c.bnctx);
    Bytes A_bytes = bn_bytes(A.p);

    BN B = from_bytes(B_in);
    BN u = from_bytes(compute_u_impl(c, A.p, B.p));
    BN x = from_bytes(compute_x_impl(c, username, password, salt));

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
    BN_add(exp.p, a.p, ux.p);
    BN S;
    BN_mod_exp(S.p, base.p, exp.p, c.N, c.bnctx);
    Bytes K = c.H({bn_bytes(S.p)});

    Bytes hN = c.H({pad(c.N, c.width)});
    Bytes hg = c.H({pad(c.g, c.width)});
    Bytes hNg(hN.size());
    for (size_t i = 0; i < hN.size(); ++i) hNg[i] = hN[i] ^ hg[i];
    Bytes hI = c.H({to_bytes(username)});
    Bytes M1 = c.H({hNg, hI, salt, A_bytes, B_in, K});
    Bytes M2 = c.H({A_bytes, M1, K});
    return ClientProof{A_bytes, M1, M2, K};
}

}  // namespace testing

}  // namespace meridian::srp
