// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — engine-free headless BOT core implementation (#111). Drives
// the CLIENT side of worldd's #84 handshake + #86 movement over the login::
// ILoginTransport frame seam #99 uses. Clean-room from the wire contracts
// (schema/net/world.fbs), worldd #84/#86 as the interop reference, and the #102
// movement core; no GPL source consulted (CONTRIBUTING.md).
//
// COORDINATE FRAMES (the one subtle interop detail). The #102 client integrator
// uses a Y-UP frame: MovementSnapshot.position is (x = ground-X, y = height,
// z = ground-Z). The world.fbs / worldd frame is Z-UP: MovementIntent/State
// (x, y) is the ground plane and z is height. So the wire mapping is:
//     wire.x = snapshot.x   (ground X)
//     wire.y = snapshot.z   (ground Z)
//     wire.z = snapshot.y   (height)
// and inbound MovementState maps back the same way. worldd seeds the spawn at
// (kZoneMaxXY/2, kZoneMaxXY/2, kFlatGroundZ) = (64, 64, 0) in WIRE coords, so the
// client integrator starts at snapshot (64, 0, 64).

#include "bot_core.h"

#include <chrono>
#include <cmath>

#include "movement_constants.h"
#include "movement_controller.h"
#include "movement_query.h"
#include "world_generated.h"

