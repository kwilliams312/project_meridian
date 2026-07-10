// SPDX-License-Identifier: Apache-2.0
//
// worldd — per-map tick orchestrator (issue #349, CMB-01/02; the epic-#18
// capstone). The single-threaded map tick the SAD §2.5 / §3.2 describe, assembled
// from the four combat modules that landed ahead of it:
//
//     AbilityStore (#343) ─┐
//     Unit model   (#342) ─┼─►  MapTick  ──►  ordered SAD §2.5 phases per tick:
//     CombatResolver(#344/5)│         drain inbound → movement/commands → AI →
//     CreatureAi   (#347/8) │         combat/auras → spawns/respawns →
//     AuraContainer(#346) ──┘         AoI delta build → flush
//
// WHAT THIS FILE IS: a self-contained, deterministic driver that runs ONE map's
// simulation forward one tick at a time, in the exact SAD §2.5 phase order, and
// emits a byte-stable event stream. It is BOTH:
//
//   1. the seam that CLOSES the #354 deferral — cast-time resolution
//      (`CastResult` on cast completion) and resource-spend-for-casts are resolved
//      here in the combat/auras phase (`CombatSession::take_completed` → spend
//      resource → `resolve_ability` → apply aura effects). Instants still resolve
//      inline in the drain-inbound/command phase, exactly as the live dispatch
//      handler does today (world_dispatch.cpp CAST_REQUEST);
//
//   2. the substrate for the seeded GOLDEN SIM-HARNESS scenarios (#349) that prove
//      the epic end-to-end: with a fixed CombatRng seed the whole tick is
//      byte-stable, so the golden test gates in CI — any drift in an attack-table
//      band, a damage/heal formula, an aura cadence, an AI transition, or the
//      phase ORDER flips a golden line and fails loudly.
//
// PURE / DB-FREE / SOCKET-FREE / CLOCK-FREE: like creature_ai's tick() and
// aura_container's tick(), MapTick reads NO wall clock (its map-tick clock is
// accumulated from `dt_ms`), touches NO socket, DB, or FlatBuffer, and rolls only
// the seeded CombatRng. So a given (seed, spawn set, command stream, dt) yields
// the same event stream on every platform — the whole module runs in the plain
// `server` ctest with no MariaDB (mirrors aoi_grid / ability_store / creature_ai).
//
// THREADING (SAD §2.5/§6): a map is single-threaded by construction — "the tick
// owns entity state". MapTick owns its combatants + creatures + auras + the
// per-map RNG and carries NO lock of its own; the map that owns it is the single
// caller. The multi-worker map manager (SAD §2.5 "recast at M3") hosts one MapTick
// per active map, each pinned to one worker.
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md §2.5 (the per-map tick order),
// §3.2 (the one-map-tick sequence: "drain inbound … AI update … combat + auras
// (casts complete, periodics, deaths) … spawn/respawn timers … AoI pass …
// enqueue outbound"), §3.3 (the D-10 cast lifecycle), §8.1 (the 20 Hz / 40 ms soft
// budget), and the #342-#348 module headers ONLY. No GPL / AGPL / CMaNGOS /
// TrinityCore / leaked emulator source consulted — every phase, ordering, and
// event here is ORIGINAL, derived from OUR SAD. See CONTRIBUTING.md.

#ifndef MERIDIAN_WORLDD_MAP_TICK_H
#define MERIDIAN_WORLDD_MAP_TICK_H

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ability_store.h"    // AbilityStore / Ability / AbilityId
#include "aura_container.h"    // AuraContainer
#include "combat_resolver.h"   // CombatRng / CombatSession / resolve_ability
#include "combat_unit.h"       // Unit / Player / ObjectGuid / UnitStats
#include "creature_ai.h"       // CreatureAi / CreatureSpawnDef / AiState
#include "death_state.h"       // DeathStateMachine (CMB-03 #359)
#include "leveling.h"          // xp_for_kill / grant_xp (CHR-03 #360)
#include "movement_damage.h"   // MovementDamageState / MovementEnv / MovementDamageParams (#362)
#include "movement_validation.h"  // Position

