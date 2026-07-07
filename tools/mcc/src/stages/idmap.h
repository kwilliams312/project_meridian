// tools/mcc/src/stages/idmap.h — IF-9 idmap allocator (Tools SAD §2.4).
//
// The `idmap.lock` file is the SOLE authority mapping string content ids to the
// numeric runtime ids used as DB keys in IF-4 (Tools SAD §2.4, "this section is
// the contract"). This header models that lock file and the deterministic,
// append-only allocation the link stage performs against it.
//
// Contract summary (verbatim policy from SAD §2.4):
//   * One lock file per pack at /content/<namespace>/idmap.lock — a pack carries
//     its own map inside its .mcpack (SAD §2.4 "File placement").
//   * Format: YAML — schema/namespace/band/released_watermark/map/retired.
//   * `map` is APPEND-ONLY: `<string id>: <local index>` (not the full numeric
//     id). Numeric runtime id = band * 2^20 + local_index, a uint32; band 0
//     covers local indices 1..1,048,575 (0 reserved as null). (SAD §2.4
//     "Numeric ID derivation".)
//   * Determinism: existing string->index mappings are PRESERVED across builds;
//     new entities are allocated the next free index deterministically
//     (lexicographic by string id, matching the reassign tie-break), so a fresh
//     allocation and a re-run on unchanged content produce an identical lock.
//   * Retirement: deleting an entity moves its entry from `map` to `retired`;
//     retired indices are NEVER reallocated (SAD §2.4 "Retirement").

#ifndef MCC_STAGES_IDMAP_H
#define MCC_STAGES_IDMAP_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mcc::idmap {

// Local indices are per-pack, 1-based (0 = null, reserved). The band multiplies
// into the high bits: numeric_id = band * kBandStride + local_index.
using LocalIndex = std::uint32_t;

// SAD §2.4: band spans 2^20 local indices; 0 reserved as null within each band.
constexpr std::uint32_t kBandBits = 20;
constexpr std::uint32_t kBandStride = 1u << kBandBits;  // 1,048,576
constexpr LocalIndex kMaxLocalIndex = kBandStride - 1;  // 1,048,575

// Compute the uint32 runtime id from a band + local index (SAD §2.4).
constexpr std::uint32_t numeric_id(std::uint32_t band, LocalIndex local_index) {
    return band * kBandStride + local_index;
}

// The parsed/served contents of one pack's idmap.lock. `map` and `retired` use
// an ordered std::map so serialization is deterministic (sorted by string id)
// regardless of allocation history — the on-disk form is stable and diffable.
struct IdMap {
    std::string schema = "meridian/idmap@1";
    std::string namespace_;                     // e.g. "core"
    std::uint32_t band = 0;                      // SAD §2.4: core is band 0
    LocalIndex released_watermark = 0;           // highest local index frozen by a release
    std::map<std::string, LocalIndex> map;       // string id -> local index (append-only)
    std::map<std::string, LocalIndex> retired;   // deleted entities; never reallocated

    // Highest local index currently in use (across both live and retired), so a
    // new allocation never collides with a retired-but-frozen index.
    LocalIndex high_water() const;
};

// Outcome of an allocation pass, for diagnostics + reporting.
struct AllocationResult {
    std::vector<std::string> newly_allocated;  // string ids that got a fresh index (sorted)
    std::vector<std::string> retired;          // string ids moved live -> retired this pass
    bool changed = false;                      // true if the lock differs from what was read
    bool overflow = false;                     // band exhausted (> kMaxLocalIndex)
};

// Parse an idmap.lock from YAML text. Returns false (and leaves `out` holding
// whatever it managed to read) on a malformed document; `err` gets a one-line
// reason. A missing file is NOT a parse error — the caller starts from a
// default IdMap and lets the allocator populate it.
bool parse(const std::string& yaml_text, IdMap& out, std::string& err);

// Serialize an IdMap to canonical idmap.lock YAML text (SAD §2.4 format). Keys
// are emitted sorted (std::map order) so byte output is deterministic. The
// `retired:` block is omitted entirely when empty (matches a clean first lock).
std::string serialize(const IdMap& m);

// Deterministic, append-only allocation (SAD §2.4). `live_ids` is the set of
// string ids that currently exist in content (order-independent).
//   * ids already in `m.map` keep their index (idempotent / stable).
//   * ids in `live_ids` but not mapped get the next free index, assigned in
//     lexicographic order so a fresh allocation is fully determined by content.
//   * ids in `m.map` but no longer live are moved to `m.retired` (SAD §2.4).
// `m` is updated in place. Never reuses a retired or in-use index.
AllocationResult allocate(IdMap& m, const std::vector<std::string>& live_ids);

}  // namespace mcc::idmap

#endif  // MCC_STAGES_IDMAP_H
