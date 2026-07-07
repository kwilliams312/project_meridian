// SPDX-License-Identifier: Apache-2.0
//
// worldd — shard worker / map simulation daemon (entry point).
//
// What this BECOMES (server SAD §2.5, recast at M3): the shard worker that runs
// the simulation. Per-map update threads run the 20 Hz tick (drain inbound →
// movement → AI → combat/auras → spawns → AoI delta → flush); a grid/AoI engine
// (533 m grids, 8×8 cells); the entity/aggro/threat model; the combat resolver;
// quest/loot/inventory/vendor services; spatial chat; the generated opcode
// dispatcher; the script-hook seam; the IF-7 hot-reload service; and the async
// DB I/O layer. At M0 it also carries the net gate + dispatcher + IF-2 codec
// (SAD §7) before those move to gatewayd at M2. From M3 it is bus-attached with
// no client listener and MapKey = (map_id, instance_id, shard_index) keying.
//
// What this file IS NOW (issues #82, #83): worldd as a PROCESS —
//   - the world/update thread + a map/IO worker pool (WorldServer, SAD §2.5/§6),
//   - a meridian::net TLS 1.3 listener + IF-2 length framing (M0 transport),
//   - the IF-2 opcode dispatcher (u16 Opcode -> handler table) with M0 stub
//     handlers; unknown/reserved/malformed frames -> Disconnect + close.
//
// Concurrency model (M0, documented decision): an ACCEPTOR on the main thread
// hands each accepted Session to the IO WORKER POOL (a bounded set of worker
// threads owned by main). Each worker runs one connection's read→dispatch loop
// to completion (WorldServer::serve_connection), owns its socket, and NEVER
// touches game state — decoded simulation work is enqueued to the WORLD THREAD
// over the WorldServer inbound queue (SAD §6.1). This is the same "acceptor
// hands each Session to a worker" shape authd uses (§2.1), extended with the
// world-thread + queue that worldd (unlike authd) needs.
//
// DELIBERATELY NOT HERE (later stories): grant/session validation (#84),
// movement simulation (#86), AoI (#87), the real tick body, the message bus,
// AEAD session crypto. TLS is the M0 transport; per-session AEAD is #84.
//
// Clean-room: implemented from the SAD, no GPL source consulted (CONTRIBUTING).

#include "meridian/core/log.hpp"
#include "meridian/core/version.hpp"
#include "meridian/db/connection.h"
#include "meridian/metrics/catalog.h"
#include "meridian/metrics/exposer.h"
#include "meridian/net/tls_listener.h"

#include "world_boot.h"
#include "world_dispatch.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kDaemonName = "worldd";

// Runtime configuration, assembled from flags + env (same shape as authd's
// AuthdConfig: flags override env override defaults; no config file yet).
struct WorlddConfig {
    // TLS listener (meridian-net ListenConfig fields).
    std::string cert_path;
    std::string key_path;
    std::string bind_addr = "0.0.0.0";
    std::uint16_t port = 7200;  // IF-2 default (SAD §5.2)

    // World-process scaffold sizing (WorldServerConfig).
    meridian::worldd::WorldServerConfig world;

    // World content DB (IF-4; #89). The read-only mcc artifact worldd boots from:
    // it reads the `world_manifest` (content version + BLAKE3 hash mcc recorded)
    // and refuses to boot on a schema-version mismatch / missing manifest (SAD
    // §4.3). Same MERIDIAN_*DB_* env shape as the auth/char DBs. When `user` is
    // empty (no MERIDIAN_WORLDDB_* set), the boot check is SKIPPED — the daemon
    // still serves (dispatcher, session path) without a world DB wired, which
    // keeps the DB-less smoke run / dispatch test runnable at M0.
    meridian::db::ConnectParams world_db;
    // Optional operator-pinned expected content hash (SAD §5.4.3 content-hash
    // tie). When set + it disagrees with the loaded hash -> a loud WARNING at
    // M0–M1 (bootable), not a refusal. Empty -> the tie is not checked.
    std::string expected_content_hash;