#include "inventory.h"        // meridian::items::Inventory (loot → inventory transfer, #366)
#include "loot_roll.h"        // meridian::loot::LootRng (ITM-02 #369)
#include "loot_session.h"     // meridian::loot::LootSession / QuestPredicate (ITM-02 #369)
#include "loot_table.h"       // meridian::loot::LootTableStore / PlaceholderLootTableStore

namespace meridian::worldd {

// The map tick rate + soft budget (SAD §8.1: "50 ms hard / 40 ms soft per map",
// 20 Hz). One tick advances the map-tick clock by kTickDtMs.
inline constexpr std::uint32_t kTickHz = 20;
inline constexpr std::uint32_t kTickDtMs = 1000 / kTickHz;   // 50 ms (20 Hz)
inline constexpr std::uint32_t kTickSoftBudgetMs = 40;       // soft budget (overrun-logged)

// One inbound ability-use command — the tick's "drain inbound" unit for combat
// (the world.fbs CastRequest lifted off the wire; here a plain struct so the
// harness + the future dispatch seam feed the tick identically).
struct AbilityUseCmd {
    ObjectGuid   caster_guid = 0;
    AbilityId    ability_id = 0;
    ObjectGuid   target_guid = 0;   // 0 = self / no explicit target
};

// The phase of the tick that produced an event (SAD §2.5 order). Recorded so the
// golden stream shows WHERE in the tick each effect happened.
enum class TickPhase : std::uint8_t {
    kInbound = 0,   // drain inbound + command processing (instant resolution)
    kAi = 1,        // creature AI (threat/aggro/leash/evade/respawn/patrol)
    kCombat = 2,    // cast completion + resource spend + resolution
    kAura = 3,      // periodic DoT/HoT ticks + expiry
    kDeath = 4,     // spawns/respawns: player death FSM (release/corpse-run/resurrect)
};

const char* tick_phase_name(TickPhase p);

// The typed CLASS of a tick event, so the world loop can consume a structured
// event without parsing `text`. Most events are kGeneric (their meaning lives in
// the byte-stable `text`); a few carry structured payload the world loop routes.
enum class TickEventKind : std::uint8_t {
    kGeneric = 0,       // meaning is in `text` only (the default for every event)
    kCreatureKill = 1,  // a creature died to a player — carries killer_guid + npc_template_id
                        // (the QST-01 event-bus seam: the world loop credits the KILLER's
                        // session quest log; MapTick no longer owns quest state).
    kVitalsChanged = 2, // a player's authoritative vitals changed in the tick (primarily a
                        // level-up from a kill, CHR-03 #360) — carries `vitals` (the UI-01
                        // event-bus seam #437: the world loop MIRRORS the new vitals onto
                        // the owning session's WorldState unit and pushes a VITALS_UPDATE to
                        // it + its AoI observers; MapTick itself touches no socket).
};

// The post-change authoritative vitals a kVitalsChanged event carries (#437, UI-01
// HUD). All-ABSOLUTE values (the state AFTER the change), so the world loop can set
// exactly these on the WorldState session unit — no delta math. A level-up tops the
// player off, so health == max_health and power == max_power for that case.
struct VitalsSnapshot {
    ObjectGuid    guid = 0;         // the subject player's guid
    std::uint16_t level = 0;        // level after the change
    std::uint32_t health = 0;       // current health after the change
    std::uint32_t max_health = 0;   // health cap after the change
    std::uint32_t power = 0;        // current secondary resource after the change
    std::uint32_t max_power = 0;    // secondary-resource cap after the change
};

// One deterministic tick event. `text` is the byte-stable golden line; the typed
// fields let a test assert programmatically without parsing. The event stream a
// tick returns is the map's "AoI delta build → flush" seam in miniature — the set
// of state changes an observer would be sent this tick.
struct TickEvent {
    std::uint64_t tick = 0;       // 1-based tick index
    std::uint64_t now_ms = 0;     // map-tick clock at the START of this tick
    TickPhase     phase = TickPhase::kInbound;
    std::string   text;           // deterministic "kind key=val …" content

