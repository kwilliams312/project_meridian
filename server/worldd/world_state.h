// SPDX-License-Identifier: Apache-2.0
//
// worldd — shared world state + the AoI movement relay (issue #87; the IT-M0
// "two clients see each other move" capstone).
//
// CLEAN-ROOM: designed from docs/sad/server-sad.md only — §2.5 (the world
// thread owns game state; the Grid/AoI engine; the "authoritative state →
// interest set → per-subscriber egress" flow), §8.3 IT-M0 row ("echo world
// state — movement + AoI relay only"), §5.2 (IF-2 framing + per-session AEAD),
// §6 (concurrency: game state serialized), decision D-19 (flat bootstrap map),
// and world.fbs (EntityEnter / EntityUpdate / EntityLeave / MovementState). No
// GPL source consulted. See CONTRIBUTING.md.
//
// WHAT THIS FILE IS: the piece that turns #86's authoritative MovementState into
// what the OTHER clients see. aoi_grid.{h,cpp} answers "who is in range of whom"
// (pure). WorldState is the world-thread-owned registry that
//   1. tracks every ENTERED session (its AoI id / entity guid, authoritative
//      position, type, spawn state, and an EGRESS callback that writes a frame to
//      THAT session's socket), keyed by a session slot id;
//   2. on each authoritative MovementState (from the #86 validator, delivered on
//      the world thread), updates the mover's grid position, recomputes its
//      interest set WITH HYSTERESIS, and DIFFS it against the previous set to
//      emit, per subscriber:
//        • EntityEnter — subscribers newly seeing the mover (mover's full state)
//        • EntityUpdate — subscribers already seeing the mover (position delta)
//        • EntityLeave — subscribers who no longer see the mover (OUT_OF_RANGE)
//      and, BIDIRECTIONALLY, sends the mover the matching EntityEnter/Leave for
//      the sessions that entered/left ITS view (so both ends see each other);
//   3. on world-leave / disconnect, sends EntityLeave to everyone who saw the
//      departing session and drops it from the grid.
//
// ─── EGRESS THROUGH THE ESTABLISHED WorldSession (SAD §5.2) ──────────────────
// Every relayed frame is a server→client (s2c) frame. Each subscriber has its
// OWN established WorldSession (the #84 AEAD channel keyed off its grant's
// session_key). The egress path routes each frame THROUGH that subscriber's
// WorldSession so the s2c sequence counter advances monotonically per subscriber
// (the nonce/seq the AEAD channel owns — §5.2 "nonce = direction ∥ 64-bit
// sequence counter"). At M0 the wire body is still the plaintext IF-2 frame
// inside TLS 1.3 (confidentiality/integrity from TLS; the per-session AEAD wrap
// is the documented seam that flips on when the client unseals — see
// world_dispatch.h's M0 transport note; the MOVEMENT_STATE reply to the mover
// takes the same plaintext path today). Routing through the WorldSession now
// means the seq numbering + the seal() seam are already per-subscriber correct;
// activating the wrap is a one-line change at SealEgress::emit's seam.
//
// ─── THREADING (SAD §6) ──────────────────────────────────────────────────────
// The SAD end-state runs the relay on the ONE world thread that owns all game
// state. At M0 the process scaffold serves each connection inline on its own IO
// worker (world_dispatch.cpp serve_connection), so two movers can call the relay
// from two different IO-worker threads AND a subscriber's socket is written by a
// DIFFERENT thread than the one serving it. WorldState therefore serializes ALL
// of its state mutation + egress under ONE mutex — this is the world-thread
// invariant realized as a lock until the real per-map world-thread drain lands
// (the drain is the WorldServer queue seam; #87 keeps the relay behind this lock
// so the semantics are identical when it migrates onto the thread). Each
// subscriber's egress callback also takes the target socket's write under this
// lock, so no two threads write the same net::Session concurrently.

#ifndef MERIDIAN_WORLDD_WORLD_STATE_H
#define MERIDIAN_WORLDD_WORLD_STATE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aoi_grid.h"
#include "area_triggers.h"        // AreaTriggerSet — area triggers + POI discovery (#368)
#include "combat_resolver.h"      // CombatRng — per-map seeded combat RNG (#345)
#include "combat_unit.h"          // WorldObject→Unit→Player hierarchy (#342)
#include "movement_validation.h"  // Position
#include "world_generated.h"
#include "world_session.h"        // WorldSession (AEAD s2c channel)
#include "zone_geometry.h"        // active_zone_aoi_config() — the #559 zone seam

namespace meridian::net {
class Session;  // fwd — the TLS socket (meridian/net/tls_listener.h)
}  // namespace meridian::net

