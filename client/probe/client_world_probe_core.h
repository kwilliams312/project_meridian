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
    bool entered_world = false;     // HandshakeOk received (entered the world)
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
};

// Drive the full GUI net path live: authd login → NetThreadCore world session →
// drain the relay + (optionally) walk a square → capture the peer's frames. This is
// the function client/test/run_client_sees_bot_it.sh exercises against a real bot.
// Never throws for a normal protocol outcome (a login/grant failure is a populated
// result).
ProbeResult run_probe(const ProbeConfig& cfg);

}  // namespace meridian::probe

#endif  // MERIDIAN_CLIENT_WORLD_PROBE_CORE_H
