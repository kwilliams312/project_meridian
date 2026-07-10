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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

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
        "  --build    N           client build (default 1000; must be in realm range)\n"
        "  --reconnect [MODE]     after the first session, force a transient drop and\n"
        "                         exercise the #96 reconnect state machine. MODE:\n"
        "                           relogin (default) — fresh authd login + re-enter\n"
        "                                               (WORKS at M0; a re-login, not\n"
        "                                               a token resume)\n"
        "                           resume            — re-present the SAME grant to\n"
        "                                               worldd (a true token resume;\n"
        "                                               REJECTED at M0 — single-use\n"
        "                                               grant. Proves the server gap.)\n",
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
    const char* char_name_s = arg_after(argc, argv, "--character-name");  // default: --user
    const bool want_reconnect = has_flag(argc, argv, "--reconnect");
    const char* reconnect_mode_s = arg_after(argc, argv, "--reconnect");  // optional MODE token

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
    // Character name to create-if-empty on ENTER_WORLD (D-35). Defaults to the
    // account username (matches the harness's add-characters), overridable via
    // --character-name. The harness usually pre-creates it, so CharList finds it.
    wcfg.character_name = char_name_s ? std::string(char_name_s) : std::string(user);

    bot::BotRunResult run;
    if (!want_reconnect) {
        login::TlsLoginTransport wtransport(worldd_host, worldd_port);
        if (!wtransport.ok()) {
            std::printf("  FAIL: could not connect to worldd (%s)\n", wtransport.error().c_str());
            return 1;
        }
        std::printf("  ok    connected to worldd (%s)\n", wtransport.tls_version().c_str());
        run = bot::run_world_session(wtransport, grant, wcfg);
    } else {
        // ===== 2r. #96 reconnect: enter world, force a transient drop, drive the
        //           connection state machine's backoff/attempt loop over REAL TLS. =
        const bool resume_mode =
            reconnect_mode_s && std::strcmp(reconnect_mode_s, "resume") == 0;
        std::printf("  reconnect: enabled (mode=%s)\n",
                    resume_mode ? "resume (token; single-use grant -> REJECTED at M0)"
                                : "relogin (fresh grant -> works at M0)");

        bot::ReconnectConfig rc;
        rc.strategy = resume_mode ? bot::ReconnectStrategy::kResumeWithGrant
                                  : bot::ReconnectStrategy::kFullRelogin;
        // Seed the reconnect window from the grant's server-owned reconnect_window_ms
        // (#66) — the budget past which the grant is presumed expired (SAD §5.1).
        rc.backoff.window_ms = grant.reconnect_window_ms;
        rc.backoff.base_delay_ms = 250;
        rc.backoff.max_delay_ms = 4000;
        rc.backoff.max_attempts = 6;
        rc.world = wcfg;

        // A fresh worldd TLS connection per attempt (the dropped socket is dead).
        auto connect = [&]() -> std::unique_ptr<login::ILoginTransport> {
            auto t = std::make_unique<login::TlsLoginTransport>(worldd_host, worldd_port);
            if (!t->ok()) return nullptr;
            return t;
        };
        // A fresh authd login per relogin attempt (kFullRelogin — the working path).
        static std::uint32_t s_realm2 = realm_id;
        static bool s_want_realm2 = want_realm;
        auto relogin = [&]() -> login::LoginResult {
            login::TlsLoginTransport lt(authd_host, authd_port);
            if (!lt.ok()) {
                login::LoginResult r;
                r.status = login::LoginStatus::kConnectFailed;
                return r;
            }
            auto pick = [](const std::vector<login::RealmInfo>& realms,
                           const login::LoginConfig&) -> std::uint32_t {
                if (s_want_realm2) return s_realm2;
                return realms.empty() ? 0 : realms.front().id;
            };
            return login::run_login(lt, cfg, user, password, pick, nullptr);
        };
        // The first connection for the initial enter-world.
        auto first = std::make_unique<login::TlsLoginTransport>(worldd_host, worldd_port);
        if (!first->ok()) {
            std::printf("  FAIL: could not connect to worldd (%s)\n", first->error().c_str());
            return 1;
        }
        std::printf("  ok    connected to worldd (%s)\n", first->tls_version().c_str());
        // Force exactly one transient drop after the first session.
        bool dropped_once = false;
        auto inject_drop = [&dropped_once]() {
            if (dropped_once) return false;
            dropped_once = true;
            return true;
        };
        // Real clock seams: steady_clock + a real sleep for the backoff.
        auto now = []() -> std::uint64_t {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        };
        auto wait = [](std::uint32_t ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };

        bot::ReconnectRunReport rr = bot::run_session_with_reconnect(
            std::move(first), grant, rc, connect, relogin, inject_drop, now, wait);

        run = rr.first_session;

        std::printf("\n2r. reconnect episode report\n");
        std::printf("  drop injected     : %s\n", rr.dropped ? "yes" : "no");
        std::printf("  strategy          : %s\n", bot::to_string(rc.strategy));
        std::printf("  attempts          : %u\n", rr.reconnect.attempts);
        std::printf("  reconnect_ms      : %llu\n",
                    static_cast<unsigned long long>(rr.reconnect.total_reconnect_ms));
        std::printf("  reconnected       : %s\n", rr.reconnect.reconnected ? "yes" : "no");
        std::printf("  resumed w/o relog : %s%s\n",
                    rr.reconnect.resumed_without_relogin ? "YES" : "no",
                    rr.reconnect.resumed_without_relogin
                        ? " (true token-reconnect)"
                        : " (server has no session-resume path at M0)");
        std::printf("  final state       : %s\n", bot::to_string(rr.final_state));
        std::printf("  detail            : %s\n", rr.reconnect.detail.c_str());
        for (const auto& a : rr.reconnect.attempt_log) {
            std::printf("    attempt %u (backoff %ums, +%llums): %s\n", a.attempt_index,
                        a.backoff_ms, static_cast<unsigned long long>(a.elapsed_ms),
                        bot::to_string(a.outcome));
        }
        if (rr.reconnect.reconnected) run = rr.resumed_session;
        // Machine-readable line the reconnect harness greps.
        std::printf("RECONNECT_RESULT dropped=%d reconnected=%d resumed_without_relogin=%d "
                    "attempts=%u final_state=%s strategy=%s\n",
                    rr.dropped ? 1 : 0, rr.reconnect.reconnected ? 1 : 0,
                    rr.reconnect.resumed_without_relogin ? 1 : 0, rr.reconnect.attempts,
                    bot::to_string(rr.final_state), bot::to_string(rc.strategy));
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