namespace meridian::worldd {

// A stable per-connection slot id in the world (distinct from the AoiId / entity
// guid, so two sessions could in principle share a guid without colliding as
// slots — at M0 they are 1:1). Assigned by WorldState::enter.
using SessionSlot = std::uint64_t;

// The egress sink for ONE session: given an s2c opcode + its FlatBuffer payload,
// SEAL/SEQUENCE it through that session's established WorldSession (the s2c AEAD
// channel — §5.2: the WorldSession owns the s2c seq counter / nonce), frame it,
// and write it to that session's socket. Implemented in the serve loop, which
// owns both the socket AND the WorldSession; the relay never touches either
// directly — it only names the message (opcode + payload). Returns false if the
// write failed (peer gone) so the relay can prune a dead subscriber. Must be safe
// to call from a thread other than the one that created the session (WorldState
// holds its lock across the call — see the threading note above).
using EgressFn =
    std::function<bool(net::Opcode opcode, const std::vector<std::uint8_t>& payload)>;

// ---------------------------------------------------------------------------
// ②/T1 (#538): character visual-assembly projection relayed on a PLAYER's
// EntityEnter (client-character-assembler design §2). The client turns these ids
// into an assembled character (per-race body + worn gear + dyes) by resolving
// every id against pck content — only ids travel here. NPC/creature EntityEnter
// carries NONE of this (EntityIdentity::visual stays nullopt), so those render
// via the monolithic visual.model path. Kept as plain worldd structs (not the
// generated wire types) so the encoder owns the FlatBuffer layout in one place.
// ---------------------------------------------------------------------------

// One dyed channel on an equipped item (contract ① §6). `dye_id` is the IF-9
// NUMERIC dye id (dye@1 is pck-only, #467 — no world DB dye table), so it is the
// resolved numeric id, never a content string. Empty at M1 (no dye path yet).
struct DyeChoiceRec {
    std::uint8_t channel = 0;   // 0 = primary, 1 = secondary, 2 = accent
    std::uint32_t dye_id = 0;   // IF-9 numeric dye id (0 = unset)
};

// One VISIBLE equipped item, projected to the wire EquippedVisual. Empty
// paperdoll positions and non-visual (jewellery) slots are never included.
struct EquippedVisualRec {
    std::uint8_t slot = 0;            // items::EquipSlot id (0-based)
    std::uint32_t item_template = 0;  // IF-9 item_template id (never 0)
    std::vector<DyeChoiceRec> dyes;   // empty at M1 (no dye-application path yet)
};

// A player's appearance + worn gear, resolved once at enter-world time from the
// character row (race + §5.2 appearance JSON) and the equipment container, then
// relayed on every EntityEnter for the session. Present ONLY for player entities.
struct CharacterVisual {
    std::uint8_t race = 0;    // M0-frozen roster race id (roster.h); 0 = unset
    std::uint8_t sex = 0;     // reserved; 0 = male (M1)
    // §5.2 appearance record scalars — already normalised (AppearanceRecord bounds
    // rule) by the caller. Held as scalars so world_state.h stays free of the
    // meridian::characters dependency (the parse happens in world_dispatch.cpp).
    std::uint8_t appearance_version = 1;
    std::uint8_t hair = 1;
    std::uint8_t face = 1;
    std::uint8_t skin = 1;
    std::vector<EquippedVisualRec> equipment;  // visible slots only
};

// The spawn/type identity a session presents to observers on EntityEnter.
struct EntityIdentity {
    AoiId entity_guid = 0;   // the mover's stable id (placeholder char guid)
    std::uint32_t type_id = 0;  // entity/template kind (world.fbs EntityEnter.type_id)
    std::uint8_t char_class = 0;  // M0-frozen class id (roster.h Class; #328) — relayed
                                  // on EntityEnter so every client colors the placeholder
                                  // capsule by class. 0 = unset/unknown.
    std::string name;             // character name (#367 SOC-01) — the whisper name key
                                  // (case-insensitive) + the ChatDeliver sender_name. Empty
                                  // for the D-11 placeholder (no characters DB): such a
                                  // session is unaddressable by whisper but still chats
                                  // spatially / on channels by guid.
    // ②/T1 (#538): the visual-assembly projection, set for ANY entity that assembles
    // like a player — every PLAYER, and (npc@2, contract ①/§7, #821) an NPC that
    // carries an appearance_catalog (install_spawns copies the DB projection here).
    // nullopt for a model-only entity (every M1 NPC/creature): its EntityEnter omits
    // race/appearance/equipment, so the client renders it via the monolithic
    // visual.model path. The encoder is NOT gated on player-vs-NPC — it emits the
    // block whenever this is set.
    std::optional<CharacterVisual> visual;
};

// ---------------------------------------------------------------------------
// SOC-01 chat limits (#367). Clean-room from server SAD §2.5 + §3.8 (slow-mode
// chat rate). The say/yell RADII are spatial tuning and live in aoi_grid.h
// (kChatSayRadiusM / kChatYellRadiusM) next to the AoI radii.
// ---------------------------------------------------------------------------
// Server-enforced max chat body length (bytes). An over-length send is refused
// with ChatRejected{TOO_LONG} — no silent truncation (server is authoritative).
inline constexpr std::size_t kChatMaxTextBytes = 255;

// Chat rate class (OPS-03): max chat sends admitted per rolling 1 s per
// connection. Over-rate sends are refused with ChatRejected{RATE_LIMITED}.
inline constexpr int kChatMaxPerSecond = 5;

// ---------------------------------------------------------------------------
// ChatIntake — the per-connection chat rate gate (OPS-03 rate class; #367).
// ---------------------------------------------------------------------------
// Mirrors MovementIntake (the existing dispatcher rate mechanism): a sliding
// 1 s window that admits at most kChatMaxPerSecond sends, dropping the rest. One
// per connection (single-threaded on its IO worker, like MovementIntake), so it
// needs no locking.
class ChatIntake {
public:
    // Admit a chat send arriving at `now_ms` (a steady ms clock) if fewer than
    // kChatMaxPerSecond sends were admitted in the trailing 1000 ms; otherwise
    // drop it. Expires stamps older than the window on each call.
    bool admit(std::uint64_t now_ms);

