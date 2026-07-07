// SPDX-License-Identifier: Apache-2.0
//
// libmeridian-metrics — portable process resource sampling (OPS-05; SAD §8.5).
//
// The catalog declares meridian_rss_bytes{realm,process} (process resident
// memory). To turn that declared series into a LIVE one — so the memory-growth /
// process-memory alerts in server/ops/prometheus/rules and the RSS panels on the
// worldd / realm-overview dashboards actually have data — a daemon must sample
// its own RSS periodically (RssSampler, below reads this) and stamp the gauge.
//
// PORTABLE: the daemons run on both macOS (dev) and Linux (CI + reference deploy),
// so RSS is read two ways behind one function:
//   · Linux : /proc/self/statm field 2 (resident pages) × page size.
//   · macOS : task_info(mach_task_self(), MACH_TASK_BASIC_INFO).resident_size.
// Clean-room from the documented /proc and Mach task_info interfaces only.

#ifndef MERIDIAN_METRICS_PROCESS_STATS_H
#define MERIDIAN_METRICS_PROCESS_STATS_H

#include <cstdint>

namespace meridian::metrics {

// Current resident set size (physical memory) of THIS process, in bytes. Returns
// 0 if it cannot be determined (unsupported platform or a read failure) — a 0 is
// a benign "no sample" that leaves the gauge unchanged from the caller's view.
std::uint64_t read_process_rss_bytes();

}  // namespace meridian::metrics

#endif  // MERIDIAN_METRICS_PROCESS_STATS_H
