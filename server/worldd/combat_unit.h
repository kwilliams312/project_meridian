// SPDX-License-Identifier: Apache-2.0
//
// worldd — Combat Unit model (issue #342, CMB-01 foundation; part of epic #18).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md only — §2.5 ("Entity / aggro /
// threat — shallow class hierarchy (WorldObject → Unit → {Player, Creature},
// GameObject, Corpse), NOT a generic ECS") and §9 (the "Entity model" decision:
// "Shallow class hierarchy + component-style containers … Full ECS rejected …
// our maps are deliberately single-threaded and gameplay is branchy and
// content-driven"). No GPL / AGPL / CMaNGOS / TrinityCore / leaked emulator
// source consulted — the numbers and shape here are ORIGINAL, derived from OUR
// SAD and the existing worldd code (roster.h classes, movement Position). See
// CONTRIBUTING.md.
//
// WHAT THIS FILE IS: the data model + lifecycle for everything that lives on a
// map, as the shallow hierarchy the SAD mandates:
//
//     WorldObject                      // guid + leaf type + position
//       └── Unit                       // + health / resource / level / faction /
//             ├── Player               //   alive-dead — the core COMBAT STATE
//             └── Creature
//       GameObject   (stub)            // non-Unit world entities (doors, chests…)
//       Corpse       (stub)            // a dead Unit's lootable remains
//
// SCOPE (issue #342): the model + lifecycle ONLY — spawn, damage → health, and
// the death transition (health hits 0 → LifeState::kDead). Stats come from a
// clean-room PLACEHOLDER (like the D-11 placeholder character): see
// placeholder_player_stats / placeholder_creature_stats.
//
// OUT OF SCOPE (separate stories #344-#348, which BUILD ON this): the combat
// resolver (target/range/LoS, attack tables, GCD), auras, and mob AI (threat,
// aggro, leash). This file deliberately holds NO targeting, NO rolls, NO AI — a
// Unit only knows how to take damage / healing and to die. Faction is stored as
// data; "can A attack B" is the resolver's call, not ours.
//
// THREADING (SAD §2.5 / §6): a map is single-threaded by construction — "the
// tick owns entity state". A Unit therefore carries NO lock of its own; the map
// tick (or, at M0, WorldState's mutex — see world_state.h) serializes all access.

#ifndef MERIDIAN_WORLDD_COMBAT_UNIT_H
#define MERIDIAN_WORLDD_COMBAT_UNIT_H

#include <cstdint>
#include <string>

#include "movement_validation.h"  // Position

namespace meridian::worldd {

// The stable world id of a WorldObject — the same 64-bit id the AoI grid and the
// wire (world.fbs EntityEnter.entity_guid) use (aoi_grid.h AoiId). Kept as a
// plain alias here so the model has no dependency on the grid header.
using ObjectGuid = std::uint64_t;

// The concrete leaf kind of a WorldObject (the shallow hierarchy of SAD §2.5).
// Lets code branch on kind without a dynamic_cast; every WorldObject stamps it.
enum class ObjectType : std::uint8_t {
    kUnit = 0,        // an abstract Unit never instantiated directly
    kPlayer = 1,      // a player-controlled Unit
    kCreature = 2,    // a server-controlled Unit (NPC / mob)
    kGameObject = 3,  // a non-Unit interactable (door, chest, node) — stub
    kCorpse = 4,      // the lootable remains of a dead Unit — stub
};

// Alive / dead — the death transition #342 is scoped to. A Unit starts kAlive on
// spawn; apply_damage that drops health to 0 transitions it to kDead. The combat
// resolver / respawn logic (out of scope) drives what happens next.
enum class LifeState : std::uint8_t {
    kAlive = 0,
    kDead = 1,
};

// The secondary power a Unit spends on abilities. M0 placeholder set — the combat
// resolver (#344+) decides costs/regen; here it is just a typed pool. kNone means
// the Unit has no secondary resource (e.g. a basic melee creature).
enum class ResourceType : std::uint8_t {
    kNone = 0,
    kMana = 1,    // caster pool (Runcaller / Mender)
    kEnergy = 2,  // fast-regen pool (Warden)
    kRage = 3,    // combat-generated pool (Vanguard)
};

// Coarse allegiance. Stored as data ONLY at #342 — the resolver's target
// validation (#344+) is what turns faction into "hostile to". Clean-room
// placeholder set; the real faction/reputation model is later content.
enum class Faction : std::uint16_t {
    kNeutral = 0,   // attackable by no one by default
    kPlayer = 1,    // player-aligned
    kHostile = 2,   // hostile to players (mobs)
    kFriendly = 3,  // friendly NPCs
};

// ---------------------------------------------------------------------------
// WorldObject — the base of the shallow hierarchy (SAD §2.5).
// ---------------------------------------------------------------------------
//
// The minimum every on-map entity has: a stable guid, its leaf type, and a
// position. Polymorphic (virtual dtor) so a map can hold heterogeneous entities
// behind a WorldObject* / Unit*; leaf types are stored BY VALUE by their owners
// (WorldState stores a Player), so there is no slicing.
class WorldObject {
public:
    WorldObject(ObjectGuid guid, ObjectType type, const Position& pos)
        : guid_(guid), type_(type), pos_(pos) {}
    virtual ~WorldObject() = default;

