// SPDX-License-Identifier: Apache-2.0
//
// worldd — server-authoritative combat resolver (issues #344 + #345, CMB-01;
// epic #18). The two halves of ONE code path (ability use -> resolution):
//
//   #344  GCD + cast lifecycle + D-10 accept/reject — CombatSession +
//         begin_ability_use(): validate the use (known ability, caster alive,
//         GCD clock, in-progress cast, resource, target legality/range/LoS) and
//         reply ACCEPT (start GCD + cast timer) or REJECT so the client's
//         optimistic GCD/cast (client SAD §2.2/§3c, D-10) confirms or ROLLS BACK
//         within one RTT. Cast-time abilities run a cast timer with interrupt +
//         pushback; instants resolve immediately.
//
//   #345  resolution — resolve_ability(): roll the ATTACK TABLE (miss / dodge /
//         parry / hit / crit per SAD §2.5) with a SEEDED RNG, apply damage or
//         heal to the target Unit, and trigger the death transition at 0 health.
//         Outcomes are server-only (never predicted).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md §2.5 ("Combat resolver —
// CMB-01: target/range/LoS validation, GCD enforcement, cast timers with
// interrupt/pushback, attack tables, damage/heal application … All rolls
// server-side, seeded-RNG unit-testable") + §3.3 (the CastRequest validate list:
// known ability, GCD clock, resource, range, LoS, target legality) and client
// SAD §2.2/§3c (the D-10 optimistic GCD/cast + one-RTT rollback). No GPL / AGPL /
// CMaNGOS / TrinityCore / leaked emulator source consulted — every constant and
// formula here is ORIGINAL, derived from OUR SAD and reasonable first values.
// See CONTRIBUTING.md.
//
// PURE / DB-FREE / SOCKET-FREE: this module is a plain in-memory core over the
// #342 Unit model + the #343 AbilityStore. It reads/mutates Unit combat state
// and rolls a seeded RNG; it touches NO socket, DB, or FlatBuffer. The dispatch
// handler (world_dispatch.cpp) lifts the wire CastRequest into these calls and
// serialises the CastStart/CastFailed/CastResult replies. So the whole resolver
// runs in the plain `server` ctest with no MariaDB (like aoi_grid / ability_store).
//
// THREADING (SAD §2.5/§6): a map is single-threaded — "the tick owns entity
// state". CombatSession is per-session mutable state owned by that thread (like
// SessionMovementState); the resolver carries no lock of its own.
//
// OUT OF SCOPE (separate stories): auras / periodics / stat-mods (#346 — the
// kAura effect is recognised and SKIPPED here), threat (CMB-04 — kThreat
// skipped), the 20 Hz tick loop that drives cast completion (#349), and mob AI.

#ifndef MERIDIAN_WORLDD_COMBAT_RESOLVER_H
#define MERIDIAN_WORLDD_COMBAT_RESOLVER_H

#include <cstdint>
#include <functional>
#include <optional>
#include <random>

#include "ability_store.h"        // Ability / AbilityEffect / EffectKind / TargetKind
#include "combat_unit.h"          // Unit / Faction / DamageResult
#include "movement_validation.h"  // Position

