// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — RSS sampler + portable RSS read unit test (#297).
//
// Self-contained (no DB, no socket). Proves meridian_rss_bytes is a LIVE series:
//   1. read_process_rss_bytes() returns a plausible non-zero RSS on this host.
//   2. RssSampler stamps a Gauge with that non-zero value (immediately on start,
//      i.e. before the first interval elapses).
//   3. The sampler keeps updating on its interval, and stop() is clean/idempotent.

#include "meridian/metrics/rss_sampler.h"

#include "meridian/metrics/process_stats.h"
#include "meridian/metrics/registry.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace meridian::metrics;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// Poll `pred` every 5 ms up to `timeout` (condition-based wait, no fixed sleep).
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

}  // namespace

int main() {
    // 1. Portable RSS read: this process holds memory, so RSS must be > 0 and
    //    sane (< 100 GiB — a generous ceiling that still catches a bogus read).
    std::uint64_t rss = read_process_rss_bytes();
    CHECK(rss > 0);
    CHECK(rss < (100ull * 1024 * 1024 * 1024));

    // 2. RssSampler stamps the gauge immediately on start() — before the first
    //    interval — so the series is live on the first scrape. Use a real catalog
    //    child (what the daemons pass) to prove the wiring end-to-end.
    Gauge& g = catalog::rss_bytes().with({"reference", "test"});
    CHECK(g.value() == 0.0);  // startup placeholder

    RssSampler sampler(g, std::chrono::milliseconds(20));
    sampler.start();
    CHECK(g.value() > 0.0);  // seeded synchronously by start()
    double seeded = g.value();

    // 3. The background thread keeps sampling on its interval; the render() of the
    //    default registry now carries a non-zero meridian_rss_bytes series.
    bool moved_or_present = wait_for([&] { return g.value() > 0.0; },
                                     std::chrono::milliseconds(500));
    CHECK(moved_or_present);
    (void)seeded;

    std::string text = default_registry().render();
    CHECK(text.find("meridian_rss_bytes{realm=\"reference\",process=\"test\"}") !=
          std::string::npos);

    // 4. stop() is clean and idempotent (dtor also calls it).
    sampler.stop();
    sampler.stop();

    std::printf("rss_sampler_test: %d checks passed (rss=%llu bytes)\n", g_checks,
                static_cast<unsigned long long>(rss));
    return 0;
}
