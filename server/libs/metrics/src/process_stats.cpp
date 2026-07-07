// SPDX-License-Identifier: Apache-2.0
//
// read_process_rss_bytes() — portable current-RSS read (macOS + Linux).

#include "meridian/metrics/process_stats.h"

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>

#include <cstdio>
#endif

namespace meridian::metrics {

std::uint64_t read_process_rss_bytes() {
#if defined(__APPLE__)
    // Mach task self-inspection: MACH_TASK_BASIC_INFO.resident_size is the current
    // resident set in bytes.
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                 reinterpret_cast<task_info_t>(&info), &count);
    if (kr != KERN_SUCCESS) return 0;
    return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
    // /proc/self/statm: "size resident shared ..." in PAGES. Field 2 (resident)
    // × page size = RSS in bytes.
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    unsigned long size_pages = 0, resident_pages = 0;
    int matched = std::fscanf(f, "%lu %lu", &size_pages, &resident_pages);
    std::fclose(f);
    if (matched != 2) return 0;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return 0;
    return static_cast<std::uint64_t>(resident_pages) * static_cast<std::uint64_t>(page);
#else
    return 0;  // unsupported platform: no sample (gauge stays as-is).
#endif
}

}  // namespace meridian::metrics