    ObjectGuid guid() const { return guid_; }
    ObjectType type() const { return type_; }

    const Position& position() const { return pos_; }
    void set_position(const Position& pos) { pos_ = pos; }

protected:
    ObjectGuid guid_ = 0;
    ObjectType type_ = ObjectType::kUnit;
    Position pos_;
};

// The initial stats a Unit spawns with. A plain struct so a placeholder provider
// (or, later, a compiled spawn table) can hand a Unit its numbers without the
// Unit knowing where they came from — the D-11 placeholder pattern.
struct UnitStats {
    std::uint16_t level = 1;
    std::uint32_t max_health = 1;  // clamped to >= 1 by Unit (a live Unit has HP)
    // Effective physical armor at spawn. Authored NPC armor enters here; player
    // equipment/stat aggregation may replace it through set_effective_armor().
    // Signed so future debuffs can reduce it below zero; the mitigation contract
    // clamps negative effective armor to zero when damage is resolved.
    std::int64_t armor = 0;
    ResourceType resource_type = ResourceType::kNone;
    std::uint32_t max_resource = 0;
    Faction faction = Faction::kNeutral;
};

// The result of applying damage to a Unit: how much HP was actually removed
// (clamped to remaining health) and whether THIS damage killed it (drove health
// to 0). `lethal` fires exactly once — the call that crosses the threshold.
// `absorbed` is how much of the incoming amount a `shield` effect (SP2.3 #693)
// soaked BEFORE it reached health — mitigation, not HP loss. `absorbed` is kept
// as a trailing field (zero-default) so the existing brace-inits stay valid.
struct DamageResult {
    std::uint32_t applied = 0;
    bool lethal = false;
    std::uint32_t absorbed = 0;
};

// ---------------------------------------------------------------------------
// Unit — a WorldObject with combat state (SAD §2.5 core: health / resource /
// level / faction / alive-dead / position).
// ---------------------------------------------------------------------------
//
// The lifecycle #342 owns:
//   • spawn()          — (re)enter the world at full health, alive.
//   • apply_damage()   — reduce health; on reaching 0, transition to kDead.
//   • apply_healing()  — restore health up to max (a DEAD unit must be
//                        resurrect()'d first — you cannot heal a corpse).
//   • kill()           — force the death transition (e.g. a fall / instakill).
//   • resurrect()      — bring a dead Unit back with some health.
// Resource spend/restore is here too so an ability cost has a home, but no cost
// is CHARGED anywhere yet (resolver = #344+).
class Unit : public WorldObject {
public:
    Unit(ObjectGuid guid, ObjectType type, const Position& pos, const UnitStats& stats);

    std::uint16_t level() const { return level_; }
    void set_level(std::uint16_t level) { level_ = level; }

    Faction faction() const { return faction_; }
    void set_faction(Faction faction) { faction_ = faction; }

