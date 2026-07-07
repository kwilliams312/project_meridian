// SPDX-License-Identifier: Apache-2.0
//
// RssSampler — background thread that stamps the rss_bytes gauge periodically.

#include "meridian/metrics/rss_sampler.h"

#include "meridian/metrics/process_stats.h"

namespace meridian::metrics {

RssSampler::RssSampler(Gauge& gauge, std::chrono::milliseconds interval)
    : gauge_(gauge), interval_(interval) {}

RssSampler::~RssSampler() { stop(); }

void RssSampler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;  // already running
    // Seed immediately so the series is live on the first scrape. A 0 read (e.g.
    // unsupported platform) is skipped so the gauge is not clobbered to 0.
    if (std::uint64_t rss = read_process_rss_bytes(); rss != 0) {
        gauge_.set(static_cast<double>(rss));
    }
    thread_ = std::thread([this] { run(); });
}

void RssSampler::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;  // not running
    cv_.notify_all();  // wake the wait_for immediately
    if (thread_.joinable()) thread_.join();
}

void RssSampler::run() {
    std::unique_lock<std::mutex> lk(mtx_);
    while (running_.load()) {
        // Wait up to one interval; return early if stop() flipped `running_`.
        cv_.wait_for(lk, interval_, [this] { return !running_.load(); });
        if (!running_.load()) break;
        if (std::uint64_t rss = read_process_rss_bytes(); rss != 0) {
            gauge_.set(static_cast<double>(rss));
        }
    }
}

}  // namespace meridian::metrics
