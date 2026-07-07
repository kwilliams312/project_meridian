// SPDX-License-Identifier: Apache-2.0
//
// telemetryd — the ingest HTTP/1.1 server (OPS-05 / D-29; issue #167).
//
// CLEAN-ROOM: a minimal HTTP/1.1 request responder implemented from RFC 7230
// (message framing: request line, headers, CRLF terminator, Content-Length body)
// + the BSD sockets API only — the SAME single-purpose responder STYLE as
// meridian::metrics::Exposer (server/libs/metrics/src/exposer.cpp), extended to
// read a bounded POST BODY (the exposer only sniffs the verb; the ingest needs
// the body). No HTTP library (civetweb / cpp-httplib) consulted. See
// CONTRIBUTING.md.
//
// WHAT IT IS: a Sentry-compatible ingest endpoint. It binds a TCP socket on a
// CONFIGURABLE ingest port + path, accepts connections on a background acceptor
// thread, reads each POST body (capped at kMaxEnvelopeBytes BEFORE allocation —
// a Content-Length above the cap is refused 413 without reading the body), hands
// the body to parse_and_validate(), rate-limits per (build, IP), forwards
// accepted events to the sink, and answers a status code:
//
//   POST <ingest_path>  valid #168 envelope         → 200 (forwarded)
//   POST <ingest_path>  malformed / bad type / level → 400
//   POST <ingest_path>  PII-shaped field / unknown tag→ 422
//   POST <ingest_path>  body over the size cap        → 413
//   POST <ingest_path>  over the per-build/IP window   → 429
//   any other method / path                            → 404 / 405
//
// WHY PLAIN HTTP AT M0 (documented decision, mirrors the exposer's rationale):
// this is the client-facing ingest surface, but at M0 it runs behind the
// operator's edge/gateway (privacy §2a "one project-hosted endpoint"; TLS
// termination + a public gateway are an M1+ concern — the client transport #168
// POSTs to a configured URL, so pointing it at an https gateway that proxies to
// this plain-HTTP ingest is a deployment edit, no daemon change). The bind
// address is operator-controlled (default 127.0.0.1; 0.0.0.0 opt-in).
//
// CONCURRENCY: one acceptor thread; each request handled inline (ingest is
// low-volume relative to the game path and the rate limiter bounds it). The
// RateLimiter + Registry are thread-safe, so this stays correct if the handler
// is later moved to a worker pool. start()/stop() are idempotent; stop() joins
// the acceptor (the dtor calls it).

#ifndef MERIDIAN_TELEMETRYD_INGEST_HTTP_H
#define MERIDIAN_TELEMETRYD_INGEST_HTTP_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <thread>

#include "ingest.h"

namespace meridian::metrics {
class Registry;
}

namespace meridian::telemetryd {

// Configuration for the ingest HTTP server.
struct IngestServerConfig {
    // Ingest port. Default 9469 — a distinct port from the daemons' /metrics
    // scrape port (9464). 0 => OS-assigned ephemeral (the in-process test uses
    // this so it never collides with a real service or another test).
    std::uint16_t port = 9469;

    // Bind interface. Default loopback (safe default); set "0.0.0.0" to expose on
    // the operator's network / behind the edge gateway.
    std::string bind_addr = "127.0.0.1";

    // The Sentry-compatible ingest path the client POSTs to. Default mirrors the
    // Sentry store endpoint shape. POST to any other path → 404.
    std::string ingest_path = "/api/1/store/";

    // listen() backlog.
    int backlog = 16;

    // Realm label stamped on forwarded log lines + emitted metrics.
    std::string realm_label = "reference";

    // Rate-limit knobs (per build+IP).
    RateLimiter::Config rate_limit;
};

// The ingest HTTP server. Borrows a sink ostream + a metrics Registry (both must
// outlive the server). The Registry is optional (nullptr → no metrics emitted;
// graceful degradation, D-29 §9 rule 6).
class IngestServer {
public:
    IngestServer(const IngestServerConfig& cfg, std::ostream& sink,
                 meridian::metrics::Registry* registry);
    ~IngestServer();

    IngestServer(const IngestServer&) = delete;
    IngestServer& operator=(const IngestServer&) = delete;

    // Bind + listen + spawn the acceptor thread. Throws std::runtime_error on a
    // bind/listen failure so the daemon can log + exit (or continue) rather than
    // serve a half-open socket.
    void start();

    // Stop accepting and join the acceptor thread. Idempotent; called by the dtor.
    void stop();

    // The actual bound port (equals cfg.port unless 0 was requested). 0 until
    // start() has bound — the in-process test reads it.
    std::uint16_t port() const;

private:
    void run();
    void handle_client(int client_fd, const std::string& peer_ip);

    IngestServerConfig cfg_;
    std::ostream& sink_;
    meridian::metrics::Registry* registry_;
    RateLimiter rate_limiter_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace meridian::telemetryd

#endif  // MERIDIAN_TELEMETRYD_INGEST_HTTP_H