namespace meridian::bot {
namespace {

namespace fb = flatbuffers;
namespace mn = meridian::net;
namespace mv = meridian::movement;

// M0 flat bootstrap spawn (worldd world_dispatch.cpp: kZoneMaxXY*0.5 on x/y,
// kFlatGroundZ on z). WIRE coords (z = height).
constexpr float kSpawnWireX = 64.0f;  // kZoneMaxXY * 0.5
constexpr float kSpawnWireY = 64.0f;  // kZoneMaxXY * 0.5
constexpr float kSpawnWireZ = 0.0f;   // kFlatGroundZ

// The square path half-extent (metres). ±10 m around the spawn stays well inside
// the [0,128]² bootstrap play area, so every legal move is accepted on bounds.
constexpr float kSquareHalfExtent = 10.0f;

// AoI drain tuning (#248). The transport read timeout that bounds each
// recv_frame_nb poll: long enough that a promptly-sent MovementState is never
// mistaken for "nothing pending" on a loaded box, short enough that a drain with
// no pending frame returns quickly. At 20 Hz the send loop's own cadence is ~50 ms;
// 40 ms keeps the drain responsive without spinning.
constexpr unsigned kDrainTimeoutMs = 40;
// Max frames one drain call pulls before returning to the send loop — caps a chatty
// relay so it cannot starve our own movement sends. A 2-bot run sees at most a
// handful of frames per tick (our MovementState + the peer's one EntityUpdate).
constexpr int kDrainBudget = 64;
// How many times to sweep for trailing frames after the movement loop ends, so a
// late-arriving EntityUpdate from the peer is captured rather than dropped.
constexpr int kFinalDrainSweeps = 8;

// ---- FlatBuffer encode/decode (world.fbs) ----------------------------------

Bytes finish(fb::FlatBufferBuilder& b) {
    return Bytes(b.GetBufferPointer(), b.GetBufferPointer() + b.GetSize());
}

// The MoveMode enum values worldd #86 reads out of the LOW 3 BITS of state_flags
// (server/worldd/movement_validation.cpp mode_from_flags -> movement_constants.h
// MoveMode). This is the wire contract the SERVER's speed check keys off, and it
// is DIFFERENT from the #102 client `flags::` bitfield (kForward/kStrafe/… — see
// below): the client bitfield's low bits are NOT a MoveMode. worldd reads
// state_flags & 0x7 as the mode selector (Idle=0, Walk=1, Run=2, Jump=3) and picks
// server_speed(mode) as the per-packet displacement cap. So the bot MUST put the
// server-readable mode in those low 3 bits or the server caps it at the wrong
// speed and rejects the move. (This #102/#86 flags-layout disagreement is flagged
// as a follow-up in BOTH headers — movement_controller.h §state_flags and
// movement_validation.h mode_from_flags. The bot resolves it at the wire boundary
// by writing the server-mode bits; a shared canonical encoding is the real fix.)
enum class ServerMoveMode : std::uint32_t { kIdle = 0, kWalk = 1, kRun = 2, kJump = 3 };

// Compose the wire state_flags: keep the #102 client bitfield in the HIGH bits (so
// a future server that decodes the richer layout still sees it) but stamp the
// server-readable MoveMode into the low 3 bits. The bot's square path runs on the
// ground -> Run; idle -> Idle.
std::uint32_t wire_state_flags(std::uint32_t client_flags, ServerMoveMode mode) {
    // Shift the client bitfield above the low 3 mode bits, then OR the mode in.
    return (client_flags << 3) | static_cast<std::uint32_t>(mode);
}

// Encode a MovementIntent payload from a #102 integrator output, converting the
// client Y-UP frame to the wire Z-UP frame (see the file header) and stamping the
// server-readable MoveMode into state_flags (see wire_state_flags).
Bytes enc_movement_intent(const mv::MovementIntentOut& out, ServerMoveMode mode) {
    fb::FlatBufferBuilder b;
    auto mi = mn::CreateMovementIntent(b, out.seq, wire_state_flags(out.state_flags, mode),
                                       /*x=*/out.x,   // ground X
                                       /*y=*/out.z,   // ground Z (client z)
                                       /*z=*/out.y,   // height  (client y)
                                       out.orientation, out.client_time_ms);
    b.Finish(mi);
    return finish(b);
}

// Verify-then-GetRoot on untrusted server bytes (never GetRoot without the
// Verifier — the same discipline login_core applies).
template <typename T>
const T* decode(const Bytes& buf) {
    fb::Verifier v(buf.data(), buf.size());
    if (!v.VerifyBuffer<T>(nullptr)) return nullptr;
    return fb::GetRoot<T>(buf.data());
}

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Record an inbound EntityEnter/Update/Leave frame as a captured sighting (#248).
// Verifies the untrusted payload, appends an EntitySighting to `res.sightings`, and
// bumps the per-guid tallies. Returns true if a well-formed entity frame was
// captured (so the caller can also bump the legacy entity_updates counter).
bool capture_entity_frame(BotRunResult& res, const WorldFrame& f) {
    if (f.opcode == kOpEntityEnter) {
        const mn::EntityEnter* e = decode<mn::EntityEnter>(f.payload);
        if (e == nullptr) return false;
        EntitySighting s;
        s.kind = SightingKind::kEnter;
        s.entity_guid = e->entity_guid();
        s.x = e->x();
        s.y = e->y();
        s.z = e->z();
        s.has_position = true;
        res.sightings.push_back(s);
        ++res.enters_by_guid[s.entity_guid];
        return true;
    }
    if (f.opcode == kOpEntityUpdate) {
        const mn::EntityUpdate* u = decode<mn::EntityUpdate>(f.payload);
        if (u == nullptr) return false;
        EntitySighting s;
        s.kind = SightingKind::kUpdate;
        s.entity_guid = u->entity_guid();
        // world.fbs marks x/y/z optional (default null) — present only on a move
        // delta. The generated accessors return flatbuffers::Optional<float>; a
        // move relay carries them, an attribute-only delta would not. worldd's move
        // relay always sends them; honour whatever the wire carried.
        const auto ox = u->x();
        const auto oy = u->y();
        const auto oz = u->z();
        s.has_position = ox.has_value() || oy.has_value() || oz.has_value();
        s.x = ox.has_value() ? *ox : 0.0f;
        s.y = oy.has_value() ? *oy : 0.0f;
        s.z = oz.has_value() ? *oz : 0.0f;
        res.sightings.push_back(s);
        ++res.updates_by_guid[s.entity_guid];
        return true;
    }
    if (f.opcode == kOpEntityLeave) {
        const mn::EntityLeave* l = decode<mn::EntityLeave>(f.payload);
        if (l == nullptr) return false;
        EntitySighting s;
        s.kind = SightingKind::kLeave;
        s.entity_guid = l->entity_guid();
        s.has_position = false;
        s.leave_reason = static_cast<std::uint16_t>(l->reason());
        res.sightings.push_back(s);
        ++res.leaves_by_guid[s.entity_guid];
        return true;
    }
    return false;
}

// The scripted-input generator: given the tick index, produce the #102
// MovementInput for the chosen path. Square = four equal legs, +Z, +X, -Z, -X,
// turning at each corner; the leg length is chosen so run speed traces the square.
mv::MovementInput input_for_tick(BotPath path, std::uint32_t tick,
                                 std::uint32_t ticks_per_leg, float orientation) {
    mv::MovementInput in;
    in.walk = false;                // run (largest legal speed budget)
    in.orientation = orientation;
    if (path == BotPath::kIdle) return in;  // no movement

    const std::uint32_t leg = (tick / ticks_per_leg) % 4;
    switch (leg) {
        case 0: in.move_z = 1.0f;  break;  // +ground-Z
        case 1: in.move_x = 1.0f;  break;  // +ground-X
        case 2: in.move_z = -1.0f; break;  // -ground-Z
        case 3: in.move_x = -1.0f; break;  // -ground-X
    }
    return in;
}

}  // namespace

BotPath parse_path(const std::string& s) {
    if (s == "idle") return BotPath::kIdle;
    return BotPath::kSquare;  // default + "square"
}

BotRunResult run_world_session(login::ILoginTransport& transport,
                               const login::LoginResult& grant,
                               const BotWorldConfig& cfg) {
    BotRunResult res;

    // --- 1. WorldHello (opcode 0x0001, seq 0). Build it from the grant via the
    //        #99 builder, wrap it in the IF-2 frame header, and send. -----------
    login::Bytes nonce;
    login::Bytes wh_payload = login::build_world_hello(grant, cfg.client_build, &nonce);
    Bytes wh_frame = encode_world_frame(kOpWorldHello, /*seq=*/0,
                                        Bytes(wh_payload.begin(), wh_payload.end()));
    if (!transport.send_frame(login::Bytes(wh_frame.begin(), wh_frame.end()))) {
        res.detail = "failed to send WorldHello";
        return res;
    }
    res.connected = true;

    // --- 2. Establish the client AEAD session (mirror of worldd #84). At M0 the
    //        wire body is plaintext, but we build + hold the session so the seam
    //        is client-ready; its correctness is proven by the AEAD-interop test. -
    ClientWorldSession session(grant.session_key);
    (void)session;  // held for the seam; unused on the plaintext-at-M0 wire

    // --- 3. Read HandshakeOk (or a Disconnect on a grant reject). --------------
    std::optional<login::Bytes> reply = transport.recv_frame();
    if (!reply) {
        res.detail = "no reply to WorldHello (peer closed)";
        return res;
    }
    std::optional<WorldFrame> f = decode_world_frame(Bytes(reply->begin(), reply->end()));
    if (!f) {
        res.detail = "malformed IF-2 frame in reply to WorldHello";
        return res;
    }
    if (f->opcode == kOpDisconnect) {
        res.disconnected = true;
        if (const mn::Disconnect* d = decode<mn::Disconnect>(f->payload)) {
            res.disconnect_reason = static_cast<std::uint16_t>(d->reason());
            res.detail = d->message() ? d->message()->str() : "server disconnect";
        } else {
            res.detail = "server disconnect (undecodable)";
        }
        return res;
    }
    if (f->opcode != kOpHandshakeOk) {
        res.detail = "unexpected opcode in reply to WorldHello (expected HandshakeOk)";
        return res;
    }
    if (decode<mn::HandshakeOk>(f->payload) == nullptr) {
        res.detail = "undecodable HandshakeOk payload";
        return res;
    }
    res.handshake_ok = true;
    res.detail = "entered world (HandshakeOk)";

    // --- 3b. AoI drain seam (#248). worldd's enter() sends EntityEnter for anyone
    //         ALREADY in range the instant we enter (login-time visibility, #87),
    //         and later relays EntityUpdate/Leave asynchronously as OTHER sessions
    //         move — interleaved with our own MovementState replies and NOT in
    //         lockstep with our sends. To capture the OTHER bot's frames without
    //         blocking forever, we put the transport in timed-read mode and DRAIN
    //         every pending frame after each send (and once up front). recv_frame_nb
    //         reports would_block when nothing is pending, ending a drain cleanly. --
    transport.set_recv_timeout_ms(kDrainTimeoutMs);

    // Track the previous authoritative position (starts at spawn) so a move can be
    // counted accepted only when the server's position actually ADVANCES. Declared
    // before the frame handler (below) so it can close over them.
    float last_auth_x = kSpawnWireX;
    float last_auth_y = kSpawnWireY;

    // Reconciler is set up below (movement only); the handler feeds it authoritative
    // states via a pointer so both the idle-drain and the movement loop share it.
    mv::PredictionReconciler* reconciler_ptr = nullptr;

    // Handle ONE decoded server frame: authoritative MovementState (ours, for
    // reconcile + accept accounting), relayed EntityEnter/Update/Leave (the OTHER
    // players — captured for the mutual-visibility assertion), or a Disconnect.
    // Returns false if the caller should stop the loop (a Disconnect arrived).
    auto process_frame = [&](const WorldFrame& sf) -> bool {
        if (sf.opcode == kOpMovementState) {
            if (const mn::MovementState* st = decode<mn::MovementState>(sf.payload)) {
                ++res.states_received;
                const float ddx = st->x() - last_auth_x;
                const float ddy = st->y() - last_auth_y;
                const float advance = std::sqrt(ddx * ddx + ddy * ddy);
                if (advance > 0.001f) ++res.moves_accepted;
                last_auth_x = st->x();
                last_auth_y = st->y();

                res.final_x = st->x();
                res.final_y = st->y();
                res.final_z = st->z();
                const float dx = st->x() - kSpawnWireX;
                const float dy = st->y() - kSpawnWireY;
                res.moved_distance = std::sqrt(dx * dx + dy * dy);

                if (reconciler_ptr != nullptr) {
                    mv::MovementStateIn in_state;
                    in_state.ack_seq = st->ack_seq();
                    in_state.state_flags = st->state_flags();
                    in_state.position = {st->x(), st->z(), st->y()};
                    in_state.orientation = st->orientation();
                    in_state.server_time_ms = st->server_time_ms();
                    reconciler_ptr->reconcile(in_state);
                }
            }
        } else if (sf.opcode == kOpEntityEnter || sf.opcode == kOpEntityUpdate ||
                   sf.opcode == kOpEntityLeave) {
            // #248: CAPTURE the OTHER player's frame (guid + position), not just count.
            if (capture_entity_frame(res, sf)) ++res.entity_updates;
        } else if (sf.opcode == kOpDisconnect) {
            res.disconnected = true;
            if (const mn::Disconnect* d = decode<mn::Disconnect>(sf.payload)) {
                res.disconnect_reason = static_cast<std::uint16_t>(d->reason());
                res.detail = d->message() ? d->message()->str() : "server disconnect";
            }
            return false;  // stop
        }
        return true;
    };

    // Drain every frame currently pending on the transport (non-blocking within the
    // read timeout). Returns false if a Disconnect arrived (stop). `budget` caps how
    // many frames one drain pulls so a chatty relay can never starve the send loop.
    auto drain_available = [&](int budget) -> bool {
        for (int i = 0; i < budget; ++i) {
            bool would_block = false;
            std::optional<login::Bytes> srv = transport.recv_frame_nb(would_block);
            if (!srv) {
                if (would_block) return true;   // nothing more pending — done
                if (res.detail.empty()) res.detail = "peer closed during drain";
                return false;                   // peer closed / error
            }
            std::optional<WorldFrame> sf =
                decode_world_frame(Bytes(srv->begin(), srv->end()));
            if (!sf) continue;
            if (!process_frame(*sf)) return false;
        }
        return true;
    };

    // Rendezvous (#248): if a barrier is wired, wait here until the OTHER bot has
    // also entered the world. After this returns, worldd has run enter() for BOTH
    // sessions, so each has been sent an EntityEnter for the other — which the
    // login-time drain below then captures. This makes login-time MUTUAL visibility
    // deterministic instead of racing the two logins. No-op for the single bot.
    if (cfg.on_entered_world) cfg.on_entered_world();

    // Login-time visibility: pull any EntityEnter frames worldd queued the instant
    // we (and, post-barrier, the peer) entered range. Harmless if none are pending.
    // Sweep a few times so the peer's enter — which may land a beat after the
    // barrier releases — is captured before the movement loop begins.
    for (int i = 0; i < kFinalDrainSweeps; ++i) {
        if (!drain_available(kDrainBudget)) break;
        if (res.distinct_entities_seen() > 0) break;  // saw the peer — proceed
    }

    // --- 4. Movement loop. Drive the #102 integrator at 20 Hz over the scripted
    //        path; emit MovementIntents gated by the ≤10/s + on-state-change cap;
    //        after each send, drain any pending server MovementState/EntityUpdate. -
    if (cfg.path == BotPath::kIdle || cfg.movement_ticks == 0) {
        if (res.states_received == 0) {
            res.final_x = kSpawnWireX;
            res.final_y = kSpawnWireY;
            res.final_z = kSpawnWireZ;
        }
        // Idle bots still observe: keep draining so a moving peer's EntityUpdate
        // frames are captured across the (simulated) idle duration.
        for (std::uint32_t tick = 0; tick < cfg.movement_ticks; ++tick) {
            if (!drain_available(kDrainBudget)) break;
        }
        return res;
    }

    // Flat bootstrap ground plane (D-19). In the client Y-UP frame the ground is
    // at y = 0 — the same height worldd seeds the spawn z at (kFlatGroundZ = 0).
    mv::FlatWorldQuery world(0.0f);
    // Start the reconciler at the spawn, in the CLIENT (Y-UP) frame.
    mv::MovementSnapshot start;
    start.position = {kSpawnWireX, /*y=height*/ kSpawnWireZ, /*z=ground*/ kSpawnWireY};
    start.grounded = true;
    mv::PredictionReconciler reconciler(world, start);
    reconciler_ptr = &reconciler;  // the frame handler now reconciles into it

    // Ticks per leg: walk kSquareHalfExtent*2 metres per leg at run speed. At 20 Hz
    // each tick advances kRunSpeed * kTickSeconds = 0.3 m, so a 20 m leg is ~67
    // ticks. Clamp so the whole path fits within movement_ticks if it is short.
    const float leg_metres = kSquareHalfExtent * 2.0f;
    const float per_tick_m = mv::kRunSpeed * static_cast<float>(mv::kTickSeconds);
    std::uint32_t ticks_per_leg =
        static_cast<std::uint32_t>(std::lround(leg_metres / per_tick_m));
    if (ticks_per_leg == 0) ticks_per_leg = 1;

    const std::uint64_t t0 = now_ms();
    for (std::uint32_t tick = 0; tick < cfg.movement_ticks; ++tick) {
        // Sim clock advances a fixed 50 ms/tick (the server keys Δt off
        // client_time_ms; a fixed cadence keeps the per-packet speed budget legal).
        const std::uint64_t client_time_ms = t0 + tick * mv::kTickMillis;

        mv::MovementInput in = input_for_tick(cfg.path, tick, ticks_per_leg, /*orient=*/0.0f);
        mv::MovementIntentOut intent = reconciler.predict(in, client_time_ms);

        if (reconciler.should_emit_intent(client_time_ms, intent.state_flags)) {
            // The bot's scripted path runs on the flat ground -> the server-readable
            // mode is Run (largest legal cap). Idle path never reaches here.
            Bytes payload = enc_movement_intent(intent, ServerMoveMode::kRun);
            std::uint64_t seq = intent.seq;  // wire seq == intent seq (in lockstep)
            Bytes frame = encode_world_frame(kOpMovementIntent, seq, payload);
            if (!transport.send_frame(login::Bytes(frame.begin(), frame.end()))) {
                res.detail = "movement send failed (peer closed)";
                break;
            }
            ++res.intents_sent;
        }

        // Drain the server's reply(ies): our authoritative MovementState for the
        // intent we just sent AND any relayed EntityEnter/Update/Leave about the
        // OTHER bot (the #87 AoI relay, captured for the mutual-visibility proof).
        // Every tick drains (not only on a send) so the OTHER bot's asynchronously
        // relayed frames are captured even on ticks we throttle our own intent.
        if (!drain_available(kDrainBudget)) break;  // Disconnect / peer close
    }

    // Final drain: the LAST intent's MovementState + the OTHER bot's trailing
    // EntityUpdate frames can still be in flight after the loop ends. Sweep a few
    // times so a late relay frame is captured rather than dropped on teardown.
    for (int i = 0; i < kFinalDrainSweeps; ++i) {
        if (!drain_available(kDrainBudget)) break;
    }

    return res;
}

}  // namespace meridian::bot
