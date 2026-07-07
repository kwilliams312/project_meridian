// SPDX-License-Identifier: Apache-2.0
//
// telemetryd — client telemetry INGEST core (OPS-05 / D-29; issue #167).
//
// The SERVER half of the D-29 client telemetry loop: it RECEIVES the ERROR/
// CRITICAL log batches (and, when the client emits them, crash + missing-content
// events) that the #168 client shipper POSTs, validates them against the D-29
// privacy contract (docs/telemetry-privacy.md), rate-limits per build/IP, and
// forwards the validated events to the structured-log sink (Loki, via the M0
// stdout-JSON sink documented below).
//
//   client (#168) → serialize_envelope() → POST → [ingest #167] → Loki/logs
//
// ─────────────────────────────────────────────────────────────────────────────
//   THE ENVELOPE WE PARSE — EXACTLY what #168 serialize_envelope() produces
//   (client/gdextension/meridian/src/telemetry_log_core.cpp). It is a Sentry-
//   compatible, newline-delimited JSON envelope:
//
//     {"sdk":{"name":"meridian.client.telemetry","version":"1"}}\n   <- header
//     {"type":"event","content_type":"application/json"}\n           <- item hdr
//     {"level":"error","message":"...","timestamp":N,"logger":"...",
//      "tags":{"session_id":"...","build":"...","platform":"..."}}\n  <- payload
//     ...one (item-header, payload) pair per captured LogEvent...
//     {"type":"event",...}\n                                          <- optional
//     {"level":"warning","message":"...","rate_limited_dropped":N,
//      "tags":{...}}\n                                     <- rate-limit summary
//
//   Item-payload `level` is a Sentry level string: "fatal" (Critical), "error"
//   (Error), "warning" (the drop-summary synthetic event). "info"/"debug" never
//   appear (the client gate drops sub-ERROR at the door) — the ingest REJECTS
//   any payload whose level is not error/fatal/warning (defense-in-depth: the
//   client shouldn't send them; the server refuses them if it does).
// ─────────────────────────────────────────────────────────────────────────────
//
// ─────────────────────────────────────────────────────────────────────────────
//   THE D-29 PRIVACY CONTRACT this core ENFORCES (docs/telemetry-privacy.md):
//     • Accept ONLY the D-29 client-triple event types: ERROR/CRITICAL log
//       events, crash reports, missing-content events. Reject anything else.
//     • REJECT any payload carrying a PII-shaped field (username / email / ip /
//       account_id / hardware fingerprint …). The client's SessionContext is
//       structurally PII-free (session_id/build/platform only); the server
//       rejects PII if a rogue/forged client sends it anyway.
//     • CAP payload size BEFORE allocation (like meridian-net's kMaxFrameBytes)
//       so the endpoint can never be an exhaustion vector (OPS-05 / #173).
//     • The ingest surfaces client signals only as COUNTS + forwarded log lines
//       (meridian_client_log_ingest_total); it never becomes a reflection or
//       amplification vector.
// ─────────────────────────────────────────────────────────────────────────────
//
// CLEAN-ROOM: the JSON reader is a minimal hand-rolled recursive-descent scanner
// written from RFC 8259, mirroring the DEPENDENCY-LIGHT discipline of
// meridian::metrics and the client's hand-rolled JSON WRITER. No third-party JSON
// library (nlohmann / rapidjson / simdjson) is consulted or linked. See
// CONTRIBUTING.md.
//
// ENGINE/DAEMON-FREE: this header + ingest.cpp are pure logic (parse / validate /
// rate-limit / format-for-sink). No sockets, no daemon state — so the whole
// contract is unit-tested as pure functions in the plain `server` ctest, with no
// DB and no network. The HTTP transport (ingest_http.*) and the daemon (main.cpp)
// compose these pure pieces.

