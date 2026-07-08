// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — TWO-BOT AoI E2E driver: meridian-two-bot (#248). The
// see-each-other-move capstone (IT-M0 §Step 3, DC-4): two HEADLESS bots log into
// the REAL authd, both enter the REAL worldd near each other, both MOVE, and each
// RECEIVES the OTHER's entity updates via the #87 AoI relay — the automatable
// evidence for the #151 formal execution gate.
//
// This composes the #111 single-bot core TWICE, concurrently:
//   bot A (thread)                          bot B (thread)
//     login::run_login (authd)                login::run_login (authd)
//     connect worldd                          connect worldd
//     WorldHello -> HandshakeOk               WorldHello -> HandshakeOk
//     ── rendezvous barrier ──────────────────── (both now in-world) ──
//     move (square) + capture B's frames      move (square) + capture A's frames
//
// The BARRIER is the crux of mutual visibility: it holds each bot right after it
// enters the world until BOTH have entered, so worldd's #87 enter() has registered
// both sessions in the AoI grid and sent each an EntityEnter for the other BEFORE
// either starts moving. Then, as both walk their square paths, worldd relays each
// mover's MovementState to the OTHER as an EntityUpdate — which the #248
// entity-capture drain records. So each bot ends with: 1 EntityEnter (saw the peer
// on login) + N EntityUpdate (saw the peer MOVE). That is the proof.
//
// ENGINE-FREE (Client SAD §9.2): NO Godot. Same TlsLoginTransport + bot core the
// single-bot CLI (#111) uses. Two bots = two threads over that same seam.
//
// Usage:
//   meridian-two-bot --authd HOST:PORT --worldd HOST:PORT \
//                    --userA UA --passA PA --userB UB --passB PB \
//                    [--realm R] [--duration S] [--build N]
//
// Exit codes: 0 = BOTH bots entered the world AND each saw the other ENTER and
// MOVE (mutual visibility fully proven); non-zero otherwise (the report + the
// TWO_BOT_RESULT line say exactly which leg failed — honest about partial results).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
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
        "usage: %s --authd HOST:PORT --worldd HOST:PORT\n"
        "          --userA UA --passA PA --userB UB --passB PB\n"
        "          [--realm R] [--duration S] [--build N]\n"
        "\n"
        "  --authd    HOST:PORT   authd IF-1 TLS listener (both bots log in here)\n"
        "  --worldd   HOST:PORT   worldd IF-2 TLS listener (both bots enter here)\n"
        "  --userA/--passA        account A credentials\n"
        "  --userB/--passB        account B credentials\n"
        "  --realm    R           realm id both bots select (default: first in range)\n"
        "  --duration S           movement-loop seconds per bot (default 6; 20 Hz)\n"
        "  --build    N           client build (default 1000; must be in realm range)\n",
        prog);
}

