// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — a minimal, dependency-free, thread-safe Prometheus
// metric registry (OPS-05; server SAD §8.5; docs/telemetry-architecture.md §5).
//
// CLEAN-ROOM: designed from the Prometheus text exposition format specification
// (the public "Exposition formats" / OpenMetrics grammar) and the Meridian
// telemetry signal catalog (docs/telemetry-architecture.md §5) only. No GPL
// source (prometheus-cpp, CMaNGOS/TrinityCore, or otherwise) was consulted. See
// CONTRIBUTING.md.
//
// WHY HAND-ROLLED (D-29 §9 rule 2, task brief): the in-process instrumentation
// surface is small and fixed by the SAD §8.5 metric set; the export contract is
// the Prometheus TEXT format that server/ops/otel-collector scrapes. A tiny
// registry keeps the server tree dependency-light (no prometheus-cpp / abseil /
// civetweb pulled in) and clean-room. The rendering seam is the ONLY external
// contract (SAD §10 (i): "the Prometheus scrape format is a de-facto external
// contract") — so this file targets the exposition grammar exactly.
//
// THREADING (server daemons are multi-threaded — world thread + IO workers, SAD
// §6): every metric handle is safe to update concurrently from any thread. A
// scalar Counter/Gauge is a single std::atomic; a Histogram is per-bucket atomics
// plus an atomic sum. The registry's own maps (name -> family, label-set ->
// child) are guarded by a mutex taken only on FIRST creation of a given
// label-set; a hot path caches the returned handle and never re-locks.
//
// LABELS: metrics carry label dimensions per the catalog (e.g. opcode, reason,
// realm, zone, shard). A concrete child (one label-value tuple) is looked up /
// created via with(); the returned handle is a stable pointer for the process
// lifetime (families never evict), so a hot path can cache it.
//
// USAGE (see the daemons' instrumentation for real call sites):
//   auto& reg = meridian::metrics::default_registry();
//   auto& fam = reg.counter("meridian_opcode_total",
//                           "Per-opcode message rate",
//                           {"realm", "zone", "shard", "opcode"});
//   fam.with({"reference", "0", "0", "WORLD_HELLO"}).inc();
//   ...
//   std::string text = reg.render();   // Prometheus exposition text

#ifndef MERIDIAN_METRICS_REGISTRY_H
#define MERIDIAN_METRICS_REGISTRY_H

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace meridian::metrics {

// A label value tuple, positionally matched to a family's declared label names.
// (Kept as a small vector<string> — the label-set count per family is tiny.)
using LabelValues = std::vector<std::string>;

// The metric kind, for the `# TYPE` exposition line and render dispatch.
enum class MetricType { kCounter, kGauge, kHistogram };

// ---------------------------------------------------------------------------
// Counter — a monotonically increasing value (resets only on process restart).
// Catalog counters: meridian_opcode_total, _dropped_total, _errors_total,
// meridian_disconnects_total, meridian_movement_*_total,
// meridian_client_log_ingest_total.
// ---------------------------------------------------------------------------
class Counter {
public:
    // Add `v` (default 1). `v` must be >= 0 (a counter never decreases); a
    // negative argument is ignored (defensive — a bug must not corrupt the
    // series). Lock-free, safe from any thread.
    void inc(double v = 1.0) {
        if (v < 0.0) return;
        double cur = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
        }
    }
    double value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0.0};
};

// ---------------------------------------------------------------------------
// Gauge — a value that can go up and down (CCU, sessions, RSS, AoI entities,
// IO-worker pool/busy). Catalog gauges: meridian_ccu, meridian_sessions,
// meridian_aoi_entities, meridian_rss_bytes, meridian_io_workers{,_busy}.
// ---------------------------------------------------------------------------
class Gauge {
public:
    void set(double v) { value_.store(v, std::memory_order_relaxed); }
    void inc(double v = 1.0) {
        double cur = value_.load(std::memory_order_relaxed);
        while (!value_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
        }
    }
    void dec(double v = 1.0) { inc(-v); }
    double value() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> value_{0.0};
};

// ---------------------------------------------------------------------------
// Histogram — cumulative bucketed observations + sum + count. Catalog
// histograms: meridian_db_latency_seconds, meridian_tick_duration_seconds,
// meridian_auth_srp_duration_seconds. Renders the Prometheus histogram triple:
//   <name>_bucket{le="..."}  (cumulative), <name>_sum, <name>_count.
// Buckets are UPPER BOUNDS ("le"); the implicit +Inf bucket == count.
// ---------------------------------------------------------------------------
class Histogram {
public:
    // Construct with the sorted upper-bound bucket boundaries (ascending, without
    // +Inf — the renderer adds +Inf). A copy is taken; boundaries are immutable
    // after construction. If `bounds` is empty a single-bucket histogram results
    // (only +Inf), which still yields a valid _sum/_count.
    explicit Histogram(std::vector<double> bounds);

    // Record one observation. Lock-free: finds the first bucket whose upper bound
    // is >= v (linear scan — bucket counts are small, ~10) and bumps it plus the
    // running sum + count. Safe from any thread.
    void observe(double v);

    // Accessors (test/diagnostic + the renderer).
    const std::vector<double>& bounds() const { return bounds_; }
    std::uint64_t bucket_count(std::size_t i) const {
        return buckets_[i].load(std::memory_order_relaxed);
    }
    std::uint64_t count() const { return count_.load(std::memory_order_relaxed); }
    double sum() const { return sum_.load(std::memory_order_relaxed); }

private:
    std::vector<double> bounds_;                       // ascending upper bounds (no +Inf)
    std::vector<std::atomic<std::uint64_t>> buckets_;  // per-bound counts (non-cumulative)
    std::atomic<std::uint64_t> count_{0};
    std::atomic<double> sum_{0.0};
};

