// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — OTLP/JSON serialization + exporter batching UNIT test
// (OPS-05, #166). PURE / in-process: no socket, no live collector. The exporter
// is driven through an INJECTED sink that captures each OTLP/JSON body, so the
// batch-and-POST path is exercised deterministically without a network. Runs in
// the plain `server` CI job's ctest (like the metrics registry test).
//
// A tiny hand-rolled JSON field scanner (no dependency) parses the serialized
// bodies back so the asserts check the ACTUAL wire shape, not just that a string
// was produced.

#include "meridian/trace/exporter.h"
#include "meridian/trace/otlp.h"
#include "meridian/trace/session_flow.h"
#include "meridian/trace/span.h"
#include "meridian/trace/tracer.h"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace meridian::trace;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Extract the value that follows the first occurrence of `"key":` in `json`.
// Handles a "quoted string" (returns it without quotes), a {nested object}, a
// [nested array], or a bare token (number/true/false), balancing braces/brackets.
// Deliberately minimal — enough to pull traceId/spanId/name/kind/etc. back out.
std::string field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    std::size_t p = json.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return "";
    if (json[p] == '"') {
        std::size_t start = ++p;
        std::string out;
        while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size()) {  // keep escapes crude
                out.push_back(json[p + 1]);
                p += 2;
                continue;
            }
            out.push_back(json[p]);
            ++p;
        }
        (void)start;
        return out;
    }
    if (json[p] == '{' || json[p] == '[') {
        const char open = json[p];
        const char close = (open == '{') ? '}' : ']';
        int depth = 0;
        std::size_t start = p;
        for (; p < json.size(); ++p) {
            if (json[p] == open) ++depth;
            else if (json[p] == close) {
                if (--depth == 0) { ++p; break; }
            }
        }
        return json.substr(start, p - start);
    }
    // Bare token.
    std::size_t start = p;
    while (p < json.size() && json[p] != ',' && json[p] != '}' && json[p] != ']') ++p;
    return json.substr(start, p - start);
}

// Count non-overlapping occurrences of `needle` in `hay`.
int count(const std::string& hay, const std::string& needle) {
    int n = 0;
    std::size_t p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) {
        ++n;
        p += needle.size();
    }
    return n;
}

}  // namespace