    std::uint64_t dropped() const { return dropped_; }
    std::uint64_t admitted() const { return admitted_; }

private:
    std::deque<std::uint64_t> window_;  // admit timestamps within the last 1000 ms
    std::uint64_t dropped_ = 0;
    std::uint64_t admitted_ = 0;
};

// The outcome of a whisper (WorldState::whisper) — routed cross-session by name.
enum class ChatWhisperOutcome {
    kDelivered,      // the named in-world target received the whisper
    kNoTarget,       // no / empty target name supplied
    kTargetOffline,  // the name is not held by any in-world session
};

// ---------------------------------------------------------------------------
// SessionEgress — the per-connection s2c write channel the relay routes through.
// ---------------------------------------------------------------------------
//
// Owns nothing; borrows this connection's `net::Session` (socket) and its
// established `WorldSession` (the AEAD s2c channel — the #84 session). It exists
// to satisfy the two invariants the relay needs (SAD §5.2 / §6):
//   1. ALL writes to one socket are serialized under ONE mutex — the relay
//      writes s2c frames from OTHER threads (another mover's IO worker), while
//      the serve loop of THIS connection writes its own replies (MovementState,
//      Disconnect) on its own thread. Both go through emit()/emit_frame(), so a
//      single mutex guarantees no two threads SSL_write the same socket at once.
//   2. Every s2c frame's seq comes from the WorldSession's s2c counter and is
//      SEALED through it (the §5.2 nonce = direction ∥ seq). emit() calls
//      seal() to advance that counter and (at M0) frames the plaintext body with
//      the WorldSession-owned seq — the AEAD wrap is the documented one-line seam
//      (see world_state.h header + world_dispatch.h M0 transport note).
//
// Held by shared_ptr so the relay's captured EgressFn keeps it alive even if the
// serve loop is mid-teardown; `alive_` guards a socket that has already closed.
class SessionEgress {
public:
    SessionEgress(net::Session& sess, WorldSession& session)
        : sess_(&sess), session_(&session) {}

    // Build an IF-2 frame for (opcode, payload) with a WorldSession-owned s2c seq,
    // route it through seal(), and write it to the socket. Returns false if the
    // socket has been marked closed or the write throws (peer gone). Thread-safe.
    bool emit(net::Opcode opcode, const std::vector<std::uint8_t>& payload);

    // Write an already-encoded IF-2 frame body verbatim (used by the serve loop
    // for frames it encodes itself — HandshakeOk, MovementState, Disconnect —
    // so those writes ALSO serialize under this channel's mutex). Thread-safe.
    bool emit_frame(const std::vector<std::uint8_t>& frame);

    // Mark the socket closed so no further relay write is attempted (the serve
    // loop calls this before/after close()). Thread-safe.
    void mark_closed();

private:
    std::mutex write_mtx_;
    net::Session* sess_ = nullptr;
    WorldSession* session_ = nullptr;
    bool alive_ = true;
};

// ---------------------------------------------------------------------------
// ForcedMoveMailbox — a thread-safe one-slot hand-off for a GM `.summon` (#418).
// ---------------------------------------------------------------------------
// A `.summon` runs on the SUMMONER's IO worker but must reposition the TARGET,
// whose authoritative SessionMovementState (the #420 validator + forced-move ack
// barrier) is single-threaded on the TARGET's own IO worker. Rather than race that
// state cross-thread, the summoner POSTs the destination here (the target's grid
// position + AoI relay it updates directly under WorldState's lock, which observers
// see at once); the TARGET then DRAINs it on its own worker — at the top of its
// next MOVEMENT_INTENT and after each handled frame — and applies force_correction
// there, arming the ack barrier + sending itself the authoritative snap. This keeps
// SessionMovementState strictly single-threaded (the same discipline QuestCredit
// uses for the kill bus). Held by shared_ptr so a mid-teardown summoner keeps it
// alive; a null pending slot is the common (no-summon-queued) case.
struct ForcedMoveMailbox {
    // Post a pending forced destination (last write wins — a fresh summon overrides
    // a not-yet-drained one). Thread-safe.
    void post(const Position& dest) {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_ = dest;
    }
    // Take + clear the pending destination, if any. Thread-safe.
    std::optional<Position> take() {
        std::lock_guard<std::mutex> lk(mtx_);
        std::optional<Position> out = pending_;
        pending_.reset();
        return out;
    }
    // Non-consuming test/diagnostic peek.
    bool has_pending() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return pending_.has_value();
    }

private:
    mutable std::mutex mtx_;
    std::optional<Position> pending_;
};

