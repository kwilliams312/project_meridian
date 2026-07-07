// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — a minimal, dependency-free distributed-tracing span model
// + OTLP/HTTP-JSON exporter (OPS-05; D-29 §9 rule 5; docs/telemetry-architecture
// .md §5.3). This is the TRACES leg of the OPS-05 signal triple: metrics (#164)
// on /metrics, logs (#165) as Loki JSON, and session-flow spans (this lib) pushed
// to the OTel collector's OTLP/HTTP receiver.
//
// CLEAN-ROOM: designed from two PUBLIC specifications only —
//   1. the OpenTelemetry protocol (OTLP) trace data model + its JSON/HTTP binding
//      (the `opentelemetry.proto.trace.v1` message shapes, JSON field names, and
//      the `/v1/traces` POST contract as published on opentelemetry.io), and
//   2. the W3C Trace-Context id shapes (16-byte trace_id, 8-byte span_id,
//      lower-hex encoded).
// No OTel C++ SDK, no protobuf, no third-party HTTP/JSON library, and no GPL
// source was consulted. See CONTRIBUTING.md.
//
// WHY OTLP/HTTP-JSON, NOT THE OTEL C++ SDK (documented decision, task brief):
//   The OTel collector (server/ops/otel-collector/config.yml) already exposes an
//   OTLP receiver over HTTP on :4318; OTLP explicitly supports a JSON-over-HTTP
//   encoding (protojson: the same message tree, keys in lowerCamelCase, bytes as
//   hex/base64) as a first-class alternative to protobuf/gRPC. Emitting that JSON
//   by hand costs one small serializer and keeps the server tree dependency-light
//   and clean-room — pulling in the full OTel C++ SDK would drag protobuf, gRPC,
//   abseil, and their transitive build surface into a tree whose only other deps
//   are OpenSSL / MariaDB / FlatBuffers (CONTRIBUTING.md "dependency-light"). The
//   session-flow span set is tiny + fixed (docs/telemetry-architecture.md §5.3),
//   so the export contract we must honour is exactly the OTLP/HTTP-JSON wire shape
//   — mirrored, like the metrics lib mirrors the Prometheus exposition format.
//
// THREADING: a Span is a per-operation value object built + ended on ONE thread
// (the connection's own thread — like authd's per-connection login and worldd's
// per-connection serve loop). The Tracer (id generation) and the Exporter (the
// background POST queue) are the process-shared, thread-safe pieces.

#ifndef MERIDIAN_TRACE_SPAN_H
#define MERIDIAN_TRACE_SPAN_H

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace meridian::trace {

// W3C Trace-Context ids. A trace_id is 16 bytes, a span_id is 8 bytes; both are
// rendered lower-hex on the OTLP/JSON wire (32 / 16 hex chars). An all-zero id is
// "invalid / absent" per the spec (a root span has an all-zero parent_span_id).
using TraceId = std::array<std::uint8_t, 16>;
using SpanId = std::array<std::uint8_t, 8>;

// Lower-hex encode an id (no separators). kInvalidTraceId -> 32 '0's, etc.
std::string to_hex(const TraceId& id);
std::string to_hex(const SpanId& id);

// Parse a lower/upper-hex id back to bytes. Returns false (leaving `out`
// untouched) if `hex` is not exactly the expected length or contains a non-hex
// char. Used to carry a trace context across the authd→worldd grant hop and by
// the round-trip tests.
bool trace_id_from_hex(const std::string& hex, TraceId& out);
bool span_id_from_hex(const std::string& hex, SpanId& out);

// True iff every byte is zero (the "absent" id per W3C Trace-Context).
bool is_zero(const TraceId& id);
bool is_zero(const SpanId& id);

// OTLP span status (opentelemetry.proto.trace.v1.Status.StatusCode). Unset is the
// default; a handler sets Ok on the success path and Error on a failure, with an
// optional human message (rendered as Status.message).
enum class StatusCode : int {
    kUnset = 0,
    kOk = 1,
    kError = 2,
};

// OTLP span kind (opentelemetry.proto.trace.v1.Span.SpanKind). The session-flow
// spans are Server spans (each daemon handles an inbound request): SPAN_KIND_
// SERVER == 2 on the wire. Internal (1) is offered for sub-operations.
enum class SpanKind : int {
    kInternal = 1,
    kServer = 2,
    kClient = 3,
};

// A typed attribute (opentelemetry.proto.common.v1.KeyValue). OTLP AnyValue is a
// union; the session-flow spans need only string / int / bool / double values, so
// we model exactly those four (kept small + clean-room, like the log Field type).
struct Attribute {
    enum class Type { kString, kInt, kBool, kDouble };
    std::string key;
    Type type = Type::kString;
    std::string str_value;   // kString
    std::int64_t int_value = 0;   // kInt
    bool bool_value = false;      // kBool
    double double_value = 0.0;    // kDouble
};

// Attribute constructors — the ergonomic way to attach typed context to a span,
// mirroring meridian::core::log::field(...).
Attribute attr(std::string key, std::string value);
Attribute attr(std::string key, const char* value);
Attribute attr(std::string key, std::int64_t value);
Attribute attr(std::string key, int value);
Attribute attr(std::string key, bool value);
Attribute attr(std::string key, double value);

// A completed (or in-flight) span — a value object holding everything the OTLP
// serializer needs. Times are UNIX nanoseconds (OTLP's startTimeUnixNano /
// endTimeUnixNano). name/kind/attributes/status are the operation's shape.
struct Span {
    TraceId trace_id{};
    SpanId span_id{};
    SpanId parent_span_id{};   // all-zero for a root span
    std::string name;
    SpanKind kind = SpanKind::kServer;
    std::uint64_t start_unix_nano = 0;
    std::uint64_t end_unix_nano = 0;   // 0 until end() is called
    std::vector<Attribute> attributes;
    StatusCode status = StatusCode::kUnset;
    std::string status_message;

    // Attach a typed attribute (chainable-friendly; returns *this by ref).
    Span& set(Attribute a) {
        attributes.push_back(std::move(a));
        return *this;
    }
    // Set the terminal status + optional message.
    Span& set_status(StatusCode code, std::string message = {}) {
        status = code;
        status_message = std::move(message);
        return *this;
    }
    // Stamp end_unix_nano = now (steady-derived UNIX-ns; see span.cpp).
    Span& end();
    bool ended() const { return end_unix_nano != 0; }
};

// Wall-clock UNIX nanoseconds "now" (std::chrono::system_clock). Public so a
// caller can stamp a start time explicitly and the tests can assert ordering.
std::uint64_t now_unix_nano();

}  // namespace meridian::trace

#endif  // MERIDIAN_TRACE_SPAN_H
