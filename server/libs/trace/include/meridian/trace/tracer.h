// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-trace — the Tracer: id generation, sampling, and span creation.
// See span.h for the model + the clean-room / OTLP-JSON decision.
//
// A Tracer mints trace/span ids and starts spans. It is the process-shared entry
// point the daemons instrument against (like meridian::metrics::default_registry
// for metrics). It is thread-safe: the id source is a per-thread-seeded PRNG and
// the sampler is a plain read of an atomic ratio.
//
// SAMPLING (M0, D-29 §9 rule 7 + task brief): "sample all" is the M0 default —
// the session-flow volume is tiny (one trace per login / enter-world), so head
// sampling at ratio 1.0 loses nothing and keeps the failure-point analysis
// complete. A simple ratio in [0,1] is supported for when volume grows; the
// sampling decision is made ONCE at the root and propagated (a child inherits its
// parent's sampled flag), so a trace is never half-captured.

#ifndef MERIDIAN_TRACE_TRACER_H
#define MERIDIAN_TRACE_TRACER_H

#include <atomic>
#include <cstdint>
#include <string>

#include "meridian/trace/span.h"

namespace meridian::trace {

// A propagated trace context: the ids + the sampling decision that cross an
// operation boundary (e.g. authd → worldd via the grant, D-29 §9 rule 5). A child
// span is started FROM one of these so it shares the trace_id and parents onto the
// carried span_id. `sampled` carries the head-sampling decision so a downstream
// hop honours it without re-rolling.
struct SpanContext {
    TraceId trace_id{};
    SpanId span_id{};
    bool sampled = false;

    bool valid() const { return !is_zero(trace_id) && !is_zero(span_id); }
};

class Tracer {
public:
    // `service_name` is the OTLP resource `service.name` (e.g. "authd"/"worldd"),
    // stamped on every span batch this tracer's spans are exported in. `realm` is
    // the deployment realm label (unifies with the metric/log realm grouping).
    Tracer(std::string service_name, std::string realm);

    // Head-sampling ratio in [0,1]. 1.0 (default) = sample all (M0). Values are
    // clamped to [0,1]. Thread-safe.
    void set_sample_ratio(double ratio);
    double sample_ratio() const;

    const std::string& service_name() const { return service_name_; }
    const std::string& realm() const { return realm_; }

    // Start a ROOT span: mints a fresh trace_id + span_id, makes the sampling
    // decision, and stamps start = now. `name`/`kind` describe the operation.
    Span start_span(const std::string& name, SpanKind kind = SpanKind::kServer);

    // Start a CHILD span under a propagated context: reuses parent.trace_id, mints
    // a fresh span_id, parents onto parent.span_id, and INHERITS parent.sampled
    // (no re-roll — the root's decision governs the whole trace). If `parent` is
    // invalid (e.g. no context propagated) this behaves like start_span (a fresh
    // root) so a missing upstream context degrades to a local root rather than a
    // broken/parentless span.
    Span start_child(const std::string& name, const SpanContext& parent,
                     SpanKind kind = SpanKind::kServer);

    // Build the propagatable context for a span: its trace_id + span_id plus the
    // `sampled` bit the caller decided at the root (a Span carries no flag itself,
    // so propagation passes it explicitly and downstream hops inherit it exactly).
    static SpanContext context_of(const Span& span, bool sampled);

    // Make ONE head-sampling decision from the current ratio (ratio 1.0 => always
    // true; 0.0 => always false; otherwise a single random draw < ratio). Call at
    // the ROOT and propagate the result — never re-roll mid-trace.
    bool should_sample() const;

private:

    std::string service_name_;
    std::string realm_;
    // Sampling ratio scaled to a 0..2^53 threshold read atomically; see tracer.cpp.
    std::atomic<double> sample_ratio_{1.0};
};

// Fill an id with cryptographically-unbiased-enough random bytes. Exposed for the
// tests. Uses a thread-local PRNG seeded from std::random_device (ids need to be
// unguessable-enough + collision-free, not cryptographic — they are correlation
// handles, not secrets).
void fill_random(TraceId& id);
void fill_random(SpanId& id);

}  // namespace meridian::trace

#endif  // MERIDIAN_TRACE_TRACER_H