    // Structured payload (kGeneric leaves these zero). The world loop reads these
    // on a kCreatureKill to route the kill to the killer's session (see MapTick's
    // on_unit_died / world_dispatch route_tick_events). NOT part of to_line() — the
    // golden stream is unchanged by their presence.
    TickEventKind kind = TickEventKind::kGeneric;
    ObjectGuid    killer_guid = 0;        // kCreatureKill: the crediting player's guid
    std::uint32_t npc_template_id = 0;    // kCreatureKill: the victim creature's template id

    // kVitalsChanged (#437): the post-change authoritative vitals the world loop
    // mirrors onto the subject's WorldState unit + broadcasts. Zero for other kinds.
    // NOT part of to_line() — the golden stream is unchanged by its presence.
    VitalsSnapshot vitals{};

    std::string to_line() const;  // "t=<tick> now=<ms> <phase> <text>"
};

// ---------------------------------------------------------------------------
// MapTick — the single-threaded per-map tick orchestrator.
// ---------------------------------------------------------------------------
class MapTick {
public:
    // `abilities` is the read-only ability store (borrowed; outlives the tick).
    // `rng_seed` seeds the per-map CombatRng (SAD §2.5 seeded rolls). `dt_ms` is
    // the per-tick step (defaults to the 20 Hz cadence; a scenario may coarsen it).
    MapTick(const AbilityStore& abilities, std::uint64_t rng_seed,
            std::uint32_t dt_ms = kTickDtMs);

    MapTick(const MapTick&) = delete;
    MapTick& operator=(const MapTick&) = delete;

    // --- population ---------------------------------------------------------
    // Add a player-controlled combatant at `pos` with `stats`. Returns its guid
    // (the caller-supplied `guid`). The player gets a CombatSession (GCD/cast
    // clock) and an AuraContainer. Players are fed to the AI phase as aggro targets.
    // `char_class` is the roster.h class id (0 = unset) — it selects the level
    // curve used for level-up stat growth (CHR-03 #360).
    ObjectGuid add_player(ObjectGuid guid, const Position& pos, const UnitStats& stats,
                          std::uint8_t char_class = 0);

    // Spawn a server creature from `def` via the AI. Returns its assigned guid.
    ObjectGuid add_creature(const CreatureSpawnDef& def);

    // Move a player (the "movement/commands" phase input — the harness sets the
    // authoritative position the AI phase reads as an aggro target this tick, and
    // the #362 fall/swim evaluator reads as the tick's position sample). For a ghost
    // (a released dead player) this is the corpse-run position (#359).
    void set_player_position(ObjectGuid guid, const Position& pos);

    // Enable quest-kill REPORTING (QST-01 #371, event-bus). When on, a creature
    // that dies to a player emits a typed kCreatureKill TickEvent (killer guid +
    // victim template id) the world loop routes to the KILLER's session — which
    // owns the authoritative quest log (ctx.quests) and applies on_kill. MapTick
    // itself no longer owns any quest state (the owner-signed-off decision on epic
    // #20: no shared mutable ownership between MapTick and sessions). Left OFF (the
    // default), no kill event is emitted, so existing combat/death golden scenarios
    // are byte-identical.
    void set_report_kills(bool on) { report_kills_ = on; }

    // Enable VITALS egress REPORTING (UI-01 HUD, event-bus #437). When on, a
    // MapTick-side vitals change (primarily a level-up from a kill, CHR-03 #360) tags
    // the level-up TickEvent as kVitalsChanged carrying the NEW authoritative vitals
    // — the world loop mirrors them onto the leveler's WorldState session unit and
    // pushes a VITALS_UPDATE to it + its AoI observers (WorldState::broadcast_vitals),
    // so the HUD reflects the new level / max-health / max-power at once instead of
    // lagging until a later live-path delta. Left OFF (the default), the level-up
    // event stays kGeneric; its byte-stable text is IDENTICAL either way (the typed
    // `vitals` fields are invisible to to_line()), so the no-egress combat/death
    // golden scenarios are byte-identical — exactly like set_report_kills (#397).
    void set_report_vitals(bool on) { report_vitals_ = on; }