    std::uint32_t health() const { return health_; }
    std::uint32_t max_health() const { return max_health_; }
    // Raise/lower the health cap. Current health is clamped down if it now exceeds
    // the cap; a cap of 0 is coerced to 1 (a Unit always has room for >= 1 HP).
    void set_max_health(std::uint32_t max_health);

    // Physical mitigation reads this already-aggregated value. This is the narrow
    // #785 seam: equipment/derived-stat plumbing owns recomputation, while combat
    // owns only the approved armor formula.
    std::int64_t effective_armor() const { return effective_armor_; }
    void set_effective_armor(std::int64_t armor) { effective_armor_ = armor; }

    ResourceType resource_type() const { return resource_type_; }
    std::uint32_t resource() const { return resource_; }
    std::uint32_t max_resource() const { return max_resource_; }
    // Raise/lower the secondary-resource cap (level-up stat growth, CHR-03 #360).
    // Current resource is clamped down if it now exceeds the cap. A Unit with no
    // secondary resource (kNone) ignores this — its pool stays 0.
    void set_max_resource(std::uint32_t max_resource);

    LifeState life_state() const { return life_state_; }
    bool is_alive() const { return life_state_ == LifeState::kAlive; }
    bool is_dead() const { return life_state_ == LifeState::kDead; }

    // (Re)spawn: full health, full resource, alive. Position/level/faction are
    // left as set by the ctor/caller. Idempotent — safe to call on an already-
    // alive Unit (used for respawn later; harmless now).
    void spawn();

    // Remove up to `amount` HP (clamped to remaining). Returns what was applied +
    // whether it was lethal. Damage to an already-dead Unit is a no-op ({0,false}).
    DamageResult apply_damage(std::uint32_t amount);

    // Restore up to `amount` HP (clamped to max_health). Returns HP actually
    // healed. A DEAD Unit heals 0 — resurrection is a separate transition.
    std::uint32_t apply_healing(std::uint32_t amount);

    // Force the death transition: health → 0, life → kDead. No-op if already dead.
    void kill();

    // Bring a dead Unit back to life with `health` HP (clamped to [1, max_health];
    // 0 is coerced to 1). No-op if already alive. Resource is NOT restored (the
    // resolver/respawn policy decides that later).
    void resurrect(std::uint32_t health);

    // Spend `amount` of the secondary resource. Returns false (and spends nothing)
    // if the Unit has insufficient resource — an all-or-nothing charge.
    bool spend_resource(std::uint32_t amount);
    // Restore up to `amount` resource (clamped to max_resource).
    void restore_resource(std::uint32_t amount);
    // Drain up to `amount` of the secondary resource (clamped to what is left) and
    // return how much was actually removed — the partial counterpart to
    // spend_resource's all-or-nothing charge (a `resource` drain primitive, #693).
    std::uint32_t drain_resource(std::uint32_t amount);

    // --- absorb pool (the `shield` primitive substrate, SP2.3 #693) ----------
    // A shield grants an absorb pool that soaks incoming damage BEFORE health. The
    // pool lives on the Unit because apply_damage is the single choke point every
    // damage source (direct hit, DoT tick, fall/drown) flows through, so ALL of them
    // respect shields with no per-site plumbing. The DURATION of a shield (and its
    // rollback on expiry) is owned by the AuraContainer, which grants here on apply
    // and reclaims the unspent remainder on expiry.
    std::uint32_t absorb() const { return absorb_; }
    void add_absorb(std::uint32_t amount);      // grant absorb (saturating)
    void remove_absorb(std::uint32_t amount);   // reclaim (clamped to what remains)

protected:
    std::uint16_t level_ = 1;
    Faction faction_ = Faction::kNeutral;

    std::uint32_t max_health_ = 1;
    std::uint32_t health_ = 1;
    std::int64_t effective_armor_ = 0;

    ResourceType resource_type_ = ResourceType::kNone;
    std::uint32_t max_resource_ = 0;
    std::uint32_t resource_ = 0;

    std::uint32_t absorb_ = 0;  // shield pool (soaked before health; #693)