namespace meridian::worldd {

// ---------------------------------------------------------------------------
// Clean-room combat constants (ORIGINAL — see file header). Basis points: 1 bp =
// 0.01 %, so a d10000 roll compares directly against these.
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kAttackRollScale = 10000;  // d10000 roll domain

// The attack table for a DAMAGE ability, as a single d10000 roll partitioned into
// contiguous bands (SAD §2.5 "attack tables"). Placeholder first values; M2
// tuning replaces them with stat-derived chances (defense skill, crit rating, …).
inline constexpr std::uint32_t kBaseMissBp  = 500;   // [0,500)      = 5 %  MISS
inline constexpr std::uint32_t kBaseDodgeBp = 500;   // [500,1000)   = 5 %  DODGE
inline constexpr std::uint32_t kBaseParryBp = 500;   // [1000,1500)  = 5 %  PARRY
inline constexpr std::uint32_t kBaseCritBp  = 1000;  // [1500,2500)  = 10 % CRIT
//                                                      [2500,10000) = 75 % HIT

// A HEAL cannot be avoided (no miss/dodge/parry) — only crit vs normal.
inline constexpr std::uint32_t kHealCritBp = 1000;   // [0,1000) = 10 % CRIT, else HIT

// A crit deals / heals this multiple of the rolled amount.
inline constexpr float kCritMultiplier = 2.0f;

// The global cooldown a GCD-triggering ability starts (SAD §3.3 "GCD clock"). One
// clean-room value; a per-category GCD table is a later concern (ability_store.h
// keeps the triggers_gcd boolean as that seam).
inline constexpr std::uint64_t kGlobalCooldownMs = 1500;

// How much one pushback event delays an in-progress cast (SAD §2.5 "pushback").
inline constexpr std::uint32_t kCastPushbackMs = 500;

// ---------------------------------------------------------------------------
// Attack-table outcome (mirrors world.fbs AttackOutcome; kept as its own enum so
// the resolver core has no FlatBuffers dependency).
// ---------------------------------------------------------------------------
enum class AttackOutcome : std::uint8_t {
    kMiss = 0,
    kDodge = 1,
    kParry = 2,
    kHit = 3,
    kCrit = 4,
};

// Why an ability use was refused (mirrors world.fbs CastFailReason). kNone means
// the use is legal. The dispatch handler maps this to the wire reason.
enum class CastReject : std::uint8_t {
    kNone = 0,
    kUnknownAbility,
    kNotInWorld,
    kCasterDead,
    kOnGcd,
    kAlreadyCasting,
    kInsufficientResource,
    kNoTarget,
    kTargetDead,
    kWrongFaction,
    kOutOfRange,
    kNoLineOfSight,
    kInterrupted,
    kCasterStunned,   // a `cc` stun on the caster blocks all ability use (#693)
    kCasterSilenced,  // a `cc` silence on the caster blocks casting (#693)
};

// ---------------------------------------------------------------------------
// CombatRng — deterministic, seeded RNG for all combat rolls (SAD §2.5 "seeded-
// RNG unit-testable"). Backed by std::mt19937_64 (whose output sequence is
// standard-mandated deterministic given a seed). Rolls are derived by plain
// modulo of the engine output — NOT std distributions — so a given seed yields
// the SAME roll sequence on every platform / stdlib (the tests depend on this).
// ---------------------------------------------------------------------------
class CombatRng {
public:
    explicit CombatRng(std::uint64_t seed) : engine_(seed) {}

    // A uniform roll in [0, kAttackRollScale) — the d10000 attack-table roll.
    std::uint32_t roll_bp() {
        return static_cast<std::uint32_t>(engine_() % kAttackRollScale);
    }

    // A uniform amount in [lo, hi] inclusive (the damage/heal amount range). lo is
    // returned when hi <= lo (a fixed amount).
    std::uint32_t roll_amount(std::uint32_t lo, std::uint32_t hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<std::uint32_t>(engine_() % (static_cast<std::uint64_t>(hi - lo) + 1));
    }

    // Reseed (e.g. per-map at boot). Resets the sequence deterministically.
    void reseed(std::uint64_t seed) { engine_.seed(seed); }

private:
    std::mt19937_64 engine_;
};

// ---------------------------------------------------------------------------
// Faction relations — the resolver's call (the #342 Unit header defers "can A
// attack B" to here). Clean-room placeholder model over the M0-frozen factions.
// ---------------------------------------------------------------------------

// True if `attacker` may deal harm to `target` (an ENEMY-target ability). Players
// and hostile creatures are mutually hostile; neutral is attackable by no one.
bool can_attack(Faction attacker, Faction target);

// True if `caster` may help `target` (a FRIENDLY-target ability, e.g. a heal).
// Player/friendly aid each other; hostiles aid hostiles; neutral aids no one.
bool can_assist(Faction caster, Faction target);

// ---------------------------------------------------------------------------
// Line-of-sight seam. On the D-19 flat bootstrap map there are no occluders, so
// the default (flat_map_los) is always clear. A caller/test injects an occluder
// predicate to exercise the NO_LINE_OF_SIGHT reject without faking terrain; the
// real navmesh/geometry LoS lands with the content pipeline.
// ---------------------------------------------------------------------------
using LineOfSightFn = std::function<bool(const Position&, const Position&)>;
bool flat_map_los(const Position& from, const Position& to);

// Approved #784 armor contract. Physical damage is reduced by
// floor(raw * 100 / (100 + max(0, effective armor))), with a one-damage floor
// for a non-zero hit. Non-physical schools bypass armor. Armor mitigation is
// computed before Unit::apply_damage, so shields absorb the mitigated result.
std::uint32_t mitigate_damage(std::uint32_t raw_damage, School school,
                              std::int64_t effective_armor);

// ---------------------------------------------------------------------------
// Pure attack-table classifiers — map a d10000 roll to an outcome. Split out so
// each band is unit-testable at its boundary WITHOUT guessing RNG output.
// ---------------------------------------------------------------------------
AttackOutcome classify_attack(std::uint32_t roll_bp);  // damage table
AttackOutcome classify_heal(std::uint32_t roll_bp);    // heal table (no avoidance)

// Whether an ability is a HEAL (has a direct kHeal effect and no kDamage effect).
// Determines which attack table it rolls and which Unit call resolution makes.
bool is_heal_ability(const Ability& ability);

// ---------------------------------------------------------------------------
// #345 — resolution.
// ---------------------------------------------------------------------------

// The result of resolving one ability use against a target. Beyond the direct
// damage/heal (the #345 originals), it reports the SP2.3 #693 instantaneous
// primitives the resolver executes on the Units directly — `resource` (grant/drain)
// and `movement` (forced displacement) — so the map tick can log + relay them.
struct ResolveResult {
    AttackOutcome outcome = AttackOutcome::kHit;
    std::uint32_t amount = 0;       // total damage or heal actually applied
    std::uint32_t absorbed = 0;     // damage soaked by a shield (not removed from HP, #693)
    bool is_heal = false;           // true = healed, false = damaged
    bool target_died = false;       // this resolution drove the target to 0 HP
    std::uint32_t target_health = 0;  // target health AFTER application

