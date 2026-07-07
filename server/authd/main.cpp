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

// Runtime configuration, assembled from flags + env. Kept a plain struct (M0
// config shape decision: flags override env override defaults — no config file
// yet; a file loader can wrap this later without changing the daemon body).
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

const char* env(const char* k) { return std::getenv(k); }

void print_help() {
    std::printf(
        "%s — Project Meridian login / realm-list / session-grant daemon\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
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

// Fill db params from MERIDIAN_DB_* (same var names as meridian-db's test).
void load_db_env(meridian::db::ConnectParams& p) {
    if (const char* s = env("MERIDIAN_DB_SOCKET")) p.unix_socket = s;
    if (const char* h = env("MERIDIAN_DB_HOST")) p.host = h;
    if (const char* port = env("MERIDIAN_DB_PORT")) p.port = static_cast<unsigned>(std::atoi(port));
    if (const char* u = env("MERIDIAN_DB_USER")) p.user = u;
    if (const char* pw = env("MERIDIAN_DB_PASS")) p.password = pw;
    if (const char* n = env("MERIDIAN_DB_NAME")) p.database = n;
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
        if (std::strcmp(argv[i], "--build-floor") == 0) {
            cfg.login.build_floor = static_cast<std::uint32_t>(std::atoi(next("--build-floor")));
            continue;
        }
        if (std::strcmp(argv[i], "--metrics-port") == 0) {
            cfg.metrics_port = static_cast<std::uint16_t>(std::atoi(next("--metrics-port")));
            continue;
        }
        if (std::strcmp(argv[i], "--metrics-bind") == 0) {
            cfg.metrics_bind = next("--metrics-bind"); continue;
        }
        if (std::strcmp(argv[i], "--otlp-endpoint") == 0) {
            cfg.otlp_endpoint = next("--otlp-endpoint"); continue;
        }
        if (std::strcmp(argv[i], "--trace-sample-ratio") == 0) {
            cfg.trace_sample_ratio = std::atof(next("--trace-sample-ratio")); continue;
        }
        if (std::strcmp(argv[i], "--realm") == 0) { cfg.realm_label = next("--realm"); continue; }
        if (std::strcmp(argv[i], "--log-format") == 0) { cfg.log_format = next("--log-format"); continue; }
        if (std::strcmp(argv[i], "--log-level") == 0) { cfg.log_level = next("--log-level"); continue; }
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName, argv[i]);
        return 2;
    }

    // OPS-05 logging (#165): env defaults first (MERIDIAN_LOG_FORMAT/_LEVEL/
    // _REALM), then the flags below override. set_process stamps every JSON
    // record with this daemon's identity.
    meridian::core::log::configure_from_env();
    meridian::core::log::set_process(kDaemonName);

    // Env fallbacks for the metrics endpoint + realm label (flags override).
    if (const char* p = env("MERIDIAN_METRICS_PORT")) {
        cfg.metrics_port = static_cast<std::uint16_t>(std::atoi(p));
    }
    if (const char* b = env("MERIDIAN_METRICS_BIND")) cfg.metrics_bind = b;
    if (const char* r = env("MERIDIAN_REALM")) cfg.realm_label = r;
    // OPS-05 trace endpoint env fallback (flag overrides). MERIDIAN_OTLP_ENDPOINT
    // is the same var worldd + the compose stack use.
    if (const char* e = env("MERIDIAN_OTLP_ENDPOINT")) {
        if (cfg.otlp_endpoint.empty()) cfg.otlp_endpoint = e;
    }
    cfg.login.realm_label = cfg.realm_label;

    // Realm label -> the log `realm` field too (unifies metric + log grouping).
    meridian::core::log::set_realm(cfg.realm_label);
    if (!cfg.log_format.empty()) {
        meridian::core::log::set_format(
            meridian::core::log::format_from_string(cfg.log_format));
    }
    if (!cfg.log_level.empty()) {
        meridian::core::log::set_level(
            meridian::core::log::level_from_string(cfg.log_level));
    }

    load_db_env(cfg.db);

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
