// SPDX-License-Identifier: Apache-2.0
//
// authd — login / realm-list / session-grant daemon (entry point).
//
// What this IS (server SAD §2.1): a stateless, load-balanceable login service —
// a TLS 1.3 listener enforcing a protocol/client_build floor (§5.1); an original
// SRP6a auth service over a 2048-bit group (auth DB holds {salt, verifier} only,
// constant-time proofs); a realm-list service; and the IF-3 session-grant writer
// (single-use, 30 s grants). authd is "M0: full" in the SAD §7 build plan.
//
// This entry point (issue #79) loads config from flags/env, opens the meridian-
// net TLS 1.3 listener, and runs an accept loop dispatching each accepted
// Session to the IF-1 login state machine (login_session.h). Concurrency model
// (M0, documented decision): THREAD-PER-CONNECTION. authd is low-volume at M0 (a
// login is a short, blocking, DB-bound handshake that then closes — SAD §5.1),
// so a detached worker thread per connection keeps the acceptor responsive
// without pulling in an async runtime (Asio/Boost are explicitly out of scope at
// M0 — meridian-net keeps BSD sockets + OpenSSL only). Each thread owns its
// Session and its own db::Connection, so nothing is shared across threads.
//
// Clean-room: implemented from the SAD, no GPL source consulted (CONTRIBUTING).

#include "meridian/core/config.hpp"
#include "meridian/core/config_loader.hpp"
#include "meridian/core/log.hpp"
#include "meridian/core/version.hpp"
#include "meridian/db/connection.h"
#include "meridian/metrics/catalog.h"
#include "meridian/metrics/exposer.h"
#include "meridian/net/tls_listener.h"
#include "meridian/trace/exporter.h"

#include "login_session.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

namespace {

constexpr const char* kDaemonName = "authd";

// Runtime configuration, resolved from the layered loader (issue #90):
//   in-code defaults < config file < environment < command line   (highest wins)
// The daemon registers its defaults, overlays a --config/MERIDIAN_CONFIG file,
// the MERIDIAN_* environment, and the CLI flags into a meridian::core::Config,
// then reads the effective values into this plain struct for the serve path.
struct AuthdConfig {
    // TLS listener (meridian-net ListenConfig fields).
    std::string cert_path;
    std::string key_path;
    std::string bind_addr = "0.0.0.0";
    std::uint16_t port = 7100;  // IF-1 default (SAD §5.1)

    // Auth DB (meridian-db ConnectParams fields).
    meridian::db::ConnectParams db;

    // Login policy (login_session LoginConfig fields).
    meridian::authd::LoginConfig login;

    // OPS-05 metrics endpoint (server SAD §8.5; docs/telemetry-architecture.md).
    // A plain-HTTP /metrics scrape endpoint on a port SEPARATE from the game TLS
    // port. Default 9464 (the port server/ops/otel-collector scrapes) bound to
    // loopback (safe default; set --metrics-bind 0.0.0.0 for the container net).
    // metrics_port = 0 disables the endpoint entirely (graceful degradation —
    // D-29 §9 rule 6). The `realm` label the series carry (also the login metric
    // realm) groups this daemon's series in Grafana.
    std::uint16_t metrics_port = 9464;
    std::string metrics_bind = "127.0.0.1";
    std::string realm_label = "reference";

    // OPS-05 session-flow traces (#166; docs/telemetry-architecture.md §5.3). The
    // OTLP/HTTP endpoint the "authd.login" spans are POSTed to (the OTel collector,
    // server/ops/otel-collector — OTLP/HTTP on :4318). Empty (default) => tracing
    // is OFF (graceful degradation; login is unaffected). `trace_sample_ratio` is
    // the M0 head-sampling ratio (1.0 = sample all; the session-flow volume is
    // tiny so sample-all loses nothing — D-29 §9 rule 7).
    std::string otlp_endpoint;
    double trace_sample_ratio = 1.0;

