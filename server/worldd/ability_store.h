// SPDX-License-Identifier: Apache-2.0
//
// worldd — compiled ability / effect data model + read-only lookup store
// (CMB-01 foundation; issue #343, epic #18). The combat resolver (#344/#345/#346)
// reads abilities out of this store on the tick path.
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md + docs/sad/client-sad.md and
// the world DDL only. §2.5 ("Combat resolver — CMB-01: target/range/LoS
// validation, GCD enforcement, cast timers …, damage/heal application, aura
// container (periodics, stat mods, stacking)"), §3.3 (the CastRequest validate
// list: known ability, GCD clock, resource, range, LoS, target legality), client
// SAD §2.4 (the `datastore` pattern — "compiled content tables … read-only after
// mount; keyed by numeric ID … O(1) lookup on the combat path … unresolvable IDs
// return typed placeholders + telemetry — never crash"), server SAD §4.3 (the
// world DB is a read-only mcc artifact loaded into "immutable in-memory template
// stores"), and the IF-4 world DDL `schema/sql/world/30_ability.sql` +
// `schema/content/ability.schema.yaml` (the exact `ability` / `ability_effect` /
// `ability_effect_stat_mod` column set this model mirrors). No GPL / emulator
// source (CMaNGOS / TrinityCore / leaked) consulted — never any existing spell or
// DBC layout. See CONTRIBUTING.md.
//
// WHAT THIS FILE IS: the PURE, DB-free, socket-free ability template store. It is
// a plain in-memory structure — a numeric-id → Ability map — read-only after load
// so the resolver can look an ability up on the combat path in O(1) with no lock
// (single-threaded map ownership, SAD §2.5/§6). It has NO dependency on
// FlatBuffers, the DB, or a socket, so the model + lookup + the placeholder set
// run in the plain `server` ctest with no MariaDB (mirrors aoi_grid / world_boot's
// pure cores).
//
// ─── THE LOAD SEAM (M1 scope, issue #343) ────────────────────────────────────
// The store is populated from a std::vector<Ability> — the "compiled" rows. Two
// producers plug into that one seam:
//   • M1 (this story): `placeholder_ability_set()` returns a small hardcoded set
//     (a melee strike, a nuke, a heal, a DoT) so the resolver stories have data to
//     run against with no content pipeline.
//   • Epic #28 (mcc v1) — NOT built here: a `mcc emit-sql`-filled `ability` /
//     `ability_effect` world-DB table read into the SAME std::vector<Ability> via
//     the SAME AbilityStore::from_abilities() build. The DB read is the only new
//     code that story adds; the model + lookup below are unchanged. That is the
//     seam this file deliberately leaves open (SAD §4.3 IF-4 contract). The field
//     names + enum sets below mirror the DDL 1:1 so the future row→Ability map is
//     a straight column copy.
//
// The model mirrors the world DDL's flattening (schema/sql/world/README.md
// "Column-mapping rules"): nested `cast.*`/`resource.*` objects flatten to
// prefixed scalars; the `intRange` `amount` becomes amount_min/amount_max; the
// `effects[]` oneOf array becomes AbilityEffect with a `kind` discriminator; the
// aura `stat_mods[]` child array becomes a nested vector.

#ifndef MERIDIAN_WORLDD_ABILITY_STORE_H
#define MERIDIAN_WORLDD_ABILITY_STORE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace meridian::worldd {

// A numeric ability id — the wire key (CastRequest.ability_id, world.fbs) and the
// `ability.id INT UNSIGNED` PK assigned by IF-9 (schema/sql/world/README.md "Key
// scheme"). uint32 to match the DDL column width exactly.
using AbilityId = std::uint32_t;

// Damage/heal/aura school — mirrors `ability.school`
// ENUM('physical','fire','frost','nature','shadow','holy','arcane'). School is an
// ability-level property in the DDL (not per-effect), so it lives on Ability.
enum class School : std::uint8_t {
    kPhysical,
    kFire,
    kFrost,
    kNature,
    kShadow,
    kHoly,
    kArcane,
};

// Who an ability may be cast at — mirrors `ability.target`
// ENUM('self','enemy','friendly'). The resolver's "target legality" check (§3.3).
enum class TargetKind : std::uint8_t {
    kSelf,
    kEnemy,
    kFriendly,
};

// The resource an ability spends — mirrors `ability.resource_type`
// ENUM('mana','rage','energy') NULL, with kNone modelling the SQL NULL (a free
// ability spends nothing). The resolver's "resource" check (§3.3).
enum class AbilityResourceType : std::uint8_t {
    kNone,  // DDL NULL — no resource cost
    kMana,
    kRage,
    kEnergy,
};

