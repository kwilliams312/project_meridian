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

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "aoi_grid.h"
#include "combat_unit.h"          // WorldObject→Unit→Player hierarchy (#342)
#include "movement_validation.h"  // Position
#include "world_generated.h"
#include "world_session.h"        // WorldSession (AEAD s2c channel)

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

// The spawn/type identity a session presents to observers on EntityEnter.
struct EntityIdentity {
    AoiId entity_guid = 0;   // the mover's stable id (placeholder char guid)
    std::uint32_t type_id = 0;  // entity/template kind (world.fbs EntityEnter.type_id)
    std::uint8_t char_class = 0;  // M0-frozen class id (roster.h Class; #328) — relayed
                                  // on EntityEnter so every client colors the placeholder
                                  // capsule by class. 0 = unset/unknown.
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

// The result of entering a session: its slot + the EFFECTIVE entity guid the
// relay assigned it (see WorldState::enter — a 0 stub guid is replaced by a
// unique synthetic one so two D-11 placeholder sessions are distinguishable on
// the wire). The caller stamps `entity_guid` onto its MovementState so the
// mover's own echo and its relayed EntityEnter/Update carry the same id.
struct EnterResult {
    SessionSlot slot = 0;
    AoiId entity_guid = 0;
};

// The base for synthetic per-session entity guids assigned when the D-11
// placeholder guid is 0 (no characters DB / no character row). A high base keeps
// these clear of any real low-numbered character id. M0-only; at M1 every
// session has a real character guid and this path is unused.
inline constexpr AoiId kSyntheticGuidBase = 0xF000'0000'0000'0000ULL;

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
    // EntityUpdate/MovementState fields. No-op if `slot` is not entered.
    // Thread-safe.
    void on_movement(SessionSlot slot, const Position& pos, std::uint32_t ack_seq,
                     std::uint32_t state_flags, std::uint64_t server_time_ms);

    // Remove a session (world-leave / disconnect): send EntityLeave{DESPAWNED} to
    // everyone who currently sees it and drop it from the grid + registry.
    // Thread-safe. No-op if `slot` is not entered.
    void leave(SessionSlot slot);

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
    };

    // Emit an EntityEnter for `subject` to the session at `to`. Caller holds mtx_.
    void send_enter(SessionRec& to, const SessionRec& subject);
    // Emit an EntityUpdate (position delta) for `subject` to `to`. Caller holds mtx_.
    void send_update(SessionRec& to, const SessionRec& subject, std::uint32_t ack_seq,
                     std::uint64_t server_time_ms);
    // Emit an EntityLeave for `subject` to `to`. Caller holds mtx_.
    void send_leave(SessionRec& to, const SessionRec& subject, net::LeaveReason reason);

    // Map a slot's AoiId (entity guid) → the owning slot, for translating the
    // grid's id-based interest set back to session records.
    std::optional<SessionSlot> slot_of_guid(AoiId guid) const;

    mutable std::mutex mtx_;
    AoiGrid grid_;
    std::unordered_map<SessionSlot, SessionRec> sessions_;
    std::unordered_map<AoiId, SessionSlot> slot_by_guid_;
    SessionSlot next_slot_ = 1;
};

// ---------------------------------------------------------------------------
// Egress PAYLOAD builders (world.fbs). Public so the integration test can decode
// what the relay emits and the serve loop can share the encoder. Each returns
// JUST the FlatBuffer table bytes; the EgressFn adds the IF-2 opcode ‖ seq header
// (the seq coming from the subscriber's WorldSession s2c counter).
// ---------------------------------------------------------------------------

// EntityEnter payload for `subject` (full spawn state).
std::vector<std::uint8_t> encode_entity_enter_payload(const EntityIdentity& subject,
                                                      const Position& pos);

// EntityUpdate payload for `subject_guid` (position delta).
std::vector<std::uint8_t> encode_entity_update_payload(AoiId subject_guid,
                                                       const Position& pos);

// EntityLeave payload for `subject_guid`.
std::vector<std::uint8_t> encode_entity_leave_payload(AoiId subject_guid,
                                                      net::LeaveReason reason);

}  // namespace meridian::worldd

#endif  // MERIDIAN_WORLDD_WORLD_STATE_H
