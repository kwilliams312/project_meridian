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
};

const char* env(const char* k) { return std::getenv(k); }

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
        "  --ingest-port N     Ingest HTTP port (default 9469; 0=ephemeral).\n"
        "  --ingest-bind ADDR  Ingest bind address (default 127.0.0.1).\n"
        "  --ingest-path PATH  Sentry-compatible ingest path (default /api/1/store/).\n"
        "  --rate-max N        Max accepted requests per window, per build+IP (100).\n"
        "  --rate-window-ms N  Rate-limit window in ms (default 60000).\n"
        "  --metrics-port N    Prometheus /metrics port (default 9464; 0=off).\n"
        "  --metrics-bind ADDR /metrics bind address (default 127.0.0.1).\n"
        "  --realm NAME        realm label for metrics + log lines (default 'reference').\n"
        "  --version           Print version and build info, then exit.\n"
        "  --help, -h          Print this help, then exit.\n"
        "\n"
        "Env fallbacks: MERIDIAN_INGEST_PORT, MERIDIAN_INGEST_BIND,\n"
        "MERIDIAN_INGEST_PATH, MERIDIAN_METRICS_PORT, MERIDIAN_METRICS_BIND,\n"
        "MERIDIAN_REALM (flags override env override defaults).\n",
        kDaemonName, kDaemonName);
}

void print_version() {
    std::printf("%s %s\n%s\n", kDaemonName, meridian::core::version_string().c_str(),
                meridian::core::build_info().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    TelemetrydConfig cfg;

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
        if (std::strcmp(argv[i], "--ingest-port") == 0) {
            cfg.ingest_port = static_cast<std::uint16_t>(std::atoi(next("--ingest-port")));
            continue;
        }
        if (std::strcmp(argv[i], "--ingest-bind") == 0) { cfg.ingest_bind = next("--ingest-bind"); continue; }
        if (std::strcmp(argv[i], "--ingest-path") == 0) { cfg.ingest_path = next("--ingest-path"); continue; }
        if (std::strcmp(argv[i], "--rate-max") == 0) {
            cfg.rl_max = static_cast<std::uint32_t>(std::strtoul(next("--rate-max"), nullptr, 10));
            continue;
        }
        if (std::strcmp(argv[i], "--rate-window-ms") == 0) {
            cfg.rl_window_ms = std::strtoull(next("--rate-window-ms"), nullptr, 10);
            continue;
        }
        if (std::strcmp(argv[i], "--metrics-port") == 0) {
            cfg.metrics_port = static_cast<std::uint16_t>(std::atoi(next("--metrics-port")));
            continue;
        }
        if (std::strcmp(argv[i], "--metrics-bind") == 0) { cfg.metrics_bind = next("--metrics-bind"); continue; }
        if (std::strcmp(argv[i], "--realm") == 0) { cfg.realm_label = next("--realm"); continue; }
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName, argv[i]);
        return 2;
    }

    // Env fallbacks (flags already applied override these).
    if (const char* p = env("MERIDIAN_INGEST_PORT")) {
        cfg.ingest_port = static_cast<std::uint16_t>(std::atoi(p));
    }
    if (const char* b = env("MERIDIAN_INGEST_BIND")) cfg.ingest_bind = b;
    if (const char* p = env("MERIDIAN_INGEST_PATH")) cfg.ingest_path = p;
    if (const char* p = env("MERIDIAN_METRICS_PORT")) {
        cfg.metrics_port = static_cast<std::uint16_t>(std::atoi(p));
    }
    if (const char* b = env("MERIDIAN_METRICS_BIND")) cfg.metrics_bind = b;
    if (const char* r = env("MERIDIAN_REALM")) cfg.realm_label = r;

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
