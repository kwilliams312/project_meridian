// SPDX-License-Identifier: Apache-2.0
//
// telemetryd — client telemetry INGEST daemon (entry point) (OPS-05 / D-29; #167).
//
// What this IS (docs/telemetry-architecture.md §2 "Client channel"; privacy §2a):
// the minimal STANDALONE ingest service that RECEIVES the D-29 client-triple —
// the ERROR/CRITICAL log batches the #168 client shipper POSTs (plus crash +
// missing-content events when the client emits them). It validates each batch
// against the privacy contract, rate-limits per build/IP (the OPS-05/#173 anti-
// exhaustion posture), forwards validated events to the structured-log sink
// (Loki via the M0 stdout-JSON sink), and surfaces client signals as counts on
// the /metrics registry (meridian_client_log_ingest_total).
//
// WHY A STANDALONE SERVICE (not an authd endpoint) — documented M0 decision:
//   • Blast-radius isolation. The ingest is a PUBLIC-FACING, untrusted-input
//     surface (#173 flags it as a potential exhaustion vector). authd guards the
//     login/grant path (game-critical, SAD §2.1); coupling an untrusted ingest
//     into that process widens authd's attack surface for no benefit. A separate
//     process fails independently and can be scaled / firewalled on its own.
//   • Independent operations. Distinct port, distinct rate limits, distinct
//     bind — an operator points client builds at it (or runs none, privacy §6)
//     without touching authd. The catalog already attributes the client-ingest
//     counters to "the ingest endpoint" (docs/telemetry-architecture.md §5.1),
//     so a dedicated daemon matches the documented signal ownership.
//   • Minimal. It composes the pure ingest core (ingest.*) + the HTTP server
//     (ingest_http.*) + meridian::metrics for the /metrics counters — nothing
//     from the TLS/DB/proto game stack. It is the smallest service that closes
//     the client-telemetry loop.
//
// Clean-room; no GPL source consulted (CONTRIBUTING.md). Mirrors authd's flag/env
// config shape (flags override env override defaults; no config file yet).

#include "meridian/core/config.hpp"
#include "meridian/core/config_loader.hpp"
#include "meridian/core/log.hpp"
#include "meridian/core/version.hpp"
#include "meridian/metrics/exposer.h"
#include "meridian/metrics/registry.h"

#include "ingest_http.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace {

constexpr const char* kDaemonName = "telemetryd";

struct TelemetrydConfig {
    // Ingest HTTP endpoint (the client POSTs here).
    std::uint16_t ingest_port = 9469;
    std::string   ingest_bind = "127.0.0.1";
    std::string   ingest_path = "/api/1/store/";

    // Rate limit (per build+IP).
    std::uint32_t rl_max = 100;      // requests per window
    std::uint64_t rl_window_ms = 60'000;

    // /metrics scrape endpoint (SEPARATE from the ingest port; matches the other
    // daemons — the OTel collector scrapes it). 0 disables it.
    std::uint16_t metrics_port = 9464;
    std::string   metrics_bind = "127.0.0.1";

    std::string realm_label = "reference";

    // OPS-05 structured logging for telemetryd's OWN daemon logs (#165). The
    // client-ingest sink already emits Loki JSON (forward_event_json); this
    // governs telemetryd's process logs (startup/metrics/errors) so they share
    // the same schema. json (stdout, prod) or text (stderr, dev).
    std::string log_format;
    std::string log_level;
};