// Effect discriminator — the Tier-1 effect-primitive palette
// (`ability.schema.yaml` effects[] oneOf `kind`). The FIRST FOUR
// (damage/heal/aura/threat) are the SP1 originals the retired per-kind
// `ability_effect` DDL carried; the rest are the SP2.2 full-palette extension
// (#692) that db_content_store now materializes out of the generic
// `ability.effects_json` payload (SP2.1 #691 moved the effect recipe into that one
// column). The combat EXECUTION of the new kinds is a separate story (#693); this
// model is LOAD/representation only — a resolver switches on `kind`.
enum class EffectKind : std::uint8_t {
    // SP1 originals.
    kDamage,
    kHeal,
    kAura,
    kThreat,
    // SP2.2 full palette (#692) — deserialized from effects_json.
    kDot,        // periodic damage: `amount` per `tick_ms` over `duration_ms`
    kHot,        // periodic heal (same shape as kDot)
    kBuff,       // attribute modifier (typically +) for `duration_ms`
    kDebuff,     // attribute modifier (typically −) for `duration_ms`
    kShield,     // absorb pool (`amount`) for `duration_ms`
    kCc,         // crowd control (stun/root/silence) for `duration_ms`
    kResource,   // grant/drain a resource pool
    kMovement,   // forced movement (knockback/pull/dash)
    kSummon,     // summon an NPC
};

// buff/debuff application mode — mirrors effects[].modifier ENUM('flat','percent').
// flat = raw attribute units; percent = hundredths of a percent-point.
enum class AttributeModifier : std::uint8_t {
    kFlat,     // schema default
    kPercent,
};

// cc.type — the crowd-control category applied for `duration_ms`
// (effects[].type for kind:cc, ENUM('stun','root','silence')).
enum class CrowdControlKind : std::uint8_t {
    kStun,
    kRoot,
    kSilence,
};

// resource.pool — which resource a kResource effect grants/drains
// (effects[].pool ENUM('mana','rage','energy')). Distinct from AbilityResourceType
// (which models a NULLable ability COST); a resource effect's pool is never NULL.
enum class ResourcePool : std::uint8_t {
    kMana,
    kRage,
    kEnergy,
};

// resource.operation — grant restores the pool, drain removes from it
// (effects[].operation ENUM('grant','drain')).
enum class ResourceOp : std::uint8_t {
    kGrant,
    kDrain,
};

// movement.motion — forced movement relative to caster/target
// (effects[].motion ENUM('knockback','pull','dash')).
enum class MovementMotion : std::uint8_t {
    kKnockback,  // push the target away
    kPull,       // drag the target toward the caster
    kDash,       // move the caster
};

// Periodic sub-kind of an aura effect — mirrors `ability_effect.periodic_kind`
// ENUM('damage','heal') NULL. kNone models the SQL NULL (an aura with no periodic
// tick, e.g. a pure stat-mod buff).
enum class PeriodicKind : std::uint8_t {
    kNone,  // DDL NULL — aura has no periodic tick
    kDamage,
    kHeal,
};

// A character stat an aura can modify — mirrors `ability_effect_stat_mod.stat`
// ENUM('strength','agility','stamina','intellect','spirit').
enum class StatKey : std::uint8_t {
    kStrength,
    kAgility,
    kStamina,
    kIntellect,
    kSpirit,
};

// One aura stat modifier — mirrors an `ability_effect_stat_mod` row (the aura
// `stat_mods[]` child array). `amount` is signed (buffs +, debuffs -).
struct StatMod {
    StatKey      stat = StatKey::kStrength;
    std::int32_t amount = 0;
};

// One compiled effect of an ability — one entry of the `effects[]` oneOf array
// (1..6 per ability). Deserialized ONCE at load from the generic `effects_json`
// payload (db_content_store; SP2.1 #691 + SP2.2 #692) into this compact tagged
// union — the combat path never re-parses JSON. Only the fields relevant to `kind`
// are meaningful; the rest keep their zero defaults. A resolver switches on `kind`.
struct AbilityEffect {
    EffectKind kind = EffectKind::kDamage;

    // kDamage / kHeal / kDot / kHot / kShield: the `amount` intRange
    // (amount_min..amount_max, inclusive) and the caster power `coefficient`
    // (fraction of spell/attack power added, 0..2; server-side formula, M2 tuning —
    // ability.schema.yaml). For kDot/kHot the amount is PER TICK.
    std::uint32_t amount_min = 0;
    std::uint32_t amount_max = 0;
    float         coefficient = 0.0f;

