# `meridian-srp` — SRP-6a authentication (#78)

Server-side SRP-6a (Secure Remote Password) for `authd` (server SAD §2.1, §5.1).
The server never sees or stores the password: registration derives a
`{salt, verifier}` pair; login is the SRP-6a exchange.

**Clean-room** — implemented purely from **RFC 5054** (SRP for TLS) and **RFC 2945**
(the SRP protocol). No GPL source (CMaNGOS/TrinityCore or otherwise) was consulted.

## Correctness

Verified against the **RFC 5054 Appendix B test vectors** (1024-bit group, SHA-1):
the implementation reproduces the standard's published `k`, `x`, and `u` exactly.
Because `u = H(PAD(A) | PAD(B))` and `B` derives from `v`, a matching `u`
transitively confirms `A`, `B`, and `v`. The M1/M2 proof machinery (which RFC 5054
App B does not cover — it stops at the premaster secret) is covered by a full
round-trip test plus negative cases (wrong password, `A ≡ 0 mod N`).

```
cmake -S server -B build -DCMAKE_PREFIX_PATH=$(brew --prefix openssl@3)
cmake --build build --target meridian-srp-test && ctest --test-dir build
```

## Dependencies

OpenSSL (BIGNUM for modular arithmetic, EVP for digests) via `find_package(OpenSSL)`.
This is the first server library to take a real third-party dependency.

## ⚠️ Decisions for sign-off

The library is **parameterized** on both, so these are configuration choices, not
rewrites:

1. **Hash `H`** — SHA-1 vs SHA-256. SHA-1 matches RFC 5054 / classic SRP-6a and the
   test vectors; **SHA-256 is the recommended production default** (implemented as
   the default in `Parameters`) — SRP-6a's security does not hinge on collision
   resistance, but SHA-256 is the defensible modern choice for a new project. The
   RFC-vector test necessarily uses SHA-1; production uses SHA-256.
2. **Group** — RFC 5054 1024-bit (test vectors only) vs **2048-bit (production
   default)**. 2048 is the floor for new deployments. Larger groups (3072/4096) are
   a config addition if ever wanted.

Recommendation: **2048-bit group + SHA-256** for production (the current defaults);
keep 1024/SHA-1 available only for the RFC-vector test.

## Not yet wired

This is the crypto core. The TLS 1.3 listener (#77), the `libmeridian-db` layer
that reads verifiers / writes session grants (#75/#79), and the IF-1 message plumbing
(auth.fbs, via `meridian-proto`) connect it into a live `authd` in the next tasks.
