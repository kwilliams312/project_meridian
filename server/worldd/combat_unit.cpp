// SPDX-License-Identifier: Apache-2.0
//
// worldd — Combat Unit model implementation (issue #342). See combat_unit.h for
// the clean-room provenance, the shallow-hierarchy design, and the #342 scope
// boundary (model + lifecycle; NO resolver / auras / AI).
//
// PURE: this translation unit depends only on <algorithm> + the header. No net,
// no DB, no logging, no FlatBuffers — so the lifecycle is unit-testable in the
// plain server ctest (like the AoI / movement pure cores).

#include "combat_unit.h"

#include <algorithm>
#include <utility>

namespace meridian::worldd {
namespace {

// Sum two u32 saturating at UINT32_MAX, then clamp to `cap` — avoids overflow when
// a large heal/restore is added to a near-max pool.
std::uint32_t add_clamped(std::uint32_t current, std::uint32_t amount, std::uint32_t cap) {
    const std::uint64_t sum = static_cast<std::uint64_t>(current) + amount;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, cap));
}

}  // namespace

// ---------------------------------------------------------------------------
// Unit
// ---------------------------------------------------------------------------

Unit::Unit(ObjectGuid guid, ObjectType type, const Position& pos, const UnitStats& stats)
    : WorldObject(guid, type, pos),
      level_(stats.level),
      faction_(stats.faction),
      max_health_(std::max<std::uint32_t>(1, stats.max_health)),
      health_(std::max<std::uint32_t>(1, stats.max_health)),
      resource_type_(stats.resource_type),
      max_resource_(stats.resource_type == ResourceType::kNone ? 0 : stats.max_resource),
      resource_(stats.resource_type == ResourceType::kNone ? 0 : stats.max_resource),
      life_state_(LifeState::kAlive) {}

void Unit::set_max_health(std::uint32_t max_health) {
    max_health_ = std::max<std::uint32_t>(1, max_health);
    if (health_ > max_health_) health_ = max_health_;
}

void Unit::set_max_resource(std::uint32_t max_resource) {
    if (resource_type_ == ResourceType::kNone) return;  // no pool to grow
    max_resource_ = max_resource;
    if (resource_ > max_resource_) resource_ = max_resource_;
}

void Unit::spawn() {
    health_ = max_health_;
    resource_ = max_resource_;
    life_state_ = LifeState::kAlive;
}

DamageResult Unit::apply_damage(std::uint32_t amount) {
    if (life_state_ == LifeState::kDead) return DamageResult{0, false, 0};

    // A shield (SP2.3 #693) soaks incoming damage first; only the overflow reaches
    // health. absorb() == 0 (the common case) is a no-op, so shieldless damage is
    // byte-identical to the pre-shield behaviour (the combat goldens stay stable).
    const std::uint32_t absorbed = std::min(amount, absorb_);
    absorb_ -= absorbed;
    amount -= absorbed;

    const std::uint32_t applied = std::min(amount, health_);
    health_ -= applied;

    DamageResult result{applied, false, absorbed};
    if (health_ == 0) {
        life_state_ = LifeState::kDead;
        result.lethal = true;
    }
    return result;
}

std::uint32_t Unit::apply_healing(std::uint32_t amount) {
    if (life_state_ == LifeState::kDead) return 0;  // a corpse must be resurrected
    const std::uint32_t before = health_;
    health_ = add_clamped(health_, amount, max_health_);
    return health_ - before;
}

void Unit::kill() {
    if (life_state_ == LifeState::kDead) return;
    health_ = 0;
    life_state_ = LifeState::kDead;
}

void Unit::resurrect(std::uint32_t health) {
    if (life_state_ == LifeState::kAlive) return;
    const std::uint32_t hp = std::clamp<std::uint32_t>(health, 1, max_health_);
    health_ = hp;
    life_state_ = LifeState::kAlive;
}

bool Unit::spend_resource(std::uint32_t amount) {
    if (amount > resource_) return false;  // all-or-nothing
    resource_ -= amount;
    return true;
}

void Unit::restore_resource(std::uint32_t amount) {
    resource_ = add_clamped(resource_, amount, max_resource_);
}