// A shared set of default histogram buckets for latency-in-seconds metrics
// (action RTT < 150 ms budget, DB latency, tick duration). Spans sub-millisecond
// to a few seconds so p95/p99 land on real boundaries in the dashboards.
const std::vector<double>& default_latency_buckets();

// ---------------------------------------------------------------------------
// Metric families. A family is one metric NAME + HELP + TYPE + declared label
// names; it owns a child per distinct label-value tuple. with() returns a stable
// handle for a tuple, creating it on first use (mutex-guarded) and returning the
// cached handle thereafter.
// ---------------------------------------------------------------------------

class CounterFamily {
public:
    CounterFamily(std::string name, std::string help, std::vector<std::string> labels);

    // Handle for a label tuple (positionally matched to the declared labels).
    // `values.size()` MUST equal the declared label count; a mismatch throws
    // std::invalid_argument (a programming error, caught in tests).
    Counter& with(const LabelValues& values);

    // Convenience for a family declared with NO labels.
    Counter& get() { return with({}); }

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const std::vector<std::string>& labels() const { return labels_; }

    // Snapshot for rendering: each child's label values + current value.
    struct Sample {
        LabelValues values;
        double value;
    };
    std::vector<Sample> collect() const;

private:
    std::string name_, help_;
    std::vector<std::string> labels_;
    mutable std::mutex mtx_;
    std::map<LabelValues, std::unique_ptr<Counter>> children_;
};

class GaugeFamily {
public:
    GaugeFamily(std::string name, std::string help, std::vector<std::string> labels);

    Gauge& with(const LabelValues& values);
    Gauge& get() { return with({}); }

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const std::vector<std::string>& labels() const { return labels_; }

    struct Sample {
        LabelValues values;
        double value;
    };
    std::vector<Sample> collect() const;

private:
    std::string name_, help_;
    std::vector<std::string> labels_;
    mutable std::mutex mtx_;
    std::map<LabelValues, std::unique_ptr<Gauge>> children_;
};

class HistogramFamily {
public:
    HistogramFamily(std::string name, std::string help, std::vector<std::string> labels,
                    std::vector<double> bounds);

    Histogram& with(const LabelValues& values);
    Histogram& get() { return with({}); }

    const std::string& name() const { return name_; }
    const std::string& help() const { return help_; }
    const std::vector<std::string>& labels() const { return labels_; }
    const std::vector<double>& bounds() const { return bounds_; }

    // Snapshot: each child's labels + a cumulative bucket view + sum + count.
    struct Sample {
        LabelValues values;
        std::vector<std::pair<double, std::uint64_t>> cumulative;  // (le, cum count)
        std::uint64_t inf_count;                                   // +Inf == total count
        double sum;
    };
    std::vector<Sample> collect() const;

private:
    std::string name_, help_;
    std::vector<std::string> labels_;
    std::vector<double> bounds_;
    mutable std::mutex mtx_;
    std::map<LabelValues, std::unique_ptr<Histogram>> children_;
};

// ---------------------------------------------------------------------------
// Registry — owns all families and renders the Prometheus exposition text.
// A family is created once per name (idempotent: a second counter("name",...)
// with a MATCHING type + labels returns the existing family; a conflicting
// redeclaration throws — a programming error caught in tests). Thread-safe.
// ---------------------------------------------------------------------------
class Registry {
public:
    Registry() = default;
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // Get-or-create a family. `help` is used only on first creation. `labels` is
    // the declared label-name order. Re-fetching with a different type or label
    // set for an existing name throws std::invalid_argument.
    CounterFamily& counter(const std::string& name, const std::string& help,
                           std::vector<std::string> labels = {});
    GaugeFamily& gauge(const std::string& name, const std::string& help,
                       std::vector<std::string> labels = {});
    HistogramFamily& histogram(const std::string& name, const std::string& help,
                               std::vector<std::string> labels = {},
                               std::vector<double> bounds = default_latency_buckets());

    // Render every family to Prometheus text exposition format (one HELP + TYPE
    // header per family, one line per child series, buckets cumulative). The
    // output is a full, parseable exposition document (ends each line in '\n').
    std::string render() const;

private:
    struct FamilyEntry {
        MetricType type;
        std::unique_ptr<CounterFamily> counter;
        std::unique_ptr<GaugeFamily> gauge;
        std::unique_ptr<HistogramFamily> histogram;
    };

    mutable std::mutex mtx_;
    // Insertion-ordered so render() is deterministic (stable dashboards + tests).
    std::vector<std::string> order_;
    std::map<std::string, FamilyEntry> families_;
};

// The process-global registry the daemons instrument against. One per process;
// the /metrics endpoint renders it.
Registry& default_registry();

// ---------------------------------------------------------------------------
// Exposition helpers (public so the endpoint + tests can reuse them).
// ---------------------------------------------------------------------------

// Escape a label VALUE per the exposition grammar: backslash, double-quote, and
// newline are escaped (\\ \" \n). Used for every rendered label value.
std::string escape_label_value(const std::string& v);

// Format a double the way Prometheus expects: integers without a decimal point,
// +Inf / -Inf / NaN spelled out, otherwise a shortest round-trippable form.
std::string format_double(double v);

}  // namespace meridian::metrics

#endif  // MERIDIAN_METRICS_REGISTRY_H
