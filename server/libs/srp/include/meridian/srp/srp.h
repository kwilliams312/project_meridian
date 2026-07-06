// SPDX-License-Identifier: Apache-2.0
//
// meridian-srp — SRP-6a Secure Remote Password authentication for authd.
//
// CLEAN-ROOM: implemented from the published specifications only —
// RFC 5054 (Using SRP for TLS Authentication) and RFC 2945 (The SRP-6
// Authentication and Key Exchange System). No GPL source (CMaNGOS /
// TrinityCore or otherwise) was consulted. See CONTRIBUTING.md.
//
// The server never sees or stores the password: registration derives a
// {salt, verifier} pair (server SAD §2.1); authentication is the SRP-6a
// exchange (server SAD §5.1: SrpStart -> SrpChallenge{salt,B} ->
// SrpProof{A,M1} -> AuthResult{M2}). Modular arithmetic uses OpenSSL BIGNUM.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace meridian::srp {

using Bytes = std::vector<std::uint8_t>;

// Standard prime/generator groups (RFC 5054 Appendix A). The 1024-bit group is
// used for the RFC 5054 Appendix B test vectors; 2048 is the production default.
enum class Group { Rfc5054_1024, Rfc5054_2048 };

// Hash used throughout the exchange. SHA-1 matches RFC 5054 / classic SRP-6a
// (and the RFC test vectors); SHA-256 is a stronger modern choice with no
// standard vectors. Production choice is a sign-off decision (see README).
enum class Hash { Sha1, Sha256 };

struct Parameters {
    Group group = Group::Rfc5054_2048;
    Hash hash = Hash::Sha256;
};

// Result of registration: what the auth DB stores for an account.
struct Verifier {
    Bytes salt;      // random per-account salt (s)
    Bytes verifier;  // v = g^x mod N, x = H(s | H(I ":" P))
};

// Derive {salt, verifier} for a new account. If `salt` is empty a random
// 32-byte salt is generated; a caller may pass a fixed salt (test vectors).
Verifier make_verifier(std::string_view username,
                       std::string_view password,
                       const Parameters& params,
                       const Bytes& salt = {});

// Server side of one authentication attempt. Constructed from the stored
// {salt, verifier}; generates the ephemeral b and exposes B. `verify()` checks
// the client's proof M1 in constant time and, on success, returns M2.
class ServerSession {
public:
    // `fixed_b` injects a deterministic server ephemeral for reproducing test
    // vectors; production uses the default (cryptographically random b).
    ServerSession(std::string_view username,
                  Bytes salt,
                  Bytes verifier,
                  const Parameters& params,
                  const Bytes& fixed_b = {});
    ~ServerSession();

    ServerSession(const ServerSession&) = delete;
    ServerSession& operator=(const ServerSession&) = delete;

    // Server public value B, sent to the client (SrpChallenge).
    const Bytes& B() const { return b_pub_; }

    // Process the client's public value A and proof M1. Returns M2 on a valid
    // proof, std::nullopt on rejection (bad proof, or A % N == 0). The M1
    // comparison is constant-time.
    std::optional<Bytes> verify(const Bytes& A, const Bytes& M1);

    // Session key K = H(S), valid only after a successful verify(). For tests.
    const Bytes& session_key() const { return K_; }

private:
    struct Impl;
    Impl* impl_;
    Bytes b_pub_;
    Bytes K_;
};

// Test-only helpers exposing intermediate values so unit tests can assert
// against the RFC 5054 Appendix B vectors. Not part of the production surface.
namespace testing {
Bytes compute_k(const Parameters& params);
Bytes compute_x(std::string_view username, std::string_view password,
                const Bytes& salt, const Parameters& params);
Bytes compute_u(const Bytes& A, const Bytes& B, const Parameters& params);
// Full server-side premaster secret S given the client's A and a fixed b.
Bytes server_premaster(std::string_view username, const Bytes& salt,
                       const Bytes& verifier, const Bytes& A,
                       const Bytes& fixed_b, const Parameters& params);

// Test-only client side of the exchange: given a fixed client ephemeral a and
// the server's B, derive A, the client proof M1, the M2 it expects back, and K.
// Lets the round-trip test drive a full client<->server handshake.
struct ClientProof {
    Bytes A;
    Bytes M1;
    Bytes expected_M2;
    Bytes K;
};
ClientProof client_side(std::string_view username, std::string_view password,
                        const Bytes& salt, const Bytes& fixed_a, const Bytes& B,
                        const Parameters& params);
}  // namespace testing

}  // namespace meridian::srp