void print_help() {
    std::printf(
        "%s — Project Meridian client telemetry ingest daemon (OPS-05 / D-29)\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Receives the #168 client ERROR/CRITICAL log batches (+ crash / missing-\n"
        "content events), validates them against the D-29 privacy contract, rate-\n"
        "limits per build/IP, and forwards validated events as Loki-compatible\n"
        "JSON lines to stdout (the M0 sink; a collector tails it to Loki).\n"
        "\n"
        "Options:\n"
        "  --config PATH       Layered config file (TOML/INI subset). Also\n"
        "                      MERIDIAN_CONFIG. Precedence: defaults < file < env\n"
        "                      < flags. Any key also settable as --<key>=<value>.\n"
        "  --ingest-port N     Ingest HTTP port (default 9469; 0=ephemeral).\n"
        "  --ingest-bind ADDR  Ingest bind address (default 127.0.0.1).\n"
        "  --ingest-path PATH  Sentry-compatible ingest path (default /api/1/store/).\n"
        "  --rate-max N        Max accepted requests per window, per build+IP (100).\n"
        "  --rate-window-ms N  Rate-limit window in ms (default 60000).\n"
        "  --metrics-port N    Prometheus /metrics port (default 9464; 0=off).\n"
        "  --metrics-bind ADDR /metrics bind address (default 127.0.0.1).\n"
        "  --realm NAME        realm label for metrics + log lines (default 'reference').\n"
        "  --log-format FMT    daemon log output: json (Loki JSON on stdout) or\n"
        "                      text (readable on stderr). Default json.\n"
        "  --log-level LVL     min log level: trace|debug|info|warn|error (info).\n"
        "  --version           Print version and build info, then exit.\n"
        "  --help, -h          Print this help, then exit.\n"
        "\n"
        "Env fallbacks: MERIDIAN_INGEST_PORT, MERIDIAN_INGEST_BIND,\n"
        "MERIDIAN_INGEST_PATH, MERIDIAN_METRICS_PORT, MERIDIAN_METRICS_BIND,\n"
        "MERIDIAN_REALM, MERIDIAN_LOG_FORMAT, MERIDIAN_LOG_LEVEL\n"
        "(flags override env override defaults).\n",
        kDaemonName, kDaemonName);
}

