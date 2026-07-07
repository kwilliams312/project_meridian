// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — meridian-client-probe (#301): the HEADLESS see-a-bot-move
// proof. Drives the GUI client's WORLD-SESSION net path (net::NetThreadCore + the
// shared clientnet stack — the SAME code scenes/world/world.gd runs) against a real
// authd + worldd with NO display: log in, enter the world, drain the #87 AoI relay,
// and report the EntityEnter + EntityUpdate frames the client received about the
// OTHER player (a bot walking a path). This is the client-side mirror of
// meridian-two-bot: it proves the relay reaches the GUI net path, not just the bot's.
//
// Driven by client/test/run_client_sees_bot_it.sh (boots MariaDB + authd + worldd,
// launches ONE bot, then this probe). Exit 0 iff the client SAW a peer ENTER and
// MOVE (EntityUpdate stream); non-zero otherwise. Prints the received frames so a
// partial result is HONEST about what reached the client.
//
// Usage:
//   meridian-client-probe --authd HOST:PORT --worldd HOST:PORT --user U --pass P
//                         [--realm R] [--duration S] [--build N] [--no-walk]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <openssl/ssl.h>

#include "client_world_probe_core.h"

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
        "usage: %s --authd HOST:PORT --worldd HOST:PORT --user U --pass P\n"
        "          [--realm R] [--duration S] [--build N] [--no-walk]\n"
        "\n"
        "  --authd    HOST:PORT   authd IF-1 TLS listener (the client logs in here)\n"
        "  --worldd   HOST:PORT   worldd IF-2 listener (the client enters here)\n"
        "  --user/--pass          the client's account credentials\n"
        "  --realm    R           realm id to select (default: first in range)\n"
        "  --duration S           seconds to stay in-world draining the relay (default 8)\n"
        "  --build    N           client build (default 1000; must be in realm range)\n"
        "  --no-walk              enter + observe only (do not send MovementIntents)\n",
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
    const char* pass = arg_after(argc, argv, "--pass");
    const char* realm_s = arg_after(argc, argv, "--realm");
    const char* duration_s = arg_after(argc, argv, "--duration");
    const char* build_s = arg_after(argc, argv, "--build");

    if (!authd_s || !worldd_s || !user || !pass) {
        std::fprintf(stderr,
                     "meridian-client-probe: --authd, --worldd, --user, --pass required\n\n");
        usage(argv[0]);
        return 2;
    }

    probe::ProbeConfig cfg;
    if (!split_hostport(authd_s, cfg.authd_host, cfg.authd_port)) {
        std::fprintf(stderr, "bad --authd '%s' (want HOST:PORT)\n", authd_s);
        return 2;
    }
    if (!split_hostport(worldd_s, cfg.worldd_host, cfg.worldd_port)) {
        std::fprintf(stderr, "bad --worldd '%s' (want HOST:PORT)\n", worldd_s);
        return 2;
    }
    cfg.account = user;
    cfg.password = pass;
    cfg.realm_id = realm_s ? static_cast<std::uint32_t>(std::strtoul(realm_s, nullptr, 10)) : 0u;
    cfg.client_build = build_s ? static_cast<std::uint32_t>(std::atoi(build_s)) : 1000u;
    const int duration_s_int = duration_s ? std::atoi(duration_s) : 8;
    cfg.duration_ms = static_cast<std::uint32_t>(duration_s_int > 0 ? duration_s_int : 8) * 1000u;
    cfg.walk = !has_flag(argc, argv, "--no-walk");

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);

    std::printf("meridian-client-probe (#301): headless GUI net path — see-a-bot-move\n");
    std::printf("  authd=%s:%u  worldd=%s:%u  build=%u  duration=%ds  walk=%s\n",
                cfg.authd_host.c_str(), cfg.authd_port, cfg.worldd_host.c_str(),
                cfg.worldd_port, cfg.client_build, duration_s_int, cfg.walk ? "yes" : "no");
    std::printf("  account=%s  realm=%u\n\n", cfg.account.c_str(), cfg.realm_id);

    probe::ProbeResult r = probe::run_probe(cfg);

    // ===== Report ==========================================================
    std::printf("client-world probe report\n");
    std::printf("  login_ok        : %s%s\n", r.login_ok ? "yes" : "no",
                r.login_ok ? "" : (" — " + r.login_detail).c_str());
    std::printf("  entered_world   : %s%s\n", r.entered_world ? "yes" : "no",
                r.entered_world ? " (HandshakeOk from real worldd)" : "");
    std::printf("  intents_sent    : %u\n", r.intents_sent);
    std::printf("  movement_states : %u (our authoritative replies)\n", r.movement_states);
    std::printf("  entity_frames   : %u (enters=%zu, updates=%u, leaves=%zu)\n",
                r.entity_frames, r.enters_by_guid.size(), r.total_updates_seen(),
                r.leaves_by_guid.size());
    std::printf("  tracked_remote  : %u (remote interpolator entities)\n", r.tracked_remote);
    if (r.disconnected)
        std::printf("  disconnected    : reason=%u (%s)\n", r.disconnect_reason,
                    r.detail.c_str());
    if (r.transport_closed) std::printf("  transport_closed: %s\n", r.detail.c_str());
    if (r.connect_failed)   std::printf("  connect_failed  : %s\n", r.detail.c_str());

    for (const auto& s : r.sightings) {
        const char* k = s.kind == probe::SightingKind::kEnter  ? "ENTER"
                        : s.kind == probe::SightingKind::kUpdate ? "UPDATE"
                                                                 : "LEAVE";
        if (s.has_position) {
            std::printf("    saw guid=%llu %s at wire(%.2f, %.2f, %.2f)\n",
                        static_cast<unsigned long long>(s.entity_guid), k, s.x, s.y, s.z);
        } else {
            std::printf("    saw guid=%llu %s (reason=%u)\n",
                        static_cast<unsigned long long>(s.entity_guid), k, s.leave_reason);
        }
    }

    // Machine-readable summary the harness greps (honest about each leg).
    const bool saw_enter = r.distinct_entities_seen() > 0;
    const bool saw_move = r.total_updates_seen() > 0;
    std::printf(
        "\nCLIENT_PROBE_RESULT login_ok=%d entered=%d saw_peer_enter=%d saw_peer_move=%d "
        "distinct_peers=%zu updates=%u tracked_remote=%u\n",
        r.login_ok ? 1 : 0, r.entered_world ? 1 : 0, saw_enter ? 1 : 0, saw_move ? 1 : 0,
        r.distinct_entities_seen(), r.total_updates_seen(), r.tracked_remote);

    if (r.entered_world && saw_enter && saw_move) {
        std::printf("\nmeridian-client-probe: OK — the GUI net path SAW A BOT ENTER + MOVE "
                    "(EntityEnter + EntityUpdate over the #87 relay)\n");
        return 0;
    }
    std::printf("\nmeridian-client-probe: INCOMPLETE (entered=%d saw_enter=%d saw_move=%d) — "
                "see the report above\n",
                r.entered_world, saw_enter, saw_move);
    return 1;
}
