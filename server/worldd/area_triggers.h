// SPDX-License-Identifier: Apache-2.0
//
// worldd — area triggers + POI discovery core (issue #368; WLD-01/03, epic #20).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md only — §2.5 ("Script hook seam
// (QST-02) — registry of typed C++ hook points ... OnAreaTrigger ..."; the map
// tick "drain inbound → movement → ... → AoI delta" phase loop that owns entity
// state single-threaded), §4.5 ("world ... `zone`/`area`/`area_trigger`/
// `graveyard`" content tables replaced wholesale by the nightly content build),
// and world.fbs (the 0x9xxx PoiDiscovered message). No GPL source (CMaNGOS /
// TrinityCore or otherwise) consulted. See CONTRIBUTING.md.
//
// WHAT THIS FILE IS: the PURE, DB-free, socket-free area-trigger core. It answers
// one question — "given a character's authoritative position, which trigger
// volumes did it just ENTER or LEAVE, and is this the first time it discovered a
// POI?" — and it answers it with per-character occupancy + discovered bookkeeping
// so an enter/leave fires exactly ONCE per boundary crossing and a POI discovery
// fires exactly ONCE per character (re-entry never re-fires discovery). It has NO
// dependency on FlatBuffers, the DB, or a socket, so the trigger logic runs in the
// plain `server` ctest with no MariaDB — mirroring aoi_grid.{h,cpp}. The wiring
// that turns a discovery event into a POI_DISCOVERED frame and routes it through
// the mover's egress lives in world_state.{h,cpp}, on top of this core.
//
// ─── THE VOLUMES (SAD §4.5 world content; M1 placeholder → mcc #28 seam) ──────
// At M1 the volume set is a small deterministic PLACEHOLDER on the 128 m flat
// bootstrap chunk (D-19); real volumes are compiled into world content and loaded
// at boot (mcc #28). `load()` is the single seam that swaps the placeholder set
// for compiled data — no other code changes when real world data arrives. A
// volume is an axis-aligned box; membership is a point-in-box test on (x, y) with
// a z span (the flat map spans full z by default), consistent with the AoI grid
// treating the M0 map as a single plane.
//
// ─── EVALUATION (driven by the map tick against player positions, SAD §2.5) ────
// `evaluate(guid, pos)` is called with a character's authoritative position (from
// the movement phase of the tick). It diffs the volumes the character is NOW
// inside against the set it WAS inside (per-guid occupancy), emitting one
// TriggerEvent per crossing:
//   • ENTER  — now inside a volume it was not inside before.
//   • LEAVE  — no longer inside a volume it was inside before.
// For a kDiscovery volume, the FIRST enter (character never discovered this POI)
// additionally sets `discovered_now` and records it in the per-guid discovered set
// so a later re-entry produces an ENTER event with `discovered_now == false` — the
// "re-entry does not re-fire discovery" guarantee. `remove(guid)` drops a
// character's bookkeeping on world-leave / disconnect (M1 in-memory; persisting
// discovered POIs to the characters DB is a later, per-character concern).

#ifndef MERIDIAN_WORLDD_AREA_TRIGGERS_H
#define MERIDIAN_WORLDD_AREA_TRIGGERS_H

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aoi_grid.h"             // AoiId (the character's entity guid key)
#include "movement_validation.h"  // Position

namespace meridian::worldd {

// A stable content id for a trigger volume — the world DB `area_trigger` id at M1
// (placeholder ids for the M1 placeholder set). 0 is reserved "none".
using TriggerId = std::uint32_t;

// The server-side reaction a volume drives (SAD §2.5 area-trigger events). Only
// kDiscovery is client-facing (POI_DISCOVERED, world.fbs 0x9xxx); the rest fire
// server-side hooks only (the OnAreaTrigger seam) at M1.
enum class TriggerKind : std::uint8_t {
    kDiscovery = 0,       // POI discovery — mark discovered on the character + notify client
    kQuestObjective = 1,  // quest-objective hook (server-side; quest state is a later issue)
    kGraveyard = 2,       // graveyard / resurrection zone (server-side)
    kGeneric = 3,         // any other enter/leave volume (server-side event only)
};

// An axis-aligned trigger volume on the flat bootstrap map (D-19). Membership is a
// point-in-box test on (x, y); [min_z, max_z] spans z (full-range by default on
// the flat map). Compiled from world content (mcc #28) at M1; a placeholder set
// stands in until then (placeholder_area_triggers()).
struct TriggerVolume {
    TriggerId id = 0;
    TriggerKind kind = TriggerKind::kGeneric;
    std::uint32_t area_id = 0;  // owning area/zone id (world DB `area`); 0 = unspecified
    std::uint32_t name_id = 0;  // idmap string id for the display name (§4.6); 0 = unnamed
    float min_x = 0.0f;
    float min_y = 0.0f;
    float min_z = std::numeric_limits<float>::lowest();
    float max_x = 0.0f;
    float max_y = 0.0f;
    float max_z = std::numeric_limits<float>::max();

