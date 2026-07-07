// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free headless BOT core (issue #111). The tool that
// drives the FULL client protocol against the real servers with NO Godot: log in
// via authd (reusing the #99 login core), carry the grant to worldd, enter the
// world (WorldHello → HandshakeOk), and MOVE (MovementIntent → MovementState) —
// using the #102 engine-free integrator over the #101 locked constants.
//
// This core is the composition #99 left un-built: the CLIENT-side worldd session.
// #99 built authd login → SessionGrant and the WorldHello BUILDER; this core wires
// the worldd side of the handshake + the movement loop on top of it.
//
// ENGINE-FREE (Client SAD §9.2): NO Godot. The transport is the same injected
// login::ILoginTransport frame seam #99 uses (u32-LE-length framed FlatBuffer
// payloads over TLS to worldd), so the whole session is unit-tested against a MOCK
// worldd that replays the IF-2 sequence, AND run over the real TlsLoginTransport
// against a LIVE worldd by the bot CLI + the integration harness.
//
// The bot CLI (bot_main.cpp) composes: login::run_login (authd, #99) →
// run_world_session (this core, worldd) — one bot, login → enter-world → move,
// end to end. Two-bot mutual visibility (EntityEnter/Update relay) is a documented
// follow-up; this core already CONSUMES EntityEnter/Update/Leave so that step is
// additive.

#ifndef MERIDIAN_BOT_CORE_H
#define MERIDIAN_BOT_CORE_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "bot_world_session.h"
#include "login_core.h"          // login::ILoginTransport, login::LoginResult

namespace meridian::bot {

// A captured inbound AoI observation about ANOTHER entity — the #87 relay sends
// EntityEnter/Update/Leave for OTHER sessions in range. #111 merely COUNTED these;
// #248 CAPTURES them (guid + position + kind) so the two-bot harness can assert
// "bot B saw bot A enter and move". Position carries the wire (Z-UP) frame: x/y =
// ground plane, z = height (world.fbs). EntityUpdate makes position optional; the
// bot records has_position for whichever fields the delta carried.
enum class SightingKind : std::uint8_t { kEnter = 0, kUpdate = 1, kLeave = 2 };

struct EntitySighting {
    SightingKind kind = SightingKind::kEnter;
    std::uint64_t entity_guid = 0;  // the OTHER entity's server-assigned guid
    float x = 0.0f, y = 0.0f, z = 0.0f;  // wire coords (present for enter/update)
    bool has_position = false;      // EntityLeave carries no position
    std::uint16_t leave_reason = 0; // world.fbs LeaveReason (kLeave only)
};

// A scripted movement path the bot walks. v0 ships a closed square (walk the four
// edges of a box, turning at each corner) — a legal, bounded, repeating route that
// exercises the server movement authority (accepts) without ever leaving bounds.
enum class BotPath {
    kSquare,   // walk a closed square around the spawn (default)
    kIdle,     // enter world, send no movement (handshake-only smoke)
};

// Parse a --path token; unknown -> kSquare.
BotPath parse_path(const std::string& s);

// How far the bot got + what it observed. This is the honest run report the CLI
// prints and the integration harness asserts on.
struct BotRunResult {
    // --- Enter-world (WorldHello → HandshakeOk) ---
    bool connected = false;        // WorldHello sent (transport frames flowed)
    bool handshake_ok = false;     // HandshakeOk received (entered the world)
    bool disconnected = false;     // server sent a Disconnect (grant reject / error)
    std::uint16_t disconnect_reason = 0;  // world.fbs DisconnectReason (0 = none)
    std::string detail;            // human-readable note (logs / failure reason)

    // --- Movement loop ---
    std::uint32_t intents_sent = 0;       // MovementIntents put on the wire
    std::uint32_t states_received = 0;    // authoritative MovementStates consumed
    std::uint32_t entity_updates = 0;     // EntityEnter/Update/Leave frames consumed
    std::uint32_t moves_accepted = 0;     // states whose position advanced from spawn

    // The server's authoritative position at the end of the run (from the last
    // MovementState). Wire frame: x/y = ground plane, z = height (world.fbs).
    float final_x = 0.0f, final_y = 0.0f, final_z = 0.0f;
    // The euclidean ground distance the authoritative position moved from spawn —
    // the honest "did it actually move on the server?" signal.
    float moved_distance = 0.0f;

    // --- AoI mutual visibility (#248; the see-each-other-move evidence) ---------
    // Every EntityEnter/Update/Leave the bot received, IN ORDER — the OTHER
    // players it saw. The two-bot harness asserts over these: each bot must see the
    // other ENTER (login-time visibility) and UPDATE (the other moving).
    std::vector<EntitySighting> sightings;
    // Per-observed-guid tallies, derived from `sightings` as they arrive.
    std::unordered_map<std::uint64_t, std::uint32_t> enters_by_guid;
    std::unordered_map<std::uint64_t, std::uint32_t> updates_by_guid;
    std::unordered_map<std::uint64_t, std::uint32_t> leaves_by_guid;

    // Distinct OTHER guids this bot saw enter its AoI.
    std::size_t distinct_entities_seen() const { return enters_by_guid.size(); }
    // Total EntityUpdate deltas seen (the other(s) moving in view).
    std::uint32_t total_updates_seen() const {
        std::uint32_t n = 0;
        for (const auto& kv : updates_by_guid) n += kv.second;
        return n;
    }

    bool entered_world() const { return handshake_ok; }
};

// Config for one bot world session.
struct BotWorldConfig {
    std::uint32_t client_build = 1000;  // must match the grant's build gate
    BotPath path = BotPath::kSquare;
    // How many 20 Hz sim ticks to run the movement loop for. At 20 Hz, 200 ticks
    // = 10 s of simulated movement. The CLI derives this from --duration.
    std::uint32_t movement_ticks = 200;
};

// Drive the CLIENT-side worldd session over `transport` from a successful authd
// `grant`: send WorldHello, read HandshakeOk (or a Disconnect), establish the
// ClientWorldSession (AEAD, mirroring worldd #84), then run the movement loop —
// generating MovementIntents via the #102 integrator on the scripted path and
// consuming the server's MovementState / EntityUpdate replies.
//
// `transport` must already be connected to worldd (the CLI passes a
// TlsLoginTransport; the unit test passes a mock). Returns a BotRunResult that
// says exactly how far the bot got. Never throws for a normal protocol outcome
// (a grant reject is a populated result, not an exception).
BotRunResult run_world_session(login::ILoginTransport& transport,
                               const login::LoginResult& grant,
                               const BotWorldConfig& cfg);

}  // namespace meridian::bot

#endif  // MERIDIAN_BOT_CORE_H
