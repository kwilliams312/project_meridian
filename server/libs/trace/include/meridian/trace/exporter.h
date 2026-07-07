// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — the async, batching OTLP/HTTP span exporter.
//
// CLEAN-ROOM: the HTTP/1.1 CLIENT here is implemented from RFC 7230 (request-line
// + headers + CRLF framing) over the BSD sockets API — the mirror of the metrics
// lib's hand-rolled HTTP/1.1 SERVER (server/libs/metrics/src/exposer.cpp). No HTTP
// or JSON library and no OTel SDK consulted. See CONTRIBUTING.md.
//
// WHAT IT IS: a background thread that owns a bounded queue of finished Spans.
// end_and_export(span) hands a span to the queue and returns IMMEDIATELY — the
// game loop / login path NEVER blocks on a socket (D-29 §9 rule 1 "the server
// routinely sends nothing that stalls UX"; the exporter is fire-and-forget). The
// worker drains the queue on a short cadence, serializes a batch to OTLP/JSON
// (otlp.h), and POSTs it to the collector's /v1/traces endpoint.
//
// GRACEFUL DEGRADATION (D-29 §9 rule 6, task brief): when no endpoint is
// configured the exporter is a NO-OP — end_and_export drops the span, no thread
// is spawned, no socket is opened. A daemon with no collector simply runs without
// traces (exactly as it degrades to /metrics + local logs without a collector).
// A POST failure (collector down / unreachable) is swallowed after a bounded
// number of quiet retries — telemetry loss must never take the daemon down.
//
// TEST SEAM: the transport is injectable. The unit test supplies a capturing sink
// (a std::function) instead of a real socket, asserts the batched OTLP/JSON bodies
// it receives, and needs no live collector — pure/in-process (task brief). The
// production path uses the built-in BSD-socket HTTP POST.

#ifndef MERIDIAN_TRACE_EXPORTER_H
#define MERIDIAN_TRACE_EXPORTER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "meridian/trace/span.h"

namespace meridian::trace {

struct ExporterConfig {
    // OTLP/HTTP endpoint the batches are POSTed to. Two forms accepted:
    //   * a base like "http://otel-collector:4318" — "/v1/traces" is appended, or
    //   * a full traces URL ending in "/v1/traces".
    // EMPTY => the exporter is a NO-OP (graceful degradation — no collector wired).
    std::string endpoint;

    // service.name on the OTLP resource (e.g. "authd"/"worldd").
    std::string service_name = "meridian";
    // deployment realm label (unifies with metric/log realm grouping).
    std::string realm = "reference";

    // Flush cadence: the worker drains + POSTs at least this often even if the
    // batch is small (so a low-rate login trace is not held indefinitely).
    std::uint32_t flush_interval_ms = 1000;
    // Max spans per POST body (a large backlog is split across several POSTs).
    std::size_t max_batch = 512;
    // Queue cap: if producers outrun the exporter, spans past this are DROPPED
    // (telemetry is best-effort — never grow unbounded and OOM the daemon).
    std::size_t max_queue = 8192;
    // Per-POST socket timeout (connect + send + recv), milliseconds.
    std::uint32_t timeout_ms = 2000;
};

// The transport sink: given a full OTLP/JSON request body, deliver it (returns
// true on success). The production default POSTs it over HTTP; the test injects a
// capturing function. Kept as a std::function so the exporter has no compile-time
// dependency on how bytes leave the process.
using Sink = std::function<bool(const std::string& otlp_json_body)>;

class Exporter {
public:
    // Construct with config. When cfg.endpoint is empty the exporter is a no-op
    // (see start()). Does NOT start the worker — call start().
    explicit Exporter(ExporterConfig cfg);

    // Construct with an INJECTED sink (test seam): the worker calls `sink` with
    // each batch's OTLP/JSON body instead of opening a socket. A non-empty sink
    // makes the exporter active regardless of cfg.endpoint.
    Exporter(ExporterConfig cfg, Sink sink);

    ~Exporter();

    Exporter(const Exporter&) = delete;
    Exporter& operator=(const Exporter&) = delete;

    // Whether this exporter will actually export (has an endpoint OR an injected
    // sink). A no-op exporter reports false; callers can skip building spans
    // entirely when tracing is off, though export() is always safe to call.
    bool active() const { return active_; }

    // Spawn the background worker (idempotent). A no-op exporter's start() does
    // nothing. Safe to call before or after export() (queued spans flush once the
    // worker runs).
    void start();

    // Stamp `span` end = now (if not already ended) and enqueue it for export.
    // Returns immediately; NEVER blocks on the network. On a no-op exporter or a
    // full queue the span is dropped. Thread-safe (any producer thread).
    void export_span(Span span);

    // Block until the queue is drained + the current batch POSTed, or `timeout_ms`
    // elapses. For tests + clean shutdown; the hot path never calls this.
    void flush(std::uint32_t timeout_ms = 2000);

    // Stop the worker and join it (draining a final batch). Idempotent; the dtor
    // calls it.
    void stop();

    // Diagnostics (tests): total spans accepted, dropped, and successfully
    // exported (delivered to the sink). Monotonic, thread-safe.
    std::uint64_t accepted() const { return accepted_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }
    std::uint64_t exported() const { return exported_.load(); }

private:
    void run();
    // Serialize + deliver one batch through the sink. Returns delivered count.
    std::size_t deliver_batch(std::vector<Span>& batch);

    ExporterConfig cfg_;
    Sink sink_;
    bool active_ = false;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<Span> queue_;      // producer -> worker
    bool stop_requested_ = false;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> exported_{0};
};

// The built-in production sink: POST `body` to `traces_url` as OTLP/HTTP-JSON over
// a hand-rolled HTTP/1.1 client (BSD sockets). Returns true on a 2xx response.
// `traces_url` must be a resolved "http://host:port/v1/traces". Exposed so a
// daemon (or a live integration smoke test) can reuse it; the class wires it
// automatically when cfg.endpoint is set. Plain HTTP only (the collector is on the
// operator's private network, exactly like the /metrics scrape hop — TLS to the
// collector is a collector-config concern, no daemon change).
bool http_post_otlp(const std::string& traces_url, const std::string& body,
                    std::uint32_t timeout_ms);

// Normalize an endpoint (base or full) to a "http://host:port/v1/traces" URL.
// "http://c:4318" -> "http://c:4318/v1/traces"; an already-/v1/traces URL is
// returned unchanged. Exposed for the unit test.
std::string traces_url_for(const std::string& endpoint);

}  // namespace meridian::trace

#endif  // MERIDIAN_TRACE_EXPORTER_H
