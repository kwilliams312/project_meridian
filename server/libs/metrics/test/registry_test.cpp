// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — registry + exposition-format unit test (OPS-05).
//
// Self-contained (no DB, no socket): drives the registry API directly and checks
// the Prometheus text exposition it renders. Runs in the plain server ctest.
//
// Coverage:
//   1. Counter inc (default + explicit), negative-guard.
//   2. Gauge set / inc / dec.
//   3. Histogram bucketing (first-fit, cumulative), sum, count, +Inf == count.
//   4. Label arity enforcement (wrong tuple size throws).
//   5. Family idempotency (same name returns same family) + type/label conflict throws.
//   6. Exposition format: HELP/TYPE headers, labelled series line, escaping,
//      the histogram _bucket{le=...}/_sum/_count triple — parsed back + asserted.
//   7. format_double + escape_label_value edge cases.

#include "meridian/metrics/registry.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace meridian::metrics;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                      \
        }                                                                  \
    } while (0)

// True if `haystack` contains `needle` as a substring.
bool has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// Return the value on the exposition line that begins with `prefix` (the text up
// to the trailing newline), or "" if no such line. Matches a whole-line prefix.
std::string line_value(const std::string& text, const std::string& prefix) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) eol = text.size();
        std::string line = text.substr(pos, eol - pos);
        if (line.rfind(prefix, 0) == 0) {
            std::size_t sp = line.rfind(' ');
            if (sp != std::string::npos) return line.substr(sp + 1);
        }
        pos = eol + 1;
    }
    return "";
}

}  // namespace

int main() {
    // --- 1. Counter ---------------------------------------------------------
    {
        Registry reg;
        auto& fam = reg.counter("test_requests_total", "requests", {"method"});
        fam.with({"GET"}).inc();
        fam.with({"GET"}).inc(4.0);
        fam.with({"POST"}).inc();
        CHECK(fam.with({"GET"}).value() == 5.0);
        CHECK(fam.with({"POST"}).value() == 1.0);
        // Negative-guard: a counter must never decrease.
        fam.with({"GET"}).inc(-100.0);
        CHECK(fam.with({"GET"}).value() == 5.0);
    }

    // --- 2. Gauge -----------------------------------------------------------
    {
        Registry reg;
        auto& g = reg.gauge("test_ccu", "ccu", {"realm"}).with({"reference"});
        g.set(10.0);
        g.inc(5.0);
        g.dec(3.0);
        CHECK(g.value() == 12.0);
        g.set(0.0);
        CHECK(g.value() == 0.0);
    }

    // --- 3. Histogram -------------------------------------------------------
    {
        Registry reg;
        // Explicit small bucket set for a deterministic assertion.
        auto& h =
            reg.histogram("test_latency_seconds", "latency", {}, {0.1, 0.5, 1.0}).get();
        h.observe(0.05);   // -> bucket le=0.1
        h.observe(0.2);    // -> bucket le=0.5
        h.observe(0.2);    // -> bucket le=0.5
        h.observe(5.0);    // -> only +Inf
        CHECK(h.count() == 4);
        // sum = 0.05 + 0.2 + 0.2 + 5.0 = 5.45
        CHECK(h.sum() > 5.44 && h.sum() < 5.46);
        // Non-cumulative bucket counts: [le0.1]=1, [le0.5]=2, [le1.0]=0.
        CHECK(h.bucket_count(0) == 1);
        CHECK(h.bucket_count(1) == 2);
        CHECK(h.bucket_count(2) == 0);
    }

    // --- 4. Label arity -----------------------------------------------------
    {
        Registry reg;
        auto& fam = reg.counter("test_arity_total", "arity", {"a", "b"});
        bool threw = false;
        try {
            fam.with({"only-one"});  // needs 2 values
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }

    // --- 5. Family idempotency + conflict -----------------------------------
    {
        Registry reg;
        auto& a = reg.counter("test_dup_total", "help1", {"x"});
        auto& b = reg.counter("test_dup_total", "help2", {"x"});  // same -> same family
        CHECK(&a == &b);
        bool type_conflict = false;
        try {
            reg.gauge("test_dup_total", "as gauge", {"x"});  // wrong type
        } catch (const std::invalid_argument&) {
            type_conflict = true;
        }
        CHECK(type_conflict);
        bool label_conflict = false;
        try {
            reg.counter("test_dup_total", "diff labels", {"y"});  // wrong labels
        } catch (const std::invalid_argument&) {
            label_conflict = true;
        }
        CHECK(label_conflict);
    }

    // --- 6. Exposition format -----------------------------------------------
    {
        Registry reg;
        auto& c = reg.counter("meridian_opcode_total", "Per-opcode message rate",
                              {"realm", "opcode"});
        c.with({"reference", "WORLD_HELLO"}).inc(3.0);
        auto& h = reg.histogram("meridian_action_rtt_seconds", "rtt", {"realm"},
                                {0.05, 0.15});
        h.with({"reference"}).observe(0.02);
        h.with({"reference"}).observe(0.10);

        std::string text = reg.render();

        // HELP + TYPE headers present and correct.
        CHECK(has(text, "# HELP meridian_opcode_total Per-opcode message rate"));
        CHECK(has(text, "# TYPE meridian_opcode_total counter"));
        CHECK(has(text, "# TYPE meridian_action_rtt_seconds histogram"));

        // Labelled counter series line, value 3.
        CHECK(has(text, "meridian_opcode_total{realm=\"reference\",opcode=\"WORLD_HELLO\"} 3"));

        // Histogram triple. Cumulative: le=0.05 -> 1, le=0.15 -> 2, +Inf -> 2.
        CHECK(has(text, "meridian_action_rtt_seconds_bucket{realm=\"reference\",le=\"0.05\"} 1"));
        CHECK(has(text, "meridian_action_rtt_seconds_bucket{realm=\"reference\",le=\"0.15\"} 2"));
        CHECK(has(text, "meridian_action_rtt_seconds_bucket{realm=\"reference\",le=\"+Inf\"} 2"));
        CHECK(has(text, "meridian_action_rtt_seconds_count{realm=\"reference\"} 2"));
        // _sum present with the right total (0.02 + 0.10 = 0.12).
        std::string sum = line_value(text, "meridian_action_rtt_seconds_sum{realm=\"reference\"}");
        CHECK(!sum.empty());
        double sv = std::stod(sum);
        CHECK(sv > 0.119 && sv < 0.121);
    }

    // --- 7. Escaping + format_double ---------------------------------------
    {
        CHECK(escape_label_value("a\"b\\c\nd") == "a\\\"b\\\\c\\nd");
        CHECK(format_double(5.0) == "5");
        CHECK(format_double(1.0 / 0.0) == "+Inf");
        CHECK(format_double(-1.0 / 0.0) == "-Inf");
        // A labelled value carrying a quote round-trips escaped in render().
        Registry reg;
        reg.counter("test_escape_total", "esc", {"path"}).with({"a\"b"}).inc();
        std::string text = reg.render();
        CHECK(has(text, "path=\"a\\\"b\""));
    }

    std::printf("meridian-metrics-test: OK (%d checks)\n", g_checks);
    return 0;
}
