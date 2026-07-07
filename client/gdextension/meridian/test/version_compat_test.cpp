// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free unit test for the SCHEMA/PROTOCOL VERSION COMPAT
// core (issue #98). Plain-main style (no Godot, no deps), mirroring the other
// engine-free client core tests; ctest-wired via the client test CMake
// (MERIDIAN_CLIENT_TESTS, ctest name `version-compat`).
//
// Proves the client SAD §5.1 / server SAD §5.2 compat rule:
//   1. proto_ver EXACT match — equal versions are compatible; ANY difference (client
//      older OR newer) is kClientOutOfDate and ALWAYS blocking.
//   2. content_hash — equal well-formed hashes are compatible; a difference is a
//      NON-blocking warning at M0 (default policy) and a BLOCKING reject under a
//      hard-fail policy (M1+ test realm).
//   3. SAFE handling of absent / malformed content hashes (never blocks at M0; never
//      UB): an absent hash on either side skips the content check; a wrong-width hash
//      is kMalformed and warn-only at M0.
//   4. proto mismatch DOMINATES a matching/absent content hash (out-of-date wins).

#include "version_compat.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::compat;

static int g_fail = 0;
static int g_checks = 0;
static void check(const char* name, bool ok) {
    ++g_checks;
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_fail;
}

// A well-formed 32-byte (BLAKE3-256) content hash filled with byte `b`.
static HashBytes hash32(std::uint8_t b) { return HashBytes(32, b); }

int main() {
    std::printf("schema/protocol version compat core tests (#98)\n\n");

    // ===== 1. proto_ver exact-match =========================================
    std::printf("1. proto_ver exact-match rule\n");
    {
        CompatResult r = check_compat({1, {}}, {1, {}});
        check("1: equal proto -> compatible, not blocking",
              r.verdict == CompatVerdict::kCompatible && !r.blocking && r.ok());
    }
    {
        // Client older than server.
        CompatResult r = check_compat({1, {}}, {2, {}});
        check("1: client older -> kClientOutOfDate + blocking",
              r.verdict == CompatVerdict::kClientOutOfDate && r.blocking && !r.ok());
    }
    {
        // Client NEWER than server (documented handling: still out-of-date/update).
        CompatResult r = check_compat({3, {}}, {2, {}});
        check("1: client newer -> kClientOutOfDate + blocking (documented)",
              r.verdict == CompatVerdict::kClientOutOfDate && r.blocking);
    }
    {
        // proto mismatch is ALWAYS blocking regardless of the content-hash policy.
        CompatPolicy soft;  // content_hash_hard_fail = false
        CompatResult r = check_compat({1, {}}, {9, {}}, soft);
        check("1: proto mismatch blocking even under soft policy", r.blocking);
    }

    // ===== 2. content_hash arm (proto matches) ==============================
    std::printf("2. content_hash comparison (proto matches)\n");
    {
        CompatResult r = check_compat({1, hash32(0xAB)}, {1, hash32(0xAB)});
        check("2: equal well-formed hashes -> compatible",
              r.verdict == CompatVerdict::kCompatible && !r.blocking);
    }
    {
        // Default M0 policy: content mismatch is a WARNING, not blocking.
        CompatResult r = check_compat({1, hash32(0xAB)}, {1, hash32(0xCD)});
        check("2: hash differs @ M0 -> kContentMismatch, NON-blocking (warn)",
              r.verdict == CompatVerdict::kContentMismatch && !r.blocking && r.ok());
    }
    {
        // Hard-fail policy (M1+ test realm): content mismatch REJECTS.
        CompatPolicy hard;
        hard.content_hash_hard_fail = true;
        CompatResult r = check_compat({1, hash32(0xAB)}, {1, hash32(0xCD)}, hard);
        check("2: hash differs @ hard policy -> kContentMismatch + blocking",
              r.verdict == CompatVerdict::kContentMismatch && r.blocking);
    }

    // ===== 3. safe absent / malformed handling ==============================
    std::printf("3. absent / malformed content hash (safe, never UB)\n");
    {
        // Absent on the server side: content check simply skipped.
        CompatResult r = check_compat({1, hash32(0xAB)}, {1, {}});
        check("3: absent remote hash -> compatible (content check skipped)",
              r.verdict == CompatVerdict::kCompatible && !r.blocking);
    }
    {
        // Absent on the client side: also skipped.
        CompatResult r = check_compat({1, {}}, {1, hash32(0xAB)});
        check("3: absent local hash -> compatible (content check skipped)",
              r.verdict == CompatVerdict::kCompatible && !r.blocking);
    }
    {
        // Wrong-width (malformed) hash @ M0 -> warn, not blocking.
        HashBytes bad(31, 0xAB);  // 31 bytes, not 32
        CompatResult r = check_compat({1, bad}, {1, hash32(0xAB)});
        check("3: malformed (bad width) @ M0 -> kMalformed, NON-blocking",
              r.verdict == CompatVerdict::kMalformed && !r.blocking);
    }
    {
        // Wrong-width hash under a hard-fail policy -> blocking (conservative).
        HashBytes bad(31, 0xAB);
        CompatPolicy hard;
        hard.content_hash_hard_fail = true;
        CompatResult r = check_compat({1, bad}, {1, hash32(0xAB)}, hard);
        check("3: malformed @ hard policy -> kMalformed + blocking",
              r.verdict == CompatVerdict::kMalformed && r.blocking);
    }
    {
        // content_hash_len = 0 disables the width check: unequal-width but content
        // simply compared as bytes -> here they differ -> content mismatch (warn).
        CompatPolicy nolen;
        nolen.content_hash_len = 0;
        HashBytes a(4, 0x01), b(4, 0x02);
        CompatResult r = check_compat({1, a}, {1, b}, nolen);
        check("3: width check disabled -> compares bytes (mismatch, warn)",
              r.verdict == CompatVerdict::kContentMismatch && !r.blocking);
    }

    // ===== 4. proto mismatch dominates ======================================
    std::printf("4. proto mismatch dominates the content-hash arm\n");
    {
        // Even with IDENTICAL content hashes, a proto mismatch is out-of-date.
        CompatResult r = check_compat({1, hash32(0xAB)}, {2, hash32(0xAB)});
        check("4: proto differs + hashes equal -> kClientOutOfDate + blocking",
              r.verdict == CompatVerdict::kClientOutOfDate && r.blocking);
    }

    // ===== to_string coverage ===============================================
    check("to_string(kCompatible)", std::string(to_string(CompatVerdict::kCompatible)) == "compatible");
    check("to_string(kClientOutOfDate)",
          std::string(to_string(CompatVerdict::kClientOutOfDate)) == "client-out-of-date");

    std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "ALL VERSION-COMPAT TESTS PASSED\n"
                            : "%d VERSION-COMPAT TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