    // OPS-05 metrics endpoint (server SAD §8.5; docs/telemetry-architecture.md).
    // Plain-HTTP /metrics on a port SEPARATE from the game TLS port; default 9464
    // (the port the OTel collector scrapes) bound to loopback. 0 disables it.
    std::uint16_t metrics_port = 9464;
    std::string metrics_bind = "127.0.0.1";

    // OPS-05 structured logging (#165). json (Loki JSON on stdout, prod default)
    // or text (readable stderr, dev). Env MERIDIAN_LOG_FORMAT / _LEVEL apply
    // first, then these flags override. Empty = leave env/default.
    std::string log_format;
    std::string log_level;
};

const char* env(const char* k) { return std::getenv(k); }

void print_help() {
    std::printf(
        "%s — Project Meridian shard worker / map simulation daemon\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --cert PATH        TLS server certificate (PEM). Required to serve.\n"
        "  --key PATH         TLS private key (PEM). Required to serve.\n"
        "  --bind ADDR        Bind address (default 0.0.0.0).\n"
        "  --port N           Listen port (default 7200, IF-2).\n"
        "  --io-workers N     Size of the map/IO worker pool (default: auto).\n"
        "  --metrics-port N   Prometheus /metrics port (default 9464; 0=off).\n"
        "  --metrics-bind ADDR  /metrics bind address (default 127.0.0.1).\n"
        "  --realm NAME       realm label for metrics + logs (default 'reference').\n"
        "  --log-format FMT   log output: json (prod, Loki JSON on stdout) or\n"
        "                     text (dev, readable on stderr). Default json.\n"
        "  --log-level LVL    min log level: trace|debug|info|warn|error (info).\n"
        "  --version          Print version and build info, then exit.\n"
        "  --help, -h         Print this help, then exit.\n"
        "\n"
        "M0 (issues #82/#83): worldd runs the world thread + IO worker pool and\n"
        "the IF-2 opcode dispatcher over a TLS 1.3 listener. Grant validation\n"
        "(#84), movement (#86), AoI (#87), and the real tick land on top later.\n"
        "\n"
        "Metrics (OPS-05, #164): worldd exposes a plain-HTTP Prometheus /metrics\n"
        "endpoint on the metrics port (SEPARATE from the game TLS port) — the\n"
        "internal scrape surface for the OTel collector (server/ops). Env:\n"
        "MERIDIAN_METRICS_PORT, MERIDIAN_METRICS_BIND, MERIDIAN_REALM.\n"
        "\n"
        "Logging (OPS-05 #165): structured JSON logs (one Loki-ingestable object\n"
        "per line on stdout) share telemetryd's #167 schema — realm/process/level/\n"
        "event/severity/logger/message/timestamp_ms. Env: MERIDIAN_LOG_FORMAT,\n"
        "MERIDIAN_LOG_LEVEL (flags override).\n",
        kDaemonName, kDaemonName);
}

