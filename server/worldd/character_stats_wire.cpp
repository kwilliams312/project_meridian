// SPDX-License-Identifier: Apache-2.0
//
// worldd — implementation of the CHARACTER_STATS wire encoder (SP2.5 #897). See the
// header for the full provenance + security note. Clean-room from the #896 aggregator
// output model + the world.fbs CharacterStats contract; no GPL/emulator source
// consulted (CONTRIBUTING.md).

#include "character_stats_wire.h"

#include <algorithm>
#include <string>
#include <utility>

#include "flatbuffers/flatbuffers.h"
#include "world_generated.h"

namespace meridian::worldd {

namespace fb = flatbuffers;
namespace mn = meridian::net;

std::vector<std::uint8_t> encode_character_stats(const AggregatedCharacterStats& stats) {
    fb::FlatBufferBuilder b;

    // Collect (ref, value) then sort by ref so the wire ORDER is DETERMINISTIC: the
    // aggregator stores attributes in an unordered_map (iteration order is
    // unspecified and can vary by platform/run), which would make the conformance
    // golden drift and hand a jittery order to the client. A stable lexicographic
    // order fixes both — the golden reproduces and the panel can rely on the order.
    std::vector<std::pair<std::string, std::int32_t>> sorted(stats.attributes.begin(),
                                                             stats.attributes.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

    std::vector<fb::Offset<mn::CharacterStatEntry>> rows;
    rows.reserve(sorted.size());
    for (const auto& [ref, value] : sorted) {
        rows.push_back(mn::CreateCharacterStatEntry(b, b.CreateString(ref), value));
    }
    auto vec = b.CreateVector(rows);

    b.Finish(mn::CreateCharacterStats(b, stats.level, vec, stats.gear_armor));
    return std::vector<std::uint8_t>(b.GetBufferPointer(),
                                     b.GetBufferPointer() + b.GetSize());
}

}  // namespace meridian::worldd
