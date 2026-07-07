// SPDX-License-Identifier: Apache-2.0
//
// Client login INTEGRATION test against a REAL authd (IT-M0 auth path, #99).
//
// THE KILLER TEST: this drives the actual client login CORE (login_core.* +
// srp_client_core.* + login_transport.*) over a REAL TLS 1.3 socket against a
// RUNNING authd daemon backed by a REAL MariaDB — the same authd + auth schema
// the server integration test (server/authd/test/login_flow_test.cpp, #79) uses.
// It proves WIRE INTEROP: our client speaks the exact IF-1 bytes authd expects,
// our SRP-6a client agrees with authd's ServerSession (M1 verifies server-side,
// M2 verifies client-side), the realm list round-trips, and a RealmSelect yields a
// valid single-use SessionGrant. A wrong password is rejected with no grant.
//
// This binary is DRIVEN by run_authd_login_it.sh, which: boots a throwaway MariaDB
// on a UNIQUE socket/port (NOT the dev default /tmp/mmdb.sock:3307 — see the
// script), loads the auth schema, creates the test account (meridian-account),
// seeds a realm, launches the real authd, then runs this test pointed at it.
//
// It is inert unless the harness passes the connection details, so it is safe as a
// ctest that "SKIPs" without a live authd (exit 0). What it needs, via argv/env:
//   --host H --port P     authd's TLS listener (from the harness)
//   --account U           the seeded account username
//   --password P          its password
//   --wrong-password P    a DIFFERENT password (bad-credentials assertion)
//   --realm-id N          the seeded realm id the client selects
//   --build N             the client build (must be in the realm's range)
// DB verification of the grant row is done by the SHELL harness (mariadb client)
// after this test, so this binary stays dependency-light (no meridian-db link).

#include "login_core.h"
#include "login_transport.h"

#include <openssl/ssl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace meridian;

namespace {

int g_fail = 0;
void check(const char* what, bool ok) {
    std::printf("  %-5s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_fail;
}

const char* arg_after(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    const char* host = arg_after(argc, argv, "--host");
    const char* port_s = arg_after(argc, argv, "--port");
    const char* account = arg_after(argc, argv, "--account");
    const char* password = arg_after(argc, argv, "--password");
    const char* wrong_pw = arg_after(argc, argv, "--wrong-password");
    const char* realm_id_s = arg_after(argc, argv, "--realm-id");
    const char* build_s = arg_after(argc, argv, "--build");

    if (!host || !port_s || !account || !password || !wrong_pw || !realm_id_s) {
        std::printf("SKIP: no live authd configured (need --host --port --account "
                    "--password --wrong-password --realm-id [--build])\n");
        return 0;  // inert without the harness — same discipline as the server IT.
    }

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);

    const auto port = static_cast<std::uint16_t>(std::atoi(port_s));
    const auto realm_id = static_cast<std::uint32_t>(std::strtoul(realm_id_s, nullptr, 10));
    login::LoginConfig cfg;
    cfg.client_build = build_s ? static_cast<std::uint32_t>(std::atoi(build_s)) : 1000;
    cfg.proto_ver = 1;

    std::printf("client login integration test vs REAL authd (IT-M0, #99)\n");
    std::printf("  target authd: %s:%u  account=%s  realm_id=%u  build=%u\n\n",
                host, port, account, realm_id, cfg.client_build);

    // ===== A. FULL LOGIN with the CORRECT password ==========================
    std::printf("A. FULL LOGIN over TLS 1.3 (correct password)\n");
    {
        login::TlsLoginTransport transport(host, port);
        check("A: TLS transport connected", transport.ok());
        check("A: TLS 1.3 negotiated", transport.tls_version() == "TLSv1.3");
        if (!transport.ok()) {
            std::printf("  FATAL: could not connect to authd (%s)\n",
                        transport.error().c_str());
            return 1;
        }

        std::vector<login::RealmInfo> realms;
        // Pin the seeded realm explicitly so the assertion is exact.
        static std::uint32_t s_realm = realm_id;
        auto pick = [](const std::vector<login::RealmInfo>&,
                       const login::LoginConfig&) -> std::uint32_t { return s_realm; };
        login::LoginResult r =
            login::run_login(transport, cfg, account, password, pick, &realms);

        check("A: login status == kSuccess", r.status == login::LoginStatus::kSuccess);
        check("A: SessionGrant received (grant_id != 0)", r.grant_id != 0);
        check("A: session_key is 32 bytes", r.session_key.size() == 32);
        check("A: reconnect window surfaced (> 0)", r.reconnect_window_ms > 0);
        check("A: selected realm is the seeded realm", r.selected_realm_id == realm_id);

        bool seeded_present = false;
        for (const login::RealmInfo& ri : realms) {
            if (ri.id == realm_id) seeded_present = true;
        }
        check("A: seeded realm present in RealmList", seeded_present);
        check("A: realm list non-empty", !realms.empty());

        // Emit the grant so the shell harness can verify + consume the row in DB.
        if (r.status == login::LoginStatus::kSuccess) {
            std::printf("GRANT_ID=%llu\n",
                        static_cast<unsigned long long>(r.grant_id));

            // IF-2 kickoff: build the WorldHello from the grant and prove it is a
            // well-formed frame (the client's handoff to worldd).
            login::Bytes nonce;
            login::Bytes wh = login::build_world_hello(r, cfg.client_build, &nonce);
            login::Bytes frame =
                login::encode_world_frame(login::kOpcodeWorldHello, 0, wh);
            check("A: WorldHello IF-2 frame built (has header + payload)",
                  frame.size() > 10 && nonce.size() == 16);
        }
    }

    // ===== C. WRONG PASSWORD -> rejected, NO grant ==========================
    // (B — grant persistence + single-use — is asserted by the shell harness via
    //  the mariadb client on the GRANT_ID emitted above.)
    std::printf("\nC. WRONG PASSWORD over TLS 1.3 -> rejected, no grant\n");
    {
        login::TlsLoginTransport transport(host, port);
        check("C: TLS transport connected (wrong-pw run)", transport.ok());
        if (transport.ok()) {
            login::LoginResult r =
                login::run_login(transport, cfg, account, wrong_pw, nullptr, nullptr);
            check("C: AuthResult is FAILURE for wrong password",
                  r.status == login::LoginStatus::kBadCredentials);
            check("C: no SessionGrant issued on wrong password", r.grant_id == 0);
            check("C: no session key on wrong password", r.session_key.empty());
        }
    }

    std::printf(g_fail == 0 ? "\nALL CLIENT↔AUTHD INTEGRATION TESTS PASSED\n"
                            : "\n%d CLIENT↔AUTHD INTEGRATION TEST(S) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
