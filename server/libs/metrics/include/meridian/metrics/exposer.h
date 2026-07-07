// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — the /metrics HTTP exposer (OPS-05; SAD §8.5; D-29 §9).
//
// CLEAN-ROOM: a minimal HTTP/1.1 responder implemented from RFC 7230 (message
// framing: request line, headers, CRLF terminator) + the BSD sockets API only.
// No GPL source and no HTTP library (civetweb / cpp-httplib / prometheus-cpp's
// exposer) consulted. See CONTRIBUTING.md.
//
// WHAT IT IS: a single-purpose scrape endpoint. It binds a TCP socket on a
// CONFIGURABLE metrics port (default 9464 — the port server/ops/otel-collector/
// config.yml already points its `meridian-daemons` scrape job at), accepts
// connections on a background thread, and for every request replies with the
// Prometheus text exposition of a Registry (Content-Type text/plain;
// version=0.0.4). It answers GET on ANY path with the metrics body (Prometheus
// scrapes a fixed path; being path-agnostic keeps it trivial and matches the
// collector's `metrics_path: /metrics`).
//
// WHY PLAIN HTTP (documented decision): this is the INTERNAL scrape surface. The
// collector reaches it over the operator's private `meridian` docker network
// (server/ops/docker-compose.observability.yml), never the public internet — the
// game's TLS 1.3 port (IF-1/IF-2) is entirely separate. Per D-29 §9 rule 3 the
// in-process instrumentation stays Prometheus-style; TLS on the scrape hop is the
// collector/operator's concern (mTLS is a collector-config edit, no daemon
// change). So this endpoint speaks plain HTTP on a bind address the operator
// controls (default 127.0.0.1 in the daemons is the safe default; 0.0.0.0 for
// the container network is opt-in via --metrics-bind).
//
// CONCURRENCY: one acceptor thread; each request handled inline (scrapes are
// infrequent and cheap). Rendering takes the Registry's own lock, so scraping is
// safe while the daemon's hot paths mutate metrics. start()/stop() are idempotent
// and stop() joins the thread (the dtor calls it).

#ifndef MERIDIAN_METRICS_EXPOSER_H
#define MERIDIAN_METRICS_EXPOSER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "meridian/metrics/registry.h"

namespace meridian::metrics {

// Configuration for the /metrics HTTP exposer.
struct ExposerConfig {
    // The metrics scrape port. Default 9464 — the port the OTel Collector's
    // `meridian-daemons` job scrapes (server/ops/otel-collector/config.yml).
    // 0 => let the OS assign an ephemeral port (used by the integration test).
    std::uint16_t port = 9464;

    // Bind interface. Default loopback (safe default for a bare daemon); set to
    // "0.0.0.0" to expose on the container `meridian` network for the collector.
    std::string bind_addr = "127.0.0.1";

    // listen() backlog.
    int backlog = 16;
};

// A background HTTP/1.1 server that renders `registry` as Prometheus text on GET.
// Borrows the registry (must outlive the exposer — the daemons pass the process
// default_registry(), which lives for the whole run).
class Exposer {
public:
    Exposer(const ExposerConfig& cfg, Registry& registry);
    ~Exposer();

    Exposer(const Exposer&) = delete;
    Exposer& operator=(const Exposer&) = delete;

    // Bind + listen + spawn the acceptor thread. Throws std::runtime_error on a
    // bind/listen failure (e.g. port in use) so the daemon can log + continue
    // without metrics rather than crash (graceful degradation — D-29 §9 rule 6).
    void start();

    // Stop accepting and join the acceptor thread. Idempotent; called by the dtor.
    void stop();

    // The actual bound port (equals cfg.port unless 0 was requested, in which case
    // the OS-assigned ephemeral port — the integration test reads it). 0 until
    // start() has bound.
    std::uint16_t port() const;

private:
    void run();

    ExposerConfig cfg_;
    Registry& registry_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meridian::metrics

#endif  // MERIDIAN_METRICS_EXPOSER_H
