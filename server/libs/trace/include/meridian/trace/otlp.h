// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — OTLP/HTTP-JSON serialization of spans.
// See span.h for the model + the clean-room / OTLP-JSON decision.
//
// This renders a batch of Spans as the OTLP `ExportTraceServiceRequest` JSON
// body the collector's OTLP/HTTP receiver accepts at POST /v1/traces. The wire
// shape (opentelemetry.proto.trace.v1 + protojson rules):
//
//   {"resourceSpans":[{
//      "resource":{"attributes":[{"key":"service.name",
//                                 "value":{"stringValue":"authd"}}, ...]},
//      "scopeSpans":[{
//         "scope":{"name":"meridian.trace","version":"..."},
//         "spans":[{
//            "traceId":"<32 hex>","spanId":"<16 hex>",
//            "parentSpanId":"<16 hex, omitted/empty for root>",
//            "name":"authd.login","kind":2,
//            "startTimeUnixNano":"<uint64 as string>",
//            "endTimeUnixNano":"<uint64 as string>",
//            "attributes":[{"key":"...","value":{"stringValue"|"intValue"|
//                          "boolValue"|"doubleValue":...}}],
//            "status":{"code":1,"message":"..."}
//         }]
//      }]
//   }]}
//
// PROTOJSON DETAILS honoured (why they matter — the collector rejects a body that
// gets them wrong): (a) uint64 nano timestamps are STRINGS, not numbers (JSON
// numbers cannot safely hold 2^63); (b) trace/span ids are lower-hex strings
// (NOT base64 — protojson allows either for `bytes`; hex is the readable, spec-
// listed form the collector accepts); (c) enum values (kind, status code) are
// bare integers; (d) an unset status / all-zero parent is OMITTED rather than
// emitted as a wrong-typed default.

#ifndef MERIDIAN_TRACE_OTLP_H
#define MERIDIAN_TRACE_OTLP_H

#include <string>
#include <vector>

#include "meridian/trace/span.h"

namespace meridian::trace {

// The OTLP instrumentation-scope name/version stamped on every batch.
inline constexpr const char* kScopeName = "meridian.trace";
inline constexpr const char* kScopeVersion = "0.1.0";

// Escape a JSON string value (RFC 8259: backslash, double-quote, control chars).
// Public so the tests + the exporter can reuse it.
std::string json_escape(const std::string& s);

// Render ONE span as an OTLP/JSON `span` object (the innermost element). Exposed
// for the unit test (parse it back + assert traceId/spanId/name/attributes).
std::string span_to_otlp_json(const Span& span);

// Render a batch of spans as a complete OTLP `ExportTraceServiceRequest` JSON
// body — the exact bytes POSTed to /v1/traces. All spans share one resource
// (service.name = `service_name`, deployment realm) and one scope. An empty
// `spans` yields a well-formed request with an empty spans array.
std::string spans_to_otlp_json(const std::vector<Span>& spans,
                               const std::string& service_name,
                               const std::string& realm);

}  // namespace meridian::trace

#endif  // MERIDIAN_TRACE_OTLP_H
