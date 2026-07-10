// SPDX-License-Identifier: Apache-2.0
//
// #510 REGRESSION — real-socket SOAK over the post-SRP realm-list wire path.
//
// The #99 killer test (authd_login_it.cpp) proves ONE account logs in once over a
// real TLS 1.3 socket. It passes on loopback — yet on the live dev realm ~some
// accounts get "server returned an empty realm list" at the RealmListRequest ->
// RealmList step even though SRP-6a mutual auth SUCCEEDS (issue #510). The failure
// is intermittent and per-account, which the single-shot IT can never surface.
//
// This binary drives the FULL client login CORE (login_core + srp_client_core +
// login_transport) over a REAL TLS 1.3 socket against a RUNNING authd, MANY times:
//   * --soak-prefix / --soak-count / --soak-width iterate over N DISTINCT accounts
//     the harness provisioned (prefix0000..prefix{N-1}); a distinct account per
//     login exercises the PER-ACCOUNT SRP verifier width (~1/256 verifiers have a
//     zero most-significant byte — the #440 class of length-sensitivity).
//   * --repeat R re-runs the whole sweep R times; each login is a fresh TLS
//     connection with fresh SRP ephemerals (random a/b), so ~1/256 SESSIONS hit a
//     zero-MSB server ephemeral B / premaster S / session key K — the PER-SESSION
//     length-sensitivity the coordinator flagged.
//
// For EVERY login it asserts the full chain reaches a SessionGrant with a
// non-empty realm list. On ANY failure it prints the account, the LoginStatus, the
// server error code, and the detail — so a live-style intermittent failure is
// captured with the exact account + wire outcome, not silently averaged away.
//
// Inert (SKIP, exit 0) without --host/--port so it is a safe no-arg ctest; the
// harness (run_authd_realm_list_soak_it.sh) provisions the accounts + realm,
// launches authd, and passes the connection details.

#include "login_core.h"
#include "login_transport.h"

#include <openssl/ssl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace meridian;

namespace {

const char* arg_after(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

const char* status_name(login::LoginStatus s) {
    switch (s) {
        case login::LoginStatus::kSuccess:           return "kSuccess";
        case login::LoginStatus::kConnectFailed:     return "kConnectFailed";
        case login::LoginStatus::kProtocolMismatch:  return "kProtocolMismatch";
        case login::LoginStatus::kBadCredentials:    return "kBadCredentials";
        case login::LoginStatus::kServerProofFailed: return "kServerProofFailed";
        case login::LoginStatus::kRealmUnavailable:  return "kRealmUnavailable";
        case login::LoginStatus::kProtocolError:     return "kProtocolError";
        case login::LoginStatus::kTransportClosed:   return "kTransportClosed";
    }
    return "??";
}

// Zero-padded account name: prefix + width-padded index (matches the harness /
// add-users.sh naming so the soak drives the exact accounts provisioned).
std::string account_name(const std::string& prefix, int index, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*d", width, index);
    return prefix + buf;
}

}  // namespace

int main(int argc, char** argv) {
    const char* host = arg_after(argc, argv, "--host");
    const char* port_s = arg_after(argc, argv, "--port");
    const char* prefix_s = arg_after(argc, argv, "--soak-prefix");
    const char* password = arg_after(argc, argv, "--password");
    const char* count_s = arg_after(argc, argv, "--soak-count");
    const char* width_s = arg_after(argc, argv, "--soak-width");
    const char* repeat_s = arg_after(argc, argv, "--repeat");
    const char* realm_id_s = arg_after(argc, argv, "--realm-id");
    const char* build_s = arg_after(argc, argv, "--build");

    if (!host || !port_s || !prefix_s || !password || !count_s || !realm_id_s) {
        std::printf("SKIP: no live authd configured (need --host --port "
                    "--soak-prefix --password --soak-count --realm-id "
                    "[--soak-width --repeat --build])\n");
        return 0;  // inert without the harness — same discipline as authd_login_it.
    }

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);

    const auto port = static_cast<std::uint16_t>(std::atoi(port_s));
    const int count = std::atoi(count_s);
    const int width = width_s ? std::atoi(width_s) : 4;
    const int repeat = repeat_s ? std::atoi(repeat_s) : 1;
    const std::string prefix = prefix_s;
    static std::uint32_t s_realm =
        static_cast<std::uint32_t>(std::strtoul(realm_id_s, nullptr, 10));

    login::LoginConfig cfg;
    cfg.client_build = build_s ? static_cast<std::uint32_t>(std::atoi(build_s)) : 1000;
    cfg.proto_ver = 1;

    auto pick = [](const std::vector<login::RealmInfo>&,
                   const login::LoginConfig&) -> std::uint32_t { return s_realm; };

    std::printf("#510 realm-list SOAK vs REAL authd\n");
    std::printf("  target authd: %s:%u  accounts=%s0000..%d  repeat=%d  realm_id=%u\n\n",
                host, port, prefix.c_str(), count - 1, repeat, s_realm);

    int attempts = 0;
    int failures = 0;
    int empty_realm_list = 0;  // the exact #510 symptom (kRealmUnavailable pre-select)

    for (int r = 0; r < repeat; ++r) {
        for (int i = 0; i < count; ++i) {
            const std::string account = account_name(prefix, i, width);
            ++attempts;

            login::TlsLoginTransport transport(host, port);
            if (!transport.ok()) {
                std::printf("  FAIL [%s] TLS connect: %s\n", account.c_str(),
                            transport.error().c_str());
                ++failures;
                continue;
            }

            std::vector<login::RealmInfo> realms;
            login::LoginResult res =
                login::run_login(transport, cfg, account, password, pick, &realms);

            const bool ok = res.status == login::LoginStatus::kSuccess &&
                            res.grant_id != 0 && !realms.empty();
            if (!ok) {
                ++failures;
                // The #510 fingerprint: auth-clearing accounts that still get an
                // empty realm list (or an Error frame mislabelled as one).
                if (realms.empty()) ++empty_realm_list;
                std::printf("  FAIL [%s] status=%s err_code=%u realms=%zu detail=\"%s\"\n",
                            account.c_str(), status_name(res.status),
                            res.server_error_code, realms.size(),
                            res.detail.c_str());
            }
        }
    }

    std::printf("\nSOAK COMPLETE: %d attempts, %d failures (%d empty-realm-list)\n",
                attempts, failures, empty_realm_list);
    if (failures == 0) {
        std::printf("ALL %d REAL-SOCKET LOGINS REACHED A GRANT WITH A NON-EMPTY "
                    "REALM LIST\n", attempts);
        return 0;
    }
    std::printf("%d/%d SOAK LOGINS FAILED — see per-account lines above\n",
                failures, attempts);
    return 1;
}
