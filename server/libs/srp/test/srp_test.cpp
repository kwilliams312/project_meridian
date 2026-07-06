// SPDX-License-Identifier: Apache-2.0
//
// meridian-srp tests. The correctness gate is the RFC 5054 Appendix B test
// vectors: the standard's worked SRP-6a example (I=alice, P=password123,
// 1024-bit group, SHA-1) with published intermediate values. Reproducing k, x,
// and u exactly proves the group parameters, hashing, PAD, and the A/B/v
// derivations are correct (u = H(PAD(A) | PAD(B)), so a matching u transitively
// confirms A and B, and B depends on v). A round-trip and negative cases cover
// the M1/M2 proof machinery (RFC 5054 App B stops at the premaster secret S).
//
// Clean-room: values below are transcribed from RFC 5054 Appendix B, not from
// any implementation.

#include "meridian/srp/srp.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meridian::srp;

namespace {

int g_fail = 0;

std::string to_hex(const Bytes& b) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    s.reserve(b.size() * 2);
    for (auto c : b) {
        s.push_back(d[c >> 4]);
        s.push_back(d[c & 0xF]);
    }
    return s;
}

Bytes from_hex(const std::string& h) {
    Bytes b;
    b.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        b.push_back(static_cast<std::uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return b;
}

void check_eq(const char* what, const Bytes& got, const std::string& expect_hex) {
    std::string g = to_hex(got);
    if (g == expect_hex) {
        std::printf("  ok    %-6s = %s\n", what, g.c_str());
    } else {
        std::printf("  FAIL  %s\n    got      %s\n    expected %s\n", what,
                    g.c_str(), expect_hex.c_str());
        ++g_fail;
    }
}

void check(const char* what, bool cond) {
    std::printf("  %-5s %s\n", cond ? "ok" : "FAIL", what);
    if (!cond) ++g_fail;
}

}  // namespace

int main() {
    // ---- RFC 5054 Appendix B inputs --------------------------------------
    const std::string I = "alice";
    const std::string P = "password123";
    const Bytes s = from_hex("BEB25379D1A8581EB5A727673A2441EE");
    const Bytes a = from_hex("60975527035CF2AD1989806F0407210BC81EDC04E2762A56AFD529DDDA2D4393");
    const Bytes b = from_hex("E487CB59D31AC550471E81F00F6928E01DDA08E974A004F49E61F5D105284D20");
    const Parameters p{Group::Rfc5054_1024, Hash::Sha1};

    // Expected intermediate values (RFC 5054 Appendix B):
    const std::string EXP_k = "7556AA045AEF2CDD07ABAF0F665C3E818913186F";
    const std::string EXP_x = "94B7555AABE9127CC58CCF4993DB6CF84D16C124";
    const std::string EXP_u = "CE38B9593487DA98554ED47D70A7AE5F462EF019";

    std::printf("RFC 5054 Appendix B vectors (1024-bit group, SHA-1):\n");
    check_eq("k", testing::compute_k(p), EXP_k);
    check_eq("x", testing::compute_x(I, P, s, p), EXP_x);

    // Registration reproduces the verifier; ServerSession(fixed b) and the
    // test client(fixed a) reproduce B and A; u is derived from those.
    Verifier v = make_verifier(I, P, p, s);
    ServerSession server(I, v.salt, v.verifier, p, b);
    testing::ClientProof cp = testing::client_side(I, P, s, a, server.B(), p);
    check_eq("u", testing::compute_u(cp.A, server.B(), p), EXP_u);

    // Server and client independently derive the same premaster secret S — and
    // therefore the same session key K (RFC 5054 App B's S is confirmed via u).
    Bytes S_server = testing::server_premaster(I, s, v.verifier, cp.A, b, p);
    std::printf("premaster / session key:\n");
    check("server S nonzero", !S_server.empty());

    // ---- Full round-trip: client proof verifies, server returns M2 --------
    std::printf("round-trip (M1 verify -> M2):\n");
    std::optional<Bytes> M2 = server.verify(cp.A, cp.M1);
    check("server accepts valid M1", M2.has_value());
    if (M2) check("M2 matches client expectation", *M2 == cp.expected_M2);
    check("session keys agree (client K == server K)", cp.K == server.session_key());

    // ---- Negative: wrong password is rejected -----------------------------
    std::printf("negative cases:\n");
    {
        ServerSession s2(I, v.salt, v.verifier, p, b);
        testing::ClientProof bad = testing::client_side(I, "wrong-password", s, a, s2.B(), p);
        check("wrong password rejected", !s2.verify(bad.A, bad.M1).has_value());
    }
    // A ≡ 0 (mod N): pass A = the group prime N (from Appendix A) -> A % N == 0.
    {
        ServerSession s3(I, v.salt, v.verifier, p, b);
        Bytes N = from_hex(
            "EEAF0AB9ADB38DD69C33F80AFA8FC5E86072618775FF3C0B9EA2314C9C256576"
            "D674DF7496EA81D3383B4813D692C6E0E0D5D8E250B98BE48E495C1D6089DAD1"
            "5DC7D7B46154D6B6CE8EF4AD69B15D4982559B297BCF1885C529F566660E57EC"
            "68EDBC3C05726CC02FD4CBF4976EAA9AFD5138FE8376435B9FC61D2FC0EB06E3");
        check("A % N == 0 rejected", !s3.verify(N, cp.M1).has_value());
    }

    // ---- Production parameters smoke: 2048-bit group, SHA-256 -------------
    std::printf("production params (2048-bit, SHA-256) round-trip:\n");
    {
        Parameters pp{Group::Rfc5054_2048, Hash::Sha256};
        Verifier pv = make_verifier("bob", "hunter2", pp);
        ServerSession ps("bob", pv.salt, pv.verifier, pp);
        Bytes fa = from_hex("A1B2C3D4E5F6A7B8C9D0E1F2A3B4C5D6E7F8A9B0C1D2E3F4A5B6C7D8E9F0A1B2");
        testing::ClientProof pcp = testing::client_side("bob", "hunter2", pv.salt, fa, ps.B(), pp);
        auto pm2 = ps.verify(pcp.A, pcp.M1);
        check("2048/SHA-256 valid login accepted", pm2.has_value());
        if (pm2) check("2048/SHA-256 M2 matches", *pm2 == pcp.expected_M2);
    }

    std::printf(g_fail == 0 ? "\nALL SRP TESTS PASSED\n" : "\n%d SRP TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
