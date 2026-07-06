// SPDX-License-Identifier: Apache-2.0
//
// authd — login / realm-list / session-grant daemon (entry point).
//
// What this BECOMES (server SAD §2.1): a stateless, load-balanceable login
// service — TLS 1.3 listener enforcing protocol/client_build floor; an original
// SRP6a auth service over a 2048-bit group (auth DB holds {salt, verifier}
// only, constant-time proofs, per-IP/per-account rate limits); a realm-list
// service; and the IF-3 session-grant writer (single-use, 30 s grants). From M3
// the auth DB also hosts the realm_control lease for coordinator leader election
// (§8.6). authd is "M0: full" in the SAD §7 build plan.
//
// This file is the M0 SKELETON: it establishes the binary, its identity, and
// its CLI surface. No TLS, no SRP, no DB yet — those land as libmeridian-db and
// the auth subsystems arrive. Clean-room: implemented from the SAD, no GPL
// source consulted (CONTRIBUTING.md).

#include "meridian/core/log.hpp"
#include "meridian/core/version.hpp"

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kDaemonName = "authd";

void print_help() {
    std::printf(
        "%s — Project Meridian login / realm-list / session-grant daemon\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --version    Print version and build info, then exit.\n"
        "  --help, -h   Print this help, then exit.\n"
        "\n"
        "M0 skeleton (server SAD §2.1): TLS/SRP6a/realm-list/grants land on top\n"
        "of this entry point in later work.\n",
        kDaemonName, kDaemonName);
}

void print_version() {
    // name + version on the first line, then the multi-line build_info() block.
    std::printf("%s %s\n%s\n", kDaemonName,
                meridian::core::version_string().c_str(),
                meridian::core::build_info().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName,
                     argv[i]);
        return 2;
    }

    // No subsystems to start yet — announce identity and exit cleanly so the
    // binary is observably alive. A real run would enter the accept/serve loop.
    meridian::core::log::info(kDaemonName,
                              "authd skeleton up — no listeners at M0; exiting");
    std::printf("%s %s (%s) — skeleton, nothing to serve yet\n", kDaemonName,
                meridian::core::version_string().c_str(), meridian::core::kMilestone);
    return 0;
}