// A per-session control signal the world holds so a GM command targeting ANOTHER
// session by name can reach it: `disconnect` tears the session down (`.kick`), and
// `forced_move` is the summon mailbox above. Registered via set_session_control
// after enter(); both default-empty (an un-controlled session is un-summonable /
// un-kickable — the DB-less smoke path never wires them).
using DisconnectFn = std::function<void()>;

// The result of entering a session: its slot + the EFFECTIVE entity guid the
// relay assigned it (see WorldState::enter — a 0 stub guid is replaced by a
// unique synthetic one so two D-11 placeholder sessions are distinguishable on
// the wire). The caller stamps `entity_guid` onto its MovementState so the
// mover's own echo and its relayed EntityEnter/Update carry the same id.
struct EnterResult {
    SessionSlot slot = 0;
    AoiId entity_guid = 0;
    // Area-trigger crossings the spawn position produced (#368/#396). A character
    // that logs in already standing inside a discovery volume fires it here; the
    // ENTER_WORLD handler credits any explore quest objective these satisfy.
    std::vector<TriggerEvent> triggers;
};

// The base for synthetic per-session entity guids assigned when the D-11
// placeholder guid is 0 (no characters DB / no character row). A high base keeps
// these clear of any real low-numbered character id. M0-only; at M1 every
// session has a real character guid and this path is unused.
inline constexpr AoiId kSyntheticGuidBase = 0xF000'0000'0000'0000ULL;

// The base for wire guids assigned to STATIC WORLD ENTITIES — the server-controlled
// creatures / NPCs spawned from authored spawn_point content (#486). A distinct high
// band keeps them clear of real character guids, the kSyntheticGuidBase placeholder-
// player band (0xF000…), and the map-tick creature band (creature_ai.h
// kCreatureGuidBase 0xC000…) so a spawned NPC, a placeholder player, and a MapTick
// creature never collide in the AoI grid or on the wire. Each installed spawn is
// assigned kWorldEntityGuidBase + index in install order.
inline constexpr AoiId kWorldEntityGuidBase = 0xE000'0000'0000'0000ULL;

// ---------------------------------------------------------------------------
// WorldState — the world-thread-owned session registry + AoI relay.
// ---------------------------------------------------------------------------
class WorldState {
public:
    WorldState() = default;

    WorldState(const WorldState&) = delete;
    WorldState& operator=(const WorldState&) = delete;

    // Register an entered session. Returns its slot + effective entity guid.
    // `identity` is the mover's guid+type; `spawn` its authoritative spawn
    // position; `egress` the sink that writes s2c frames to this session's client.
    // If identity.entity_guid is 0 (the D-11 placeholder stub — no characters DB),
    // a UNIQUE synthetic guid (kSyntheticGuidBase + slot) is assigned so two
    // placeholder sessions do not collide in the grid or on the wire; the assigned
    // guid is returned in EnterResult. On enter, the relay:
    //   • inserts the session into the grid at `spawn`,
    //   • computes its initial interest set (who it can already see) and sends IT
    //     an EntityEnter for each of those,
    //   • sends each of THOSE sessions an EntityEnter for the newcomer,
    // so a session that logs in next to another immediately sees it (and is seen)
    // without waiting for a move. Thread-safe.
    EnterResult enter(const EntityIdentity& identity, const Position& spawn,
                      EgressFn egress);

    // Apply an authoritative MovementState for `slot` (produced by #86): move the
    // session in the grid to `pos`, recompute its interest set with hysteresis,
    // and relay the enter/update/leave deltas both ways (see the file header).
    // `ack_seq` / `state_flags` / `server_time_ms` are echoed into the
    // EntityUpdate/MovementState fields. Returns the mover's area-trigger crossings
    // this move produced (#368/#396) so the caller can credit explore quest
    // objectives on the session path; empty if `slot` is not entered or nothing
    // crossed. Thread-safe.
    std::vector<TriggerEvent> on_movement(SessionSlot slot, const Position& pos,
                                          std::uint32_t ack_seq, std::uint32_t state_flags,
                                          std::uint64_t server_time_ms);

    // Remove a session (world-leave / disconnect): send EntityLeave{DESPAWNED} to
    // everyone who currently sees it and drop it from the grid + registry.
    // Thread-safe. No-op if `slot` is not entered.
    void leave(SessionSlot slot);

