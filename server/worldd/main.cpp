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

#include "meridian/core/config.hpp"
#include "meridian/core/config_loader.hpp"
#include "meridian/core/log.hpp"
#include "meridian/core/version.hpp"
#include "meridian/db/connection.h"
#include "meridian/metrics/catalog.h"
#include "meridian/metrics/exposer.h"
#include "meridian/metrics/rss_sampler.h"
#include "meridian/net/tls_listener.h"
#include "meridian/trace/exporter.h"

#include "area_triggers.h"
#include "db_content_store.h"
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

// Runtime configuration, resolved from the layered loader (issue #90):
//   in-code defaults < config file < environment < command line   (highest wins)
// Same shape as authd: the daemon overlays defaults, a --config/MERIDIAN_CONFIG
// file, the MERIDIAN_* environment, and CLI flags into a meridian::core::Config,
// then reads effective values into this struct for the serve path.
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
    // Boot policy for an EMPTY / not-yet-seeded world DB (issue #485). Default
    // false = DEGRADE: a configured-but-empty world DB boots WITHOUT content and
    // self-heals once the seed lands (no k8s CrashLoopBackOff on the seed race).
    // MERIDIAN_WORLDDB_REQUIRE_CONTENT=1 -> strict fail-fast (empty world DB
    // refuses to boot, as before). Integrity faults hard-fail regardless.
    bool world_db_require_content = false;

    // Realm THEME (env MERIDIAN_REALM_THEME; §4 of the chibi-theme design). Selects
    // which world_manifest pack_namespace is the PRIMARY content pack at boot — the
    // one kernel seam that lets a realm serve a different theme pack (e.g. the dev
    // realm sets "chibi"). Default "core" preserves the historical behaviour exactly.
    std::string realm_theme{meridian::worldd::kDefaultRealmTheme};

    // OPS-05 metrics endpoint (server SAD §8.5; docs/telemetry-architecture.md).
    // Plain-HTTP /metrics on a port SEPARATE from the game TLS port; default 9464
    // (the port the OTel collector scrapes) bound to loopback. 0 disables it.
    std::uint16_t metrics_port = 9464;
    std::string metrics_bind = "127.0.0.1";

    // OPS-05 session-flow traces (#166; docs/telemetry-architecture.md §5.3). The
    // OTLP/HTTP endpoint the "worldd.enter_world" spans are POSTed to (the OTel
    // collector, server/ops — OTLP/HTTP on :4318). Empty => tracing OFF (graceful
    // degradation; the handshake is unaffected). Sample-all at M0 (D-29 §9 rule 7).
    std::string otlp_endpoint;
    double trace_sample_ratio = 1.0;

    // OPS-05 structured logging (#165). json (Loki JSON on stdout, prod default)
    // or text (readable stderr, dev). Env MERIDIAN_LOG_FORMAT / _LEVEL apply
    // first, then these flags override. Empty = leave env/default.
    std::string log_format;
    std::string log_level;
};