void print_version() {
    std::printf("%s %s\n%s\n", kDaemonName,
                meridian::core::version_string().c_str(),
                meridian::core::build_info().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    WorlddConfig cfg;
    bool io_workers_set = false;

    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: %s needs an argument\n", kDaemonName, flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--version") == 0) { print_version(); return 0; }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_help(); return 0;
        }
        if (std::strcmp(argv[i], "--cert") == 0) { cfg.cert_path = next("--cert"); continue; }
        if (std::strcmp(argv[i], "--key") == 0) { cfg.key_path = next("--key"); continue; }
        if (std::strcmp(argv[i], "--bind") == 0) { cfg.bind_addr = next("--bind"); continue; }
        if (std::strcmp(argv[i], "--port") == 0) {
            cfg.port = static_cast<std::uint16_t>(std::atoi(next("--port"))); continue;
        }
        if (std::strcmp(argv[i], "--io-workers") == 0) {
            int n = std::atoi(next("--io-workers"));
            if (n > 0) { cfg.world.io_workers = static_cast<unsigned>(n); io_workers_set = true; }
            continue;
        }
        if (std::strcmp(argv[i], "--metrics-port") == 0) {
            cfg.metrics_port = static_cast<std::uint16_t>(std::atoi(next("--metrics-port")));
            continue;
        }
        if (std::strcmp(argv[i], "--metrics-bind") == 0) {
            cfg.metrics_bind = next("--metrics-bind"); continue;
        }
        if (std::strcmp(argv[i], "--realm") == 0) {
            cfg.world.labels.realm = next("--realm"); continue;
        }
        if (std::strcmp(argv[i], "--log-format") == 0) { cfg.log_format = next("--log-format"); continue; }
        if (std::strcmp(argv[i], "--log-level") == 0) { cfg.log_level = next("--log-level"); continue; }
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName, argv[i]);
        return 2;
    }

    // OPS-05 logging (#165): env defaults first, flags override below.
    meridian::core::log::configure_from_env();
    meridian::core::log::set_process(kDaemonName);

    // Default IO pool size from hardware concurrency (leave headroom for the
    // world thread + acceptor). The SAD's "M ≈ cores − 3" sizing is a later
    // concern; at M0 a small pool suffices.
    if (!io_workers_set) {
        unsigned hc = std::thread::hardware_concurrency();
        cfg.world.io_workers = (hc > 3) ? (hc - 3) : 1;
    }
    if (const char* p = env("MERIDIAN_WORLDD_PORT")) {
        cfg.port = static_cast<std::uint16_t>(std::atoi(p));
    }

    // OPS-05 metrics endpoint + realm label env fallbacks (flags override).
    if (const char* p = env("MERIDIAN_METRICS_PORT")) {
        cfg.metrics_port = static_cast<std::uint16_t>(std::atoi(p));
    }
    if (const char* b = env("MERIDIAN_METRICS_BIND")) cfg.metrics_bind = b;
    if (const char* r = env("MERIDIAN_REALM")) cfg.world.labels.realm = r;

    // Realm label -> the log `realm` field (unifies metric + log grouping);
    // then the log-format/level flags override any env/default.
    meridian::core::log::set_realm(cfg.world.labels.realm);
    if (!cfg.log_format.empty()) {
        meridian::core::log::set_format(
            meridian::core::log::format_from_string(cfg.log_format));
    }
    if (!cfg.log_level.empty()) {
        meridian::core::log::set_level(
            meridian::core::log::level_from_string(cfg.log_level));
    }

    // Auth DB connection for IF-3 grant validation (#84). worldd consumes
    // session_grant rows to admit players (worldd is client-facing until the M2
    // gateway split — SAD §2.2/§5.3). Read the same MERIDIAN_DB_* vars authd/db
    // use. When unset (no user), grant validation is disabled — the daemon still
    // serves (dispatcher, clock-sync) but a WorldHello is rejected GRANT_INVALID.
    // The characters DB (MERIDIAN_CHARDB_*) is optional: absent -> enter-world
    // uses the D-11 placeholder stub.
    if (const char* s = env("MERIDIAN_DB_SOCKET")) cfg.world.auth_db.unix_socket = s;
    if (const char* h = env("MERIDIAN_DB_HOST")) cfg.world.auth_db.host = h;
    if (const char* p = env("MERIDIAN_DB_PORT")) cfg.world.auth_db.port = static_cast<unsigned>(std::atoi(p));
    if (const char* u = env("MERIDIAN_DB_USER")) cfg.world.auth_db.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) cfg.world.auth_db.password = pw;
    if (const char* n = env("MERIDIAN_DB_NAME")) cfg.world.auth_db.database = n;

    if (const char* s = env("MERIDIAN_CHARDB_SOCKET")) cfg.world.char_db.unix_socket = s;
    if (const char* h = env("MERIDIAN_CHARDB_HOST")) cfg.world.char_db.host = h;
    if (const char* p = env("MERIDIAN_CHARDB_PORT")) cfg.world.char_db.port = static_cast<unsigned>(std::atoi(p));
    if (const char* u = env("MERIDIAN_CHARDB_USER")) cfg.world.char_db.user = u;
    if (const char* pw = env("MERIDIAN_CHARDB_PASS")) cfg.world.char_db.password = pw;
    if (const char* n = env("MERIDIAN_CHARDB_NAME")) cfg.world.char_db.database = n;

    // World content DB (IF-4 boot; #89). Its own MERIDIAN_WORLDDB_* env so it can
    // point at a different DB/host than the auth DB (the world DB is the mcc
    // artifact; the auth DB is operational state — SAD §2.2 3-DB split).
    if (const char* s = env("MERIDIAN_WORLDDB_SOCKET")) cfg.world_db.unix_socket = s;
    if (const char* h = env("MERIDIAN_WORLDDB_HOST")) cfg.world_db.host = h;
    if (const char* p = env("MERIDIAN_WORLDDB_PORT")) cfg.world_db.port = static_cast<unsigned>(std::atoi(p));
    if (const char* u = env("MERIDIAN_WORLDDB_USER")) cfg.world_db.user = u;
    if (const char* pw = env("MERIDIAN_WORLDDB_PASS")) cfg.world_db.password = pw;
    if (const char* n = env("MERIDIAN_WORLDDB_NAME")) cfg.world_db.database = n;
    if (const char* eh = env("MERIDIAN_WORLDDB_EXPECTED_HASH")) cfg.expected_content_hash = eh;

    if (const char* r = env("MERIDIAN_WORLDD_REALM_ID")) {
        cfg.world.realm_id = static_cast<std::uint32_t>(std::atoi(r));
    }

    if (cfg.cert_path.empty() || cfg.key_path.empty()) {
        std::fprintf(stderr,
                     "%s: --cert and --key are required to serve (try --help)\n",
                     kDaemonName);
        return 2;
    }

    // --- World-DB boot: manifest check + content hash (IF-4; #89) --------------
    // BEFORE accepting connections. Connect to the world content DB, read the
    // manifest mcc recorded, verify it (schema this binary serves + a well-formed
    // content hash), and log the loaded content version. Policy (SAD §4.3 / §5.4.3):
    //   * missing / malformed manifest OR a schema-version mismatch -> FAIL-FAST:
    //     the daemon refuses to boot (exit 3). Serving a corrupt / un-serveable
    //     world DB is worse than not serving.
    //   * a pinned expected-hash disagreement -> loud WARNING, boots anyway
    //     (advisory content-hash tie at M0–M1; becomes a hard fail on the test
    //     realm from M1).
    // When no world DB is wired (MERIDIAN_WORLDDB_* unset -> empty user), the
    // check is SKIPPED — the daemon serves without content (dispatcher / session
    // path only), which keeps the DB-less smoke run + dispatch test runnable.
    if (!cfg.world_db.user.empty()) {
        std::optional<std::string> expected;
        if (!cfg.expected_content_hash.empty()) expected = cfg.expected_content_hash;
        try {
            meridian::db::Connection world_db(cfg.world_db);
            meridian::worldd::BootReport boot =
                meridian::worldd::boot_world_db(world_db, expected);
            if (boot.hard_fail) {
                std::fprintf(stderr,
                             "%s: world-DB boot refused [%s]: %s\n", kDaemonName,
                             meridian::worldd::boot_verdict_name(boot.verdict),
                             boot.reason.c_str());
                return 3;
            }
        } catch (const meridian::db::DbError& e) {
            // Could not even connect to the world DB. A world DB was explicitly
            // configured, so a connect failure is a hard boot failure, not a skip.
            std::fprintf(stderr,
                         "%s: world-DB boot refused: cannot connect to world DB: %s\n",
                         kDaemonName, e.what());
            return 3;
        }
    } else {
        meridian::core::log::warn(
            kDaemonName,
            "no world DB configured (MERIDIAN_WORLDDB_* unset) — serving without "
            "content; IF-4 manifest check skipped");
    }

    // --- OPS-05 metrics endpoint ------------------------------------------------
    // Start the plain-HTTP /metrics exposer BEFORE serving so the OTel collector
    // can scrape immediately. A bind failure is logged + the daemon continues
    // WITHOUT metrics (graceful degradation, D-29 §9 rule 6). Seed the RSS gauge
    // label so the series exists before any sampler runs.
    std::optional<meridian::metrics::Exposer> exposer;
    if (cfg.metrics_port != 0) {
        meridian::metrics::ExposerConfig ec;
        ec.port = cfg.metrics_port;
        ec.bind_addr = cfg.metrics_bind;
        try {
            exposer.emplace(ec, meridian::metrics::default_registry());
            exposer->start();
            meridian::metrics::catalog::rss_bytes()
                .with({cfg.world.labels.realm, kDaemonName})
                .set(0.0);
            meridian::core::log::info(
                kDaemonName, "metrics /metrics on http://" + cfg.metrics_bind + ":" +
                                 std::to_string(exposer->port()));
        } catch (const std::exception& e) {
            meridian::core::log::warn(
                kDaemonName, std::string("metrics endpoint disabled: ") + e.what());
            exposer.reset();
        }
    }

    meridian::net::ListenConfig lc;
    lc.cert_path = cfg.cert_path;
    lc.key_path = cfg.key_path;
    lc.bind_addr = cfg.bind_addr;
    lc.port = cfg.port;

    // Shared, read-only-after-construction dispatcher + the world-process
    // scaffold. The dispatcher registers the M0 stub handlers in its ctor.
    meridian::worldd::Dispatcher dispatcher;
    meridian::worldd::WorldServer world(dispatcher, cfg.world);

    try {
        meridian::net::TlsListener listener(lc);
        world.start();  // spin up the world/update thread

        meridian::core::log::info(
            kDaemonName,
            "worldd up — TLS 1.3 IF-2 listener on " + cfg.bind_addr + ":" +
                std::to_string(listener.local_port()));
        std::printf("%s %s (%s) — IF-2 dispatcher listening on %s:%u\n", kDaemonName,
                    meridian::core::version_string().c_str(), meridian::core::kMilestone,
                    cfg.bind_addr.c_str(), listener.local_port());

        // --- Map/IO worker pool + acceptor -----------------------------------
        // A hand-rolled fixed pool: the acceptor pushes each accepted Session to
        // a shared work deque; N IO workers pop and serve one connection each to
        // completion. Sessions are move-only, so the deque holds them by value.
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<meridian::net::Session> pending;
        std::atomic<bool> shutting_down{false};

        std::vector<std::thread> pool;
        pool.reserve(cfg.world.io_workers);
        for (unsigned w = 0; w < cfg.world.io_workers; ++w) {
            pool.emplace_back([&] {
                for (;;) {
                    std::optional<meridian::net::Session> job;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv.wait(lk, [&] { return shutting_down.load() || !pending.empty(); });
                        if (pending.empty()) return;  // shutting down, drained
                        job.emplace(std::move(pending.front()));
                        pending.pop_front();
                    }
                    world.serve_connection(std::move(*job));
                }
            });
        }

        // Accept loop: one blocking accept per iteration, enqueue each Session
        // for a worker. A single failed handshake must not kill the daemon.
        for (;;) {
            try {
                meridian::net::Session sess = listener.accept();
                // OPS-05 net signal: connection accepted (post TLS 1.3 handshake).
                meridian::metrics::catalog::connections_accepted_total()
                    .with({cfg.world.labels.realm, kDaemonName})
                    .inc();
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    pending.push_back(std::move(sess));
                }
                cv.notify_one();
            } catch (const meridian::net::TlsError& e) {
                meridian::core::log::warn(kDaemonName,
                                          std::string("accept/handshake failed: ") + e.what());
            }
        }

        // Unreached at M0 (the accept loop runs until process exit). The pool
        // join + world.stop() are wired for when a shutdown signal path lands.
        shutting_down.store(true);
        cv.notify_all();
        for (std::thread& t : pool) t.join();
        world.stop();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: fatal: %s\n", kDaemonName, e.what());
        world.stop();
        return 1;
    }
}
