// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — registry + Prometheus text exposition implementation.
// See include/meridian/metrics/registry.h for the design + clean-room statement.

#include "meridian/metrics/registry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <stdexcept>

namespace meridian::metrics {

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

Histogram::Histogram(std::vector<double> bounds)
    : bounds_(std::move(bounds)), buckets_(bounds_.size()) {
    // Boundaries must be ascending for the cumulative render + the first-fit
    // observe() scan to be correct. Sort defensively (a caller passing an
    // unsorted list is a bug, but a silently-wrong histogram is worse).
    std::sort(bounds_.begin(), bounds_.end());
    // std::atomic is not copyable/movable, so buckets_ was default-constructed to
    // the right size above; the atomics are zero-initialised by their ctor.
}

void Histogram::observe(double v) {
    // First bucket whose upper bound is >= v. Values above the largest bound fall
    // only into the implicit +Inf bucket (count_), which is always incremented.
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        if (v <= bounds_[i]) {
            buckets_[i].fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    count_.fetch_add(1, std::memory_order_relaxed);
    // Atomic double add via CAS (portable; matches Counter/Gauge).
    double cur = sum_.load(std::memory_order_relaxed);
    while (!sum_.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
    }
}

const std::vector<double>& default_latency_buckets() {
    // Seconds. Sub-ms → a few seconds. Chosen so the SAD budgets (action RTT
    // < 150 ms p95, tick health, DB latency) land on real boundaries: 0.005,
    // 0.01, 0.025, 0.05, 0.1, 0.15 (the RTT budget edge), 0.25, 0.5, 1, 2.5, 5.
    static const std::vector<double> kBuckets = {
        0.005, 0.01, 0.025, 0.05, 0.1, 0.15, 0.25, 0.5, 1.0, 2.5, 5.0};
    return kBuckets;
}

// ---------------------------------------------------------------------------
// Families
// ---------------------------------------------------------------------------

namespace {

void check_arity(const std::string& name, std::size_t declared, std::size_t got) {
    if (declared != got) {
        throw std::invalid_argument(
            "metric '" + name + "': expected " + std::to_string(declared) +
            " label value(s), got " + std::to_string(got));
    }
}

}  // namespace

CounterFamily::CounterFamily(std::string name, std::string help,
                             std::vector<std::string> labels)
    : name_(std::move(name)), help_(std::move(help)), labels_(std::move(labels)) {}

Counter& CounterFamily::with(const LabelValues& values) {
    check_arity(name_, labels_.size(), values.size());
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = children_.find(values);
    if (it == children_.end()) {
        it = children_.emplace(values, std::make_unique<Counter>()).first;
    }
    return *it->second;
}

std::vector<CounterFamily::Sample> CounterFamily::collect() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Sample> out;
    out.reserve(children_.size());
    for (const auto& [vals, ctr] : children_) {
        out.push_back({vals, ctr->value()});
    }
    return out;
}

GaugeFamily::GaugeFamily(std::string name, std::string help,
                         std::vector<std::string> labels)
    : name_(std::move(name)), help_(std::move(help)), labels_(std::move(labels)) {}

Gauge& GaugeFamily::with(const LabelValues& values) {
    check_arity(name_, labels_.size(), values.size());
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = children_.find(values);
    if (it == children_.end()) {
        it = children_.emplace(values, std::make_unique<Gauge>()).first;
    }
    return *it->second;
}

std::vector<GaugeFamily::Sample> GaugeFamily::collect() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Sample> out;
    out.reserve(children_.size());
    for (const auto& [vals, g] : children_) {
        out.push_back({vals, g->value()});
    }
    return out;
}

HistogramFamily::HistogramFamily(std::string name, std::string help,
                                 std::vector<std::string> labels,
                                 std::vector<double> bounds)
    : name_(std::move(name)),
      help_(std::move(help)),
      labels_(std::move(labels)),
      bounds_(std::move(bounds)) {
    std::sort(bounds_.begin(), bounds_.end());
}

Histogram& HistogramFamily::with(const LabelValues& values) {
    check_arity(name_, labels_.size(), values.size());
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = children_.find(values);
    if (it == children_.end()) {
        it = children_.emplace(values, std::make_unique<Histogram>(bounds_)).first;
    }
    return *it->second;
}

std::vector<HistogramFamily::Sample> HistogramFamily::collect() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<Sample> out;
    out.reserve(children_.size());
    for (const auto& [vals, h] : children_) {
        Sample s;
        s.values = vals;
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < h->bounds().size(); ++i) {
            cum += h->bucket_count(i);
            s.cumulative.emplace_back(h->bounds()[i], cum);
        }
        s.inf_count = h->count();
        s.sum = h->sum();
        out.push_back(std::move(s));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

namespace {

void ensure_labels_match(const std::string& name, const std::vector<std::string>& have,
                         const std::vector<std::string>& want) {
    if (have != want) {
        throw std::invalid_argument(
            "metric '" + name + "' re-declared with a different label set");
    }
}

}  // namespace

CounterFamily& Registry::counter(const std::string& name, const std::string& help,
                                 std::vector<std::string> labels) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = families_.find(name);
    if (it != families_.end()) {
        if (it->second.type != MetricType::kCounter) {
            throw std::invalid_argument("metric '" + name + "' already exists as a different type");
        }
        ensure_labels_match(name, it->second.counter->labels(), labels);
        return *it->second.counter;
    }
    FamilyEntry entry;
    entry.type = MetricType::kCounter;
    entry.counter = std::make_unique<CounterFamily>(name, help, std::move(labels));
    CounterFamily& ref = *entry.counter;
    families_.emplace(name, std::move(entry));
    order_.push_back(name);
    return ref;
}