    // ── Static world entities (content spawns; NPC-01 spawn seam, #486) ──────
    // Register a server-controlled creature / NPC spawned from authored spawn_point
    // content into the AoI world so a player entering range SEES it (ENTITY_ENTER with
    // its #430 vitals + name) and can TARGET it (unit_for_guid resolves it; a spawned
    // gossip NPC's guid maps to `npc_template_id` for GOSSIP_HELLO). Unlike a session,
    // an entity has NO egress — it is a pure SUBJECT of the relay: observers are told
    // about it, it is told about nobody. It is inserted into the SAME grid as sessions
    // so the existing interest-set query finds it; it never moves at M1 (respawn/AI is
    // #346-#348), so it only ever generates EntityEnter (on an observer coming into
    // range) and EntityLeave (on leaving range). `identity.entity_guid` must be unique
    // (see kWorldEntityGuidBase); `npc_template_id` is the content id GOSSIP_HELLO /
    // kill objectives key on. Call at boot before sessions enter. Thread-safe. Returns
    // the assigned guid (== identity.entity_guid).
    AoiId add_world_entity(const EntityIdentity& identity, const UnitStats& stats,
                           std::uint32_t npc_template_id, const Position& pos);

    // The npc_template id of the spawned world entity with wire guid `guid`, or nullopt
    // if `guid` names no registered world entity (a session, a placeholder, or unknown).
    // The GOSSIP_HELLO / interaction path uses this to resolve a clicked entity's guid
    // to the npc content it should serve gossip/trainer/vendor from (#486). Thread-safe.
    std::optional<std::uint32_t> npc_template_for_guid(AoiId guid) const;

    // The authoritative WORLD position of the spawned world entity with wire guid
    // `guid`, or nullopt if `guid` names no registered world entity (a session, a
    // placeholder, or unknown). Returned BY VALUE (a copy taken under the lock) so
    // the caller never dereferences an entity pointer outside the lock. The NPC
    // interaction-range gate (GOSSIP_HELLO / quest accept + turn-in, #842) reads
    // this to reject an interaction attempted from too far. Thread-safe.
    std::optional<Position> world_entity_position(AoiId guid) const;

    // The world position of the NEAREST spawned world entity of npc_template id
    // `npc_template_id` to `from`, or nullopt if that template has NO spawned instance.
    // The interaction-range gate (#842) uses this when the client addresses an NPC by
    // its TEMPLATE id instead of a spawned world-entity guid — the greybox / pre-spawn
    // path (env-default gossip npc, or the pre-spawn 1:1 guid==template mapping). Since
    // a template can have MANY spawns, the interaction targets the closest one (you
    // interact with the copy you are standing next to). Returned BY VALUE (copied under
    // the lock). A template with no spawn returns nullopt (a genuinely un-positionable
    // pre-spawn interaction stays ungated). Thread-safe.
    std::optional<Position> nearest_world_entity_of_template(std::uint32_t npc_template_id,
                                                             const Position& from) const;

    // Test/diagnostic: how many static world entities are registered.
    std::size_t world_entity_count() const;

    // ── GM commands targeting a session (OPS-02b, #418) ──────────────────────
    // These let a GM command reach a session by name (`.summon`, `.kick`) or set a
    // session's level (`.setlevel`), all thread-safely under the world lock (the
    // cross-session s2c writes / teardown run through the same serialized egress
    // the AoI relay uses). Registered per-session by set_session_control after
    // enter(); a session that never registered controls is un-summonable/-kickable.

    // Register the control signals for `slot` (the summon mailbox + the disconnect
    // teardown closure). Call once, right after enter(). Thread-safe; no-op if
    // `slot` is not entered.
    void set_session_control(SessionSlot slot, ForcedMoveMailbox* forced_move,
                             DisconnectFn disconnect);

    // Outcome of a summon/kick by target name.
    enum class TargetOutcome {
        kApplied,        // the target was found + the effect performed
        kTargetOffline,  // no in-world session holds that name
    };

    // `.summon` — move the in-world player named `target_name` (case-insensitive) to
    // `dest`: update its grid position + relay the AoI deltas (observers see it move
    // at once), and POST `dest` to its ForcedMoveMailbox so the target arms its
    // forced-move ack barrier + snaps its own client on its next worker turn.
    // `ack_seq`/`state_flags`/`server_time_ms` are echoed into the relayed
    // EntityUpdate. Returns kTargetOffline for an unknown name. Thread-safe.
    TargetOutcome summon_to(const std::string& target_name, const Position& dest,
                            std::uint32_t ack_seq, std::uint32_t state_flags,
                            std::uint64_t server_time_ms);

    // `.kick` — signal the in-world player named `target_name` (case-insensitive) to
    // disconnect by invoking its registered DisconnectFn (Disconnect{KICKED} + AoI
    // leave, run OUTSIDE the world lock like the #326 kick-old teardown). The closure
    // is consumed (a second kick of the same session is a no-op). Returns
    // kTargetOffline for an unknown name or a session with no disconnect control.
    // Thread-safe.
    TargetOutcome disconnect_by_name(const std::string& target_name);