std::uint32_t Unit::drain_resource(std::uint32_t amount) {
    const std::uint32_t drained = std::min(amount, resource_);
    resource_ -= drained;
    return drained;
}

void Unit::add_absorb(std::uint32_t amount) {
    // Saturating add — an absurd content value can never wrap the pool.
    if (amount > (UINT32_MAX - absorb_)) absorb_ = UINT32_MAX;
    else absorb_ += amount;
}

void Unit::remove_absorb(std::uint32_t amount) {
    absorb_ -= std::min(amount, absorb_);
}

// ---------------------------------------------------------------------------
// Player / Creature
// ---------------------------------------------------------------------------

Player::Player()
    : Unit(0, ObjectType::kPlayer, Position{}, UnitStats{}) {}

Player::Player(ObjectGuid guid, const Position& pos, const UnitStats& stats,
               std::uint64_t account_id, std::uint8_t char_class, std::string name)
    : Unit(guid, ObjectType::kPlayer, pos, stats),
      account_id_(account_id),
      char_class_(char_class),
      name_(std::move(name)) {}

Creature::Creature(ObjectGuid guid, const Position& pos, const UnitStats& stats,
                   std::uint32_t template_id)
    : Unit(guid, ObjectType::kCreature, pos, stats),
      template_id_(template_id),
      spawn_home_(pos) {}

// ---------------------------------------------------------------------------
// Placeholder stat providers (clean-room; see header). Per-class M0 archetypes
// derived from roster.h — ORIGINAL numbers, not from any existing game.
// ---------------------------------------------------------------------------
namespace {

// One class's placeholder curve: base pool at level 1 + a per-level increment.
struct ClassCurve {
    std::uint32_t base_health;
    std::uint32_t health_per_level;
    ResourceType resource_type;
    std::uint32_t base_resource;
    std::uint32_t resource_per_level;
};

// roster.h Class ids: 1 Vanguard (melee/rage), 2 Runcaller (caster/mana),
// 3 Warden (hybrid/energy), 4 Mender (healer/mana). Index 0 is the fallback
// all-rounder used for an unknown/unset class id.
constexpr ClassCurve kClassCurves[] = {
    /* 0 fallback  */ {100, 15, ResourceType::kNone, 0, 0},
    /* 1 Vanguard  */ {120, 20, ResourceType::kRage, 100, 0},
    /* 2 Runcaller */ {80, 12, ResourceType::kMana, 150, 15},
    /* 3 Warden    */ {100, 16, ResourceType::kEnergy, 100, 0},
    /* 4 Mender    */ {90, 13, ResourceType::kMana, 140, 14},
};

std::uint32_t scale(std::uint32_t base, std::uint32_t per_level, std::uint16_t level) {
    const std::uint16_t steps = level > 0 ? static_cast<std::uint16_t>(level - 1) : 0;
    const std::uint64_t v = static_cast<std::uint64_t>(base) +
                            static_cast<std::uint64_t>(per_level) * steps;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(v, UINT32_MAX));
}

}  // namespace

UnitStats placeholder_player_stats(std::uint8_t char_class, std::uint16_t level) {
    const std::size_t idx =
        (char_class >= 1 && char_class <= 4) ? char_class : 0;
    const ClassCurve& c = kClassCurves[idx];

    UnitStats s;
    s.level = level > 0 ? level : 1;
    s.max_health = scale(c.base_health, c.health_per_level, s.level);
    s.resource_type = c.resource_type;
    s.max_resource = c.resource_type == ResourceType::kNone
                         ? 0
                         : scale(c.base_resource, c.resource_per_level, s.level);
    s.faction = Faction::kPlayer;
    return s;
}

UnitStats placeholder_creature_stats(std::uint16_t level, Faction faction) {
    UnitStats s;
    s.level = level > 0 ? level : 1;
    s.max_health = scale(/*base=*/50, /*per_level=*/30, s.level);
    s.resource_type = ResourceType::kNone;
    s.max_resource = 0;
    s.faction = faction;
    return s;
}

}  // namespace meridian::worldd
