// tools/mcc/src/stages/idmap.cpp — IF-9 idmap allocator (Tools SAD §2.4).

#include "stages/idmap.h"

#include <algorithm>
#include <sstream>
#include <unordered_set>

#include <yaml-cpp/yaml.h>

namespace mcc::idmap {

LocalIndex IdMap::high_water() const {
    LocalIndex hi = released_watermark;
    for (const auto& [id, idx] : map) hi = std::max(hi, idx);
    for (const auto& [id, idx] : retired) hi = std::max(hi, idx);
    return hi;
}

bool parse(const std::string& yaml_text, IdMap& out, std::string& err) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const std::exception& e) {
        err = std::string("malformed idmap.lock: ") + e.what();
        return false;
    }
    if (!root || !root.IsMap()) {
        err = "idmap.lock is not a YAML mapping";
        return false;
    }

    try {
        if (root["schema"]) out.schema = root["schema"].as<std::string>();
        if (root["namespace"]) out.namespace_ = root["namespace"].as<std::string>();
        if (root["band"]) out.band = root["band"].as<std::uint32_t>();
        if (root["released_watermark"])
            out.released_watermark = root["released_watermark"].as<LocalIndex>();

        if (root["map"] && root["map"].IsMap()) {
            for (const auto& kv : root["map"]) {
                out.map[kv.first.as<std::string>()] = kv.second.as<LocalIndex>();
            }
        }
        if (root["retired"] && root["retired"].IsMap()) {
            for (const auto& kv : root["retired"]) {
                out.retired[kv.first.as<std::string>()] = kv.second.as<LocalIndex>();
            }
        }
    } catch (const std::exception& e) {
        err = std::string("malformed idmap.lock field: ") + e.what();
        return false;
    }
    return true;
}

std::string serialize(const IdMap& m) {
    // Hand-written emit (not yaml-cpp's Emitter) for exact, stable control over
    // ordering, spacing, and the block-comment annotations that make the file
    // human-diffable per SAD §2.4. std::map iteration is already sorted by id.
    std::ostringstream o;
    o << "schema: " << m.schema << '\n';
    o << "namespace: " << m.namespace_ << '\n';
    o << "band: " << m.band << "                      # assigned once, immutable (SAD §2.4)\n";
    o << "released_watermark: " << m.released_watermark
      << "      # highest local index frozen by a tagged release\n";
    o << "map:                         # append-only; local index (SAD §2.4)\n";
    for (const auto& [id, idx] : m.map) {
        o << "  " << id << ": " << idx << '\n';
    }
    if (!m.retired.empty()) {
        o << "retired:                     # deleted entities; never reallocated (SAD §2.4)\n";
        for (const auto& [id, idx] : m.retired) {
            o << "  " << id << ": " << idx << '\n';
        }
    }
    return o.str();
}

AllocationResult allocate(IdMap& m, const std::vector<std::string>& live_ids) {
    AllocationResult res;

    const std::unordered_set<std::string> live(live_ids.begin(), live_ids.end());

    // 1) Retirement (SAD §2.4): any mapped id that is no longer live moves to
    //    `retired`. Its index is frozen and never reallocated. We collect first,
    //    then mutate, so we don't invalidate iterators mid-walk.
    std::vector<std::string> to_retire;
    for (const auto& [id, idx] : m.map) {
        if (live.find(id) == live.end()) to_retire.push_back(id);
    }
    for (const auto& id : to_retire) {
        m.retired[id] = m.map[id];
        m.map.erase(id);
        res.retired.push_back(id);
        res.changed = true;
    }

    // 2) Determine which live ids are unmapped, allocated in lexicographic order
    //    so a fresh allocation is fully determined by content (and matches the
    //    `mcc idmap reassign` tie-break, SAD §2.4). std::set keeps them sorted.
    std::vector<std::string> unmapped;
    for (const auto& id : live_ids) {
        if (m.map.find(id) == m.map.end()) unmapped.push_back(id);
    }
    std::sort(unmapped.begin(), unmapped.end());

    // 3) Next free index starts above the current high-water mark (never reuses
    //    a live, retired, or released-frozen index). Append-only guarantee.
    LocalIndex next = m.high_water() + 1;
    for (const auto& id : unmapped) {
        if (next > kMaxLocalIndex) {
            res.overflow = true;
            break;  // band exhausted — caller surfaces this as an L015 error.
        }
        m.map[id] = next;
        res.newly_allocated.push_back(id);
        res.changed = true;
        ++next;
    }

    return res;
}

}  // namespace mcc::idmap