    // `.setlevel` — set the authoritative Unit level of the session in `slot` (the
    // combat/simulation level; #342). The caller also sets its own gate-level in its
    // ConnCtx — this keeps the world-owned Unit in step. Thread-safe. No-op (returns
    // false) if `slot` is not entered.
    bool set_unit_level(SessionSlot slot, std::uint16_t level);

    // Broadcast a VITALS_UPDATE (#430, UI-01 HUD contract) for the unit with wire
    // guid `guid` to every client that currently sees it — the subject's OWN client
    // (its player unit frame) plus every session that has it in AoI (their target /
    // nameplate frame). Reads the CURRENT vitals straight off the authoritative Unit
    // (health/max, power/max + type, level), so callers first MUTATE the Unit (a
    // combat damage/heal, a level-up stat bump) and then call this to push the delta.
    // Server-authoritative — the client DISPLAYS it, never predicts it (Principle 1).
    // Returns the recipient count (0 if `guid` is not an entered unit). Thread-safe.
    std::size_t broadcast_vitals(AoiId guid);

    // Test/diagnostic: how many sessions are currently entered.
    std::size_t session_count() const;

    // The Unit backing the session in `slot` (its combat/lifecycle state — health,
    // level, faction, alive/dead; #342). Returns nullptr if `slot` is not entered.
    //
    // OWNERSHIP: the pointer is into WorldState-owned storage and stays valid until
    // that session leaves (std::unordered_map keeps element addresses stable across
    // rehash). Per SAD §2.5/§6 a map is single-threaded — "the tick owns entity
    // state" — so this hands the owning (map/tick) thread the entity to spawn /
    // damage / kill; it is NOT a handle for another thread to race on. The combat
    // resolver (#344+) reaches a target's Unit through here.
    Unit* unit_for_slot(SessionSlot slot);
    const Unit* unit_for_slot(SessionSlot slot) const;

    // The Unit for the entity with wire guid `guid` (the CastRequest.target_guid
    // path — the combat resolver reaches a TARGET's Unit by its guid, since the
    // client names targets by guid, not by our internal slot id). Returns nullptr
    // if no entered session holds that guid. Same ownership/threading contract as
    // unit_for_slot (map-owned storage, single-threaded access).
    Unit* unit_for_guid(AoiId guid);
    const Unit* unit_for_guid(AoiId guid) const;

    // The per-map seeded combat RNG (#345, SAD §2.5 "all rolls server-side,
    // seeded-RNG unit-testable"). One RNG per map so every combat roll on this map
    // draws from a single deterministic stream owned by the (single-threaded) map;
    // the resolver's instant-resolution path draws from here. Tests that need a
    // pinned sequence construct their own CombatRng instead.
    CombatRng& combat_rng() { return combat_rng_; }

    // ── Area triggers + POI discovery (#368; WLD-01/03) ──────────────────────
    // The map tick evaluates every entered character's authoritative position
    // against the loaded trigger volumes (SAD §2.5) each time it advances — at M0
    // that advance is enter() + on_movement(), the authoritative-position seams
    // that stand in for the tick's movement phase until the #349 map tick lands.
    // A discovery crossing marks the POI on the character and sends POI_DISCOVERED
    // to that client; every enter/leave crossing also fires the server-side
    // OnAreaTrigger hook (SAD §2.5 script-hook seam).

    // Load the trigger volume set — the mcc #28 seam (placeholder → compiled world
    // data). Call once at boot before any session enters. Thread-safe.
    void load_area_triggers(std::vector<TriggerVolume> volumes);

    // Install the server-side OnAreaTrigger hook (SAD §2.5). Invoked once per
    // enter/leave crossing (including discovery) with the mover's guid + the event,
    // under the world lock. Default (unset) logs the crossing. Thread-safe to set
    // before start; intended for boot wiring + tests. Thread-safe.
    using AreaTriggerHook = std::function<void(AoiId guid, const TriggerEvent&)>;
    void set_area_trigger_hook(AreaTriggerHook hook);

    // -----------------------------------------------------------------------
    // SOC-01 chat routing (#367). Server-authoritative chat delivery over the
    // entered sessions' egress sinks — the SAME per-subscriber s2c channel the
    // AoI relay uses (SAD §2.5 "spatial chat stays here"; §3.8). At M1 worldd is
    // the in-process "world-thread manager" the v0.1 SAD routes whisper/zone to;
    // #88 re-addresses these call sites over the real bus at M3. All are
    // thread-safe (take mtx_) and a no-op for an unentered `from`.
    // -----------------------------------------------------------------------

