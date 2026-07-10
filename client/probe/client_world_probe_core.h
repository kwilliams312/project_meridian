// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — HEADLESS client-world probe core (issue #301). The engine-free
// driver of the GUI client's WORLD-SESSION path: it runs the EXACT net stack
// scenes/world/world.gd uses — the #97/#279 net thread (net::NetThreadCore, the
// worldd socket on its own std::thread + the lock-free SPSC ring) fed the #99
// WorldHello frame from a real authd login, decoding the #87 AoI relay frames with
// the SHARED clientnet entity codec (#301), and handing each remote entity to the
// #104 remote interpolator — with NO Godot and NO display.
//
// This is the headless equivalent of world.gd's per-tick loop:
//   world.gd (Godot)                         this probe core (headless)
//     MeridianLogin.login()          ⇔        login::run_login (TlsLoginTransport)
//     MeridianNetThread.connect_to_world ⇔     NetThreadCore.start(TcpTransport)
//     _physics_process → pump()      ⇔        drain the inbound ring each tick
//     entity_frame → decode_entity_frame ⇔     clientnet::codec::decode_entity_*
//       → interpolator.on_entity_*   ⇔        remote::RemoteInterpolator.on_*
//     send_movement_intent()         ⇔        NetThreadCore.send(kMovement)
// so a green probe run proves the SAME code path that renders the bot in the GUI
// receives the bot's EntityEnter + EntityUpdate stream. The live proof is
// client/test/run_client_sees_bot_it.sh (one bot + this probe vs real authd+worldd).
//
// apply_entity_frame() is the ONE handoff step (decode a relay frame → interpolator
// + sighting log) that world.gd mirrors in GDScript; it is unit-tested in isolation
// (client/probe/test) so the decode→interpolator wiring is proven without a socket.

#ifndef MERIDIAN_CLIENT_WORLD_PROBE_CORE_H
#define MERIDIAN_CLIENT_WORLD_PROBE_CORE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "meridian/clientnet/codec.h"         // clientnet::codec::* (char-select round-trips)
#include "meridian/clientnet/framing.h"       // clientnet::Bytes
#include "remote_interpolation.h"             // remote::RemoteInterpolator

namespace meridian::probe {

using Bytes = meridian::clientnet::Bytes;

// A captured inbound AoI observation about another entity (the bot the client sees).
// Position carries the WIRE (Z-UP) frame from world.fbs: x/y = ground plane,
// z = height.
enum class SightingKind : std::uint8_t { kEnter = 0, kUpdate = 1, kLeave = 2 };

struct EntitySighting {
    SightingKind kind = SightingKind::kEnter;
    std::uint64_t entity_guid = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;  // wire coords (present for enter/update)
    bool has_position = false;
    std::uint16_t leave_reason = 0;      // world.fbs LeaveReason (kLeave only)
};

// The honest report of one probe run — the mirror of the bot's BotRunResult, but for
// the GUI net path. Says exactly how far the client got + what it saw.
struct ProbeResult {
    bool login_ok = false;          // authd login succeeded (grant received)
    std::string login_detail;       // login failure reason (empty on success)

    bool connect_failed = false;    // net thread could not reach/enter worldd
    bool entered_world = false;     // HandshakeOk received (reached CHARACTER-SELECT)
    bool spawned = false;           // EnterWorldResponse OK — actually in world (D-35/#341)
    std::uint16_t enter_world_status = 0xFFFF;  // last EnterWorldStatus (0xFFFF = none)
    std::uint64_t character_id = 0; // the owned character we entered as (0 = none)
    bool disconnected = false;      // server sent a Disconnect
    std::uint16_t disconnect_reason = 0;  // world.fbs DisconnectReason
    bool transport_closed = false;  // peer closed mid-session
    std::string detail;             // human-readable note (logs / failure reason)

    std::uint32_t intents_sent = 0;        // MovementIntents queued to the net thread
    std::uint32_t movement_states = 0;     // authoritative MovementStates consumed
    std::uint32_t entity_frames = 0;       // EntityEnter/Update/Leave frames consumed

    // Every entity frame seen, in order, + per-guid tallies (the see-a-bot-move
    // evidence). The interpolator ends tracking `tracked_remote` distinct peers.
    std::vector<EntitySighting> sightings;
    std::unordered_map<std::uint64_t, std::uint32_t> enters_by_guid;
    std::unordered_map<std::uint64_t, std::uint32_t> updates_by_guid;
    std::unordered_map<std::uint64_t, std::uint32_t> leaves_by_guid;
    std::uint32_t tracked_remote = 0;      // interp.tracked_count() at the end