#ifndef MERIDIAN_TELEMETRYD_INGEST_H
#define MERIDIAN_TELEMETRYD_INGEST_H

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace meridian::telemetryd {

// ===========================================================================
// Size cap — checked BEFORE allocation (meridian-net kMaxFrameBytes posture).
// ===========================================================================
// A client batch is bounded: #168 caps a batch at batch_max_events (20 default)
// small events, so a legitimate envelope is well under this. 256 KiB gives ample
// headroom for a full batch of long stack-trace messages while still bounding a
// hostile peer's single-request allocation. The HTTP layer refuses a
// Content-Length above this WITHOUT reading the body (413), and the parser
// refuses a body above it too (belt-and-braces).
inline constexpr std::size_t kMaxEnvelopeBytes = 256u * 1024u;  // 256 KiB

// Bound the number of events accepted from one envelope, independent of byte
// size — a defense against a single huge batch of tiny events. #168's default
// batch_max_events is 20; 512 is generous headroom while still bounded.
inline constexpr std::size_t kMaxEventsPerBatch = 512;

// ===========================================================================
// Event severity as it arrives on the wire (Sentry `level` string).
// ===========================================================================
// The client maps its Severity → Sentry level: Critical→"fatal", Error→"error".
// The synthetic rate-limit drop summary arrives as "warning". These three are
// the ONLY levels the ingest accepts on a log event; anything else is rejected.
enum class IngestSeverity : std::uint8_t {
    kError,    // "error"   (client Severity::Error)
    kFatal,    // "fatal"   (client Severity::Critical)
    kWarning,  // "warning" (the client's rate-limit drop-summary synthetic event)
};

const char* severity_str(IngestSeverity s);

// ===========================================================================
// The event kinds the D-29 client-triple permits (privacy §2a).
// ===========================================================================
// #168 currently ships only log events (the ERROR/CRITICAL channel). Crash and
// missing-content events are named by the contract and the parser classifies
// them by their item-header `type` so the endpoint accepts them the moment the
// client emits them — but ANY other `type` is rejected.
enum class EventKind : std::uint8_t {
    kLog,             // "event" item carrying an ERROR/CRITICAL log payload
    kRateLimitDrop,   // "event" item carrying the client rate-limit drop summary
};

// ===========================================================================
// ParsedEvent — one validated event extracted from an envelope.
// ===========================================================================
// Carries ONLY the no-PII context (privacy §3) + the diagnostic fields. There is
// deliberately no field for any identity attribute — the parser rejects an
// envelope that carries one before a ParsedEvent is ever produced.
struct ParsedEvent {
    EventKind      kind = EventKind::kLog;
    IngestSeverity severity = IngestSeverity::kError;
    std::string    message;                // diagnostic text (no PII by discipline)
    std::string    logger;                 // optional source tag
    std::uint64_t  timestamp_ms = 0;       // client wall-clock at capture
    std::uint32_t  rate_limited_dropped = 0;  // set on a kRateLimitDrop event
};

// ===========================================================================
// SessionContext — the ONLY context on a batch (privacy §2a). NO PII.
// ===========================================================================
// Mirrors the client's SessionContext: session_id / build / platform ONLY. The
// parser extracts these from an event's `tags` object; it rejects any tags key
// outside this allow-list (unknown-field → reject; PII-shaped key → reject).
struct IngestContext {
    std::string session_id;  // ephemeral, opaque (privacy §3)
    std::string build;       // build/version (privacy §2a)
    std::string platform;    // coarse platform tag (privacy §2a)
};

// ===========================================================================
// ParsedBatch — the full result of parsing one envelope.
// ===========================================================================
struct ParsedBatch {
    IngestContext            context;  // taken from the events' tags (all agree)
    std::vector<ParsedEvent> events;
};

// ===========================================================================
// Reject reasons — a closed set so the HTTP layer maps them to status codes and
// the tests assert the EXACT reason (not just "some 4xx").
// ===========================================================================
enum class RejectReason : std::uint8_t {
    kNone = 0,          // accepted
    kTooLarge,          // body over kMaxEnvelopeBytes → 413
    kMalformed,         // not a well-formed Sentry envelope / bad JSON → 400
    kEmpty,             // no items at all → 400
    kTooManyEvents,     // more than kMaxEventsPerBatch items → 400
    kBadSdkHeader,      // missing / wrong envelope sdk header → 400
    kBadEventType,      // item `type` not in the accepted set → 400
    kBadLevel,          // payload `level` not error/fatal/warning → 400
    kPiiField,          // a PII-shaped field present anywhere → 422
    kUnknownTag,        // a tags key outside session_id/build/platform → 422
    kMissingContext,    // no session_id/build/platform on a log event → 400
};

const char* reject_reason_str(RejectReason r);

// Parse + validate outcome. On success, `reason == kNone` and `batch` is set.
// On rejection, `reason` says why and `batch` is empty. The HTTP layer maps the
// reason to a status code; the pure tests assert the reason directly.
struct IngestResult {
    RejectReason              reason = RejectReason::kNone;
    std::optional<ParsedBatch> batch;

    bool accepted() const { return reason == RejectReason::kNone; }
};

// ===========================================================================
// parse_and_validate — the whole pure contract in one call.
// ===========================================================================
// Takes the raw POST body (the #168 envelope bytes) and returns an IngestResult.
// This is the function the HTTP handler calls and the pure tests exercise
// directly. It:
//   1. caps size (kTooLarge before doing per-line work),
//   2. requires the exact #168 sdk envelope header,
//   3. parses each (item-header, payload) pair,
//   4. validates event type + level (D-29 accepted set),
//   5. rejects ANY PII-shaped field / unknown tag (defense-in-depth),
//   6. extracts the no-PII context, and returns the ParsedBatch.
IngestResult parse_and_validate(const std::string& body);

// ===========================================================================
// RateLimiter — per (build, IP) sliding-window limiter (anti-exhaustion).
// ===========================================================================
// The OPS-05/#173 anti-exhaustion posture: a single build or IP cannot flood the
// endpoint. A fixed-window counter keyed on (build, client-ip): at most
// `max_requests` accepted per `window_ms`; the (window+1)th is refused (the HTTP
// layer answers 429). Keyed on BOTH build and IP so one noisy build on a shared
// NAT and one hostile IP spoofing many builds are both bounded. Thread-safe (the
// daemon handles connections on worker threads).
class RateLimiter {
public:
    struct Config {
        std::uint32_t max_requests = 100;  // accepted requests per window per key
        std::uint64_t window_ms = 60'000;  // 1 minute window (fixed)
    };

    explicit RateLimiter(Config cfg) : cfg_(cfg) {}

    // Returns true if a request from (build, ip) at `now_ms` is ALLOWED; false if
    // it exceeds the window cap (caller answers 429). `now_ms` is injected so the
    // tests are deterministic (no wall-clock dependence).
    bool allow(const std::string& build, const std::string& ip, std::uint64_t now_ms);

    // Number of distinct keys currently tracked (test/diagnostic).
    std::size_t tracked_keys() const;

private:
    struct Window {
        std::uint64_t window_start_ms = 0;
        std::uint32_t count = 0;
    };

    Config cfg_;
    mutable std::mutex mtx_;
    std::map<std::string, Window> windows_;  // key = build + '\x1f' + ip
};

// ===========================================================================
// Sink — where validated events are forwarded (the M0 structured-log store).
// ===========================================================================
// M0 SINK (documented decision): a Loki-compatible STRUCTURED-JSON sink. Each
// validated event is written as one JSON object per line (JSON-lines) to an
// injected std::ostream (the daemon uses stdout; the tests use an ostringstream).
// The line shape is the Loki log-body shape from telemetry-architecture.md §5.2:
//   {"realm":"...","process":"telemetryd","level":"error","event":
//    "client_log_ingest","severity":"error","build":"...","platform":"...",
//    "session_id":"...","logger":"...","message":"...","timestamp_ms":N}
// Loki labels (realm/process/level) are low-cardinality; session_id/build/etc.
// live in the body (privacy §5.2 discipline). The reference stack's Promtail/
// otel-collector tails this JSON and pushes to Loki (server/ops/) — so stdout-
// JSON at M0 needs no daemon change to become a real Loki push later.
//
// `realm` is the operator's realm label (matches the metrics `realm` label).
// Returns the JSON line it wrote (also handy for tests).
std::string forward_event_json(const std::string& realm, const IngestContext& ctx,
                               const ParsedEvent& ev);

}  // namespace meridian::telemetryd

#endif  // MERIDIAN_TELEMETRYD_INGEST_H