    // SAY / YELL — SPATIAL. Deliver a ChatDeliver(channel) to `from` (its own
    // echo) and to every OTHER entered session within the channel's radius (say =
    // kChatSayRadiusM, yell = kChatYellRadiusM), found via the #87 grid's
    // within_radius visitor. `channel` must be SAY or YELL. Returns how many
    // clients received it (including the sender's echo).
    std::size_t deliver_spatial(SessionSlot from, net::ChatChannel channel,
                                const std::string& text);

    // WHISPER — DIRECTED / CROSS-SESSION. Deliver a ChatDeliver(WHISPER) to the
    // in-world session whose character name equals `target_name` (case-
    // insensitive). A session never whispers itself into being its own target
    // through the name index (a self-whisper by name still delivers — the client
    // decides how to render it). Returns kNoTarget for an empty name,
    // kTargetOffline when no in-world session holds it, else kDelivered.
    ChatWhisperOutcome whisper(SessionSlot from, const std::string& target_name,
                               const std::string& text);

    // ZONE — CHANNEL. Deliver a ChatDeliver(ZONE) to EVERY entered session
    // (including the sender). At M1 the zone/general channel membership is "all
    // in-world sessions on this shard" (one map); realm-wide-across-shards
    // membership via servicesd is M3 (SAD §3.8). Returns the recipient count.
    std::size_t deliver_channel(SessionSlot from, const std::string& text);

private:
    struct SessionRec {
        // The wire projection relayed on EntityEnter (guid + type_id + char_class).
        EntityIdentity identity;
        // The session's Unit — the authoritative simulation entity (SAD §2.5
        // WorldObject→Unit→Player). It OWNS the authoritative position (via its
        // WorldObject base) and carries the combat/lifecycle state (#342). The grid
        // is keyed by identity.entity_guid; the Unit's position mirrors what the
        // grid is updated with each movement.
        Player unit;
        std::uint32_t state_flags = 0;
        EgressFn egress;
        // The set of OTHER slots this session currently SEES (its interest set),
        // by slot id. Diffed each movement to drive enter/update/leave + resolve
        // the hysteresis band.
        std::unordered_set<SessionSlot> visible;
        // The set of static WORLD ENTITIES (content spawns, #486) this session
        // currently sees, by entity guid. Diffed each movement alongside `visible`
        // to drive their EntityEnter/Leave — kept separate because entities are not
        // sessions (no slot, no reciprocal visibility, no egress).
        std::unordered_set<AoiId> visible_entities;
        // GM-command control signals (OPS-02b, #418). `forced_move` is the summon
        // mailbox (borrowed — owned by the target's ConnCtx); `disconnect` is the
        // .kick teardown closure. Both null until set_session_control (unset on the
        // DB-less smoke path). See ForcedMoveMailbox / DisconnectFn.
        ForcedMoveMailbox* forced_move = nullptr;
        DisconnectFn disconnect;
    };

    // Move an entered `slot` to `pos` and relay the AoI enter/update/leave deltas
    // (the body of on_movement, assuming mtx_ is already held). Shared by the public
    // on_movement (mover path) and summon_to (a GM moves another session). Returns
    // the mover's area-trigger crossings. Caller holds mtx_.
    std::vector<TriggerEvent> move_session_locked(SessionSlot slot, const Position& pos,
                                                  std::uint32_t ack_seq,
                                                  std::uint32_t state_flags,
                                                  std::uint64_t server_time_ms);

    // A STATIC world entity — a server-controlled creature / NPC spawned from authored
    // content (#486). It is a pure relay SUBJECT: it lives in the grid and is reported
    // to observers, but has no egress and receives nothing. It never moves at M1.
    struct WorldEntityRec {
        EntityIdentity identity;      // wire projection (guid + type_id + name) for EntityEnter
        Creature unit;                // authoritative Unit (health/level/faction/pos, #342)
        std::uint32_t npc_template_id = 0;  // content id GOSSIP_HELLO / kill objectives key on
    };

    // Emit an EntityEnter for `subject` to the session at `to`. Caller holds mtx_.
    void send_enter(SessionRec& to, const SessionRec& subject);

    // Emit an EntityEnter / EntityLeave for a static world ENTITY `subject` to the
    // session at `to` (#486). One-directional (the entity has no egress). Caller holds mtx_.
    void send_enter_entity(SessionRec& to, const WorldEntityRec& subject);
    void send_leave_entity(SessionRec& to, const WorldEntityRec& subject, net::LeaveReason reason);

    // Diff `self`'s CURRENT interest set against its previously-seen world entities and
    // relay each entity's EntityEnter (newly in range) / EntityLeave (out of range).
    // `now_guids` is self's freshly-computed interest set (a mix of session + entity
    // guids); only the entity guids in it are considered. Caller holds mtx_.
    void relay_visible_entities(SessionRec& self, const std::unordered_set<AoiId>& now_guids);
    // Emit an EntityUpdate (position delta) for `subject` to `to`. Caller holds mtx_.
    void send_update(SessionRec& to, const SessionRec& subject, std::uint32_t ack_seq,
                     std::uint64_t server_time_ms);
    // Emit an EntityLeave for `subject` to `to`. Caller holds mtx_.
    void send_leave(SessionRec& to, const SessionRec& subject, net::LeaveReason reason);