    std::size_t distinct_entities_seen() const { return enters_by_guid.size(); }
    std::uint32_t total_updates_seen() const {
        std::uint32_t n = 0;
        for (const auto& kv : updates_by_guid) n += kv.second;
        return n;
    }
    // The headline the harness asserts: the client saw a peer ENTER and MOVE.
    bool saw_a_peer_move() const {
        return distinct_entities_seen() > 0 && total_updates_seen() > 0;
    }
};

// THE handoff step (unit-tested; world.gd mirrors it in GDScript). Decode one inbound
// IF-2 entity-relay frame body (`opcode` + FlatBuffer `payload`) with the shared
// clientnet codec, apply it to `interp` (on_enter/on_update/on_leave) and append an
// EntitySighting + bump the per-guid tally in `res`. `recv_server_ms` is the
// interpolation-clock timestamp to buffer the snapshot under (client receipt time at
// M0, since EntityEnter/Update carry no server time; the interpolator only needs a
// monotonic per-entity time to lerp between). Returns true iff a well-formed entity
// frame was applied (opcode was EntityEnter/Update/Leave and it verified).
bool apply_entity_frame(remote::RemoteInterpolator& interp, ProbeResult& res,
                        std::uint16_t opcode, const Bytes& payload,
                        std::uint64_t recv_server_ms);

// -----------------------------------------------------------------------------
// Character-select → ENTER_WORLD sequencer (server-authoritative, D-35 / #341).
// -----------------------------------------------------------------------------
// After HandshakeOk the world session is at CHARACTER-SELECT, NOT spawned — worldd no
// longer fabricates a placeholder; a session spawns ONLY when it sends an explicit
// ENTER_WORLD_REQUEST naming an OWNED character and worldd replies EnterWorldStatus OK
// (server/worldd/world_dispatch.cpp — the single spawn seam). This tiny state machine
// drives that handshake exactly as client/bot/bot_core.cpp sequences it and
// scenes/charselect/char_select.gd gates its world-scene transition:
//
//   HandshakeOk ─▶ [forced id? ENTER_WORLD] else CharListRequest
//   CharListResponse ─▶ roster non-empty? ENTER_WORLD(first) : CharCreateRequest
//   CharCreateResponse(OK) ─▶ ENTER_WORLD(minted id)     (else: fail)
//   EnterWorldResponse(OK) ─▶ SPAWNED                    (else: fail — stay at select)
//
// It is engine-free AND socket-free: run_probe feeds it decoded inbound messages and
// sends the frames it returns, but the whole enter-world contract is unit-tested with
// NO server (client/probe/test), mirroring how apply_entity_frame is tested in isolation.
enum class EnterPhase : std::uint8_t {
    kConnecting = 0,  // pre-HandshakeOk (session establishing)
    kListing    = 1,  // CharListRequest sent — awaiting the roster
    kCreating   = 2,  // CharCreateRequest sent — awaiting the minted id
    kEntering   = 3,  // ENTER_WORLD_REQUEST sent — awaiting the typed result
    kSpawned    = 4,  // EnterWorldResponse OK — in world (the ONE spawn seam)
    kFailed     = 5,  // terminal: rejected create/enter, or empty roster w/o create
};

// The outcome of feeding the sequencer one inbound message.
struct EnterStep {
    Bytes frame;             // request frame to SEND now (empty ⇒ nothing to send)
    bool spawned = false;    // just transitioned to in-world (EnterWorld OK)
    bool failed = false;     // just reached a terminal failure
    std::string detail;      // human note (failure reason / spawn confirmation)
};

class EnterWorldSequencer {
public:
    // forced_character_id != 0 enters THAT owned character directly (deterministic,
    // like bot_core's force_character_id) — no CharList round-trip. 0 ⇒ list, and if
    // the roster is empty and auto_create is set, self-provision one first (character_name).
    EnterWorldSequencer(std::uint64_t forced_character_id, bool auto_create,
                        std::string character_name);

    EnterPhase phase() const { return phase_; }
    std::uint64_t character_id() const { return character_id_; }
    std::uint16_t last_enter_status() const { return last_enter_status_; }

    // Drive one step per inbound message (mirrors the bot's request/response chain).
    EnterStep on_handshake_ok();
    EnterStep on_char_list(const clientnet::codec::CharListResponse& roster);
    EnterStep on_char_create(const clientnet::codec::CharCreateResponse& resp);
    EnterStep on_enter_world(const clientnet::codec::EnterWorldResponse& resp);

private:
    EnterStep send_enter_();               // build ENTER_WORLD(character_id_) → kEntering
    EnterStep fail_(std::string why);      // → kFailed with a note

    EnterPhase phase_ = EnterPhase::kConnecting;
    std::uint64_t forced_id_ = 0;
    bool auto_create_ = true;
    std::string character_name_;
    std::uint64_t character_id_ = 0;
    std::uint16_t last_enter_status_ = 0xFFFF;
};

// Config for one live probe run against real authd + worldd.
struct ProbeConfig {
    std::string authd_host;
    std::uint16_t authd_port = 0;
    std::string worldd_host;
    std::uint16_t worldd_port = 0;
    std::string account;
    std::string password;
    std::uint32_t realm_id = 0;      // 0 → first build-compatible realm
    std::uint32_t client_build = 1000;
    std::uint32_t duration_ms = 8000;  // how long to stay in-world draining the relay
    bool walk = true;                  // send MovementIntents (be a live AoI mover)

    // Server-authoritative enter-world (D-35 / #341): after HandshakeOk the session is
    // at CHARACTER-SELECT, not spawned — the probe must ENTER_WORLD an OWNED character.
    std::uint64_t character_id = 0;    // 0 → CharList (first owned), else enter THIS id
    std::string character_name;        // name to self-provision when the roster is empty
    bool auto_create = true;           // create a character when the roster is empty
};

// Drive the full GUI net path live: authd login → NetThreadCore world session →
// drain the relay + (optionally) walk a square → capture the peer's frames. This is
// the function client/test/run_client_sees_bot_it.sh exercises against a real bot.
// Never throws for a normal protocol outcome (a login/grant failure is a populated
// result).
ProbeResult run_probe(const ProbeConfig& cfg);

}  // namespace meridian::probe

#endif  // MERIDIAN_CLIENT_WORLD_PROBE_CORE_H