    // The map's graveyard — where a released ghost is sent (CMB-03 #359). A single
    // per-map point for M1; the "nearest graveyard from world data" lookup is the
    // documented seam (content epic #28). Defaults to the origin.
    void set_graveyard(const Position& pos) { graveyard_ = pos; }

    // --- loot (ITM-02 #369) --------------------------------------------------
    // The loot-table source a dying creature rolls on. Defaults to the M1
    // placeholder set (loot_table.cpp) so loot works out of the box; a boot path
    // or a test overrides it (the mcc #28 world-DB store implements the same seam).
    // `store` must outlive the tick.
    void set_loot_tables(const loot::LootTableStore& store) { loot_tables_ = &store; }

    // The loot session on a corpse (the dead creature's guid), or nullptr if that
    // corpse has no loot / was never rolled / is fully looted-and-cleared. A worldd
    // loot handler (or a test) reads/mutates it to serve a client's loot pull.
    const loot::LootSession* loot_session(ObjectGuid corpse_guid) const;
    loot::LootSession* loot_session_mut(ObjectGuid corpse_guid);
    std::size_t loot_session_count() const { return loot_sessions_.size(); }

    // Convenience for the loot handler / tests: pull one slot of the corpse's loot
    // into `inv`, applying the loot session's full server-side validation
    // (ownership / in-range / not-already-looted / quest gate) and the inventory
    // add. `looter_pos` is the player's authoritative position (the range check
    // vs the corpse). Throws loot::NoSuchCorpse if the corpse has no session, else
    // the same loot::/items:: errors the session throws. On the final shared pull
    // the session is dropped (corpse fully looted → despawn).
    loot::LootStack take_loot(ObjectGuid corpse_guid, ObjectGuid looter,
                              const Position& looter_pos, std::size_t slot,
                              const loot::QuestPredicate& has_quest, items::Inventory& inv);

    // --- death-flow requests (C→S; drained in the death phase) --------------
    // Request an early graveyard release for a dead player (C→S RELEASE_REQUEST,
    // #359). No-op unless the player is a just-died corpse. Applied next advance().
    void request_release(ObjectGuid guid);
    // Request resurrection at the player's corpse (C→S RESURRECT_REQUEST, #359).
    // Succeeds next advance() only if the corpse-run is complete. Else denied.
    void request_resurrect(ObjectGuid guid);

    // --- environment (fall/swim, #362) --------------------------------------
    // The map's ground/water environment the movement-damage evaluator samples each
    // tick. Defaults to the M0 flat bootstrap map (ground = kFlatGroundZ, no water),
    // so on a flat map players never take fall damage from an ordinary jump and never
    // drown. An M1 zone (or a test) sets a heightfield ground / water surface here.
    void set_environment(const MovementEnv& env) { env_ = env; }
    const MovementEnv& environment() const { return env_; }

    // The fall/swim tuning applied to players added AFTER this call (each player's
    // MovementDamageState is constructed with the params current at add_player time).
    // Defaults to the production curve; a test tightens the numbers for a fast run.
    void set_movement_damage_params(const MovementDamageParams& params) {
        move_dmg_params_ = params;
    }

    // --- inbound ------------------------------------------------------------
    // Queue an ability use to be drained on the NEXT advance() (SAD §3.2 "drain
    // inbound"). Processed in tick order; instants resolve that tick, cast-time
    // abilities arm a cast timer that completes a later tick.
    void enqueue_cast(const AbilityUseCmd& cmd);

    // --- the tick ------------------------------------------------------------
    // Run ONE map tick in SAD §2.5 order and return the events it produced (also
    // appended to the running log()). The map-tick clock advances by dt_ms.
    std::vector<TickEvent> advance();
    // Run `ticks` ticks (convenience for scenarios); events accumulate in log().
    void advance(int ticks);

    // Whether the map has anything to simulate (any player or creature). The world
    // thread uses this to skip the tick body on a truly idle map (SAD §2.5
    // "inactive grids do not tick").
    bool has_simulation() const;