    // OPS-05 structured logging (#165). Prod default: JSON (one Loki-ingestable
    // object per line on stdout, matching telemetryd's #167 sink). Dev/run-local
    // sets text for a readable stderr line. Env MERIDIAN_LOG_FORMAT /
    // MERIDIAN_LOG_LEVEL apply first, then these flags override.
    std::string log_format;  // empty = leave env/default (json)
    std::string log_level;   // empty = leave env/default (info)
};

void print_help() {
    std::printf(
        "%s — Project Meridian login / realm-list / session-grant daemon\n"
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
        "  --port N           Listen port (default 7100, IF-1).\n"
        "  --build-floor N    Reject client builds below N (default 0 = none).\n"
        "  --metrics-port N   Prometheus /metrics port (default 9464; 0=off).\n"
        "  --metrics-bind ADDR  /metrics bind address (default 127.0.0.1).\n"
        "  --otlp-endpoint URL  OTLP/HTTP endpoint for session-flow trace spans\n"
        "                     (e.g. http://otel-collector:4318). Empty = tracing off.\n"
        "  --trace-sample-ratio R  head-sample ratio 0..1 for traces (default 1.0).\n"
        "  --realm NAME       realm label for metrics + logs (default 'reference').\n"
        "  --log-format FMT   log output: json (prod, Loki JSON on stdout) or\n"
        "                     text (dev, readable on stderr). Default json.\n"
        "  --log-level LVL    min log level: trace|debug|info|warn|error (info).\n"
        "  --version          Print version and build info, then exit.\n"
        "  --help, -h         Print this help, then exit.\n"
        "\n"
        "DB connection is read from the environment (MERIDIAN_DB_HOST, _PORT,\n"
        "_USER, _PASS, _NAME, or _SOCKET). authd needs a live auth DB to serve.\n"
        "\n"
        "Logging (OPS-05 #165): structured JSON logs (one Loki-ingestable object\n"
        "per line on stdout) share telemetryd's #167 schema — realm/process/level/\n"
        "event/severity/logger/message/timestamp_ms. Env: MERIDIAN_LOG_FORMAT,\n"
        "MERIDIAN_LOG_LEVEL (flags override).\n"
        "\n"
        "Metrics: authd exposes a plain-HTTP Prometheus /metrics endpoint on the\n"
        "metrics port (SEPARATE from the game TLS port) — the internal scrape\n"
        "surface for the OPS-05 OTel collector (server/ops). Env: MERIDIAN_METRICS_\n"
        "PORT, MERIDIAN_METRICS_BIND, MERIDIAN_REALM.\n",
        kDaemonName, kDaemonName);
}

void print_version() {
    std::printf("%s %s\n%s\n", kDaemonName,
                meridian::core::version_string().c_str(),
                meridian::core::build_info().c_str());
}

// Handle one accepted connection: open a fresh DB connection, run the login flow,
// log the outcome, close. Runs on its own detached thread; owns everything it
// touches, so it never races another connection.
void handle_connection(meridian::net::Session sess, meridian::db::ConnectParams db_params,
                       meridian::authd::LoginConfig login) {
    const std::string peer = sess.peer();
    try {
        meridian::db::Connection db(db_params);
        meridian::authd::LoginResult r =
            meridian::authd::run_login(sess, db, login);
        // OPS-05 (#166 + #165): carry the session-flow span's trace/span ids into
        // the log fields so a Loki log line pivots to the matching trace (empty
        // when tracing is off). trace_id/span_id are low-cardinality per-login.
        meridian::core::log::Fields fields{
            meridian::core::log::field("peer", peer),
            meridian::core::log::field("outcome", static_cast<int>(r.outcome)),
            meridian::core::log::field("account_id",
                                       static_cast<std::int64_t>(r.account_id)),
            meridian::core::log::field("grant_issued", r.grant_id != 0),
            meridian::core::log::field("detail", r.detail)};
        if (!r.trace_id.empty()) {
            fields.push_back(meridian::core::log::field("trace_id", r.trace_id));
            fields.push_back(meridian::core::log::field("span_id", r.span_id));
        }
        meridian::core::log::info(kDaemonName, "login processed", fields);
    } catch (const std::exception& e) {
        meridian::core::log::warn(
            kDaemonName, "connection failed",
            {meridian::core::log::field("peer", peer),
             meridian::core::log::field("error", std::string(e.what()))});
    }
    sess.close();
    // OPS-05 net signal: this connection is now closed (SAD §8.5 accept/close).
    meridian::metrics::catalog::connections_closed_total()
        .with({login.realm_label, kDaemonName})
        .inc();
}

}  // namespace

