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
        case TickPhase::kDeath:   return "death";
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
    : abilities_(abilities),
      rng_(rng_seed),
      dt_ms_(dt_ms == 0 ? kTickDtMs : dt_ms),
      owned_loot_tables_(std::make_unique<loot::PlaceholderLootTableStore>()),
      loot_tables_(owned_loot_tables_.get()),
      // Derive the loot seed from the map seed so loot is reproducible for a given
      // map, but on a SEPARATE stream from the combat rng_ (mixed with the golden
      // ratio so the two seeds never coincide). loot_rng_ is reseeded per corpse.
      loot_seed_(rng_seed ^ 0x9E3779B97F4A7C15ULL),
      loot_rng_(loot_seed_) {}

ObjectGuid MapTick::add_player(ObjectGuid guid, const Position& pos, const UnitStats& stats,
                               std::uint8_t char_class) {
    Player p(guid, pos, stats, /*account_id=*/0, char_class, /*name=*/"");
    auto pc = std::make_unique<PlayerCombatant>(std::move(p), move_dmg_params_);
    // Give the player a quest log when a quest store is installed (QST-01 #371) —
    // the "kill count via on_unit_died" seam feeds this killer's kill objectives.
    if (quest_store_ != nullptr) pc->quests = std::make_unique<QuestLog>(*quest_store_);
    players_.emplace(guid, std::move(pc));
    return guid;
}