    LifeState life_state_ = LifeState::kAlive;
};

// ---------------------------------------------------------------------------
// Player — a player-controlled Unit.
// ---------------------------------------------------------------------------
//
// Adds the player identity worldd already tracks per session (account, the
// M0-frozen class id from roster.h, the character name). Default-constructed as an
// empty (guid 0) placeholder so it can live by value inside WorldState's
// per-session record and be assigned on enter; the full ctor is what enter() uses.
class Player : public Unit {
public:
    // Empty placeholder (guid 0, level 1, 1 HP) — replaced by the full ctor on
    // enter. Exists so WorldState::SessionRec is default-constructible.
    Player();

    Player(ObjectGuid guid, const Position& pos, const UnitStats& stats,
           std::uint64_t account_id, std::uint8_t char_class, std::string name);

    std::uint64_t account_id() const { return account_id_; }
    std::uint8_t char_class() const { return char_class_; }
    const std::string& name() const { return name_; }

private:
    std::uint64_t account_id_ = 0;
    std::uint8_t char_class_ = 0;  // roster.h Class id (0 = unset)
    std::string name_;
};

// ---------------------------------------------------------------------------
// Creature — a server-controlled Unit (NPC / mob).
// ---------------------------------------------------------------------------
//
// Adds the spawn-table identity a creature carries. AI (threat, aggro radius,
// leash-to-home, patrols — SAD §2.5) is a SEPARATE story (#346-#348) and lives
// nowhere here; `spawn_home` is stored purely as data (the future leash anchor),
// not acted on.
class Creature : public Unit {
public:
    Creature(ObjectGuid guid, const Position& pos, const UnitStats& stats,
             std::uint32_t template_id);

    std::uint32_t template_id() const { return template_id_; }

    // The position the creature spawned at (future leash anchor — data only now).
    const Position& spawn_home() const { return spawn_home_; }

private:
    std::uint32_t template_id_ = 0;
    Position spawn_home_;
};

// ---------------------------------------------------------------------------
// GameObject / Corpse — non-Unit world entities (SAD §2.5 hierarchy). STUBS: they
// exist so the hierarchy is complete and code can hold them behind WorldObject*,
// but they carry no behavior yet (GO interaction = ITM/QST stories; corpse loot =
// ITM-02). Deliberately minimal — no gold-plating ahead of those stories.
// ---------------------------------------------------------------------------
class GameObject : public WorldObject {
public:
    GameObject(ObjectGuid guid, const Position& pos, std::uint32_t template_id)
        : WorldObject(guid, ObjectType::kGameObject, pos), template_id_(template_id) {}

    std::uint32_t template_id() const { return template_id_; }

private:
    std::uint32_t template_id_ = 0;
};

class Corpse : public WorldObject {
public:
    Corpse(ObjectGuid guid, const Position& pos, ObjectGuid owner_guid)
        : WorldObject(guid, ObjectType::kCorpse, pos), owner_guid_(owner_guid) {}

    // The Unit this corpse is the remains of (loot/resurrect target owner).
    ObjectGuid owner_guid() const { return owner_guid_; }

private:
    ObjectGuid owner_guid_ = 0;
};

// ---------------------------------------------------------------------------
// Placeholder stat providers (the D-11 pattern — see file header). ORIGINAL,
// clean-room numbers derived from the roster.h archetypes; NOT from any existing
// game's data. These stand in until the content pipeline / compiled spawn tables
// exist (out of scope for #342).
// ---------------------------------------------------------------------------

// Placeholder stats for a player of roster.h `char_class` at `level`. Health and
// the secondary resource are class-flavored (a melee Vanguard is tankier and uses
// rage; a Runcaller is squishier and uses mana; etc.). Faction is always kPlayer.
// An unknown class id falls back to a neutral all-rounder profile.
UnitStats placeholder_player_stats(std::uint8_t char_class, std::uint16_t level = 1);

// Placeholder stats for a creature at `level`: level-scaled health, no secondary
// resource, hostile by default. Stands in for a compiled creature template.
UnitStats placeholder_creature_stats(std::uint16_t level,
                                     Faction faction = Faction::kHostile);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_COMBAT_UNIT_H
