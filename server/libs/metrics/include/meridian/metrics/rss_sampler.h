// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — periodic RSS sampler (OPS-05; SAD §8.5).
//
// Turns the declared meridian_rss_bytes gauge into a LIVE series. A daemon holds
// one RssSampler for its process lifetime, pointed at its own
// catalog::rss_bytes().with({realm, process}) child; a background thread stamps
// that gauge with read_process_rss_bytes() every `interval`. Without this the
// gauge only ever holds the startup seed (0), so the process-memory / RSS-growth
// alerts and the RSS dashboard panels have no data to fire on or chart.
//
// The FIRST sample is taken synchronously in start() so the series is non-zero
// the moment the /metrics endpoint is scrapeable (no wait for the first tick).
// stop() wakes the thread immediately (condition variable, not a sleep) and joins;
// the dtor calls it. start()/stop() are idempotent.
//
// The borrowed Gauge MUST outlive the sampler — a catalog child is a stable
// pointer for the whole process run (families never evict), so the daemons pass
// one directly.

#ifndef MERIDIAN_METRICS_RSS_SAMPLER_H
#define MERIDIAN_METRICS_RSS_SAMPLER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "meridian/metrics/registry.h"

namespace meridian::metrics {

class RssSampler {
public:
    // `gauge` is the rss_bytes child to stamp; `interval` is the sample period
    // (10 s is a sensible default for a slow-moving memory gauge).
    RssSampler(Gauge& gauge, std::chrono::milliseconds interval = std::chrono::seconds(10));
    ~RssSampler();

    RssSampler(const RssSampler&) = delete;
    RssSampler& operator=(const RssSampler&) = delete;

    // Take one immediate sample, then spawn the background sampling thread.
    void start();

    // Signal the thread to exit, wake it, and join. Idempotent (dtor calls it).
    void stop();

private:
    void run();

    Gauge& gauge_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

}  // namespace meridian::metrics

#endif  // MERIDIAN_METRICS_RSS_SAMPLER_H