    // `resource` primitive (#693): net resource granted / drained on the target.
    std::uint32_t resource_granted = 0;
    std::uint32_t resource_drained = 0;

    // `movement` primitive (#693): a forced displacement was applied this resolution.
    bool          moved = false;
    ObjectGuid    moved_guid = 0;   // whose position changed (caster for dash, else target)
    Position      moved_to;         // its new position after the displacement
};

// One server-authored hostile basic attack. It uses the same attack table and
// seeded CombatRng as abilities, is always physical, applies armor before shields,
// and mutates only the authoritative target Unit.
struct BasicAttackResult {
    AttackOutcome outcome = AttackOutcome::kMiss;
    std::uint32_t raw_amount = 0;
    std::uint32_t amount = 0;
    std::uint32_t absorbed = 0;
    std::uint32_t target_health = 0;
    bool target_died = false;
};

BasicAttackResult resolve_basic_attack(Unit& attacker, Unit& target,
                                       std::uint32_t damage_min,
                                       std::uint32_t damage_max, CombatRng& rng);

// Validate target legality + range + line-of-sight for `ability` cast by `caster`
// at `target` (SAD §3.3). `target` may be null (no such entity). Returns kNone
// when the target is legal; otherwise the specific reject. A SELF ability ignores
// `target` (always legal for a live caster). Callers check caster-liveness / GCD /
// resource separately (begin_ability_use bundles all of them).
CastReject validate_target(const Ability& ability, const Unit& caster,
                           const Unit* target, const LineOfSightFn& los);

// Roll the attack table for `ability` (heal table if is_heal_ability, else the
// damage table) using `rng`. Pure w.r.t. Units — just the roll.
AttackOutcome roll_attack(const Ability& ability, CombatRng& rng);

// Apply an ability's DIRECT + instantaneous effects to `target` given a rolled
// `outcome`, using `rng` for amount rolls. Damage effects are avoided on
// miss/dodge/parry (0 applied); crit doubles the amount; a shield on the target
// absorbs first. Heal effects are never avoided. The SP2.3 #693 instantaneous
// primitives ALWAYS apply (they carry no attack-table roll, like auras): `resource`
// grants/drains the target's pool, `movement` displaces caster/target. Triggers the
// target's death transition when damage reaches 0 HP. Timed kinds (kAura/kDot/kHot/
// kBuff/kDebuff/kShield/kCc — the AuraContainer's job), kThreat (CMB-04), and
// kSummon (the map tick's job) are SKIPPED here. Assumes validate_target passed.
ResolveResult apply_outcome(const Ability& ability, Unit& caster, Unit& target,
                            AttackOutcome outcome, CombatRng& rng);

// resolve_ability = roll_attack then apply_outcome (the full #345 path). `caster`
// and `target` may alias (a self-target heal). Returns the applied outcome.
ResolveResult resolve_ability(const Ability& ability, Unit& caster, Unit& target,
                              CombatRng& rng);

// ---------------------------------------------------------------------------
// #344 — GCD + cast lifecycle (per-session, single-threaded map-owned state).
// ---------------------------------------------------------------------------

// An in-progress cast (a cast-time ability between CastStart and CastResult).
struct PendingCast {
    AbilityId ability_id = 0;
    ObjectGuid target_guid = 0;     // 0 = self
    std::uint64_t cast_end_ms = 0;  // absolute ms when the cast completes
    std::uint32_t cast_time_ms = 0;  // original cast time (for logs / pushback caps)
};

// Per-session combat clock: the GCD end time + the active cast. Owned by the map
// tick / the connection's IO worker (like SessionMovementState); no lock.
class CombatSession {
public:
    // --- GCD ---------------------------------------------------------------
    bool on_gcd(std::uint64_t now_ms) const { return now_ms < gcd_end_ms_; }
    std::uint32_t gcd_remaining_ms(std::uint64_t now_ms) const {
        return on_gcd(now_ms) ? static_cast<std::uint32_t>(gcd_end_ms_ - now_ms) : 0;
    }
    // Start a fresh global cooldown ending kGlobalCooldownMs from now.
    void trigger_gcd(std::uint64_t now_ms) { gcd_end_ms_ = now_ms + kGlobalCooldownMs; }