QuestLog* MapTick::player_quest_log(ObjectGuid guid) {
    auto it = players_.find(guid);
    return it == players_.end() ? nullptr : it->second->quests.get();
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
    //    periodics, deaths"). This CLOSES the #354 cast-completion seam. Deaths
    //    route through the single on_unit_died hook (corpse+XP side effects).
    phase_combat_auras(out);

    // 4b) movement-derived (fall/swim) environmental damage (#362), evaluated in
    //     the combat phase after casts/auras: fall damage on landing + drowning while
    //     breath is exhausted, applied via Unit::apply_damage. A lethal fall/drown
    //     routes through the SAME on_unit_died hook (corpse + death FSM). Emits only
    //     when it actually deals damage (a flat map with no water is silent).
    phase_movement_damage(out);

    // 5) spawns/respawns: the player death FSM — auto-release timers, drained
    //    release/resurrect requests, corpse-run resurrect (SAD §2.5; CMB-03 #359).
    //    Creature respawns are owned by the AI phase (creature_ai respawn timers).
    phase_death(out);

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
            // Periodic death has no direct caster — the hook resolves the killer via
            // the threat table (#360) and runs the death state machine (#359).
            on_unit_died(g, host, /*killer_guid=*/0, "PERIODIC", TickPhase::kAura, out);
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
                // Lethal fall routes through the single death hook (corpse + FSM, #359).
                on_unit_died(g, unit, /*killer_guid=*/0, "FALL", TickPhase::kCombat, out);
            if (unit.is_dead()) continue;  // a fatal fall pre-empts drowning this tick
        }

        if (r.drown_damage > 0) {
            const DamageResult dr = unit.apply_damage(r.drown_damage);
            emit(out, TickPhase::kCombat,
                 "drown_damage guid=" + u(g) + " ticks=" + u(r.drown_ticks) + " dmg=" +
                     u(dr.applied) + " breath_ms=" + u(r.breath_remaining_ms) + " hp=" +
                     u(unit.health()) + " died=" + u(dr.lethal ? 1 : 0));
            if (dr.lethal)
                // Lethal drown routes through the single death hook (corpse + FSM, #359).
                on_unit_died(g, unit, /*killer_guid=*/0, "DROWN", TickPhase::kCombat, out);
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
            // THE single death hook: corpse+ghost for a player (#359), kill XP for a
            // creature (#360). caster_guid is the direct killer.
            on_unit_died(target_guid, target, caster_guid, u(caster_guid), phase, out);

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

// ---------------------------------------------------------------------------
// THE single "unit died" hook (CMB-03 #359 + CHR-03 #360). One death event, then
// the two cleanly-separated concerns dispatched by victim kind.
// ---------------------------------------------------------------------------
void MapTick::on_unit_died(ObjectGuid victim_guid, Unit& victim, ObjectGuid killer_guid,
                           const std::string& by_label, TickPhase phase,
                           std::vector<TickEvent>& out) {
    emit(out, phase, "death guid=" + u(victim_guid) + " by=" + by_label);
    if (victim.type() == ObjectType::kPlayer) {
        handle_player_death(victim_guid, victim, phase, out);   // #359
    } else if (victim.type() == ObjectType::kCreature) {
        award_kill_xp(victim_guid, victim, killer_guid, phase, out);       // #360
        roll_creature_loot(victim_guid, victim, killer_guid, phase, out);  // #369
    }
}

void MapTick::handle_player_death(ObjectGuid victim_guid, Unit& victim, TickPhase phase,
                                  std::vector<TickEvent>& out) {
    // Spawn a corpse at the death spot + enter the death FSM (kCorpse, release
    // timer armed). The graveyard is the map's release destination (world-data seam).
    const Position death_pos = victim.position();
    const ObjectGuid corpse = deaths_.on_death(victim_guid, death_pos, graveyard_);
    emit(out, phase,
         "corpse_spawn owner=" + u(victim_guid) + " corpse=" + u(corpse) + " x=" +
             std::to_string(static_cast<long long>(death_pos.x)) + " y=" +
             std::to_string(static_cast<long long>(death_pos.y)));
    emit(out, phase,
         "player_died guid=" + u(victim_guid) + " release_in=" +
             u(deaths_.config().auto_release_ms));
}

void MapTick::award_kill_xp(ObjectGuid victim_guid, Unit& victim, ObjectGuid killer_guid,
                            TickPhase phase, std::vector<TickEvent>& out) {
    // Resolve the killer PLAYER. The direct killing blow (a cast/melee) names the
    // caster; a periodic tick names no one, so fall back to the top threat holder —
    // "attribution via the threat table / damage attribution" (#347/#360).
    ObjectGuid kg = killer_guid;
    if (players_.find(kg) == players_.end()) kg = ai_.top_threat(victim_guid);
    auto pit = players_.find(kg);
    if (pit == players_.end()) return;  // no player killer (nothing to award)

    Player& killer = pit->second->unit;
    std::uint32_t& xp_into = pit->second->xp_into_level;

    // Quest kill objectives (QST-01 #371): a creature death advances the killer's
    // matching kill objectives, independent of the XP award (a grey mob gives no
    // XP but still counts for a kill quest). Gated on an installed quest store so
    // combat/death golden scenarios without quests emit no new events.
    if (quest_store_ != nullptr && pit->second->quests) {
        const std::uint32_t npc_tid = static_cast<Creature&>(victim).template_id();
        if (pit->second->quests->on_kill(npc_tid))
            emit(out, phase, "quest_kill killer=" + u(kg) + " npc=" + u(npc_tid));
    }

    const std::uint16_t kl = killer.level();
    const std::uint32_t xp = xp_for_kill(victim.level(), kl);

    const LevelProgress lp = grant_xp(kl, xp_into, xp);
    xp_into = lp.xp_into_level;

    emit(out, phase,
         "xp_award killer=" + u(kg) + " victim=" + u(victim_guid) + " xp=" + u(xp) +
             " level=" + u(kl) + " into=" + u(xp_into) + " next=" +
             u(xp_to_next_level(lp.level)));

    if (lp.levels_gained == 0) return;

    // Level-up: apply stat growth from the level curve (placeholder_player_stats;
    // the real per-class/level table loads from the world DB via #28) and top the
    // player off to the new caps.
    const UnitStats ns = placeholder_player_stats(killer.char_class(), lp.level);
    killer.set_level(lp.level);
    killer.set_max_health(ns.max_health);
    killer.set_max_resource(ns.max_resource);
    killer.apply_healing(ns.max_health);          // heal to the new full
    killer.restore_resource(ns.max_resource);     // top off the new resource pool
    emit(out, phase,
         "level_up guid=" + u(kg) + " level=" + u(kl) + "->" + u(lp.level) + " hp=" +
             u(killer.max_health()) + " res=" + u(killer.max_resource()));
}

// ---------------------------------------------------------------------------
// Loot on creature death (ITM-02 #369). Roll the dead creature's loot table (if
// any) into a session on its corpse; a client loots from it with server-side
// validation (loot_session.h). Uses a SEPARATE seeded loot RNG (never the combat
// rng_) reseeded per corpse — so a roll is a pure function of (map seed, victim
// guid) and the combat golden stream is unaffected.
// ---------------------------------------------------------------------------
ObjectGuid MapTick::resolve_killer_player(ObjectGuid victim_guid,
                                          ObjectGuid killer_guid) const {
    // The direct killing blow names the caster; a periodic/environment death names
    // no one, so fall back to the top threat holder (mirrors award_kill_xp, #360).
    ObjectGuid kg = killer_guid;
    if (players_.find(kg) == players_.end()) kg = ai_.top_threat(victim_guid);
    return players_.find(kg) != players_.end() ? kg : 0;
}

void MapTick::roll_creature_loot(ObjectGuid victim_guid, Unit& victim,
                                 ObjectGuid killer_guid, TickPhase phase,
                                 std::vector<TickEvent>& out) {
    if (loot_tables_ == nullptr) return;
    const auto& creature = static_cast<const Creature&>(victim);
    const loot::LootTable* table = loot_tables_->find(creature.template_id());
    if (table == nullptr) return;  // this creature drops nothing (no loot table)

    // Reseed the loot RNG per corpse so the roll depends only on (map seed, victim
    // guid) — reproducible, and independent of tick timing / the combat stream.
    loot_rng_.reseed(loot_seed_ ^ (victim_guid * 0x9E3779B97F4A7C15ULL));
    loot::LootRoll roll = loot::roll_loot(*table, loot_rng_);
    if (roll.empty()) return;  // rolled nothing this time — no session

    // Eligible looters: the resolved killer player (direct or top-threat). No
    // player killer → no owner (loot is unlootable, e.g. an environment kill).
    const ObjectGuid owner = resolve_killer_player(victim_guid, killer_guid);
    std::vector<loot::LooterId> owners;
    if (owner != 0) owners.push_back(owner);

    const Position p = victim.position();
    const loot::LootPoint corpse_pt{p.x, p.y, p.z};
    const std::size_t item_count = roll.stacks.size();
    const items::Copper copper = roll.copper;
    loot_sessions_.insert_or_assign(
        victim_guid,
        loot::LootSession(victim_guid, corpse_pt, std::move(roll), std::move(owners)));

    emit(out, phase,
         "loot_roll corpse=" + u(victim_guid) + " owner=" + u(owner) + " items=" +
             u(item_count) + " copper=" + u(static_cast<std::uint64_t>(copper)));
}

const loot::LootSession* MapTick::loot_session(ObjectGuid corpse_guid) const {
    auto it = loot_sessions_.find(corpse_guid);
    return it == loot_sessions_.end() ? nullptr : &it->second;
}

loot::LootSession* MapTick::loot_session_mut(ObjectGuid corpse_guid) {
    auto it = loot_sessions_.find(corpse_guid);
    return it == loot_sessions_.end() ? nullptr : &it->second;
}

loot::LootStack MapTick::take_loot(ObjectGuid corpse_guid, ObjectGuid looter,
                                   const Position& looter_pos, std::size_t slot,
                                   const loot::QuestPredicate& has_quest,
                                   items::Inventory& inv) {
    auto it = loot_sessions_.find(corpse_guid);
    if (it == loot_sessions_.end()) throw loot::NoSuchCorpse();
    const loot::LootPoint pt{looter_pos.x, looter_pos.y, looter_pos.z};
    loot::LootStack got = it->second.take_item(looter, pt, slot, has_quest, inv);
    // Drop the session once the corpse's SHARED loot is exhausted (the map may then
    // despawn the corpse). Personal quest stacks do not keep it alive.
    if (it->second.fully_looted()) loot_sessions_.erase(it);
    return got;
}

// ---------------------------------------------------------------------------
// Spawns/respawns phase: the player death FSM (CMB-03 #359).
// ---------------------------------------------------------------------------
void MapTick::phase_death(std::vector<TickEvent>& out) {
    // A) explicit release requests (C→S RELEASE_REQUEST), ascending guid.
    std::sort(release_requests_.begin(), release_requests_.end());
    for (ObjectGuid g : release_requests_) {
        if (!deaths_.request_release(g)) continue;  // not a just-died corpse
        const DeathRecord* r = deaths_.record(g);
        if (r != nullptr) set_player_position(g, r->graveyard_pos);  // ghost → graveyard
        emit(out, TickPhase::kDeath, "player_release guid=" + u(g) + " mode=requested");
    }
    release_requests_.clear();

    // B) auto-release timers (kCorpse → kGhost when the countdown elapses).
    std::vector<ObjectGuid> auto_released;
    deaths_.advance(dt_ms_, auto_released);
    for (ObjectGuid g : auto_released) {
        const DeathRecord* r = deaths_.record(g);
        if (r != nullptr) set_player_position(g, r->graveyard_pos);
        emit(out, TickPhase::kDeath, "player_release guid=" + u(g) + " mode=auto");
    }

    // C) resurrect requests (C→S RESURRECT_REQUEST) — corpse-run resurrect, ascending.
    std::sort(resurrect_requests_.begin(), resurrect_requests_.end());
    for (ObjectGuid g : resurrect_requests_) {
        auto pit = players_.find(g);
        if (pit == players_.end()) continue;
        Player& pl = pit->second->unit;
        ResurrectReject why = ResurrectReject::kNone;
        if (!deaths_.can_resurrect(g, pl.position(), why)) {
            const char* reason = why == ResurrectReject::kNotDead      ? "NOT_DEAD"
                                 : why == ResurrectReject::kNotReleased ? "NOT_RELEASED"
                                                                        : "TOO_FAR";
            emit(out, TickPhase::kDeath,
                 "resurrect_denied guid=" + u(g) + " reason=" + reason);
            continue;
        }
        const std::uint32_t hp = deaths_.resurrect_health(pl.max_health());
        const ObjectGuid corpse = deaths_.resurrect(g);  // clears record + corpse
        pl.resurrect(hp);
        emit(out, TickPhase::kDeath,
             "player_resurrect guid=" + u(g) + " hp=" + u(pl.health()) + "/" +
                 u(pl.max_health()) + " corpse_despawn=" + u(corpse));
    }
    resurrect_requests_.clear();
}

void MapTick::request_release(ObjectGuid guid) { release_requests_.push_back(guid); }
void MapTick::request_resurrect(ObjectGuid guid) { resurrect_requests_.push_back(guid); }

std::uint32_t MapTick::player_xp_into_level(ObjectGuid guid) const {
    auto it = players_.find(guid);
    return it == players_.end() ? 0 : it->second->xp_into_level;
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
