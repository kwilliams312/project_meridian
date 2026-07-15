// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-Unit combat aura container (issue #346, CMB-01; epic #18). The
// fourth CMB-01 module, building on #342 (Unit), #343 (AbilityStore / the kAura
// effect), and #344/#345 (the resolver + seeded CombatRng):
//
//     Unit (#342)  ─┐
//     AbilityEffect ├─►  AuraContainer  ──►  Unit::apply_damage / apply_healing
//     kAura (#343) ─┘        │  (periodic ticks roll a seeded CombatRng #345)
//                            └──►  net stat-modifier deltas the Unit exposes
//
// WHAT THIS FILE IS: the per-Unit aura component the SAD's shallow hierarchy
// mandates (§9 "Shallow class hierarchy + component-style containers (auras,
// inventory, threat)", NOT a generic ECS). It owns the auras currently on ONE
// host Unit and implements the three CMB-01 aura behaviours (server-sad.md §2.5
// "aura container (periodics, stat mods, stacking)"):
//
//   • APPLY / REFRESH / EXPIRE — an aura enters with a duration; re-applying it
//     refreshes (or stacks); it leaves when its duration elapses.
//   • PERIODIC ticks (DoT / HoT) — an aura with a periodic sub-effect deals
//     damage / applies healing to the host on a fixed cadence, rolled with the
//     seeded CombatRng (§2.5 "All rolls server-side, seeded-RNG unit-testable").
//   • STAT-MODIFIER auras — an aura carrying stat_mods contributes a signed delta
//     to the host's primary stats for as long as it is active.
//   • STACKING — refresh-duration (max_stacks == 1), stack-count (max_stacks > 1),
//     and independent instances (a different caster is always its own aura).
//   • DISPEL (issue #361, CMB-04) — each aura carries a dispel_type class
//     (none/magic/curse/poison/disease); dispel(type) strips every aura of that
//     class, respecting undispellable (kNone) auras. See DispelType below.
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md §2.5 (the CMB-01 aura-container
// line + the tick order "combat/auras", §3.2 "combat + auras (casts complete,
// periodics, deaths)"), §9 (the component-container entity-model decision), the
// IF-4 world DDL (schema/sql/world/30_ability.sql `duration_ms` / `max_stacks` /
// `periodic_*` + `ability_effect_stat_mod`) + schema/content/ability.schema.yaml
// (`max_stacks` 1..20, periodic `tick_ms` >= 500), and the existing #342-#345
// headers. Every rule below is ORIGINAL — derived from OUR SAD + DDL, never from
// any existing emulator's aura / spell-aura system (no GPL / AGPL / CMaNGOS /
// TrinityCore / leaked source consulted). See CONTRIBUTING.md.
//
// PURE / DB-FREE / SOCKET-FREE / CLOCK-FREE: like creature_ai's tick(), this is a
// plain deterministic step over in-memory state. It touches NO socket, DB, or
// FlatBuffer, and reads NO wall clock — time is passed IN as `dt_ms`, so the whole
// module is unit-testable in the plain `server` ctest with no MariaDB, and a given
// (seed, dt sequence) yields the same result on every platform. WIRING the tick
// into the 20 Hz per-map loop + the end-to-end golden scenarios are a SEPARATE
// story (#349) that CALLS this module; nothing here reaches into the tick loop.
//
// THREADING (SAD §2.5/§6): a map is single-threaded — "the tick owns entity
// state". A container lives beside its host Unit on the map thread and carries no
// lock of its own.

#ifndef MERIDIAN_WORLDD_AURA_CONTAINER_H
#define MERIDIAN_WORLDD_AURA_CONTAINER_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ability_store.h"     // AbilityId / Ability / AbilityEffect / PeriodicKind / StatKey / StatMod
#include "combat_resolver.h"   // CombatRng
#include "combat_unit.h"       // Unit / ObjectGuid / DamageResult

namespace meridian::worldd {

// Number of primary stats an aura can modify (ability_store.h StatKey: strength,
// agility, stamina, intellect, spirit) — the width of the net-delta table.
inline constexpr std::size_t kStatKeyCount = 5;

// ---------------------------------------------------------------------------
// DispelType — the dispel classification of an aura (issue #361, CMB-04).
// ---------------------------------------------------------------------------
//
// CLEAN-ROOM: this is the aura-dispel taxonomy CMB-04 needs — a dispel targets a
// CLASS of auras (not a single ability), so every aura carries a class tag and a
// dispel of type T removes the auras tagged T. The categories below are the
// canonical MMORPG dispel families (magic / curse / poison / disease) plus a
// `kNone` sentinel for auras that are UNDISPELLABLE (physical/self buffs, boss
// mechanics, etc.) — dispelling kNone is defined as a no-op so those always
// survive. Derived from OUR combat design conventions; NO GPL / AGPL / emulator
// source (CMaNGOS / TrinityCore / leaked) consulted. See CONTRIBUTING.md.
//
// DDL SEAM (mirrors ability_store.h's documented seams): the IF-4 world DDL
// (schema/sql/world/30_ability.sql) does not yet carry a dispel column. When the
// content pipeline (epic #28, mcc) adds an `ability_effect.dispel_type
// ENUM('none','magic','curse','poison','disease')` column, this enum is its 1:1
// image and the classification simply flows into apply()'s `dispel_type` argument
// — no logic here changes. Until then a caller (resolver / test) supplies it
// explicitly; the default (kNone) leaves an aura undispellable.
enum class DispelType : std::uint8_t {
    kNone,     // DDL 'none' / NULL — UNDISPELLABLE (never removed by dispel())
    kMagic,    // DDL 'magic'
    kCurse,    // DDL 'curse'
    kPoison,   // DDL 'poison'
    kDisease,  // DDL 'disease'
};

// ---------------------------------------------------------------------------
// ActiveAura — one live aura instance on a host Unit.
// ---------------------------------------------------------------------------
//
// Identity is the (ability_id, effect_index, caster_guid) triple: the SAME
// ability effect from the SAME caster is ONE stacking/refreshing instance;
// the SAME effect from a DIFFERENT caster is an INDEPENDENT instance (the third
// stacking rule). The aura snapshots the effect's config at apply time (rather
// than chasing an AbilityStore pointer) so the container is self-contained and
// stays valid regardless of the store's lifetime — the config is tiny.
struct ActiveAura {
    // --- identity ---
    AbilityId     ability_id = 0;
    std::uint32_t effect_index = 0;   // which AbilityEffect of the ability
    ObjectGuid    caster_guid = 0;    // who applied it (independent-instance key)

    // --- config snapshot (from the source AbilityEffect) ---
    // The source primitive (SP2.3 #693): kAura for a classic aura, or one of the
    // timed extension kinds (kDot/kHot/kBuff/kDebuff/kShield/kCc) this container now
    // hosts. Drives the extension handling on apply/expiry beyond the shared
    // duration/periodic/stat machinery below.
    EffectKind    kind = EffectKind::kAura;
    School        school = School::kPhysical;
    std::uint32_t duration_ms = 0;    // full duration; a refresh resets remaining to this
    std::uint16_t max_stacks = 1;     // 1 = refresh-only; > 1 = stack-count aura
    PeriodicKind  periodic_kind = PeriodicKind::kNone;
    std::uint32_t periodic_amount_min = 0;  // per-stack periodic amount range (inclusive)
    std::uint32_t periodic_amount_max = 0;
    std::uint32_t periodic_tick_ms = 0;     // cadence; 0 (or kNone) = no periodic
    std::vector<StatMod> stat_mods;         // stat deltas contributed PER STACK
    DispelType dispel_type = DispelType::kNone;  // dispel class; kNone = undispellable

    // --- kBuff / kDebuff — attribute modifier (SP2.3 #693) ------------------
    // The buff/debuff `attribute` contentId ref (verbatim from effects_json). A
    // PRIMARY-stat flat modifier also folds into stat_totals_ (via a StatKey);
    // percent + derived attributes ride the container's attribute ledger, which the
    // SP2.4 #694 EffectiveStats framework (effective_stats.h) consumes to compute a
    // character's effective stat. attr_amount is the PER-STACK signed modifier;
    // attr_modifier picks flat vs percent.
    std::string       attr_ref;
    std::int32_t      attr_amount = 0;
    AttributeModifier attr_modifier = AttributeModifier::kFlat;

    // --- kShield — absorb pool contribution (SP2.3 #693) --------------------
    // The absorb this instance still contributes to host_.absorb(). Granted to the
    // host on apply; reclaimed (clamped) on expiry/removal. Rolled once at apply.
    std::uint32_t shield_amount = 0;

    // --- kCc — crowd-control category (SP2.3 #693) --------------------------
    CrowdControlKind cc_kind = CrowdControlKind::kStun;

    // --- runtime state ---
    std::uint16_t stacks = 1;              // current stack count in [1, max_stacks]
    std::uint32_t remaining_ms = 0;        // time left before expiry
    std::uint32_t since_last_tick_ms = 0;  // periodic accumulator (survives a refresh)

    bool is_periodic() const {
        return periodic_kind != PeriodicKind::kNone && periodic_tick_ms > 0;
    }
    bool is_control() const { return kind == EffectKind::kCc; }
    bool is_shield() const { return kind == EffectKind::kShield; }
};

// The net attribute modifier a buff/debuff query returns (SP2.3 #693). `flat` is in
// raw attribute units; `percent` is in hundredths of a percent-point (the schema's
// `modifier: percent` unit). This is the live buff/debuff LAYER the SP2.4 #694
// EffectiveStats framework (effective_stats.h) folds together with a character's
// base + class/race mods to compute an effective stat (primary AND derived, flat
// AND percent).
struct AttributeDelta {
    std::int32_t flat = 0;
    std::int32_t percent = 0;
};

// What apply() did to the container — so a caller (and the tests) can tell a fresh
// application from a refresh from a stack gain.
enum class AuraApplyAction : std::uint8_t {
    kAdded,      // a new instance was inserted
    kRefreshed,  // an existing instance's duration was reset (no stack change)
    kStacked,    // an existing instance gained a stack (and was refreshed)
    kRejected,   // the effect was not a kAura effect (nothing changed)
};

struct AuraApplyResult {
    AuraApplyAction action = AuraApplyAction::kRejected;
    std::uint16_t   stacks = 0;  // the instance's stack count after the call
};

// What one tick(dt) did — the totals the future tick loop (#349) turns into
// damage/heal events, death handling, and aura-change broadcasts.
struct AuraTickResult {
    std::uint32_t periodic_damage = 0;   // total HP removed by periodic damage this step
    std::uint32_t periodic_healing = 0;  // total HP restored by periodic healing this step
    std::uint32_t ticks_fired = 0;       // periodic ticks that landed
    std::uint32_t auras_expired = 0;     // instances removed because their duration elapsed
    bool host_died = false;              // a periodic damage tick drove the host to 0 HP

    bool any_periodic() const { return ticks_fired > 0; }
};

// ---------------------------------------------------------------------------
// AuraContainer — the per-Unit aura component.
// ---------------------------------------------------------------------------
//
// Bound to one host Unit for its lifetime. Auras are held in a flat vector
// iterated linearly (SAD §7 "aura containers are flat arrays iterated linearly"),
// in application order — so a given (seed, dt) sequence is fully deterministic.
class AuraContainer {
public:
    explicit AuraContainer(Unit& host) : host_(host) {}

    // Apply one kAura effect of `ability` (its effect at `effect_index`) cast by
    // `caster_guid`, resolving the stacking rules against any existing instance
    // with the same (ability_id, effect_index, caster_guid):
    //   • no existing instance            → ADD a fresh instance (stacks = 1).
    //   • existing, max_stacks == 1        → REFRESH duration only (refresh rule).
    //   • existing, max_stacks  > 1        → gain one stack (clamped to max_stacks)
    //                                        AND refresh duration (stack-count rule).
    // A DIFFERENT caster never matches an existing instance, so it always ADDs an
    // INDEPENDENT instance (independent-instances rule). Stat-mod deltas are folded
    // into / out of the host's net stat totals as stacks change. Returns what
    // happened. If `effect_index` is out of range or the effect is not kAura the
    // call is a no-op returning kRejected.
    //
    // `dispel_type` tags the instance for CMB-04 dispel (issue #361): a fresh ADD
    // stores it on the instance; a refresh/stack keeps the original instance's tag
    // (the classification is fixed at first application). It defaults to kNone —
    // an unclassified aura is UNDISPELLABLE.
    // `rng` (when non-null) rolls a kShield effect's absorb amount at apply time
    // (deterministic seeded roll, SP2.3 #693); null falls back to amount_min so a
    // shieldless caller stays deterministic without an RNG. It is unused by every
    // other kind (their amounts are rolled at tick time, or are fixed).
    AuraApplyResult apply(const Ability& ability, std::uint32_t effect_index,
                          ObjectGuid caster_guid,
                          DispelType dispel_type = DispelType::kNone,
                          CombatRng* rng = nullptr);

    // Convenience: apply EVERY container-hosted timed effect of `ability` — the
    // classic kAura AND the SP2.3 #693 timed extensions (dot/hot/buff/debuff/shield/
    // cc). Instantaneous kinds (damage/heal/threat/resource/movement/summon) are
    // skipped here (the resolver / map tick execute those). Each applied effect is
    // tagged with `dispel_type`; `rng` rolls shield amounts. Returns how many effects
    // were applied (added/refreshed/stacked).
    std::size_t apply_ability_effects(const Ability& ability, ObjectGuid caster_guid,
                                      DispelType dispel_type = DispelType::kNone,
                                      CombatRng* rng = nullptr);

    // Advance every aura by `dt_ms`, firing due periodic ticks into the host and
    // expiring auras whose duration has elapsed. Periodic amounts are rolled from
    // the seeded `rng` (× stack count); a damage tick applies the source ability's
    // school through the shared armor contract before Unit::apply_damage (and
    // reports host death), a heal tick Unit::apply_healing. Cadence is accumulator-
    // based, so the result is independent of the `dt_ms` granularity (a single
    // dt = N·tick fires N ticks, matching N steps of dt = tick). Once the host is
    // dead no further periodics fire (a corpse takes no DoT/HoT). Expired auras
    // roll back their stat deltas. Pure & deterministic given (rng state, dt).
    AuraTickResult tick(std::uint32_t dt_ms, CombatRng& rng);

    // Remove a specific aura instance (e.g. a dispel), rolling back its stat
    // deltas. Returns true if an instance was found and removed.
    bool remove(AbilityId ability_id, std::uint32_t effect_index, ObjectGuid caster_guid);

    // DISPEL (issue #361, CMB-04) — remove EVERY aura tagged with dispel type
    // `type`, rolling back each removed instance's stat deltas (all stacks). This
    // is the class-targeted dispel: a "dispel magic" (type == kMagic) strips only
    // the magic auras and leaves curses / poisons / diseases untouched. `type ==
    // kNone` is the UNDISPELLABLE sentinel — dispelling it is a defined no-op, so
    // unclassified / undispellable auras always survive. Returns the number of
    // instances removed. O(n) over the flat vector; iterates back-to-front so
    // erasures don't disturb the scan.
    std::size_t dispel(DispelType type);

    // Remove every aura (e.g. on death / zone change), rolling back all stat deltas.
    void clear();

    // --- queries -----------------------------------------------------------
    std::size_t size() const { return auras_.size(); }
    bool empty() const { return auras_.empty(); }
    const std::vector<ActiveAura>& auras() const { return auras_; }

    // The matching instance, or nullptr. Const + non-const overloads.
    const ActiveAura* find(AbilityId ability_id, std::uint32_t effect_index,
                           ObjectGuid caster_guid) const;

    // The net signed stat modifier currently applied to the host for `stat` — the
    // sum over all active auras of (stat_mod.amount × stacks). This is HOW a
    // stat-mod aura "applies to the Unit": the Unit's effective stat is its base
    // stat plus this delta (base stats land with the stat system; the aura layer
    // is this container). O(1) — maintained incrementally on apply/stack/expire.
    // A kBuff/kDebuff on a PRIMARY attribute with a FLAT modifier also folds in here
    // (SP2.3 #693), so this reflects both authored kAura stat_mods and buff/debuff.
    std::int32_t stat_delta(StatKey stat) const;

    // The net buff/debuff modifier on `attribute_ref` (SP2.3 #693) — the live aura
    // LAYER the SP2.4 #694 EffectiveStats framework consumes. Flat + percent are
    // summed over active buff/debuff auras (× stacks). For a primary attribute the
    // flat part equals stat_delta(that StatKey); percent + derived attributes live
    // ONLY here (no StatKey), so EffectiveStats reads them through this query.
    // Zeroes for an untouched ref.
    AttributeDelta attribute_delta(const std::string& attribute_ref) const;

    // --- crowd control (SP2.3 #693) ----------------------------------------
    // Whether a live kCc aura of the given kind is currently on the host. The map
    // tick reads these to gate actions: a stun or silence blocks casting, a root (or
    // stun) blocks self-movement. Any active instance suffices (stacking is refresh-
    // only for cc). O(n) over the flat vector (n is tiny).
    bool has_control(CrowdControlKind kind) const;
    bool is_stunned() const { return has_control(CrowdControlKind::kStun); }
    bool is_rooted() const { return has_control(CrowdControlKind::kRoot); }
    bool is_silenced() const { return has_control(CrowdControlKind::kSilence); }

private:
    // Fold this aura's modifiers (kAura stat_mods AND a kBuff/kDebuff attribute mod)
    // into the net ledgers: sign = +1 to add `stack_count` stacks' worth, -1 to
    // remove. Reclaims a shield's absorb from the host when sign = -1.
    void fold_modifiers(const ActiveAura& aura, std::uint16_t stack_count, int sign);

    Unit& host_;
    std::vector<ActiveAura> auras_;
    std::int32_t stat_totals_[kStatKeyCount] = {0, 0, 0, 0, 0};
    // Live buff/debuff attribute ledger keyed by attribute ref — the aura LAYER the
    // SP2.4 #694 EffectiveStats framework reads via attribute_delta().
    std::unordered_map<std::string, std::int32_t> attr_flat_;
    std::unordered_map<std::string, std::int32_t> attr_percent_;
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_AURA_CONTAINER_H