void print_version() {
    std::printf("%s %s\n%s\n", kDaemonName, meridian::core::version_string().c_str(),
                meridian::core::build_info().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    TelemetrydConfig cfg;

    // --- Layered config (issue #90) --------------------------------------------
    // defaults < file < env < flags; named flags key the same as MERIDIAN_* env
    // (e.g. --ingest-port and MERIDIAN_INGEST_PORT both -> "ingest.port"), so the
    // documented env vars keep working while flags win. See authd for the model.
    namespace core = meridian::core;
    core::Config c;
    c.set("ingest.port", "9469", core::ConfigLayer::Default);
    c.set("ingest.bind", "127.0.0.1", core::ConfigLayer::Default);
    c.set("ingest.path", "/api/1/store/", core::ConfigLayer::Default);
    c.set("rate.max", "100", core::ConfigLayer::Default);
    c.set("rate.window_ms", "60000", core::ConfigLayer::Default);
    c.set("metrics.port", "9464", core::ConfigLayer::Default);
    c.set("metrics.bind", "127.0.0.1", core::ConfigLayer::Default);
    c.set("realm", "reference", core::ConfigLayer::Default);

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
        if (std::strcmp(argv[i], "--ingest-port") == 0) { c.set("ingest.port", next("--ingest-port"), cl); continue; }
        if (std::strcmp(argv[i], "--ingest-bind") == 0) { c.set("ingest.bind", next("--ingest-bind"), cl); continue; }
        if (std::strcmp(argv[i], "--ingest-path") == 0) { c.set("ingest.path", next("--ingest-path"), cl); continue; }
        if (std::strcmp(argv[i], "--rate-max") == 0) { c.set("rate.max", next("--rate-max"), cl); continue; }
        if (std::strcmp(argv[i], "--rate-window-ms") == 0) { c.set("rate.window_ms", next("--rate-window-ms"), cl); continue; }
        if (std::strcmp(argv[i], "--metrics-port") == 0) { c.set("metrics.port", next("--metrics-port"), cl); continue; }
        if (std::strcmp(argv[i], "--metrics-bind") == 0) { c.set("metrics.bind", next("--metrics-bind"), cl); continue; }
        if (std::strcmp(argv[i], "--realm") == 0) { c.set("realm", next("--realm"), cl); continue; }
        if (std::strcmp(argv[i], "--log-format") == 0) { c.set("log.format", next("--log-format"), cl); continue; }
        if (std::strcmp(argv[i], "--log-level") == 0) { c.set("log.level", next("--log-level"), cl); continue; }
        if (std::strncmp(argv[i], "--", 2) == 0 && std::strchr(argv[i], '=') != nullptr) continue;
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName, argv[i]);
        return 2;
    }
    core::load_args_kv(c, argc, argv);        // CommandLine: --key=value form
    core::load_env_prefixed(c);               // Environment: MERIDIAN_* -> keys
    core::load_config_file(c, c.get_string_or("config", ""));  // File (from --config/MERIDIAN_CONFIG)

    // Resolve effective values.
    cfg.ingest_port = static_cast<std::uint16_t>(c.get_int_or("ingest.port", 9469));
    cfg.ingest_bind = c.get_string_or("ingest.bind", "127.0.0.1");
    cfg.ingest_path = c.get_string_or("ingest.path", "/api/1/store/");
    cfg.rl_max = static_cast<std::uint32_t>(c.get_int_or("rate.max", 100));
    cfg.rl_window_ms = static_cast<std::uint64_t>(c.get_int_or("rate.window_ms", 60'000));
    cfg.metrics_port = static_cast<std::uint16_t>(c.get_int_or("metrics.port", 9464));
    cfg.metrics_bind = c.get_string_or("metrics.bind", "127.0.0.1");
    cfg.realm_label = c.get_string_or("realm", "reference");
    cfg.log_format = c.get_string_or("log.format", "");
    cfg.log_level = c.get_string_or("log.level", "");

    // OPS-05 logging (#165): env baseline first, then layered realm + log flags.
    meridian::core::log::configure_from_env();
    meridian::core::log::set_process(kDaemonName);
    meridian::core::log::set_realm(cfg.realm_label);
    if (!cfg.log_format.empty()) {
        meridian::core::log::set_format(
            meridian::core::log::format_from_string(cfg.log_format));
    }
    if (!cfg.log_level.empty()) {
        meridian::core::log::set_level(
            meridian::core::log::level_from_string(cfg.log_level));
    }

    // --- /metrics endpoint (client-ingest counts; SAD §8.5). ------------------
    // Start it BEFORE the ingest so the very first accepted batch's counter is
    // scrapeable. A bind failure disables metrics but the ingest still serves
    // (graceful degradation — D-29 §9 rule 6).
    std::optional<meridian::metrics::Exposer> exposer;
    if (cfg.metrics_port != 0) {
        meridian::metrics::ExposerConfig ec;
        ec.port = cfg.metrics_port;
        ec.bind_addr = cfg.metrics_bind;
        try {
            exposer.emplace(ec, meridian::metrics::default_registry());
            exposer->start();
            meridian::core::log::info(
                kDaemonName, "metrics /metrics on http://" + cfg.metrics_bind + ":" +
                                 std::to_string(exposer->port()));
        } catch (const std::exception& e) {
            meridian::core::log::warn(kDaemonName,
                                      std::string("metrics endpoint disabled: ") + e.what());
            exposer.reset();
        }
    }

    // --- Ingest HTTP server. --------------------------------------------------
    meridian::telemetryd::IngestServerConfig sc;
    sc.port = cfg.ingest_port;
    sc.bind_addr = cfg.ingest_bind;
    sc.ingest_path = cfg.ingest_path;
    sc.realm_label = cfg.realm_label;
    sc.rate_limit.max_requests = cfg.rl_max;
    sc.rate_limit.window_ms = cfg.rl_window_ms;

    // M0 sink: Loki-compatible JSON lines to stdout (a collector tails it).
    meridian::telemetryd::IngestServer server(sc, std::cout,
                                              &meridian::metrics::default_registry());
    try {
        server.start();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s: fatal: %s\n", kDaemonName, e.what());
        return 1;
    }

    meridian::core::log::info(
        kDaemonName, "telemetryd up — ingest on http://" + cfg.ingest_bind + ":" +
                         std::to_string(server.port()) + cfg.ingest_path);
    std::printf("%s %s (%s) — ingest on %s:%u%s\n", kDaemonName,
                meridian::core::version_string().c_str(), meridian::core::kMilestone,
                cfg.ingest_bind.c_str(), server.port(), cfg.ingest_path.c_str());
    std::fflush(stdout);

    // Serve until signalled. The acceptor runs on its own thread; block here.
    // (M0: no signal handler wiring beyond process termination — SIGTERM/SIGINT
    // tears the process down; stop() runs via the server dtor on unwinding.)
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}