void print_help() {
    std::printf(
        "%s — Project Meridian shard worker / map simulation daemon\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --config PATH      Layered config file (TOML/INI subset). Also\n"
        "                     MERIDIAN_CONFIG. Precedence: defaults < file < env\n"
        "                     < flags. Any key also settable as --<key>=<value>.\n"
        "  --cert PATH        TLS server certificate (PEM). Required to serve.\n"
        "  --key PATH         TLS private key (PEM). Required to serve.\n"
        "  --bind ADDR        Bind address (default 0.0.0.0).\n"
        "  --port N           Listen port (default 7200, IF-2).\n"
        "  --io-workers N     Size of the map/IO worker pool (default: auto).\n"
        "  --metrics-port N   Prometheus /metrics port (default 9464; 0=off).\n"
        "  --metrics-bind ADDR  /metrics bind address (default 127.0.0.1).\n"
        "  --otlp-endpoint URL  OTLP/HTTP endpoint for session-flow trace spans\n"
        "                     (e.g. http://otel-collector:4318). Empty = tracing off.\n"
        "  --trace-sample-ratio R  head-sample ratio 0..1 for traces (default 1.0).\n"
        "  --realm NAME       realm label for metrics + logs (default 'reference').\n"
        "  --realm-theme NS   content theme: the world_manifest pack_namespace to\n"
        "                     serve as PRIMARY (env MERIDIAN_REALM_THEME; default\n"
        "                     'core'). A realm selects its theme pack here.\n"
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

    // --- Layered config (issue #90) --------------------------------------------
    // Build a meridian::core::Config from defaults < file < env < flags. Named
    // flags map to the SAME keys the MERIDIAN_* env vars map to, so the documented
    // env vars keep working while flags take precedence. See authd for the model.
    namespace core = meridian::core;
    core::Config c;
    c.set("bind", "0.0.0.0", core::ConfigLayer::Default);
    c.set("port", "7200", core::ConfigLayer::Default);         // IF-2 (SAD §5.2)
    c.set("metrics.port", "9464", core::ConfigLayer::Default);
    c.set("metrics.bind", "127.0.0.1", core::ConfigLayer::Default);
    c.set("realm", "reference", core::ConfigLayer::Default);
    c.set("trace.sample_ratio", "1.0", core::ConfigLayer::Default);

    for (int i = 1; i < argc; ++i) {
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s: %s needs an argument\n", kDaemonName, flag);
                std::exit(2);
            }
            return argv[++i];
        };
        const auto cl = core::ConfigLayer::CommandLine;
        if (std::strcmp(argv[i], "--version") == 0) { print_version(); return 0; }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_help(); return 0;
        }
        if (std::strcmp(argv[i], "--config") == 0) { c.set("config", next("--config"), cl); continue; }
        if (std::strcmp(argv[i], "--cert") == 0) { c.set("tls.cert", next("--cert"), cl); continue; }
        if (std::strcmp(argv[i], "--key") == 0) { c.set("tls.key", next("--key"), cl); continue; }
        if (std::strcmp(argv[i], "--bind") == 0) { c.set("bind", next("--bind"), cl); continue; }
        if (std::strcmp(argv[i], "--port") == 0) { c.set("port", next("--port"), cl); continue; }
        if (std::strcmp(argv[i], "--io-workers") == 0) { c.set("io.workers", next("--io-workers"), cl); continue; }
        if (std::strcmp(argv[i], "--metrics-port") == 0) { c.set("metrics.port", next("--metrics-port"), cl); continue; }
        if (std::strcmp(argv[i], "--metrics-bind") == 0) { c.set("metrics.bind", next("--metrics-bind"), cl); continue; }
        if (std::strcmp(argv[i], "--otlp-endpoint") == 0) { c.set("otlp.endpoint", next("--otlp-endpoint"), cl); continue; }
        if (std::strcmp(argv[i], "--trace-sample-ratio") == 0) { c.set("trace.sample_ratio", next("--trace-sample-ratio"), cl); continue; }
        if (std::strcmp(argv[i], "--realm") == 0) { c.set("realm", next("--realm"), cl); continue; }
        if (std::strcmp(argv[i], "--realm-theme") == 0) { c.set("realm.theme", next("--realm-theme"), cl); continue; }
        if (std::strcmp(argv[i], "--log-format") == 0) { c.set("log.format", next("--log-format"), cl); continue; }
        if (std::strcmp(argv[i], "--log-level") == 0) { c.set("log.level", next("--log-level"), cl); continue; }
        if (std::strncmp(argv[i], "--", 2) == 0 && std::strchr(argv[i], '=') != nullptr) continue;
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName, argv[i]);
        return 2;
    }
    core::load_args_kv(c, argc, argv);        // CommandLine: --key=value form
    core::load_env_prefixed(c);               // Environment: MERIDIAN_* -> keys
    // Legacy alias: MERIDIAN_WORLDD_PORT (generic map -> "worldd.port") also feeds
    // the canonical "port" key so the documented env var keeps working.
    if (auto v = c.get_string("worldd.port")) c.set("port", *v, core::ConfigLayer::Environment);
    core::load_config_file(c, c.get_string_or("config", ""));  // File (from --config/MERIDIAN_CONFIG)

    // Resolve effective values.
    cfg.cert_path = c.get_string_or("tls.cert", "");
    cfg.key_path = c.get_string_or("tls.key", "");
    cfg.bind_addr = c.get_string_or("bind", "0.0.0.0");
    cfg.port = static_cast<std::uint16_t>(c.get_int_or("port", 7200));
    cfg.metrics_port = static_cast<std::uint16_t>(c.get_int_or("metrics.port", 9464));
    cfg.metrics_bind = c.get_string_or("metrics.bind", "127.0.0.1");
    cfg.otlp_endpoint = c.get_string_or("otlp.endpoint", "");
    cfg.trace_sample_ratio = std::atof(c.get_string_or("trace.sample_ratio", "1.0").c_str());
    cfg.world.labels.realm = c.get_string_or("realm", "reference");
    cfg.log_format = c.get_string_or("log.format", "");
    cfg.log_level = c.get_string_or("log.level", "");

    // IO pool size (--io-workers / io.workers). When set (>0) it wins; otherwise
    // default from hardware concurrency, leaving headroom for the world thread +
    // acceptor (SAD's "M ≈ cores − 3" sizing is a later concern; M0 small pool).
    if (auto v = c.get_int("io.workers"); v && *v > 0) {
        cfg.world.io_workers = static_cast<unsigned>(*v);
    } else {
        unsigned hc = std::thread::hardware_concurrency();
        cfg.world.io_workers = (hc > 3) ? (hc - 3) : 1;
    }

    // Auth DB for IF-3 grant validation (#84) — MERIDIAN_DB_* -> db.*. Only assign
    // when present so ConnectParams internal defaults survive when unset (grant
    // validation is then disabled; WorldHello -> GRANT_INVALID). Characters DB
    // (MERIDIAN_CHARDB_* -> chardb.*) is optional (absent -> D-11 stub).
    if (auto v = c.get_string("db.socket")) cfg.world.auth_db.unix_socket = *v;
    if (auto v = c.get_string("db.host")) cfg.world.auth_db.host = *v;
    if (auto v = c.get_int("db.port")) cfg.world.auth_db.port = static_cast<unsigned>(*v);
    if (auto v = c.get_string("db.user")) cfg.world.auth_db.user = *v;
    if (auto v = c.get_string("db.pass")) cfg.world.auth_db.password = *v;
    if (auto v = c.get_string("db.name")) cfg.world.auth_db.database = *v;

    if (auto v = c.get_string("chardb.socket")) cfg.world.char_db.unix_socket = *v;
    if (auto v = c.get_string("chardb.host")) cfg.world.char_db.host = *v;
    if (auto v = c.get_int("chardb.port")) cfg.world.char_db.port = static_cast<unsigned>(*v);
    if (auto v = c.get_string("chardb.user")) cfg.world.char_db.user = *v;
    if (auto v = c.get_string("chardb.pass")) cfg.world.char_db.password = *v;
    if (auto v = c.get_string("chardb.name")) cfg.world.char_db.database = *v;

    // World content DB (IF-4 boot; #89) — its own MERIDIAN_WORLDDB_* -> worlddb.*
    // so it can point at a different host than the auth DB (SAD §2.2 3-DB split).
    if (auto v = c.get_string("worlddb.socket")) cfg.world_db.unix_socket = *v;
    if (auto v = c.get_string("worlddb.host")) cfg.world_db.host = *v;
    if (auto v = c.get_int("worlddb.port")) cfg.world_db.port = static_cast<unsigned>(*v);
    if (auto v = c.get_string("worlddb.user")) cfg.world_db.user = *v;
    if (auto v = c.get_string("worlddb.pass")) cfg.world_db.password = *v;
    if (auto v = c.get_string("worlddb.name")) cfg.world_db.database = *v;
    if (auto v = c.get_string("worlddb.expected.hash")) cfg.expected_content_hash = *v;
    // MERIDIAN_WORLDDB_REQUIRE_CONTENT -> "worlddb.require.content" (#485). When
    // truthy, an empty/unseeded world DB hard-fails instead of degrading.
    if (auto v = c.get_bool("worlddb.require.content")) cfg.world_db_require_content = *v;
    // MERIDIAN_REALM_THEME -> "realm.theme" (§4 seam). Selects the primary content
    // pack namespace at boot; empty/unset keeps the "core" default. An empty string
    // is treated as unset (falls back to the default) so a blank env never selects a
    // nonexistent "" namespace.
    if (auto v = c.get_string("realm.theme"); v && !v->empty()) cfg.realm_theme = *v;

    // MERIDIAN_WORLDD_REALM_ID -> "worldd.realm.id".
    if (auto v = c.get_int("worldd.realm.id")) {
        cfg.world.realm_id = static_cast<std::uint32_t>(*v);
    }

    // OPS-05 logging (#165): env baseline first, then the layered realm + log
    // flags override. set_process stamps every record with this daemon's identity.
    meridian::core::log::configure_from_env();
    meridian::core::log::set_process(kDaemonName);
    meridian::core::log::set_realm(cfg.world.labels.realm);
    if (!cfg.log_format.empty()) {
        meridian::core::log::set_format(
            meridian::core::log::format_from_string(cfg.log_format));
    }
    if (!cfg.log_level.empty()) {
        meridian::core::log::set_level(
            meridian::core::log::level_from_string(cfg.log_level));
    }

    if (cfg.cert_path.empty() || cfg.key_path.empty()) {
        std::fprintf(stderr,
                     "%s: --cert and --key are required to serve (try --help)\n",
                     kDaemonName);
        return 2;
    }

    // --- World-DB boot: manifest check + content hash (IF-4; #89, #485) --------
    // BEFORE accepting connections. Connect to the world content DB, read the
    // manifest mcc recorded, verify it (schema this binary serves + a well-formed
    // content hash), and log the loaded content version. Policy (SAD §4.3 / §5.4.3):
    //   * EMPTY / missing manifest (world DB configured but not yet seeded) ->
    //     DEGRADE by default (#485): serve WITHOUT content (leave the stores as the
    //     placeholders, exactly like the WORLDDB-unset path) so a client still
    //     connects and the realm self-heals once the seed lands — rather than
    //     exiting into a k8s CrashLoopBackOff on the seed race. Opt into strict
    //     fail-fast with MERIDIAN_WORLDDB_REQUIRE_CONTENT=1.
    //   * malformed manifest OR a schema-version mismatch -> FAIL-FAST regardless:
    //     the daemon refuses to boot (exit 3). Serving corrupt / un-serveable
    //     content is worse than not serving — an integrity fault, not an empty DB.
    //   * a pinned expected-hash disagreement -> loud WARNING, boots anyway
    //     (advisory content-hash tie at M0–M1; becomes a hard fail on the test
    //     realm from M1).
    // When no world DB is wired (MERIDIAN_WORLDDB_* unset -> empty user), the
    // check is SKIPPED — the daemon serves without content (dispatcher / session
    // path only), which keeps the DB-less smoke run + dispatch test runnable.
    // The DB-backed content stores (#390), loaded from the world DB below once the
    // manifest check passes. Held at main() scope for the PROCESS LIFETIME so the
    // pointers install_content_stores() / world.set_loot_tables() hand to the seams
    // stay valid for every served connection. Empty (all null) when no world DB is
    // wired OR when the boot degraded (#485) — the seams then keep the M1
    // placeholder stores.
    // libmariadb one-time library init on the MAIN thread, before the boot DB
    // connection below and before any IO worker opens its own connection (#510).
    // Each IO worker additionally runs mysql_thread_init via db::ThreadGuard.
    meridian::db::global_init();

    meridian::worldd::WorldContent content;
    if (!cfg.world_db.user.empty()) {
        std::optional<std::string> expected;
        if (!cfg.expected_content_hash.empty()) expected = cfg.expected_content_hash;
        try {
            meridian::db::Connection world_db(cfg.world_db);

            // Boot compat/migration gate (#698): open the CHARACTERS DB (the
            // durable realm store — realm_content_state survives content reloads,
            // unlike the wholesale-replaced world DB) so the gate can compare the
            // loaded pack's compatibility_version against the version this realm
            // last booted with. Optional: when no characters DB is wired
            // (MERIDIAN_CHARDB_* unset -> empty user), the gate is SKIPPED, exactly
            // like the auth-DB-less grant path. Held for the boot check's scope.
            std::optional<meridian::db::Connection> char_db;
            if (!cfg.world.char_db.user.empty()) {
                char_db.emplace(cfg.world.char_db);
            }
            meridian::worldd::BootReport boot = meridian::worldd::boot_world_db(
                world_db, expected, cfg.world_db_require_content,
                char_db ? &*char_db : nullptr, cfg.realm_theme);
            if (boot.hard_fail) {
                std::fprintf(stderr,
                             "%s: world-DB boot refused [%s]: %s\n", kDaemonName,
                             meridian::worldd::boot_verdict_name(boot.verdict),
                             boot.reason.c_str());
                return 3;
            }
            if (boot.degraded) {
                // Empty / unseeded world DB (#485). boot_world_db already logged a
                // loud DEGRADED error; do NOT attempt load_world_content (there is
                // no content and its tables may not exist yet — a query would throw
                // and defeat the degrade). Leave `content` all-null so the seams
                // keep the placeholder stores, exactly like the WORLDDB-unset path.
                // The realm self-heals on the next boot once the seed lands.
                meridian::core::log::warn(
                    kDaemonName,
                    "world DB configured but unseeded — serving WITHOUT content "
                    "(degraded); DB-backed stores not installed until content is "
                    "seeded (set MERIDIAN_WORLDDB_REQUIRE_CONTENT=1 to fail fast)");
            } else {
                // --- World-DB content load (#390) -----------------------------
                // The manifest check passed, so the world DB is serveable — load the
                // authored quest / npc / loot / vendor (+ item template) content and
                // install the DB-backed stores behind the M1 seams. The QUEST/GOSSIP/
                // VENDOR dispatch handlers (#388) are UNCHANGED — only the concrete
                // store swaps. A load fault is a hard boot failure (a half-loaded
                // world is not served), same policy as the connect fault below.
                content = meridian::worldd::load_world_content(world_db);
                meridian::worldd::install_content_stores(content.items.get(),
                                                         content.vendor.get(),
                                                         content.quests.get(),
                                                         content.npcs.get());
                meridian::core::log::info(
                    kDaemonName,
                    "world content loaded (DB-backed stores installed): " +
                        std::to_string(content.quests->ids().size()) + " quests, " +
                        std::to_string(content.npcs->ids().size()) + " npcs, " +
                        std::to_string(content.loot->ids().size()) + " loot tables, " +
                        std::to_string(content.vendor->ids().size()) + " vendors, " +
                        std::to_string(content.items->ids().size()) + " item templates, " +
                        std::to_string(content.abilities->size()) + " abilities");
            }
        } catch (const meridian::db::DbError& e) {
            // Could not even connect to the world DB, or a content query failed. A
            // world DB was explicitly configured, so this is a hard boot failure, not
            // a skip.
            std::fprintf(stderr,
                         "%s: world-DB boot refused: cannot load world DB: %s\n",
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
    // WITHOUT metrics (graceful degradation, D-29 §9 rule 6). A periodic
    // RssSampler (#297) stamps meridian_rss_bytes{realm,process=worldd} so the
    // process-memory / RSS-growth alerts + dashboard panels have live data
    // (previously the gauge only held a startup 0). Owned for the process lifetime
    // alongside the exposer.
    std::optional<meridian::metrics::Exposer> exposer;
    std::optional<meridian::metrics::RssSampler> rss_sampler;
    if (cfg.metrics_port != 0) {
        meridian::metrics::ExposerConfig ec;
        ec.port = cfg.metrics_port;
        ec.bind_addr = cfg.metrics_bind;
        try {
            exposer.emplace(ec, meridian::metrics::default_registry());
            exposer->start();
            rss_sampler.emplace(meridian::metrics::catalog::rss_bytes().with(
                {cfg.world.labels.realm, kDaemonName}));
            rss_sampler->start();
            meridian::core::log::info(
                kDaemonName, "metrics /metrics on http://" + cfg.metrics_bind + ":" +
                                 std::to_string(exposer->port()));
        } catch (const std::exception& e) {
            meridian::core::log::warn(
                kDaemonName, std::string("metrics endpoint disabled: ") + e.what());
            exposer.reset();
            rss_sampler.reset();
        }
    }

    // --- OPS-05 session-flow trace exporter (#166) -----------------------------
    // Start the async OTLP/HTTP span exporter before serving. A no-op when
    // --otlp-endpoint is empty (tracing off, handshake unaffected — graceful
    // degradation, D-29 §9 rule 6). Wired onto WorldServerConfig so every
    // connection's WORLD_HELLO handler can emit the "worldd.enter_world" span
    // (stitched to authd's login span via the grant). Owned here for the process
    // lifetime; serve_connection copies the pointer onto each ConnCtx.
    meridian::trace::ExporterConfig tec;
    tec.endpoint = cfg.otlp_endpoint;
    tec.service_name = kDaemonName;
    tec.realm = cfg.world.labels.realm;
    meridian::trace::Exporter trace_exporter(tec);
    trace_exporter.start();
    if (trace_exporter.active()) {
        cfg.world.tracer = &trace_exporter;
        meridian::core::log::info(
            kDaemonName, "session-flow traces -> OTLP " +
                             meridian::trace::traces_url_for(cfg.otlp_endpoint));
    } else {
        meridian::core::log::info(kDaemonName,
                                  "session-flow traces disabled (no --otlp-endpoint)");
    }
    (void)cfg.trace_sample_ratio;  // M0: sample-all; ratio wired for M1 volume

    meridian::net::ListenConfig lc;
    lc.cert_path = cfg.cert_path;
    lc.key_path = cfg.key_path;
    lc.bind_addr = cfg.bind_addr;
    lc.port = cfg.port;

    // Shared, read-only-after-construction dispatcher + the world-process
    // scaffold. The dispatcher registers the M0 stub handlers in its ctor.
    meridian::worldd::Dispatcher dispatcher;
    meridian::worldd::WorldServer world(dispatcher, cfg.world);

    // Area triggers + POI discovery (#368; WLD-01/03): load the trigger volume set
    // the map tick evaluates against player positions. When a world DB is wired, the
    // AUTHORED `area` (POI) rows are loaded into discovery volumes carrying the real
    // `poi` (#398) — so a crossing credits explore objectives against authored content
    // (on_explore(zone_id, poi)). With no world DB (content.quests null) the M1
    // deterministic PLACEHOLDER set on the flat bootstrap map stands in.
    if (content.quests) {
        const std::size_t n = content.area_triggers.size();
        world.world_state().load_area_triggers(std::move(content.area_triggers));
        meridian::core::log::info(
            kDaemonName,
            "area-trigger POI volumes loaded from world DB: " + std::to_string(n));
    } else {
        world.world_state().load_area_triggers(meridian::worldd::placeholder_area_triggers());
    }

    // Install the DB-backed loot tables on the per-map tick (#390) when world content
    // was loaded, so a creature death rolls AUTHORED loot rather than the placeholder
    // set. No-op when no world DB is wired (content.loot is null) — the tick keeps its
    // placeholder loot tables.
    if (content.loot) world.set_loot_tables(*content.loot);

    // Install the DB-backed ability catalog on the LIVE cast path (#481) when world
    // content was loaded, so a CAST_REQUEST resolves AUTHORED ability ids (e.g.
    // minor_healing=1) instead of answering UNKNOWN_ABILITY against the placeholder
    // store's synthetic ids. No-op when no world DB is wired (content.abilities is
    // null) — the cast path keeps the M1 placeholder store for DB-free dispatch tests.
    if (content.abilities) world.set_abilities(std::move(*content.abilities));

    // Install the pack-loaded playable roster on the CHAR_CREATE validation path
    // (SP2.5 #695), replacing the server's default offline roster so a create
    // validates against pack data (`race`/`class` rows merged with the compiled
    // fallback). No-op when no world DB is wired (content.roster is nullopt) — the
    // create path keeps Roster::offline_full() for DB-free dispatch/tooling.
    if (content.roster) world.set_roster(std::move(*content.roster));

    // Install the pack-loaded per-class equip-gating + role catalog (SP2.7 #697) so
    // the live map's resolver->AI threat seam scales a Tank-role player's threat
    // (threat_multiplier). No-op semantics when no world DB is wired: the catalog is
    // empty, so every class's multiplier is 1.0 and threat is unscaled.
    world.set_class_catalog(std::move(content.classes));

    // Spawn the authored content placements into the live world (NPC-01 spawn seam,
    // #486): read from spawn_point at boot, each becomes a live creature in the map
    // tick AND an AoI-visible entity (ENTITY_ENTER with #430 vitals + name), so the
    // seeded quest-givers/creatures EXIST, are visible, and are interactable (gossip /
    // kill objectives) instead of a player entering to "see 0 other(s)". No-op when no
    // world DB is wired (content.spawns empty) — the DB-less smoke path stays empty.
    if (!content.spawns.empty()) {
        world.install_spawns(content.spawns);
        meridian::core::log::info(
            kDaemonName,
            "content spawns installed into the live world: " +
                std::to_string(content.spawns.size()) + " placement(s)");
    }

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

        // OPS-05 IO-worker saturation signal (#278/#297): pool size is a constant
        // for the run; busy is incremented while a worker serves a connection.
        // busy/pool = utilization — the real signal the worldd dashboard charts
        // (max concurrent CCU per worldd ≈ io_workers). process="worldd".
        auto& io_workers_gauge =
            meridian::metrics::catalog::io_workers().with({cfg.world.labels.realm, kDaemonName});
        auto& io_workers_busy_gauge = meridian::metrics::catalog::io_workers_busy().with(
            {cfg.world.labels.realm, kDaemonName});
        io_workers_gauge.set(static_cast<double>(cfg.world.io_workers));
        io_workers_busy_gauge.set(0.0);

        std::vector<std::thread> pool;
        pool.reserve(cfg.world.io_workers);
        for (unsigned w = 0; w < cfg.world.io_workers; ++w) {
            pool.emplace_back([&] {
                // libmariadb thread-init/-end for this IO worker thread (#510).
                // serve_connection opens this worker's own auth/char DB
                // connection(s); without the per-thread init these concurrent
                // connections race on libmariadb's thread-local state and
                // intermittently return EMPTY result sets. One guard per worker
                // thread brackets every connection it serves for the run.
                meridian::db::ThreadGuard db_thread_guard;
                for (;;) {
                    std::optional<meridian::net::Session> job;
                    {
                        std::unique_lock<std::mutex> lk(mtx);
                        cv.wait(lk, [&] { return shutting_down.load() || !pending.empty(); });
                        if (pending.empty()) return;  // shutting down, drained
                        job.emplace(std::move(pending.front()));
                        pending.pop_front();
                    }
                    // In-flight for the duration of the (blocking) connection serve.
                    io_workers_busy_gauge.inc();
                    world.serve_connection(std::move(*job));
                    io_workers_busy_gauge.dec();
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
        meridian::db::global_end();  // libmariadb teardown after all workers joined (#510)
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: fatal: %s\n", kDaemonName, e.what());
        world.stop();
        return 1;
    }
}
