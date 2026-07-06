// SPDX-License-Identifier: Apache-2.0
//
// worldd — shard worker / map simulation daemon (entry point).
//
// What this BECOMES (server SAD §2.5, recast at M3): the shard worker that runs
// the simulation. Per-map update threads run the 20 Hz tick (drain inbound →
// movement → AI → combat/auras → spawns → AoI delta → flush); a grid/AoI engine
// (533 m grids, 8×8 cells); the entity/aggro/threat model; the combat resolver;
// quest/loot/inventory/vendor services; spatial chat; the generated opcode
// dispatcher; the script-hook seam; the IF-7 hot-reload service; and the async
// DB I/O layer. At M0 it also carries the net gate + dispatcher + IF-2 codec
// (SAD §7) before those move to gatewayd at M2. From M3 it is bus-attached with
// no client listener and MapKey = (map_id, instance_id, shard_index) keying.
//
// This file is the M0 SKELETON: binary, identity, CLI surface only. No tick, no
// maps, no bus yet. Clean-room: implemented from the SAD, no GPL source
// consulted (CONTRIBUTING.md).

#include "meridian/core/log.hpp"
#include "meridian/core/version.hpp"

#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kDaemonName = "worldd";

void print_help() {
    std::printf(
        "%s — Project Meridian shard worker / map simulation daemon\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --version    Print version and build info, then exit.\n"
        "  --help, -h   Print this help, then exit.\n"
        "\n"
        "M0 skeleton (server SAD §2.5): the map tick, grid/AoI, combat, and DB\n"
        "I/O layer land on top of this entry point in later work.\n",
        kDaemonName, kDaemonName);
}

void print_version() {
    std::printf("%s %s\n%s\n", kDaemonName,
                meridian::core::version_string().c_str(),
                meridian::core::build_info().c_str());
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        }
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            print_help();
            return 0;
        }
        std::fprintf(stderr, "%s: unknown option '%s' (try --help)\n", kDaemonName,
                     argv[i]);
        return 2;
    }

    meridian::core::log::info(kDaemonName,
                              "worldd skeleton up — no maps/tick at M0; exiting");
    std::printf("%s %s (%s) — skeleton, no maps to simulate yet\n", kDaemonName,
                meridian::core::version_string().c_str(), meridian::core::kMilestone);
    return 0;
}