    // --- cast timer --------------------------------------------------------
    // A cast is IN PROGRESS if one is recorded and has not yet reached its end.
    bool is_casting(std::uint64_t now_ms) const {
        return cast_.has_value() && now_ms < cast_->cast_end_ms;
    }
    void begin_cast(const PendingCast& c) { cast_ = c; }
    // Cancel the active cast (interrupt — e.g. movement, SAD §2.5). No-op if idle.
    void interrupt() { cast_.reset(); }
    // Delay the active cast's completion by `ms` (pushback). No-op if idle.
    void apply_pushback(std::uint32_t ms) {
        if (cast_) cast_->cast_end_ms += ms;
    }
    // If a cast has reached its end time, remove and return it (the tick resolves
    // it into a CastResult). Returns nullopt while still casting or when idle.
    std::optional<PendingCast> take_completed(std::uint64_t now_ms) {
        if (cast_ && now_ms >= cast_->cast_end_ms) {
            PendingCast done = *cast_;
            cast_.reset();
            return done;
        }
        return std::nullopt;
    }
    const std::optional<PendingCast>& pending() const { return cast_; }

private:
    std::uint64_t gcd_end_ms_ = 0;
    std::optional<PendingCast> cast_;
};

// The accept/reject decision for one CastRequest (#344, D-10). On accept the
// caller sends CastStart{cast_ms}; on reject CastFailed{reason, gcd_remaining_ms}.
struct CastDecision {
    bool accepted = false;
    CastReject reject = CastReject::kNone;
    std::uint32_t gcd_remaining_ms = 0;  // for the reject reply (client resync)
    std::uint32_t cast_ms = 0;           // for the accept reply (0 = instant)
    bool instant = false;                // resolve immediately (cast_time_ms == 0)
};

// Decide whether `caster` may use `ability` on `target` right now, and — on
// accept — START the lifecycle in `combat`: trigger the GCD (if the ability
// triggers it) and, for a cast-time ability, record the PendingCast. The
// validation order follows SAD §3.3: caster alive, GCD clock, in-progress cast,
// resource, then target legality/range/LoS. `target_guid` is stored on the
// PendingCast so cast completion knows whom to resolve against. This mutates
// `combat` ONLY on accept (no partial state on a reject — the rollback stays
// clean). Resource is spent at RESOLUTION, not here.
//
// `caster_stunned` / `caster_silenced` are the caster's current crowd-control state
// (SP2.3 #693), read by the map tick from the caster's AuraContainer and passed in
// (the resolver core has no container dependency). A stun blocks ALL ability use; a
// silence blocks casting. Both default false — an un-CC'd caster is unaffected, so
// the existing call path is unchanged.
CastDecision begin_ability_use(CombatSession& combat, const Ability& ability,
                               Unit& caster, const Unit* target,
                               ObjectGuid target_guid, const LineOfSightFn& los,
                               std::uint64_t now_ms, bool caster_stunned = false,
                               bool caster_silenced = false);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_COMBAT_RESOLVER_H