    // kThreat: flat threat added (signed — taunts/detaunts special-cased later).
    std::int32_t threat_amount = 0;

    // kAura / kDot / kHot / kBuff / kDebuff / kShield / kCc / kSummon: how long it
    // lasts. kAura / kDot / kHot / kBuff / kDebuff / kShield also stack up to
    // max_stacks. (For kSummon, duration_ms 0 = permanent/until-dismissed.)
    std::uint32_t duration_ms = 0;
    std::uint16_t max_stacks = 1;

    // kAura: an optional periodic tick (kNone = a pure stat-mod aura). NOTE kDot/kHot
    // are DISTINCT kinds carrying their own per-tick `amount` (above) + `tick_ms`
    // (below); the aura periodic_* fields belong to kAura alone.
    PeriodicKind  periodic_kind = PeriodicKind::kNone;
    std::uint32_t periodic_amount_min = 0;
    std::uint32_t periodic_amount_max = 0;
    std::uint32_t periodic_tick_ms = 0;
    std::vector<StatMod> stat_mods;  // aura stat modifiers (may be empty)

    // ─── SP2.2 full palette (#692) — deserialized from effects_json ──────────

    // kDot / kHot: interval between ticks (the per-tick amount is amount_min/max,
    // duration_ms bounds the total, coefficient + max_stacks also apply).
    std::uint32_t tick_ms = 0;

    // kBuff / kDebuff: modify one `attribute` by a signed `attribute_amount` for
    // `duration_ms` (+ max_stacks). `attribute` is the contentId ref carried
    // VERBATIM from effects_json (an unresolved `<ns>:attribute.<name>` — the combat
    // resolver #693 resolves it against the attribute catalog).
    std::string       attribute;
    std::int32_t      attribute_amount = 0;
    AttributeModifier attribute_modifier = AttributeModifier::kFlat;

    // kCc: crowd-control category applied for `duration_ms`.
    CrowdControlKind cc_kind = CrowdControlKind::kStun;

    // kResource: grant/drain `resource_amount` of `resource_pool`.
    ResourcePool  resource_pool = ResourcePool::kMana;
    ResourceOp    resource_op = ResourceOp::kGrant;
    std::uint32_t resource_amount = 0;

    // kMovement: forced `movement_motion` of `distance_m` metres.
    MovementMotion movement_motion = MovementMotion::kKnockback;
    float          distance_m = 0.0f;

    // kSummon: summon `summon_count` of the NPC `summon_npc` (a contentId ref carried
    // verbatim from effects_json; resolved by #693), for `duration_ms` (0 = permanent).
    std::string   summon_npc;
    std::uint16_t summon_count = 1;
};

// One compiled ability — mirrors an `ability` row + its `ability_effect` children.
// Read-only once inside an AbilityStore. Field set is the resolver's §3.3 validate
// inputs (known ability, GCD, resource, range, target) + the effect list it
// applies.
struct Ability {
    AbilityId    id = 0;
    std::string  name;  // displayName (tooling/log readability; not on the hot path)
    School       school = School::kPhysical;
    TargetKind   target = TargetKind::kEnemy;
    float        range_m = 5.0f;  // max cast range; melee = 5 (ability.range_m)

    // cast.* — cast_time_ms 0 = instant; cast_channel_ms 0 = not channeled.
    std::uint32_t cast_time_ms = 0;
    std::uint32_t cast_channel_ms = 0;

    std::uint32_t cooldown_ms = 0;
    // triggers_gcd — issue #343's "gcd_category" in M1 terms: whether the ability
    // is on the global cooldown. The DDL models the GCD as this boolean at v1 (no
    // multi-category GCD table yet); kept as the seam for a future category enum.
    bool triggers_gcd = true;

    // resource.* — kNone + 0 models the SQL NULL (a free ability).
    AbilityResourceType  resource_type = AbilityResourceType::kNone;
    std::uint32_t resource_amount = 0;

    std::vector<AbilityEffect> effects;  // 1..4, in DDL ordinal order
};

// ---------------------------------------------------------------------------
// AbilityStore — the read-only, O(1) numeric-id → Ability template store.
// ---------------------------------------------------------------------------
//
// Owned by the world thread (SAD §2.5/§6: game state is single-threaded), loaded
// once at boot and never mutated after — so `find()` on the combat path needs no
// lock. Backed by an unordered_map for O(1) average lookup by numeric id (client
// SAD §2.4 "O(1) lookup on the combat path").
class AbilityStore {
public:
    AbilityStore() = default;