GaugeFamily& Registry::gauge(const std::string& name, const std::string& help,
                             std::vector<std::string> labels) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = families_.find(name);
    if (it != families_.end()) {
        if (it->second.type != MetricType::kGauge) {
            throw std::invalid_argument("metric '" + name + "' already exists as a different type");
        }
        ensure_labels_match(name, it->second.gauge->labels(), labels);
        return *it->second.gauge;
    }
    FamilyEntry entry;
    entry.type = MetricType::kGauge;
    entry.gauge = std::make_unique<GaugeFamily>(name, help, std::move(labels));
    GaugeFamily& ref = *entry.gauge;
    families_.emplace(name, std::move(entry));
    order_.push_back(name);
    return ref;
}

HistogramFamily& Registry::histogram(const std::string& name, const std::string& help,
                                     std::vector<std::string> labels,
                                     std::vector<double> bounds) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = families_.find(name);
    if (it != families_.end()) {
        if (it->second.type != MetricType::kHistogram) {
            throw std::invalid_argument("metric '" + name + "' already exists as a different type");
        }
        ensure_labels_match(name, it->second.histogram->labels(), labels);
        return *it->second.histogram;
    }
    FamilyEntry entry;
    entry.type = MetricType::kHistogram;
    entry.histogram =
        std::make_unique<HistogramFamily>(name, help, std::move(labels), std::move(bounds));
    HistogramFamily& ref = *entry.histogram;
    families_.emplace(name, std::move(entry));
    order_.push_back(name);
    return ref;
}

// ---------------------------------------------------------------------------
// Exposition helpers
// ---------------------------------------------------------------------------

std::string escape_label_value(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string format_double(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    // Integers render without a decimal point (Prometheus convention: counts).
    if (v == std::floor(v) && std::abs(v) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
        return std::string(buf);
    }
    // Otherwise a shortest-ish round-trippable form: %.17g is exact but noisy;
    // %g with generous precision reads cleanly and reparses within tolerance.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.12g", v);
    return std::string(buf);
}

namespace {

// Render one series line: name{label="value",...} value\n. `extra` appends a
// synthetic label (the histogram "le") already formatted "key=\"val\"".
void write_series(std::ostringstream& os, const std::string& name,
                  const std::vector<std::string>& label_names,
                  const LabelValues& label_values, const std::string& extra,
                  const std::string& value) {
    os << name;
    const bool have_labels = !label_names.empty() || !extra.empty();
    if (have_labels) {
        os << '{';
        bool first = true;
        for (std::size_t i = 0; i < label_names.size(); ++i) {
            if (!first) os << ',';
            first = false;
            os << label_names[i] << "=\"" << escape_label_value(label_values[i]) << '"';
        }
        if (!extra.empty()) {
            if (!first) os << ',';
            os << extra;
        }
        os << '}';
    }
    os << ' ' << value << '\n';
}

}  // namespace

std::string Registry::render() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::ostringstream os;

    for (const std::string& name : order_) {
        auto it = families_.find(name);
        if (it == families_.end()) continue;  // defensive
        const FamilyEntry& fe = it->second;

        switch (fe.type) {
            case MetricType::kCounter: {
                const CounterFamily& f = *fe.counter;
                os << "# HELP " << name << ' ' << f.help() << '\n';
                os << "# TYPE " << name << " counter\n";
                for (const auto& s : f.collect()) {
                    write_series(os, name, f.labels(), s.values, "", format_double(s.value));
                }
                break;
            }
            case MetricType::kGauge: {
                const GaugeFamily& f = *fe.gauge;
                os << "# HELP " << name << ' ' << f.help() << '\n';
                os << "# TYPE " << name << " gauge\n";
                for (const auto& s : f.collect()) {
                    write_series(os, name, f.labels(), s.values, "", format_double(s.value));
                }
                break;
            }
            case MetricType::kHistogram: {
                const HistogramFamily& f = *fe.histogram;
                os << "# HELP " << name << ' ' << f.help() << '\n';
                os << "# TYPE " << name << " histogram\n";
                const std::string bucket = name + "_bucket";
                for (const auto& s : f.collect()) {
                    // Cumulative bucket lines, then the +Inf bucket, then _sum/_count.
                    for (const auto& [le, cum] : s.cumulative) {
                        std::string extra = "le=\"" + format_double(le) + '"';
                        write_series(os, bucket, f.labels(), s.values, extra,
                                     format_double(static_cast<double>(cum)));
                    }
                    write_series(os, bucket, f.labels(), s.values, "le=\"+Inf\"",
                                 format_double(static_cast<double>(s.inf_count)));
                    write_series(os, name + "_sum", f.labels(), s.values, "",
                                 format_double(s.sum));
                    write_series(os, name + "_count", f.labels(), s.values, "",
                                 format_double(static_cast<double>(s.inf_count)));
                }
                break;
            }
        }
    }

    return os.str();
}

// ---------------------------------------------------------------------------
// Process-global registry
// ---------------------------------------------------------------------------

Registry& default_registry() {
    static Registry* reg = new Registry();  // leak-on-exit: outlives all threads
    return *reg;
}

}  // namespace meridian::metrics
