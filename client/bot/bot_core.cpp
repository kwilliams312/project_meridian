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

    // --- 4. Movement loop. Drive the #102 integrator at 20 Hz over the scripted
    //        path; emit MovementIntents gated by the ≤10/s + on-state-change cap;
    //        after each send, drain any pending server MovementState/EntityUpdate. -
    if (cfg.path == BotPath::kIdle || cfg.movement_ticks == 0) {
        res.final_x = kSpawnWireX;
        res.final_y = kSpawnWireY;
        res.final_z = kSpawnWireZ;
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

    // Ticks per leg: walk kSquareHalfExtent*2 metres per leg at run speed. At 20 Hz
    // each tick advances kRunSpeed * kTickSeconds = 0.3 m, so a 20 m leg is ~67
    // ticks. Clamp so the whole path fits within movement_ticks if it is short.
    const float leg_metres = kSquareHalfExtent * 2.0f;
    const float per_tick_m = mv::kRunSpeed * static_cast<float>(mv::kTickSeconds);
    std::uint32_t ticks_per_leg =
        static_cast<std::uint32_t>(std::lround(leg_metres / per_tick_m));
    if (ticks_per_leg == 0) ticks_per_leg = 1;

    // Track the previous authoritative position (starts at spawn) so a move can be
    // counted accepted only when the server's position actually ADVANCES.
    float last_auth_x = kSpawnWireX;
    float last_auth_y = kSpawnWireY;

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

            // Drain the server's reply(ies) for this intent: an authoritative
            // MovementState (accept = advanced, reject = snap-back) and possibly
            // relayed EntityEnter/Update/Leave (the 2-bot AoI case). The server
            // sends one MovementState per accepted/rejected intent.
            std::optional<login::Bytes> srv = transport.recv_frame();
            if (!srv) {
                res.detail = "peer closed during movement";
                break;
            }
            std::optional<WorldFrame> sf =
                decode_world_frame(Bytes(srv->begin(), srv->end()));
            if (!sf) continue;
            if (sf->opcode == kOpMovementState) {
                if (const mn::MovementState* st = decode<mn::MovementState>(sf->payload)) {
                    ++res.states_received;
                    // ACCEPT vs SNAP-BACK: worldd sends an authoritative MovementState
                    // for BOTH (accept = advanced position, reject = snap-back to the
                    // last authoritative position). There is no "accepted" flag on the
                    // wire, so we infer acceptance HONESTLY from motion: the
                    // authoritative position ADVANCED from the previous state. A
                    // snap-back repeats the last position (Δ ~ 0) -> not counted.
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

                    // Feed the authoritative state back into the reconciler so the
                    // client tracks the server (Y-UP frame: wire z -> snapshot.y,
                    // wire y -> snapshot.z).
                    mv::MovementStateIn in_state;
                    in_state.ack_seq = st->ack_seq();
                    in_state.state_flags = st->state_flags();
                    in_state.position = {st->x(), st->z(), st->y()};
                    in_state.orientation = st->orientation();
                    in_state.server_time_ms = st->server_time_ms();
                    reconciler.reconcile(in_state);
                }
            } else if (sf->opcode == kOpEntityEnter || sf->opcode == kOpEntityUpdate ||
                       sf->opcode == kOpEntityLeave) {
                ++res.entity_updates;
            } else if (sf->opcode == kOpDisconnect) {
                res.disconnected = true;
                if (const mn::Disconnect* d = decode<mn::Disconnect>(sf->payload)) {
                    res.disconnect_reason = static_cast<std::uint16_t>(d->reason());
                    res.detail = d->message() ? d->message()->str() : "server disconnect";
                }
                break;
            }
        }
    }

    return res;
}

}  // namespace meridian::bot