int main() {
    // === 1. A span serializes to valid OTLP/HTTP JSON (parse it back) =========
    {
        Tracer tracer("authd", "reference");
        Span s = tracer.start_span(flow::kAuthdLogin, SpanKind::kServer);
        s.set(attr(flow::kAttrRealm, std::string("reference")));
        s.set(attr(flow::kAttrOutcome, std::string("granted")));
        s.set(attr(flow::kAttrGrantIssued, true));
        s.set(attr("meridian.account_id", static_cast<std::int64_t>(4242)));
        s.set_status(StatusCode::kOk);
        s.end();

        CHECK(!is_zero(s.trace_id));
        CHECK(!is_zero(s.span_id));
        CHECK(is_zero(s.parent_span_id));      // root
        CHECK(s.end_unix_nano >= s.start_unix_nano);

        const std::string js = span_to_otlp_json(s);

        // traceId / spanId are lower-hex of the right length and round-trip.
        const std::string tid = field(js, "traceId");
        const std::string sid = field(js, "spanId");
        CHECK(tid.size() == 32);
        CHECK(sid.size() == 16);
        CHECK(tid == to_hex(s.trace_id));
        CHECK(sid == to_hex(s.span_id));
        TraceId parsed_tid{};
        CHECK(trace_id_from_hex(tid, parsed_tid));
        CHECK(parsed_tid == s.trace_id);

        // name / kind.
        CHECK(field(js, "name") == std::string(flow::kAuthdLogin));
        CHECK(field(js, "kind") == "2");  // SERVER

        // No parentSpanId key on a root span (omitted).
        CHECK(!has(js, "parentSpanId"));

        // Timestamps are STRINGS (protojson uint64 rule) and equal the span's.
        CHECK(field(js, "startTimeUnixNano") == std::to_string(s.start_unix_nano));
        CHECK(field(js, "endTimeUnixNano") == std::to_string(s.end_unix_nano));

        // Attributes: string / bool / int values present with correct typing.
        CHECK(has(js, "\"stringValue\":\"granted\""));
        CHECK(has(js, "\"boolValue\":true"));
        CHECK(has(js, "\"intValue\":\"4242\""));  // int64 as a STRING

        // Status Ok emitted.
        CHECK(has(js, "\"status\":{\"code\":1"));
    }

    // === 2. A parent/child span relationship is well-formed ===================
    {
        Tracer tracer("authd", "reference");
        Span parent = tracer.start_span("authd.login");
        SpanContext pctx = Tracer::context_of(parent, /*sampled=*/true);
        Span child = tracer.start_child("authd.grant_issue", pctx, SpanKind::kInternal);

        CHECK(child.trace_id == parent.trace_id);          // same trace
        CHECK(child.span_id != parent.span_id);            // distinct span
        CHECK(child.parent_span_id == parent.span_id);     // child points at parent
        CHECK(!is_zero(child.parent_span_id));

        const std::string cj = span_to_otlp_json(child.end());
        CHECK(field(cj, "traceId") == to_hex(parent.trace_id));
        CHECK(field(cj, "parentSpanId") == to_hex(parent.span_id));  // present on a child
        CHECK(field(cj, "kind") == "1");  // INTERNAL
    }

    // === 3. Cross-process grant->trace stitching is deterministic + agrees =====
    {
        const std::uint64_t grant = 0xDEADBEEFCAFEF00DULL;
        // authd derives a context from the grant and stamps its login span with
        // the derived parent span_id (so ITS span is the parent worldd points at).
        SpanContext a = flow::trace_context_from_grant(grant, /*sampled=*/true);
        SpanContext b = flow::trace_context_from_grant(grant, /*sampled=*/true);
        CHECK(a.trace_id == b.trace_id);   // both hops derive the SAME trace
        CHECK(a.span_id == b.span_id);
        CHECK(a.valid());

        // A different grant yields a different trace (no accidental collisions).
        SpanContext c = flow::trace_context_from_grant(grant ^ 0x1, true);
        CHECK(c.trace_id != a.trace_id);

        // worldd starts its enter-world span as a CHILD of the derived context:
        // same trace_id, parent = the authd-derived span_id.
        Tracer worldd("worldd", "reference");
        Span enter = worldd.start_child(flow::kWorlddEnterWorld, a, SpanKind::kServer);
        CHECK(enter.trace_id == a.trace_id);
        CHECK(enter.parent_span_id == a.span_id);
    }

    // === 4. Exporter BATCHES finished spans + POSTs to a MOCK sink =============
    {
        std::mutex m;
        std::vector<std::string> bodies;
        Sink sink = [&](const std::string& body) {
            std::lock_guard<std::mutex> lk(m);
            bodies.push_back(body);
            return true;  // pretend the collector accepted it (2xx)
        };

        ExporterConfig cfg;
        cfg.service_name = "authd";
        cfg.realm = "reference";
        cfg.flush_interval_ms = 20;  // drain quickly for the test
        Exporter exp(cfg, sink);
        CHECK(exp.active());
        exp.start();

        Tracer tracer("authd", "reference");
        const int kSpans = 5;
        for (int i = 0; i < kSpans; ++i) {
            Span s = tracer.start_span("authd.login");
            s.set(attr("i", static_cast<std::int64_t>(i)));
            exp.export_span(std::move(s));  // returns immediately (async)
        }

        exp.flush(2000);
        exp.stop();

        // The mock received at least one batch; the bodies together carry all 5
        // spans as valid OTLP requests.
        std::string all;
        {
            std::lock_guard<std::mutex> lk(m);
            CHECK(!bodies.empty());
            for (const std::string& b : bodies) all += b;
        }
        CHECK(exp.accepted() == static_cast<std::uint64_t>(kSpans));
        CHECK(exp.exported() == static_cast<std::uint64_t>(kSpans));
        CHECK(exp.dropped() == 0);

        // Each body is a well-formed ExportTraceServiceRequest with the resource
        // service.name; across all bodies there are exactly kSpans span objects.
        CHECK(has(all, "\"resourceSpans\""));
        CHECK(has(all, "\"service.name\""));
        CHECK(has(all, "\"stringValue\":\"authd\""));
        CHECK(count(all, "\"name\":\"authd.login\"") == kSpans);
    }

    // === 5. An UNCONFIGURED exporter is a no-op (drops, exports nothing) =======
    {
        ExporterConfig cfg;  // endpoint empty
        Exporter exp(cfg);   // default (endpoint) ctor -> inactive
        CHECK(!exp.active());
        exp.start();  // no-op

        Tracer tracer("worldd", "reference");
        exp.export_span(tracer.start_span("worldd.enter_world"));
        exp.export_span(tracer.start_span("worldd.enter_world"));
        exp.flush(100);
        exp.stop();

        CHECK(exp.accepted() == 2);
        CHECK(exp.exported() == 0);   // nothing left the process
        CHECK(exp.dropped() == 2);    // both dropped (graceful degradation)
    }

    // === 6. Endpoint normalization (base -> .../v1/traces) ====================
    {
        CHECK(traces_url_for("http://otel-collector:4318") ==
              "http://otel-collector:4318/v1/traces");
        CHECK(traces_url_for("http://c:4318/") == "http://c:4318/v1/traces");
        CHECK(traces_url_for("http://c:4318/v1/traces") == "http://c:4318/v1/traces");
        CHECK(traces_url_for("").empty());
    }

    std::printf("meridian-trace-test: OK (%d checks)\n", g_checks);
    return 0;
}
