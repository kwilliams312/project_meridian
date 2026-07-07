// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — headless bot CLI: meridian-bot (#111). The thin argv/stdio
// wrapper around the engine-free bot core. Composes the full client protocol
// against the REAL servers with NO Godot:
//   1. authd login (login::run_login over TLS 1.3, #99) -> SessionGrant,
//   2. worldd connect + enter-world + move (bot::run_world_session, this repo).
//
// This is the tool that enables the two-client see-each-other-move E2E (#151),
// soaks (#152), and the TD-11 load gate (#147). Bot v0 = ONE bot, login ->
// enter-world -> move; two-bot mutual visibility is a documented follow-up.
//
// Usage:
//   meridian-bot --authd HOST:PORT --worldd HOST:PORT --user U --password P
//                [--realm R] [--duration S] [--path square|idle] [--build N]
//
// Exit codes: 0 = the bot reached the requested milestone (enter-world always;
// movement when --path != idle and the run produced accepted server states);
// non-zero = login/handshake/movement failure (the message says which).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <openssl/ssl.h>

#include "bot_core.h"
#include "login_core.h"
#include "login_transport.h"

using namespace meridian;

namespace {

const char* arg_after(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return nullptr;
}

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

// Split "host:port" into (host, port). Returns false if there is no ':' or the
// port is not a positive number. IPv6 literals are out of scope for the M0 CLI
// (127.0.0.1 / a DNS name is what the harness passes).
bool split_hostport(const std::string& s, std::string& host, std::uint16_t& port) {
    const auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) return false;
    host = s.substr(0, colon);
    const long p = std::strtol(s.c_str() + colon + 1, nullptr, 10);
    if (p <= 0 || p > 65535) return false;
    port = static_cast<std::uint16_t>(p);
    return true;
}

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s --authd HOST:PORT --worldd HOST:PORT --user U --password P\n"
        "          [--realm R] [--duration S] [--path square|idle] [--build N]\n"
        "\n"
        "  --authd    HOST:PORT   authd IF-1 TLS listener (login)\n"
        "  --worldd   HOST:PORT   worldd IF-2 TLS listener (enter-world + move)\n"
        "  --user     U           account username\n"
        "  --password P           account password\n"
        "  --realm    R           realm id to select (default: first in range)\n"
        "  --duration S           movement-loop seconds (default 10; 20 Hz sim)\n"
        "  --path     P           scripted path: square (default) | idle\n"
        "  --build    N           client build (default 1000; must be in realm range)\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        usage(argv[0]);
        return 0;
    }

    const char* authd_s = arg_after(argc, argv, "--authd");
    const char* worldd_s = arg_after(argc, argv, "--worldd");
    const char* user = arg_after(argc, argv, "--user");
    const char* password = arg_after(argc, argv, "--password");
    const char* realm_s = arg_after(argc, argv, "--realm");
    const char* duration_s = arg_after(argc, argv, "--duration");
    const char* path_s = arg_after(argc, argv, "--path");
    const char* build_s = arg_after(argc, argv, "--build");

    if (!authd_s || !worldd_s || !user || !password) {
        std::fprintf(stderr, "meridian-bot: --authd, --worldd, --user, --password are required\n\n");
        usage(argv[0]);
        return 2;
    }

    std::string authd_host, worldd_host;
    std::uint16_t authd_port = 0, worldd_port = 0;
    if (!split_hostport(authd_s, authd_host, authd_port)) {
        std::fprintf(stderr, "meridian-bot: bad --authd '%s' (want HOST:PORT)\n", authd_s);
        return 2;
    }
    if (!split_hostport(worldd_s, worldd_host, worldd_port)) {
        std::fprintf(stderr, "meridian-bot: bad --worldd '%s' (want HOST:PORT)\n", worldd_s);
        return 2;
    }

    const std::uint32_t client_build =
        build_s ? static_cast<std::uint32_t>(std::atoi(build_s)) : 1000u;
    const int duration_s_int = duration_s ? std::atoi(duration_s) : 10;
    const std::uint32_t movement_ticks =
        duration_s_int > 0 ? static_cast<std::uint32_t>(duration_s_int) * 20u : 0u;  // 20 Hz
    const bot::BotPath path = bot::parse_path(path_s ? path_s : "square");
    const bool want_realm = (realm_s != nullptr);
    const std::uint32_t realm_id =
        want_realm ? static_cast<std::uint32_t>(std::strtoul(realm_s, nullptr, 10)) : 0u;

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);

    std::printf("meridian-bot v0 (#111): login -> enter-world -> move (headless, no Godot)\n");
    std::printf("  authd=%s:%u  worldd=%s:%u  user=%s  build=%u  path=%s  duration=%ds\n\n",
                authd_host.c_str(), authd_port, worldd_host.c_str(), worldd_port,
                user, client_build, path == bot::BotPath::kIdle ? "idle" : "square",
                duration_s_int);

    // ===== 1. authd LOGIN over TLS 1.3 (#99) ================================
    std::printf("1. authd login over TLS 1.3\n");
    login::LoginConfig cfg;
    cfg.client_build = client_build;
    cfg.proto_ver = 1;

    login::LoginResult grant;
    {
        login::TlsLoginTransport transport(authd_host, authd_port);
        if (!transport.ok()) {
            std::printf("  FAIL: could not connect to authd (%s)\n", transport.error().c_str());
            return 1;
        }
        std::printf("  ok    connected to authd (%s)\n", transport.tls_version().c_str());

        static std::uint32_t s_realm = realm_id;
        static bool s_want_realm = want_realm;
        auto pick = [](const std::vector<login::RealmInfo>& realms,
                       const login::LoginConfig&) -> std::uint32_t {
            if (s_want_realm) return s_realm;
            return realms.empty() ? 0 : realms.front().id;
        };
        grant = login::run_login(transport, cfg, user, password, pick, nullptr);
    }

    if (grant.status != login::LoginStatus::kSuccess) {
        std::printf("  FAIL: login status != success (%s)\n", grant.detail.c_str());
        return 1;
    }
    std::printf("  ok    login succeeded (grant_id=%llu, realm=%u, session_key=%zuB)\n",
                static_cast<unsigned long long>(grant.grant_id),
                grant.selected_realm_id, grant.session_key.size());

    // ===== 2. worldd ENTER-WORLD + MOVE (this repo) ========================
    std::printf("\n2. worldd enter-world + move over TLS 1.3\n");
    bot::BotWorldConfig wcfg;
    wcfg.client_build = client_build;
    wcfg.path = path;
    wcfg.movement_ticks = movement_ticks;

    bot::BotRunResult run;
    {
        login::TlsLoginTransport wtransport(worldd_host, worldd_port);
        if (!wtransport.ok()) {
            std::printf("  FAIL: could not connect to worldd (%s)\n", wtransport.error().c_str());
            return 1;
        }
        std::printf("  ok    connected to worldd (%s)\n", wtransport.tls_version().c_str());
        run = bot::run_world_session(wtransport, grant, wcfg);
    }

    // ===== 3. HONEST run report ============================================
    std::printf("\n3. run report\n");
    std::printf("  connected         : %s\n", run.connected ? "yes" : "no");
    std::printf("  handshake_ok      : %s%s\n", run.handshake_ok ? "yes" : "no",
                run.handshake_ok ? " (ENTERED WORLD)" : "");
    if (run.disconnected) {
        std::printf("  disconnected      : yes (reason=%u: %s)\n",
                    run.disconnect_reason, run.detail.c_str());
    }
    std::printf("  intents_sent      : %u\n", run.intents_sent);
    std::printf("  states_received   : %u\n", run.states_received);
    std::printf("  moves_accepted    : %u\n", run.moves_accepted);
    std::printf("  entity_updates    : %u\n", run.entity_updates);
    // #248: the OTHER players this bot SAW (the #87 AoI relay). One bot alone sees
    // nobody; the two-bot harness is where these are non-zero (see-each-other-move).
    std::printf("  entities_seen     : %zu distinct (enters=%zu, updates=%u, leaves=%zu)\n",
                run.distinct_entities_seen(), run.enters_by_guid.size(),
                run.total_updates_seen(), run.leaves_by_guid.size());
    for (const auto& s : run.sightings) {
        const char* k = s.kind == bot::SightingKind::kEnter  ? "ENTER"
                        : s.kind == bot::SightingKind::kUpdate ? "UPDATE"
                                                               : "LEAVE";
        if (s.has_position) {
            std::printf("    saw guid=%llu %s at (%.2f, %.2f, %.2f)\n",
                        static_cast<unsigned long long>(s.entity_guid), k, s.x, s.y, s.z);
        } else {
            std::printf("    saw guid=%llu %s (reason=%u)\n",
                        static_cast<unsigned long long>(s.entity_guid), k, s.leave_reason);
        }
    }
    std::printf("  final position    : (%.2f, %.2f, %.2f) [wire x,y,z]\n",
                run.final_x, run.final_y, run.final_z);
    std::printf("  moved from spawn  : %.2f m\n", run.moved_distance);
    std::printf("  detail            : %s\n", run.detail.c_str());

    // Machine-readable summary line for the integration harness to grep. #248 adds
    // the AoI-visibility fields so a two-bot harness can assert mutual visibility
    // straight off this line (entities_seen / entity_enters / entity_updates).
    std::printf("BOT_RESULT handshake_ok=%d intents_sent=%u states_received=%u "
                "moves_accepted=%u moved_distance=%.3f disconnect_reason=%u "
                "entities_seen=%zu entity_enters=%zu entity_updates=%u\n",
                run.handshake_ok ? 1 : 0, run.intents_sent, run.states_received,
                run.moves_accepted, run.moved_distance, run.disconnect_reason,
                run.distinct_entities_seen(), run.enters_by_guid.size(),
                run.total_updates_seen());

    if (!run.handshake_ok) {
        std::printf("\nmeridian-bot: FAILED to enter world\n");
        return 1;
    }
    if (path != bot::BotPath::kIdle && run.moves_accepted == 0) {
        std::printf("\nmeridian-bot: entered world but NO movement was accepted by the server\n");
        return 1;
    }
    std::printf("\nmeridian-bot: OK (login -> enter-world%s)\n",
                path != bot::BotPath::kIdle ? " -> move" : "");
    return 0;
}