    // --- introspection -------------------------------------------------------
    std::uint64_t now_ms() const { return now_ms_; }
    std::uint64_t tick_count() const { return tick_no_; }
    const std::vector<TickEvent>& log() const { return log_; }
    std::string log_text() const;  // newline-joined to_line() — the golden blob

    Unit* unit_for_guid(ObjectGuid guid);
    CreatureAi& ai() { return ai_; }
    CombatRng& rng() { return rng_; }
    // The player death state machine (CMB-03 #359) — corpse/ghost/release queries.
    const DeathStateMachine& deaths() const { return deaths_; }
    // A player's XP accumulated toward the next level (CHR-03 #360). 0 if unknown.
    std::uint32_t player_xp_into_level(ObjectGuid guid) const;

private:
    // A player combatant: its Unit + per-session combat clock + aura container +
    // XP progress toward the next level (CHR-03 #360). Heap-boxed so the
    // AuraContainer's Unit& binding stays valid across rehash.
    struct PlayerCombatant {
        Player              unit;
        CombatSession       combat;
        AuraContainer       auras;
        std::uint32_t       xp_into_level = 0;  // XP toward the next level (resets on level-up, #360)
        MovementDamageState move_dmg;           // per-player fall/breath tracker (#362)
        PlayerCombatant(Player u, const MovementDamageParams& mdp)
            : unit(std::move(u)), auras(unit), move_dmg(mdp) {}
    };

    // --- SAD §2.5 phases (each appends to `out`) ----------------------------
    void phase_drain_inbound(std::vector<TickEvent>& out);
    void phase_ai(std::vector<TickEvent>& out);
    void phase_combat_auras(std::vector<TickEvent>& out);
    // Spawns/respawns phase: advance the player death FSM (auto-release timers,
    // drain release/resurrect requests, corpse-run resurrect). CMB-03 #359.
    void phase_death(std::vector<TickEvent>& out);

    // THE SINGLE "unit died" hook (every death source consumes it): the direct-
    // damage path (resolve_and_log), the periodic path (phase_combat_auras), and
    // the fall/drown path (phase_movement_damage, #362). Emits one death event
    // ("by=<by_label>"), then dispatches the two cleanly-separated concerns: a dead
    // PLAYER enters the death state machine (#359); a dead CREATURE awards kill XP
    // to its killer (#360). `killer_guid` drives XP attribution (0 = none/periodic/
    // environment → resolved via the threat table); `by_label` is the death event's
    // "by=" text ("<guid>" / "PERIODIC" / "FALL" / "DROWN").
    void on_unit_died(ObjectGuid victim_guid, Unit& victim, ObjectGuid killer_guid,
                      const std::string& by_label, TickPhase phase,
                      std::vector<TickEvent>& out);
    // #359: spawn the corpse + enter the death FSM for a dead player.
    void handle_player_death(ObjectGuid victim_guid, Unit& victim, TickPhase phase,
                             std::vector<TickEvent>& out);
    // #360: award XP to the killer player (resolved directly or via threat).
    void award_kill_xp(ObjectGuid victim_guid, Unit& victim, ObjectGuid killer_guid,
                       TickPhase phase, std::vector<TickEvent>& out);
    // #369 (ITM-02): roll the dead creature's loot table (if any) into a loot
    // session on its corpse. The eligible looters are the resolved killer player
    // (direct or top-threat, mirroring award_kill_xp attribution). Uses a SEPARATE
    // seeded loot RNG (never the combat rng_, so combat's byte-stable stream is
    // unperturbed), reseeded per corpse so the roll is a pure function of (map
    // seed, victim guid). Emits one deterministic `loot_roll` event when it drops.
    void roll_creature_loot(ObjectGuid victim_guid, Unit& victim, ObjectGuid killer_guid,
                            TickPhase phase, std::vector<TickEvent>& out);
    // The player guid credited with a creature kill (direct killer if a player,
    // else the top threat holder). 0 if no player is attributable.
    ObjectGuid resolve_killer_player(ObjectGuid victim_guid, ObjectGuid killer_guid) const;