    // Map a slot's AoiId (entity guid) → the owning slot, for translating the
    // grid's id-based interest set back to session records.
    std::optional<SessionSlot> slot_of_guid(AoiId guid) const;

    // Evaluate `self`'s current authoritative position against the trigger volumes
    // and dispatch each crossing: a first-time discovery sends POI_DISCOVERED to
    // `self`'s client; every crossing fires the OnAreaTrigger hook. Returns the
    // crossings so the caller can surface them (explore crediting, #396). Caller
    // holds mtx_.
    std::vector<TriggerEvent> fire_area_triggers(SessionRec& self);

    // Emit a ChatDeliver(channel) for (`sender_guid`, `sender_name`, `text`) to
    // the session at `to`. Caller holds mtx_. (#367)
    void send_chat(SessionRec& to, net::ChatChannel channel, AoiId sender_guid,
                   const std::string& sender_name, const std::string& text);

    mutable std::mutex mtx_;
    // The AoI grid, built on the ACTIVE zone's geometry (origin/extent/cell/radii)
    // from the single-source zone_geometry.h seam (#559) — no longer the M0
    // [0,128]/(0,0)/64 m hardcode.
    AoiGrid grid_{active_zone_aoi_config()};
    std::unordered_map<SessionSlot, SessionRec> sessions_;
    std::unordered_map<AoiId, SessionSlot> slot_by_guid_;
    // Static world entities (content spawns, #486), keyed by wire guid. Share grid_
    // with sessions; never removed at M1 (respawn is #346-#348). A guid is in EITHER
    // slot_by_guid_ (a session) OR entities_ (a spawn), never both.
    std::unordered_map<AoiId, WorldEntityRec> entities_;
    // Case-insensitive character-name → slot index, for whisper routing (#367).
    // Keyed by the lower-cased name; a session with an empty name (D-11
    // placeholder) is absent, so it is unaddressable by whisper.
    std::unordered_map<std::string, SessionSlot> slot_by_name_ci_;
    SessionSlot next_slot_ = 1;

    // Per-map combat RNG. A fixed default seed keeps a single-worker M0 boot
    // deterministic; a per-map seed derived from the MapKey is a later concern
    // (the tick loop / map manager, #349).
    CombatRng combat_rng_{0x9E3779B97F4A7C15ULL};

    // Area triggers + POI discovery (#368). Owned by the world thread; serialized
    // under mtx_ with the rest of the world state (same invariant as grid_).
    AreaTriggerSet triggers_;
    AreaTriggerHook area_trigger_hook_;
};

// ---------------------------------------------------------------------------
// Egress PAYLOAD builders (world.fbs). Public so the integration test can decode
// what the relay emits and the serve loop can share the encoder. Each returns
// JUST the FlatBuffer table bytes; the EgressFn adds the IF-2 opcode ‖ seq header
// (the seq coming from the subscriber's WorldSession s2c counter).
// ---------------------------------------------------------------------------

// EntityEnter payload for `subject` (full spawn state) — position + class (#328) +
// the #430 vitals block (health/power/level/name), read from `unit` (the subject's
// authoritative Unit) and `subject.name`.
std::vector<std::uint8_t> encode_entity_enter_payload(const EntityIdentity& subject,
                                                      const Unit& unit);

// VitalsUpdate payload (#430) for `subject_guid`, projected from `unit`'s current
// vitals (health/max, power/max + type, level). The HUD delta the relay broadcasts
// to AoI observers when combat/heal/death/level-up moves a unit's vitals.
std::vector<std::uint8_t> encode_vitals_update_payload(AoiId subject_guid, const Unit& unit);

// EntityUpdate payload for `subject_guid` (position delta).
std::vector<std::uint8_t> encode_entity_update_payload(AoiId subject_guid,
                                                       const Position& pos);

// EntityLeave payload for `subject_guid`.
std::vector<std::uint8_t> encode_entity_leave_payload(AoiId subject_guid,
                                                      net::LeaveReason reason);

// PoiDiscovered payload (world.fbs 0x9xxx) — the S→C notification that the
// character discovered `trigger_id` (in `area_id`, named by idmap `name_id`).
std::vector<std::uint8_t> encode_poi_discovered_payload(TriggerId trigger_id,
                                                        std::uint32_t area_id,
                                                        std::uint32_t name_id);

// ChatDeliver payload (#367 SOC-01): the routed channel + the author's guid/name
// + the body. Public so the chat integration test can decode what the router
// emits and the serve loop can share the encoder.
std::vector<std::uint8_t> encode_chat_deliver_payload(net::ChatChannel channel,
                                                      AoiId sender_guid,
                                                      const std::string& sender_name,
                                                      const std::string& text);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_WORLD_STATE_H