    // Build a store from compiled ability rows (the load seam — the placeholder
    // set at M1, the mcc/world-DB rows at epic #28). Later duplicate ids are a
    // content fault: `duplicate_id_out` (when non-null) receives the FIRST
    // offending id and that later row is dropped (first-wins) rather than throwing,
    // so a bad content pack degrades rather than crashes worldd's boot. Pass
    // nullptr to ignore the diagnostic.
    static AbilityStore from_abilities(const std::vector<Ability>& abilities,
                                       AbilityId* duplicate_id_out = nullptr);

    // O(1) lookup by numeric id. Returns nullptr when the id is unknown — the
    // caller (resolver) treats a miss as the client SAD §2.4 "typed placeholder /
    // silence + telemetry, never crash" case. The pointer is valid for the store's
    // lifetime (the store is read-only, so it never invalidates).
    const Ability* find(AbilityId id) const;

    // Whether `id` resolves to a loaded ability.
    bool contains(AbilityId id) const;

    // How many abilities are loaded.
    std::size_t size() const { return by_id_.size(); }

    // Whether the store holds no abilities.
    bool empty() const { return by_id_.empty(); }

private:
    std::unordered_map<AbilityId, Ability> by_id_;
};

// ---------------------------------------------------------------------------
// Placeholder content (M1 seam — issue #343). REPLACED by mcc/world-DB content at
// epic #28; do NOT author real content here.
// ---------------------------------------------------------------------------

// Base of a dedicated, obviously-synthetic id band for the M1 placeholder set.
// Real content ids are IF-9-assigned per-pack bands from idmap.lock (SAD §4.6);
// this high reserved band (0xF000_0000) cannot collide with a real pack band, so
// the placeholders never shadow compiled content and are trivial to grep out when
// epic #28 lands. NOT an IF-9 allocation — a dev-only stand-in.
inline constexpr AbilityId kPlaceholderIdBand = 0xF0000000u;

// Numeric ids of the four placeholder abilities (band base + 1..4).
inline constexpr AbilityId kPlaceholderMeleeStrikeId = kPlaceholderIdBand + 1;  // instant melee physical hit
inline constexpr AbilityId kPlaceholderNukeId        = kPlaceholderIdBand + 2;  // cast-time ranged fire nuke
inline constexpr AbilityId kPlaceholderHealId        = kPlaceholderIdBand + 3;  // cast-time friendly holy heal
inline constexpr AbilityId kPlaceholderDotId         = kPlaceholderIdBand + 4;  // instant shadow damage-over-time

// The M1 placeholder ability set: a melee strike, a nuke, a heal, and a DoT — one
// of each shape the resolver stories (#344/#345/#346) exercise (instant vs cast,
// melee vs ranged, direct damage vs heal vs periodic aura, free vs resource-cost).
// Returned as compiled rows; feed to AbilityStore::from_abilities().
std::vector<Ability> placeholder_ability_set();

// Convenience: a store pre-loaded with placeholder_ability_set() (the M1 boot path
// until epic #28's DB loader replaces the source behind AbilityStore).
AbilityStore load_placeholder_ability_store();

// Resolve a buff/debuff `attribute` contentId ref (e.g. "core:attribute.strength",
// carried VERBATIM out of effects_json by SP2.2 #692) to a primary-stat StatKey.
// Returns true + sets `out` when the ref's trailing token names one of the five
// primary attributes (strength/agility/stamina/intellect/spirit); false for a
// derived attribute (armor/crit/haste/…) or a malformed ref. This is the SP2.3
// #693 resolution of the deferred `attribute` ref: a primary-stat modifier folds
// into the existing StatKey delta layer; the rest is carried on the interim
// attribute ledger for the #694 effective-stat framework (see aura_container.h).
bool primary_attribute_stat(const std::string& attribute_ref, StatKey& out);

// Human-readable enum names (logs / tooling / test diagnostics; not the hot path).
const char* school_name(School s);
const char* target_kind_name(TargetKind t);
const char* resource_type_name(AbilityResourceType r);
const char* effect_kind_name(EffectKind k);
const char* attribute_modifier_name(AttributeModifier m);
const char* crowd_control_kind_name(CrowdControlKind c);
const char* resource_pool_name(ResourcePool p);
const char* resource_op_name(ResourceOp o);
const char* movement_motion_name(MovementMotion m);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_ABILITY_STORE_H