    // Fall/swim environmental damage (#362), run inside the combat phase: evaluate
    // each player's fall/breath tracker against its current position + this tick's
    // dt, and apply any fall/drowning damage via Unit::apply_damage. Emits a kCombat
    // event ONLY when damage is dealt (so a flat-ground map produces no events and
    // the combat golden stream is unaffected). Iterates ascending guid (determinism).
    void phase_movement_damage(std::vector<TickEvent>& out);

    // Resolve one ability against a target: spend the resource, roll the attack
    // table + apply direct damage/heal (resolve_ability), apply any aura effects to
    // the target's container, and feed resolver threat back into the AI. Shared by
    // the instant path (drain-inbound) and the cast-completion path (combat).
    void resolve_and_log(const Ability& ability, Unit& caster, ObjectGuid caster_guid,
                         Unit& target, ObjectGuid target_guid, TickPhase phase,
                         std::vector<TickEvent>& out);

    // Emit one event (stamped with the current tick + clock).
    void emit(std::vector<TickEvent>& out, TickPhase phase, std::string text);

    // Emit a typed kCreatureKill event (QST-01 event-bus): the byte-stable text is
    // the same "quest_kill killer=<g> npc=<t>" line, and the structured fields let
    // the world loop route the credit to the killer's session without parsing.
    void emit_kill(std::vector<TickEvent>& out, TickPhase phase, ObjectGuid killer,
                   std::uint32_t npc_template_id);

    // The AuraContainer for any guid (player or creature), created on demand for a
    // creature. nullptr if the guid names no live unit.
    AuraContainer* auras_for(ObjectGuid guid);

    const AbilityStore& abilities_;
    CombatRng           rng_;
    std::uint32_t       dt_ms_;
    std::uint64_t       now_ms_ = 0;
    std::uint64_t       tick_no_ = 0;

    bool report_kills_ = false;  // QST-01 event-bus: emit kCreatureKill on a creature death
    bool report_vitals_ = false; // UI-01 event-bus (#437): tag a level-up as kVitalsChanged
    CreatureAi ai_;  // server creatures (owns Creature + AI state + respawn timers)
    DeathStateMachine deaths_;  // dead players' death flow + corpses (CMB-03 #359)
    Position graveyard_;        // per-map release destination (world-data seam, #359)
    std::unordered_map<ObjectGuid, std::unique_ptr<PlayerCombatant>> players_;
    std::unordered_map<ObjectGuid, std::unique_ptr<AuraContainer>> creature_auras_;
    std::unordered_map<ObjectGuid, AiState> prev_ai_state_;  // AI transition edges
    std::vector<AbilityUseCmd> inbound_;
    std::vector<ObjectGuid> release_requests_;    // C→S RELEASE_REQUEST queue (#359)
    std::vector<ObjectGuid> resurrect_requests_;  // C→S RESURRECT_REQUEST queue (#359)
    std::vector<TickEvent> log_;

    // Fall/swim (#362): the map's environment + the tuning new players inherit.
    MovementEnv          env_;               // flat ground, no water (M0 default)
    MovementDamageParams move_dmg_params_;   // production curve by default

    // Loot (ITM-02 #369): the loot-table seam (owned placeholder default,
    // overridable via set_loot_tables), a SEPARATE seeded loot RNG (never perturbs
    // the combat rng_), and the live loot sessions keyed by corpse (dead-creature)
    // guid. A creature death rolls its table into a session here; a client loots
    // from it with server validation (loot_session.h).
    std::unique_ptr<loot::PlaceholderLootTableStore> owned_loot_tables_;
    const loot::LootTableStore* loot_tables_;   // -> owned_loot_tables_ unless overridden
    std::uint64_t             loot_seed_;        // base seed for per-corpse loot rolls
    loot::LootRng             loot_rng_;         // reseeded per corpse (determinism)
    std::unordered_map<ObjectGuid, loot::LootSession> loot_sessions_;
};

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_MAP_TICK_H
