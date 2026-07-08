// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-map tick orchestrator (issue #349). See map_tick.h for the design
// + clean-room provenance (docs/sad/server-sad.md §2.5/§3.2/§3.3/§8.1 only).

#include "map_tick.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace meridian::worldd {

// ---------------------------------------------------------------------------
// Deterministic enum → text (golden readability; NOT the hot path).
// ---------------------------------------------------------------------------
const char* tick_phase_name(TickPhase p) {
    switch (p) {
        case TickPhase::kInbound: return "inbound";
        case TickPhase::kAi:      return "ai";
        case TickPhase::kCombat:  return "combat";
        case TickPhase::kAura:    return "aura";
    }
    return "?";
}

namespace {

const char* reject_name(CastReject r) {
    switch (r) {
        case CastReject::kNone:                 return "NONE";
        case CastReject::kUnknownAbility:        return "UNKNOWN_ABILITY";
        case CastReject::kNotInWorld:            return "NOT_IN_WORLD";
        case CastReject::kCasterDead:            return "CASTER_DEAD";
        case CastReject::kOnGcd:                 return "ON_GCD";
        case CastReject::kAlreadyCasting:        return "ALREADY_CASTING";
        case CastReject::kInsufficientResource:  return "INSUFFICIENT_RESOURCE";
        case CastReject::kNoTarget:              return "NO_TARGET";
        case CastReject::kTargetDead:            return "TARGET_DEAD";
        case CastReject::kWrongFaction:          return "WRONG_FACTION";
        case CastReject::kOutOfRange:            return "OUT_OF_RANGE";
        case CastReject::kNoLineOfSight:         return "NO_LINE_OF_SIGHT";
        case CastReject::kInterrupted:           return "INTERRUPTED";
    }
    return "?";
}

const char* outcome_name(AttackOutcome o) {
    switch (o) {
        case AttackOutcome::kMiss:  return "MISS";
        case AttackOutcome::kDodge: return "DODGE";
        case AttackOutcome::kParry: return "PARRY";
        case AttackOutcome::kHit:   return "HIT";
        case AttackOutcome::kCrit:  return "CRIT";
    }
    return "?";
}

const char* ai_state_name(AiState s) {
    switch (s) {
        case AiState::kPatrol: return "PATROL";
        case AiState::kCombat: return "COMBAT";
        case AiState::kEvade:  return "EVADE";
        case AiState::kDead:   return "DEAD";
    }
    return "?";
}

std::string u(std::uint64_t v) { return std::to_string(v); }

// Whether an ability has any DIRECT (damage/heal) effect the resolver applies.
bool has_direct_effect(const Ability& a) {
    for (const AbilityEffect& e : a.effects) {
        if (e.kind == EffectKind::kDamage || e.kind == EffectKind::kHeal) return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// TickEvent
// ---------------------------------------------------------------------------
std::string TickEvent::to_line() const {
    return "t=" + u(tick) + " now=" + u(now_ms) + " " + tick_phase_name(phase) + " " + text;
}

// ---------------------------------------------------------------------------
// MapTick
// ---------------------------------------------------------------------------
MapTick::MapTick(const AbilityStore& abilities, std::uint64_t rng_seed, std::uint32_t dt_ms)
    : abilities_(abilities), rng_(rng_seed), dt_ms_(dt_ms == 0 ? kTickDtMs : dt_ms) {}

ObjectGuid MapTick::add_player(ObjectGuid guid, const Position& pos, const UnitStats& stats) {
    Player p(guid, pos, stats, /*account_id=*/0, /*char_class=*/0, /*name=*/"");
    players_.emplace(guid,
                     std::make_unique<PlayerCombatant>(std::move(p), move_dmg_params_));
    return guid;
}

ObjectGuid MapTick::add_creature(const CreatureSpawnDef& def) {
    const ObjectGuid g = ai_.add_spawn(def);
    prev_ai_state_[g] = ai_.state_of(g);  // spawn state (kPatrol) — the transition baseline
    return g;
}

void MapTick::set_player_position(ObjectGuid guid, const Position& pos) {
    auto it = players_.find(guid);
    if (it != players_.end()) it->second->unit.set_position(pos);
}

void MapTick::enqueue_cast(const AbilityUseCmd& cmd) { inbound_.push_back(cmd); }

bool MapTick::has_simulation() const { return !players_.empty() || ai_.size() > 0; }

Unit* MapTick::unit_for_guid(ObjectGuid guid) {
    auto it = players_.find(guid);
    if (it != players_.end()) return &it->second->unit;
    return ai_.creature(guid);
}

AuraContainer* MapTick::auras_for(ObjectGuid guid) {
    auto pit = players_.find(guid);
    if (pit != players_.end()) return &pit->second->auras;
    Creature* cr = ai_.creature(guid);
    if (cr == nullptr) return nullptr;
    auto cit = creature_auras_.find(guid);
    if (cit == creature_auras_.end())
        cit = creature_auras_.emplace(guid, std::make_unique<AuraContainer>(*cr)).first;
    return cit->second.get();
}

void MapTick::emit(std::vector<TickEvent>& out, TickPhase phase, std::string text) {
    out.push_back(TickEvent{tick_no_, now_ms_, phase, std::move(text)});
}

// ---------------------------------------------------------------------------
// SAD §2.5 tick: drain inbound → movement/commands → AI → combat/auras →
// spawns/respawns → AoI delta build → flush.
// ---------------------------------------------------------------------------
std::vector<TickEvent> MapTick::advance() {
    ++tick_no_;
    std::vector<TickEvent> out;

    // 1) drain inbound + 2) movement/commands: process this tick's ability-use
    //    commands (accept/reject + instant resolution). Player movement is applied
    //    by the caller via set_player_position before advance() (the harness's
    //    authoritative movement input); the AI phase reads it as an aggro target.
    phase_drain_inbound(out);

    // 3) AI: threat/aggro/leash/evade/patrol + 5) spawns/respawns (creature_ai owns
    //    respawn timers, so that phase is consolidated into the AI tick — #347).
    phase_ai(out);

    // 4) combat/auras: advance cast timers → resolve completed casts (+ spend
    //    resource) → tick aura periodics → deaths (SAD §3.2 "casts complete,
    //    periodics, deaths"). This CLOSES the #354 cast-completion seam.
    phase_combat_auras(out);

    // 4b) movement-derived (fall/swim) environmental damage (#362), evaluated in
    //     the combat phase after casts/auras: fall damage on landing + drowning while
    //     breath is exhausted, applied via Unit::apply_damage. Emits only when it
    //     actually deals damage (a flat map with no water is silent).
    phase_movement_damage(out);

    // 6) AoI delta build + 7) flush: the returned event stream IS the per-tick
    //    delta an observer would receive (the wire seam the dispatch layer serialises).
    now_ms_ += dt_ms_;
    for (const TickEvent& e : out) log_.push_back(e);
    return out;
}

void MapTick::advance(int ticks) {
    for (int i = 0; i < ticks; ++i) advance();
}

void MapTick::phase_drain_inbound(std::vector<TickEvent>& out) {
    for (const AbilityUseCmd& cmd : inbound_) {
        auto pit = players_.find(cmd.caster_guid);
        if (pit == players_.end()) {
            emit(out, TickPhase::kInbound,
                 "cast_ignored caster=" + u(cmd.caster_guid) + " reason=NO_CASTER");
            continue;
        }
        PlayerCombatant& pc = *pit->second;
        const Ability* ab = abilities_.find(cmd.ability_id);
        if (ab == nullptr) {
            emit(out, TickPhase::kInbound,
                 "cast_rejected caster=" + u(cmd.caster_guid) + " ability=" +
                     u(cmd.ability_id) + " reason=UNKNOWN_ABILITY");
            continue;
        }

        // Resolve the target: self for a self ability / no target / own guid.
        ObjectGuid tguid = cmd.target_guid;
        Unit* target = nullptr;
        if (ab->target == TargetKind::kSelf || tguid == 0 || tguid == cmd.caster_guid) {
            target = &pc.unit;
            tguid = cmd.caster_guid;
        } else {
            target = unit_for_guid(tguid);
        }

        const CastDecision d = begin_ability_use(pc.combat, *ab, pc.unit, target, tguid,
                                                 flat_map_los, now_ms_);
        if (!d.accepted) {
            emit(out, TickPhase::kInbound,
                 "cast_rejected caster=" + u(cmd.caster_guid) + " ability=" + u(ab->id) +
                     " reason=" + reject_name(d.reject) + " gcd_rem=" +
                     u(d.gcd_remaining_ms));
            continue;
        }

        if (d.instant) {
            emit(out, TickPhase::kInbound,
                 "cast_accept caster=" + u(cmd.caster_guid) + " ability=" + u(ab->id) +
                     " cast_ms=0 instant=1 target=" + u(tguid));
            if (target != nullptr)
                resolve_and_log(*ab, pc.unit, cmd.caster_guid, *target, tguid,
                                TickPhase::kInbound, out);
        } else {
            emit(out, TickPhase::kInbound,
                 "cast_start caster=" + u(cmd.caster_guid) + " ability=" + u(ab->id) +
                     " cast_ms=" + u(d.cast_ms) + " ends=" + u(now_ms_ + d.cast_ms) +
                     " target=" + u(tguid));
        }
    }
    inbound_.clear();
}

void MapTick::phase_ai(std::vector<TickEvent>& out) {
    // Build the aggro-target snapshot from the map's players, in ascending-guid
    // order so the AI's tie-breaks (and our logging) are byte-stable regardless of
    // the unordered_map iteration order.
    std::vector<AiTargetView> targets;
    targets.reserve(players_.size());
    for (const auto& kv : players_) {
        const Player& pu = kv.second->unit;
        AiTargetView v;
        v.guid = kv.first;
        v.pos = pu.position();
        v.level = pu.level();
        v.faction = pu.faction();
        v.alive = pu.is_alive();
        targets.push_back(v);
    }
    std::sort(targets.begin(), targets.end(),
              [](const AiTargetView& a, const AiTargetView& b) { return a.guid < b.guid; });

    CreatureAiTickResult r = ai_.tick(dt_ms_, targets);

    std::sort(r.spawned.begin(), r.spawned.end());
    std::sort(r.despawned.begin(), r.despawned.end());

    // AoI leave (EntityLeave{DIED}) — a creature that died this tick.
    for (ObjectGuid g : r.despawned) {
        emit(out, TickPhase::kAi, "ai_leave guid=" + u(g) + " reason=died");
        creature_auras_.erase(g);  // a corpse holds no auras
    }
    // AoI enter (EntityEnter) — a creature that (re)spawned this tick.
    for (ObjectGuid g : r.spawned) {
        const Creature* cr = ai_.creature(g);
        emit(out, TickPhase::kAi,
             "ai_enter guid=" + u(g) + " hp=" + u(cr != nullptr ? cr->health() : 0));
    }

    // FSM transition edges (aggro / leash-evade / return-patrol / death / respawn).
    // Iterate every creature this map has ever spawned, in ascending guid order.
    std::vector<ObjectGuid> guids;
    guids.reserve(prev_ai_state_.size());
    for (const auto& kv : prev_ai_state_) guids.push_back(kv.first);
    std::sort(guids.begin(), guids.end());
    for (ObjectGuid g : guids) {
        const AiState cur = ai_.state_of(g);
        AiState& prev = prev_ai_state_[g];
        if (cur != prev) {
            emit(out, TickPhase::kAi,
                 "ai_state guid=" + u(g) + " " + ai_state_name(prev) + "->" +
                     ai_state_name(cur));
            prev = cur;
        }
    }
}

void MapTick::phase_combat_auras(std::vector<TickEvent>& out) {
    // A) advance cast timers → resolve completed casts (+ spend resource). Iterate
    //    players in ascending guid order for determinism.
    std::vector<ObjectGuid> pguids;
    pguids.reserve(players_.size());
    for (const auto& kv : players_) pguids.push_back(kv.first);
    std::sort(pguids.begin(), pguids.end());

    for (ObjectGuid g : pguids) {
        PlayerCombatant& pc = *players_[g];
        std::optional<PendingCast> done = pc.combat.take_completed(now_ms_);
        if (!done) continue;

        const Ability* ab = abilities_.find(done->ability_id);
        if (ab == nullptr) {
            emit(out, TickPhase::kCombat,
                 "cast_fizzle caster=" + u(g) + " reason=UNKNOWN_ABILITY");
            continue;
        }
        ObjectGuid tguid = done->target_guid;
        Unit* target = nullptr;
        if (ab->target == TargetKind::kSelf || tguid == 0 || tguid == g) {
            target = &pc.unit;
            tguid = g;
        } else {
            target = unit_for_guid(tguid);
        }
        // Re-validate on completion: the target may have died / left / moved out of
        // range while the cast ran (SAD §2.5 cast-time interrupt/fizzle).
        const CastReject why = validate_target(*ab, pc.unit, target, flat_map_los);
        if (why != CastReject::kNone || target == nullptr) {
            emit(out, TickPhase::kCombat,
                 "cast_fizzle caster=" + u(g) + " ability=" + u(ab->id) + " reason=" +
                     reject_name(why));
            continue;
        }
        emit(out, TickPhase::kCombat,
             "cast_complete caster=" + u(g) + " ability=" + u(ab->id) + " target=" + u(tguid));
        resolve_and_log(*ab, pc.unit, g, *target, tguid, TickPhase::kCombat, out);
    }

    // B) tick aura periodics (DoT/HoT) for every host with a container, players
    //    then creatures, ascending guid — deterministic. A dead host takes none.
    auto tick_host = [&](ObjectGuid g, Unit& host, AuraContainer& c) {
        if (host.is_dead() || c.empty()) return;
        AuraTickResult r = c.tick(dt_ms_, rng_);
        if (r.ticks_fired == 0 && r.auras_expired == 0 && !r.host_died) return;
        emit(out, TickPhase::kAura,
             "aura_tick host=" + u(g) + " dmg=" + u(r.periodic_damage) + " heal=" +
                 u(r.periodic_healing) + " ticks=" + u(r.ticks_fired) + " expired=" +
                 u(r.auras_expired) + " hp=" + u(host.health()) + " host_died=" +
                 u(r.host_died ? 1 : 0));
        if (r.host_died)
            emit(out, TickPhase::kAura, "death guid=" + u(g) + " by=PERIODIC");
    };

    for (ObjectGuid g : pguids) {
        PlayerCombatant& pc = *players_[g];
        tick_host(g, pc.unit, pc.auras);
    }
    std::vector<ObjectGuid> cguids;
    cguids.reserve(creature_auras_.size());
    for (const auto& kv : creature_auras_) cguids.push_back(kv.first);
    std::sort(cguids.begin(), cguids.end());
    for (ObjectGuid g : cguids) {
        Creature* cr = ai_.creature(g);
        if (cr == nullptr) continue;
        tick_host(g, *cr, *creature_auras_[g]);
    }
}

void MapTick::phase_movement_damage(std::vector<TickEvent>& out) {
    // Iterate players in ascending guid order for determinism (mirrors the other
    // phases). Each player's tracker sees exactly one position sample per tick — the
    // authoritative position the caller set via set_player_position before advance().
    std::vector<ObjectGuid> pguids;
    pguids.reserve(players_.size());
    for (const auto& kv : players_) pguids.push_back(kv.first);
    std::sort(pguids.begin(), pguids.end());

    for (ObjectGuid g : pguids) {
        PlayerCombatant& pc = *players_[g];
        Unit& unit = pc.unit;

        // Always step the tracker (keeps apex/breath bookkeeping consistent), but a
        // dead unit takes no further environmental damage.
        const MovementDamageResult r =
            pc.move_dmg.step(unit.position(), env_, dt_ms_, unit.max_health());
        if (unit.is_dead()) continue;

        if (r.fall_damage > 0) {
            const DamageResult dr = unit.apply_damage(r.fall_damage);
            emit(out, TickPhase::kCombat,
                 "fall_damage guid=" + u(g) + " height_cm=" +
                     u(static_cast<std::uint64_t>(std::lround(r.fall_height_m * 100.0f))) +
                     " dmg=" + u(dr.applied) + " hp=" + u(unit.health()) + " died=" +
                     u(dr.lethal ? 1 : 0));
            if (dr.lethal)
                emit(out, TickPhase::kCombat, "death guid=" + u(g) + " by=FALL");
            if (unit.is_dead()) continue;  // a fatal fall pre-empts drowning this tick
        }

        if (r.drown_damage > 0) {
            const DamageResult dr = unit.apply_damage(r.drown_damage);
            emit(out, TickPhase::kCombat,
                 "drown_damage guid=" + u(g) + " ticks=" + u(r.drown_ticks) + " dmg=" +
                     u(dr.applied) + " breath_ms=" + u(r.breath_remaining_ms) + " hp=" +
                     u(unit.health()) + " died=" + u(dr.lethal ? 1 : 0));
            if (dr.lethal)
                emit(out, TickPhase::kCombat, "death guid=" + u(g) + " by=DROWN");
        }
    }
}

void MapTick::resolve_and_log(const Ability& ability, Unit& caster, ObjectGuid caster_guid,
                              Unit& target, ObjectGuid target_guid, TickPhase phase,
                              std::vector<TickEvent>& out) {
    // Spend the resource AT RESOLUTION (the #354 deferral — begin_ability_use does
    // not charge; the instant path here and the cast-completion path both do).
    if (ability.resource_amount > 0) {
        const bool ok = caster.spend_resource(ability.resource_amount);
        emit(out, phase,
             "resource_spend caster=" + u(caster_guid) + " amount=" +
                 u(ability.resource_amount) + " ok=" + u(ok ? 1 : 0) + " left=" +
                 u(caster.resource()));
    }

    // Direct damage/heal: roll the attack table + apply (skips kAura/kThreat).
    if (has_direct_effect(ability)) {
        const ResolveResult rr = resolve_ability(ability, caster, target, rng_);
        emit(out, phase,
             "resolve caster=" + u(caster_guid) + " target=" + u(target_guid) + " ability=" +
                 u(ability.id) + " outcome=" + outcome_name(rr.outcome) + " amount=" +
                 u(rr.amount) + " heal=" + u(rr.is_heal ? 1 : 0) + " target_hp=" +
                 u(rr.target_health) + " died=" + u(rr.target_died ? 1 : 0));
        if (rr.target_died)
            emit(out, phase, "death guid=" + u(target_guid) + " by=" + u(caster_guid));

        // Resolver → AI threat seam (#347): landing (or attempting) an attack on a
        // creature adds threat and pulls it into combat on the attacker. A miss
        // still aggros (a swing is a swing), so floor the threat at 1.
        if (ai_.creature(target_guid) != nullptr) {
            const float threat = static_cast<float>(rr.amount > 0 ? rr.amount : 1);
            ai_.add_threat(target_guid, caster_guid, threat);
        }
    }

    // Aura effects: the resolver SKIPS kAura, so the tick applies them to the
    // target's container (the §2.5 "combat/auras" integration point).
    if (AuraContainer* tc = auras_for(target_guid)) {
        const std::size_t n = tc->apply_ability_auras(ability, caster_guid);
        if (n > 0)
            emit(out, phase,
                 "aura_applied target=" + u(target_guid) + " ability=" + u(ability.id) +
                     " count=" + u(n));
    }
}

std::string MapTick::log_text() const {
    std::string s;
    for (std::size_t i = 0; i < log_.size(); ++i) {
        if (i) s += '\n';
        s += log_[i].to_line();
    }
    return s;
}

}  // namespace meridian::worldd
