// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free SRP-6a CLIENT core (issue #99).
//
// The CLIENT counterpart to the server's meridian-srp ServerSession
// (server/libs/srp). authd's login state machine (server/authd/login_session.cpp,
// #79) runs the SERVER side of SRP-6a; this is the CLIENT side the game client
// must run to log in. It MUST agree with the server bit-for-bit on the group,
// the hash, and every intermediate (k, x, u, S, K, M1, M2) — otherwise the M1
// the server computes will not match ours and login fails. The shared contract
// is: RFC 5054 Appendix A 2048-bit group, generator g = 2, SHA-256 throughout
// (server/authd/login_session.h LoginConfig.srp_params == {Rfc5054_2048, Sha256},
// meridian-account's make_verifier defaults). See server/libs/srp/src/srp.cpp for
// the notation this mirrors.
//
// CLEAN-ROOM: implemented from the published specifications only — RFC 5054
// (Using SRP for TLS Authentication) and RFC 2945 (The SRP-6 Authentication and
// Key Exchange System), plus this repo's own server-side meridian-srp as the
// interop reference. No GPL source (CMaNGOS / TrinityCore or otherwise) was
// consulted. See CONTRIBUTING.md.
//
// ENGINE-FREE by design (Client SAD §9.2 "engine-agnostic cores"): no Godot
// types. Plain C++17 + OpenSSL BIGNUM/EVP — the same crypto stack the server SRP
// uses — so it links into the GDExtension, the headless bot, AND the plain unit
// tests exactly like the movement / telemetry / pack-manifest cores. The thin
// Godot binding is meridian_login.* (#99).
//
// Difference from the server's testing::client_side() helper: that helper takes a
// FIXED client ephemeral `a` (for reproducing RFC test vectors) and computes one
// shot. A real client generates a cryptographically RANDOM `a` per login (SRP
// requires only a != 0). This core does exactly that by default, but also accepts
// an injected `a` so a deterministic test can pin the exchange.

#ifndef MERIDIAN_SRP_CLIENT_CORE_H
#define MERIDIAN_SRP_CLIENT_CORE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace meridian::login {

using Bytes = std::vector<std::uint8_t>;

// SRP group / hash selection. Mirrors meridian::srp::Parameters — the client MUST
// use the same {group, hash} the server stored the verifier under. The production
// default (and the only pairing authd uses) is {Rfc5054_2048, Sha256}.
enum class SrpGroup { Rfc5054_1024, Rfc5054_2048 };
enum class SrpHash { Sha1, Sha256 };

struct SrpParams {
    SrpGroup group = SrpGroup::Rfc5054_2048;
    SrpHash hash = SrpHash::Sha256;
};

// One client-side SRP-6a authentication attempt.
//
// Lifecycle:
//   SrpClientSession s(username, password, params);   // picks a random `a`, has A
//   ... send SrpStart{username}, receive SrpChallenge{salt, B} ...
//   Bytes M1 = s.compute_proof(salt, B);               // derives x, S, K, M1
//   ... send SrpProof{A, M1}, receive AuthResult{success, m2} ...
//   bool ok = s.verify_server(m2);                     // authenticate the server
//   Bytes K = s.session_key();                         // shared key (== server's)
//
// A itself is available immediately after construction (public_a()); it does not
// depend on the challenge, matching the wire order (A is sent in SrpProof after
// the challenge, but is computable up front from `a`). compute_proof() must be
// called before verify_server()/session_key().
class SrpClientSession {
public:
    // Construct with a cryptographically RANDOM client ephemeral `a` (production
    // path). Computes A = g^a mod N immediately.
    SrpClientSession(std::string_view username, std::string_view password,
                     const SrpParams& params);

    // Construct with a caller-supplied `a` (deterministic tests / RFC vectors).
    // `a` must be non-zero mod N; a zero `a` throws std::invalid_argument.
    SrpClientSession(std::string_view username, std::string_view password,
                     const SrpParams& params, const Bytes& fixed_a);

    ~SrpClientSession();
    SrpClientSession(const SrpClientSession&) = delete;
    SrpClientSession& operator=(const SrpClientSession&) = delete;

    // Client public ephemeral A (sent in SrpProof). Available from construction.
    const Bytes& public_a() const { return a_pub_; }

    // Given the server's SrpChallenge{salt, B}, derive x, u, the premaster S, the
    // session key K = H(S), and the client proof M1 = H(H(N) XOR H(g) | H(I) | s |
    // A | B | K). Returns M1 (sent in SrpProof). Also stores K and the expected
    // server proof M2 = H(A | M1 | K) so verify_server() can check the reply.
    //
    // Throws std::invalid_argument if B ≡ 0 (mod N) — a mandatory SRP-6a safety
    // check on the SERVER's public value (RFC 5054 §2.5.3): a zero B would let a
    // malicious/broken server force a known key.
    Bytes compute_proof(const Bytes& salt, const Bytes& B);

    // Verify the server's proof M2 against the value we expected from
    // compute_proof(). Constant-time compare. Returns false if compute_proof() has
    // not run yet or the proof does not match (the server failed to prove it holds
    // the verifier — the client MUST abort the login on false).
    bool verify_server(const Bytes& m2) const;

    // The shared session key K = H(S), valid only after compute_proof(). Empty
    // before then. This is the SRP session key; note it is NOT the IF-2
    // session_key (that comes from the SessionGrant) — K authenticates the auth
    // channel and matches the server's K for the M1/M2 proofs.
    const Bytes& session_key() const { return K_; }

public:
    // Implementation detail exposed publicly ONLY so the .cpp's shared
    // constructor helper can name the type; it is an incomplete type here, so no
    // caller can do anything with it. (Nested-type access from a free helper in
    // the .cpp would otherwise be a private-access error.)
    struct Impl;

private:
    Impl* impl_;
    Bytes a_pub_;         // A = g^a mod N
    Bytes K_;             // session key (after compute_proof)
    Bytes expected_m2_;   // M2 we expect back (after compute_proof)
};

}  // namespace meridian::login

#endif  // MERIDIAN_SRP_CLIENT_CORE_H