int main(int argc, char** argv) {
    AuthdConfig cfg;

    // --- Layered config (issue #90) --------------------------------------------
    // Build a meridian::core::Config from the four sources. Because Config::set
    // only lets an equal-or-higher layer overwrite, the layers may be loaded in
    // any order (file last) and precedence still resolves as
    //   Default < File < Environment < CommandLine.
    // Named flags below map to the SAME keys the MERIDIAN_* env vars map to (e.g.
    // --metrics-port and MERIDIAN_METRICS_PORT both key "metrics.port"), so the
    // documented env vars keep working while flags take precedence.
    namespace core = meridian::core;
    core::Config c;
    // Default layer: the daemon's in-code defaults (mirror the struct above).
    c.set("bind", "0.0.0.0", core::ConfigLayer::Default);
    c.set("port", "7100", core::ConfigLayer::Default);         // IF-1 (SAD §5.1)
    c.set("login.build_floor", "0", core::ConfigLayer::Default);
    c.set("metrics.port", "9464", core::ConfigLayer::Default);
    c.set("metrics.bind", "127.0.0.1", core::ConfigLayer::Default);
    c.set("realm", "reference", core::ConfigLayer::Default);
    c.set("trace.sample_ratio", "1.0", core::ConfigLayer::Default);

    // CommandLine layer: the named "--flag value" legacy flags. A "--key=value"
    // token is left for load_args_kv (also CommandLine) so any key is overridable.
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
        if (std::strcmp(argv[i], "--build-floor") == 0) { c.set("login.build_floor", next("--build-floor"), cl); continue; }
        if (std::strcmp(argv[i], "--metrics-port") == 0) { c.set("metrics.port", next("--metrics-port"), cl); continue; }
        if (std::strcmp(argv[i], "--metrics-bind") == 0) { c.set("metrics.bind", next("--metrics-bind"), cl); continue; }
        if (std::strcmp(argv[i], "--otlp-endpoint") == 0) { c.set("otlp.endpoint", next("--otlp-endpoint"), cl); continue; }
        if (std::strcmp(argv[i], "--trace-sample-ratio") == 0) { c.set("trace.sample_ratio", next("--trace-sample-ratio"), cl); continue; }
        if (std::strcmp(argv[i], "--realm") == 0) { c.set("realm", next("--realm"), cl); continue; }
        if (std::strcmp(argv[i], "--log-format") == 0) { c.set("log.format", next("--log-format"), cl); continue; }
        if (std::strcmp(argv[i], "--log-level") == 0) { c.set("log.level", next("--log-level"), cl); continue; }
        // Generic "--key=value" override (e.g. --db.host=... , --login.build_floor=..).
        if (std::strncmp(argv[i], "--", 2) == 0 && std::strchr(argv[i], '=') != nullptr) continue;
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName, argv[i]);
        return 2;
    }
    core::load_args_kv(c, argc, argv);            // CommandLine: --key=value form
    core::load_env_prefixed(c);                   // Environment: MERIDIAN_* -> keys
    core::load_config_file(c, c.get_string_or("config", ""));  // File (from --config/MERIDIAN_CONFIG)

    // Resolve effective values into the serve-path struct.
    cfg.cert_path = c.get_string_or("tls.cert", "");
    cfg.key_path = c.get_string_or("tls.key", "");
    cfg.bind_addr = c.get_string_or("bind", "0.0.0.0");
    cfg.port = static_cast<std::uint16_t>(c.get_int_or("port", 7100));
    cfg.login.build_floor = static_cast<std::uint32_t>(c.get_int_or("login.build_floor", 0));
    cfg.metrics_port = static_cast<std::uint16_t>(c.get_int_or("metrics.port", 9464));
    cfg.metrics_bind = c.get_string_or("metrics.bind", "127.0.0.1");
    cfg.otlp_endpoint = c.get_string_or("otlp.endpoint", "");
    cfg.trace_sample_ratio = std::atof(c.get_string_or("trace.sample_ratio", "1.0").c_str());
    cfg.realm_label = c.get_string_or("realm", "reference");
    cfg.log_format = c.get_string_or("log.format", "");
    cfg.log_level = c.get_string_or("log.level", "");

    // Auth DB (MERIDIAN_DB_* -> db.*). Only assign when a key is present so the
    // meridian-db ConnectParams internal defaults (host 127.0.0.1, port 3306) are
    // preserved when unset — same behavior as the previous getenv block.
    if (auto v = c.get_string("db.socket")) cfg.db.unix_socket = *v;
    if (auto v = c.get_string("db.host")) cfg.db.host = *v;
    if (auto v = c.get_int("db.port")) cfg.db.port = static_cast<unsigned>(*v);
    if (auto v = c.get_string("db.user")) cfg.db.user = *v;
    if (auto v = c.get_string("db.pass")) cfg.db.password = *v;
    if (auto v = c.get_string("db.name")) cfg.db.database = *v;

    // OPS-05 logging (#165): env baseline first (MERIDIAN_LOG_FORMAT/_LEVEL/
    // _REALM), then the layered realm + log flags override. set_process stamps
    // every JSON record with this daemon's identity.
    meridian::core::log::configure_from_env();
    meridian::core::log::set_process(kDaemonName);
    cfg.login.realm_label = cfg.realm_label;
    meridian::core::log::set_realm(cfg.realm_label);
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
    if (cfg.db.user.empty()) {
        std::fprintf(stderr,
                     "%s: no auth DB configured — set MERIDIAN_DB_USER etc. "
                     "(try --help)\n",
                     kDaemonName);
        return 2;
    }

    // Fail fast if the DB is unreachable at boot — better than accepting logins
    // we cannot serve.
    try {
        meridian::db::Connection probe(cfg.db);
        (void)probe.ping();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: auth DB unreachable: %s\n", kDaemonName, e.what());
        return 1;
    }

    // --- OPS-05 metrics endpoint ------------------------------------------------
    // Start the plain-HTTP /metrics exposer BEFORE accepting logins so the OTel
    // collector can scrape from the first request. A bind failure (e.g. port in
    // use) is logged and the daemon continues WITHOUT metrics — the endpoint is an
    // OPS-01 extension the daemon degrades gracefully around (D-29 §9 rule 6). Seed
    // the process RSS gauge label so the series exists even before a sampler runs.
    std::optional<meridian::metrics::Exposer> exposer;
    if (cfg.metrics_port != 0) {
        meridian::metrics::ExposerConfig ec;
        ec.port = cfg.metrics_port;
        ec.bind_addr = cfg.metrics_bind;
        try {
            exposer.emplace(ec, meridian::metrics::default_registry());
            exposer->start();
            meridian::metrics::catalog::rss_bytes().with({cfg.realm_label, kDaemonName}).set(0.0);
            meridian::core::log::info(
                kDaemonName, "metrics /metrics on http://" + cfg.metrics_bind + ":" +
                                 std::to_string(exposer->port()));
        } catch (const std::exception& e) {
            meridian::core::log::warn(
                kDaemonName, std::string("metrics endpoint disabled: ") + e.what());
            exposer.reset();
        }
    }

    // --- OPS-05 session-flow trace exporter (#166) -----------------------------
    // Start the async OTLP/HTTP span exporter before serving. When --otlp-endpoint
    // is empty it is a NO-OP (no thread, no socket) — tracing simply off, login
    // unaffected (graceful degradation, D-29 §9 rule 6). When set, each login emits
    // an "authd.login" span (SRP verify → grant issue) whose ids derive from the
    // grant so worldd's enter-world span stitches into the same trace.
    meridian::trace::ExporterConfig tec;
    tec.endpoint = cfg.otlp_endpoint;
    tec.service_name = kDaemonName;
    tec.realm = cfg.realm_label;
    meridian::trace::Exporter trace_exporter(tec);
    trace_exporter.start();
    if (trace_exporter.active()) {
        cfg.login.tracer_exporter = &trace_exporter;
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

    try {
        meridian::net::TlsListener listener(lc);
        meridian::core::log::info(
            kDaemonName,
            "authd up — TLS 1.3 IF-1 listener on " + cfg.bind_addr + ":" +
                std::to_string(listener.local_port()));
        std::printf("%s %s (%s) — listening on %s:%u\n", kDaemonName,
                    meridian::core::version_string().c_str(), meridian::core::kMilestone,
                    cfg.bind_addr.c_str(), listener.local_port());

        // Accept loop: one blocking accept per iteration, hand each Session to a
        // detached worker thread (SAD §2.1 "acceptor hands each Session to a
        // worker"). A single failed handshake must not kill the daemon.
        for (;;) {
            try {
                meridian::net::Session sess = listener.accept();
                // OPS-05 net signal: a connection was accepted (post-handshake).
                meridian::metrics::catalog::connections_accepted_total()
                    .with({cfg.realm_label, kDaemonName})
                    .inc();
                std::thread(handle_connection, std::move(sess), cfg.db, cfg.login)
                    .detach();
            } catch (const meridian::net::TlsError& e) {
                meridian::core::log::warn(kDaemonName,
                                          std::string("accept/handshake failed: ") + e.what());
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: fatal: %s\n", kDaemonName, e.what());
        return 1;
    }
}