// A tiny two-party rendezvous barrier (C++17 — std::barrier is C++20). Both bots
// call wait() after entering the world; neither proceeds until both have arrived
// (or the timeout trips, so a stuck peer can't hang the run forever).
class Barrier {
public:
    explicit Barrier(int parties) : parties_(parties) {}
    // Returns true if both parties arrived, false on timeout (proceed anyway).
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m_);
        if (++arrived_ >= parties_) {
            cv_.notify_all();
            return true;
        }
        return cv_.wait_for(lk, timeout, [&] { return arrived_ >= parties_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int parties_;
    int arrived_ = 0;
};

// Realm selection for run_login's function-pointer `select_realm`. Both bots
// select the SAME realm (set once in main, before any thread starts), so these
// file-scope values are written-once / read-only across the two worker threads —
// no data race. A capturing lambda cannot convert to the required function pointer,
// hence this small shared-const shim.
std::uint32_t g_realm_id = 0;
bool g_want_realm = false;

std::uint32_t select_realm_fn(const std::vector<login::RealmInfo>& realms,
                              const login::LoginConfig&) {
    if (g_want_realm) return g_realm_id;
    return realms.empty() ? 0u : realms.front().id;
}

// One bot's inputs + captured result (filled by the worker thread).
struct BotSlot {
    const char* label = "";
    std::string user;
    std::string password;
    bool login_ok = false;
    std::string login_detail;
    bot::BotRunResult run;
};

// Run ONE bot end-to-end (login -> enter-world -> move), synchronizing on `barrier`
// between enter-world and movement. Fills `slot.run`. All state is thread-local
// except the shared barrier + the slot this thread owns, so two of these run
// concurrently with no further locking.
void run_one_bot(BotSlot& slot, const std::string& authd_host, std::uint16_t authd_port,
                 const std::string& worldd_host, std::uint16_t worldd_port,
                 std::uint32_t client_build, std::uint32_t movement_ticks,
                 Barrier& barrier) {
    // 1. authd login over TLS 1.3 (#99).
    login::LoginConfig cfg;
    cfg.client_build = client_build;
    cfg.proto_ver = 1;

    login::LoginResult grant;
    {
        login::TlsLoginTransport transport(authd_host, authd_port);
        if (!transport.ok()) {
            slot.login_detail = "connect authd: " + transport.error();
            return;
        }
        grant = login::run_login(transport, cfg, slot.user, slot.password,
                                 &select_realm_fn, nullptr);
    }
    if (grant.status != login::LoginStatus::kSuccess) {
        slot.login_detail = "login: " + grant.detail;
        return;
    }
    slot.login_ok = true;

    // 2. worldd enter-world + move, with the rendezvous barrier between the two.
    bot::BotWorldConfig wcfg;
    wcfg.client_build = client_build;
    wcfg.path = bot::BotPath::kSquare;
    wcfg.movement_ticks = movement_ticks;
    // Character name = this bot's account username (globally unique) so the two bots
    // create/enter DISTINCT characters when their accounts are bare (D-35 flow).
    wcfg.character_name = slot.user;
    wcfg.on_entered_world = [&barrier] {
        // Hold until BOTH bots are in-world (or 10 s safety timeout). See file header.
        barrier.wait(std::chrono::milliseconds(10000));
    };

    login::TlsLoginTransport wtransport(worldd_host, worldd_port);
    if (!wtransport.ok()) {
        slot.login_detail = "connect worldd: " + wtransport.error();
        // Still release the barrier so the peer is not stuck waiting for us.
        barrier.wait(std::chrono::milliseconds(1));
        return;
    }
    slot.run = bot::run_world_session(wtransport, grant, wcfg);
}

void print_bot_report(const BotSlot& s) {
    std::printf("  bot %s (user=%s)\n", s.label, s.user.c_str());
    std::printf("    login_ok        : %s%s\n", s.login_ok ? "yes" : "no",
                s.login_ok ? "" : (" — " + s.login_detail).c_str());
    std::printf("    handshake_ok    : %s%s\n", s.run.handshake_ok ? "yes" : "no",
                s.run.handshake_ok ? " (ENTERED WORLD)" : "");
    std::printf("    intents_sent    : %u\n", s.run.intents_sent);
    std::printf("    moves_accepted  : %u (moved %.2f m from spawn)\n",
                s.run.moves_accepted, s.run.moved_distance);
    std::printf("    entities_seen   : %zu distinct (enters=%zu, updates=%u, leaves=%zu)\n",
                s.run.distinct_entities_seen(), s.run.enters_by_guid.size(),
                s.run.total_updates_seen(), s.run.leaves_by_guid.size());
    for (const auto& sight : s.run.sightings) {
        const char* k = sight.kind == bot::SightingKind::kEnter  ? "ENTER"
                        : sight.kind == bot::SightingKind::kUpdate ? "UPDATE"
                                                                   : "LEAVE";
        if (sight.has_position) {
            std::printf("      saw guid=%llu %s at (%.2f, %.2f, %.2f)\n",
                        static_cast<unsigned long long>(sight.entity_guid), k,
                        sight.x, sight.y, sight.z);
        } else {
            std::printf("      saw guid=%llu %s (reason=%u)\n",
                        static_cast<unsigned long long>(sight.entity_guid), k,
                        sight.leave_reason);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (has_flag(argc, argv, "--help") || has_flag(argc, argv, "-h")) {
        usage(argv[0]);
        return 0;
    }

    const char* authd_s = arg_after(argc, argv, "--authd");
    const char* worldd_s = arg_after(argc, argv, "--worldd");
    const char* userA = arg_after(argc, argv, "--userA");
    const char* passA = arg_after(argc, argv, "--passA");
    const char* userB = arg_after(argc, argv, "--userB");
    const char* passB = arg_after(argc, argv, "--passB");
    const char* realm_s = arg_after(argc, argv, "--realm");
    const char* duration_s = arg_after(argc, argv, "--duration");
    const char* build_s = arg_after(argc, argv, "--build");

    if (!authd_s || !worldd_s || !userA || !passA || !userB || !passB) {
        std::fprintf(stderr, "meridian-two-bot: --authd, --worldd, --userA/--passA, "
                             "--userB/--passB are required\n\n");
        usage(argv[0]);
        return 2;
    }

    std::string authd_host, worldd_host;
    std::uint16_t authd_port = 0, worldd_port = 0;
    if (!split_hostport(authd_s, authd_host, authd_port)) {
        std::fprintf(stderr, "meridian-two-bot: bad --authd '%s' (want HOST:PORT)\n", authd_s);
        return 2;
    }
    if (!split_hostport(worldd_s, worldd_host, worldd_port)) {
        std::fprintf(stderr, "meridian-two-bot: bad --worldd '%s' (want HOST:PORT)\n", worldd_s);
        return 2;
    }

    const std::uint32_t client_build =
        build_s ? static_cast<std::uint32_t>(std::atoi(build_s)) : 1000u;
    const int duration_s_int = duration_s ? std::atoi(duration_s) : 6;
    const std::uint32_t movement_ticks =
        duration_s_int > 0 ? static_cast<std::uint32_t>(duration_s_int) * 20u : 0u;  // 20 Hz
    g_want_realm = (realm_s != nullptr);
    g_realm_id =
        g_want_realm ? static_cast<std::uint32_t>(std::strtoul(realm_s, nullptr, 10)) : 0u;

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS, nullptr);

    std::printf("meridian-two-bot (#248): two headless bots, see-each-other-move\n");
    std::printf("  authd=%s:%u  worldd=%s:%u  build=%u  duration=%ds\n",
                authd_host.c_str(), authd_port, worldd_host.c_str(), worldd_port,
                client_build, duration_s_int);
    std::printf("  bot A=%s  bot B=%s\n\n", userA, userB);

    BotSlot a;
    a.label = "A";
    a.user = userA;
    a.password = passA;
    BotSlot b;
    b.label = "B";
    b.user = userB;
    b.password = passB;

    Barrier barrier(2);

    std::thread ta(run_one_bot, std::ref(a), std::cref(authd_host), authd_port,
                   std::cref(worldd_host), worldd_port, client_build,
                   movement_ticks, std::ref(barrier));
    std::thread tb(run_one_bot, std::ref(b), std::cref(authd_host), authd_port,
                   std::cref(worldd_host), worldd_port, client_build,
                   movement_ticks, std::ref(barrier));
    ta.join();
    tb.join();

    // ===== Combined report =================================================
    std::printf("run report (both bots)\n");
    print_bot_report(a);
    std::printf("\n");
    print_bot_report(b);

    // The server assigns each D-11 placeholder session a DISTINCT synthetic guid
    // (#87 fix). So the guid bot A saw enter is bot B's, and vice versa — they must
    // differ. Pull each bot's single observed guid for the cross-check.
    auto sole_seen_guid = [](const BotSlot& s) -> std::uint64_t {
        if (s.run.enters_by_guid.size() == 1) return s.run.enters_by_guid.begin()->first;
        return 0;
    };
    const std::uint64_t a_saw = sole_seen_guid(a);
    const std::uint64_t b_saw = sole_seen_guid(b);

    // ===== The see-each-other-move verdict, per leg ========================
    const bool both_entered = a.run.handshake_ok && b.run.handshake_ok;
    const bool both_moved = a.run.moves_accepted > 0 && b.run.moves_accepted > 0;
    const bool a_saw_b_enter = a.run.distinct_entities_seen() >= 1;
    const bool b_saw_a_enter = b.run.distinct_entities_seen() >= 1;
    const bool mutual_enter = a_saw_b_enter && b_saw_a_enter;
    const bool a_saw_b_move = a.run.total_updates_seen() > 0;
    const bool b_saw_a_move = b.run.total_updates_seen() > 0;
    const bool mutual_move = a_saw_b_move && b_saw_a_move;
    // Distinct guids (server gave each placeholder a unique id) — a strong signal the
    // two are NOT rendering as one entity.
    const bool distinct_guids = (a_saw != 0 && b_saw != 0 && a_saw != b_saw);

    std::printf("\nsee-each-other-move verdict\n");
    std::printf("  both entered world        : %s\n", both_entered ? "YES" : "no");
    std::printf("  both moved (server-accepted): %s\n", both_moved ? "YES" : "no");
    std::printf("  A saw B ENTER             : %s\n", a_saw_b_enter ? "YES" : "no");
    std::printf("  B saw A ENTER             : %s\n", b_saw_a_enter ? "YES" : "no");
    std::printf("  A saw B MOVE (EntityUpdate): %s\n", a_saw_b_move ? "YES" : "no");
    std::printf("  B saw A MOVE (EntityUpdate): %s\n", b_saw_a_move ? "YES" : "no");
    std::printf("  distinct peer guids       : %s (A saw %llu, B saw %llu)\n",
                distinct_guids ? "YES" : "no",
                static_cast<unsigned long long>(a_saw),
                static_cast<unsigned long long>(b_saw));

    // Machine-readable summary line for the harness to grep. Reports EACH leg so the
    // harness (and the PR) can be honest about full vs partial mutual visibility.
    std::printf(
        "TWO_BOT_RESULT both_entered=%d both_moved=%d "
        "a_saw_b_enter=%d b_saw_a_enter=%d a_saw_b_move=%d b_saw_a_move=%d "
        "a_enters=%zu b_enters=%zu a_updates=%u b_updates=%u distinct_guids=%d\n",
        both_entered ? 1 : 0, both_moved ? 1 : 0, a_saw_b_enter ? 1 : 0,
        b_saw_a_enter ? 1 : 0, a_saw_b_move ? 1 : 0, b_saw_a_move ? 1 : 0,
        a.run.enters_by_guid.size(), b.run.enters_by_guid.size(),
        a.run.total_updates_seen(), b.run.total_updates_seen(),
        distinct_guids ? 1 : 0);

    // The headline: both entered + mutual enter + mutual move == see-each-other-move.
    if (both_entered && mutual_enter && mutual_move) {
        std::printf("\nmeridian-two-bot: OK — TWO BOTS SEE EACH OTHER MOVE "
                    "(mutual EntityEnter + EntityUpdate)\n");
        return 0;
    }
    std::printf("\nmeridian-two-bot: mutual visibility INCOMPLETE "
                "(entered=%d mutual_enter=%d mutual_move=%d) — see the verdict above\n",
                both_entered, mutual_enter, mutual_move);
    return 1;
}