    // Whether `p` is inside this volume (inclusive bounds). Half-open would drop a
    // point exactly on the max edge; inclusive is the intuitive "standing on the
    // boundary counts as inside" and keeps enter/leave symmetric with the box.
    bool contains(const Position& p) const;
};

// One boundary crossing produced by evaluate(): a character entered or left a
// volume this evaluation. Carries the volume's content ids so the caller can build
// a wire message / fire a hook without re-reading the volume table.
struct TriggerEvent {
    TriggerId trigger_id = 0;
    TriggerKind kind = TriggerKind::kGeneric;
    std::uint32_t area_id = 0;
    std::uint32_t name_id = 0;
    bool entered = false;         // true = crossed IN (enter); false = crossed OUT (leave)
    bool discovered_now = false;  // kDiscovery + first-ever entry → mark + notify the client
};

// ---------------------------------------------------------------------------
// AreaTriggerSet — the pure volume table + per-character occupancy/discovery.
// ---------------------------------------------------------------------------
//
// Owned by the world thread (SAD §2.5/§6: game state is single-threaded). Not
// thread-safe by itself — the world-thread ownership (or an external lock, as
// world_state.cpp uses at M0) provides the serialization, exactly like AoiGrid.
class AreaTriggerSet {
public:
    AreaTriggerSet() = default;

    // Replace the volume set (the mcc #28 seam: placeholder → compiled world data).
    // Does NOT clear per-character occupancy/discovery — callers load once at boot
    // before any character enters. Loading is idempotent for the same data.
    void load(std::vector<TriggerVolume> volumes);

    // How many volumes are loaded.
    std::size_t volume_count() const { return volumes_.size(); }

    // Evaluate `guid`'s authoritative position against every volume, returning the
    // enter/leave crossings since the last evaluation for `guid` and updating the
    // per-guid occupancy + discovered bookkeeping. Enter fires once per crossing;
    // leave once per crossing; a kDiscovery volume's FIRST enter sets
    // `discovered_now` (subsequent enters do not). Empty result when nothing
    // crossed. Deterministic: events are returned in volume-load order.
    std::vector<TriggerEvent> evaluate(AoiId guid, const Position& pos);

    // Drop all per-guid bookkeeping (world-leave / disconnect). No-op if absent.
    void remove(AoiId guid);

    // Test/diagnostic: whether `guid` is currently recorded inside volume `id`.
    bool is_inside(AoiId guid, TriggerId id) const;
    // Test/diagnostic: whether `guid` has ever discovered POI volume `id`.
    bool is_discovered(AoiId guid, TriggerId id) const;

private:
    std::vector<TriggerVolume> volumes_;
    // Volumes each character is CURRENTLY inside (diffed each evaluate for enter/leave).
    std::unordered_map<AoiId, std::unordered_set<TriggerId>> occupancy_;
    // POI volumes each character has ALREADY discovered (persists across leave/re-enter
    // within a session; the "re-entry does not re-fire discovery" guarantee).
    std::unordered_map<AoiId, std::unordered_set<TriggerId>> discovered_;
};

// The M1 PLACEHOLDER trigger set (the mcc #28 seam — real volumes are compiled
// into world content and loaded at boot later). Deterministic, laid out on the
// 128 m flat bootstrap map (D-19), clear of the play-area-centre spawn (64, 64) so
// a fresh login does not instantly trip a trigger. Documented volume-by-volume in
// area_triggers.cpp.
std::vector<TriggerVolume> placeholder_area_triggers();

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_AREA_TRIGGERS_H
