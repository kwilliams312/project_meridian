// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — headless client-world probe core implementation (#301). Drives
// the GUI client's world-session net path (net::NetThreadCore + the shared clientnet
// stack) headlessly, exactly as scenes/world/world.gd drives MeridianNetThread. See
// client_world_probe_core.h. Clean-room from the wire contracts + this repo's own
// net thread / bot as the interop reference; no GPL source consulted (CONTRIBUTING.md).
//
// COORDINATE FRAMES (mirrors bot_core.cpp): the #102 movement integrator uses a Y-UP
// frame (x = ground-X, y = height, z = ground-Z); the world.fbs / worldd frame is
// Z-UP (x/y = ground plane, z = height). Outbound intents map snapshot→wire; the
// remote interpolator is fed the RENDER frame (Godot Y-UP) for parity with the node.

#include "client_world_probe_core.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "login_core.h"
#include "login_transport.h"
#include "meridian/clientnet/codec.h"
#include "meridian/clientnet/tls_client.h"
#include "meridian/clientnet/wire_frame.h"
#include "movement_constants.h"
#include "movement_controller.h"
#include "movement_query.h"
#include "net_thread_core.h"
#include "net_thread_messages.h"

namespace meridian::probe {
namespace {

namespace cn = meridian::clientnet;
namespace mv = meridian::movement;
namespace net = meridian::net;

// M0 flat bootstrap spawn (worldd world_dispatch.cpp: kZoneMaxXY*0.5 on x/y,
// kFlatGroundZ on z). WIRE coords (z = height). Same as the bot.
constexpr float kSpawnWireX = 64.0f;
constexpr float kSpawnWireY = 64.0f;
constexpr float kSpawnWireZ = 0.0f;
constexpr float kSquareHalfExtent = 10.0f;

std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Realm selection for run_login's function-pointer seam (a capturing lambda cannot
// convert). Written once before login; read-only during.
std::uint32_t g_realm_id = 0;
bool g_want_realm = false;
std::uint32_t select_realm_fn(const std::vector<login::RealmInfo>& realms,
                              const login::LoginConfig&) {
    if (g_want_realm) return g_realm_id;
    return realms.empty() ? 0u : realms.front().id;
}

// The scripted square-walk input generator (mirrors bot_core.input_for_tick).
mv::MovementInput input_for_tick(std::uint32_t tick, std::uint32_t ticks_per_leg) {
    mv::MovementInput in;
    in.walk = false;  // run
    const std::uint32_t leg = (tick / ticks_per_leg) % 4;
    switch (leg) {
        case 0: in.move_z = 1.0f;  break;
        case 1: in.move_x = 1.0f;  break;
        case 2: in.move_z = -1.0f; break;
        case 3: in.move_x = -1.0f; break;
    }
    return in;
}

}  // namespace

bool apply_entity_frame(remote::RemoteInterpolator& interp, ProbeResult& res,
                        std::uint16_t opcode, const Bytes& payload,
                        std::uint64_t recv_server_ms) {
    // Map a wire (Z-UP) position to the render frame (Godot Y-UP: y = height) the
    // interpolator + scene node use — the SAME mapping MeridianNetThread applies.
    auto to_render = [](float wx, float wy, float wz) {
        return remote::Vec3{wx, /*y=height*/ wz, /*z=ground*/ wy};
    };

    if (opcode == cn::kOpEntityEnter) {
        auto e = cn::codec::decode_entity_enter(payload);
        if (!e) return false;
        interp.on_enter(e->entity_guid, to_render(e->x, e->y, e->z), e->orientation,
                        recv_server_ms);
        EntitySighting s;
        s.kind = SightingKind::kEnter;
        s.entity_guid = e->entity_guid;
        s.x = e->x;
        s.y = e->y;
        s.z = e->z;
        s.has_position = true;
        res.sightings.push_back(s);
        ++res.enters_by_guid[s.entity_guid];
        return true;
    }
    if (opcode == cn::kOpEntityUpdate) {
        auto u = cn::codec::decode_entity_update(payload);
        if (!u) return false;
        if (u->has_position()) {
            interp.on_update(u->entity_guid, to_render(u->x, u->y, u->z), u->orientation,
                             recv_server_ms);
        }
        EntitySighting s;
        s.kind = SightingKind::kUpdate;
        s.entity_guid = u->entity_guid;
        s.has_position = u->has_position();
        s.x = u->x;
        s.y = u->y;
        s.z = u->z;
        res.sightings.push_back(s);
        ++res.updates_by_guid[s.entity_guid];
        return true;
    }
    if (opcode == cn::kOpEntityLeave) {
        auto l = cn::codec::decode_entity_leave(payload);
        if (!l) return false;
        interp.on_leave(l->entity_guid);
        EntitySighting s;
        s.kind = SightingKind::kLeave;
        s.entity_guid = l->entity_guid;
        s.has_position = false;
        s.leave_reason = l->reason;
        res.sightings.push_back(s);
        ++res.leaves_by_guid[s.entity_guid];
        return true;
    }
    return false;  // not an entity-relay opcode (e.g. ClockSync)
}

// -----------------------------------------------------------------------------
// EnterWorldSequencer — the character-select → ENTER_WORLD state machine (D-35/#341).
// Socket-free: each on_* is driven by one decoded inbound message and returns the next
// request frame to send. Mirrors client/bot/bot_core.cpp's request/response chain.
// -----------------------------------------------------------------------------
EnterWorldSequencer::EnterWorldSequencer(std::uint64_t forced_character_id,
                                         bool auto_create, std::string character_name)
    : forced_id_(forced_character_id),
      auto_create_(auto_create),
      character_name_(std::move(character_name)) {}

EnterStep EnterWorldSequencer::fail_(std::string why) {
    phase_ = EnterPhase::kFailed;
    EnterStep step;
    step.failed = true;
    step.detail = std::move(why);
    return step;
}

EnterStep EnterWorldSequencer::send_enter_() {
    phase_ = EnterPhase::kEntering;
    EnterStep step;
    step.frame = cn::encode_world_frame(
        cn::kOpEnterWorldReq, /*seq=*/3,
        cn::codec::encode_enter_world_request(character_id_));
    return step;
}

EnterStep EnterWorldSequencer::on_handshake_ok() {
    // Character-select reached. A forced id enters directly (no roster round-trip, the
    // deterministic test hook); otherwise ask worldd for THIS account's roster.
    if (forced_id_ != 0) {
        character_id_ = forced_id_;
        return send_enter_();
    }
    phase_ = EnterPhase::kListing;
    EnterStep step;
    step.frame = cn::encode_world_frame(cn::kOpCharListReq, /*seq=*/1,
                                        cn::codec::encode_char_list_request());
    return step;
}

EnterStep EnterWorldSequencer::on_char_list(
    const clientnet::codec::CharListResponse& roster) {
    if (!roster.characters.empty()) {
        character_id_ = roster.characters.front().character_id;
        return send_enter_();
    }
    if (!auto_create_) {
        return fail_("character-select roster is empty and auto_create is off");
    }
    // Roster empty -> self-provision one (M0 roster: race/class 1/1), then ENTER on the
    // mint. Same fallback the bot uses so a fresh account can still see a peer.
    phase_ = EnterPhase::kCreating;
    clientnet::codec::CharCreateRequest req;
    req.name = character_name_.empty() ? std::string("probe") : character_name_;
    req.race = 1;
    req.char_class = 1;
    EnterStep step;
    step.frame = cn::encode_world_frame(cn::kOpCharCreateReq, /*seq=*/2,
                                        cn::codec::encode_char_create_request(req));
    return step;
}

EnterStep EnterWorldSequencer::on_char_create(
    const clientnet::codec::CharCreateResponse& resp) {
    if (resp.status != 0 /*CharCreateStatus::OK*/ || resp.character_id == 0) {
        return fail_("CharCreate rejected (status=" + std::to_string(resp.status) + ")");
    }
    character_id_ = resp.character_id;
    return send_enter_();
}

EnterStep EnterWorldSequencer::on_enter_world(
    const clientnet::codec::EnterWorldResponse& resp) {
    last_enter_status_ = resp.status;
    if (resp.status != 0 /*EnterWorldStatus::OK*/) {
        return fail_("ENTER_WORLD rejected (status=" + std::to_string(resp.status) + ")");
    }
    phase_ = EnterPhase::kSpawned;
    EnterStep step;
    step.spawned = true;
    step.detail = "entered world (ENTER_WORLD OK)";
    return step;
}

ProbeResult run_probe(const ProbeConfig& cfg) {
    ProbeResult res;

    // --- 1. authd login over TLS 1.3 (#99) — the MeridianLogin.login() path. ------
    login::LoginConfig lcfg;
    lcfg.client_build = cfg.client_build;
    lcfg.proto_ver = 1;
    g_want_realm = (cfg.realm_id != 0);
    g_realm_id = cfg.realm_id;

    login::LoginResult grant;
    {
        login::TlsLoginTransport transport(cfg.authd_host, cfg.authd_port);
        if (!transport.ok()) {
            res.login_detail = "connect authd: " + transport.error();
            return res;
        }
        grant = login::run_login(transport, lcfg, cfg.account, cfg.password,
                                 &select_realm_fn, nullptr);
    }
    if (!grant.ok()) {
        res.login_detail = "login: " + grant.detail;
        return res;
    }
    res.login_ok = true;

    // --- 2. Build the WorldHello frame + start the net thread (the exact
    //        MeridianNetThread.connect_to_world path world.gd runs). ---------------
    login::Bytes nonce;
    login::Bytes wh_payload = login::build_world_hello(grant, cfg.client_build, &nonce);
    login::Bytes wh_frame =
        login::encode_world_frame(login::kOpcodeWorldHello, /*seq=*/0, wh_payload);

    const std::string worldd_host = cfg.worldd_host;
    const std::uint16_t worldd_port = cfg.worldd_port;
    // worldd's IF-2 listener is TLS 1.3 (server/worldd/main.cpp — cert/key required
    // to serve; the proven bot connects via TLS). The GUI net path (MeridianNetThread)
    // uses the SAME TlsClientTransport — this mirrors it exactly.
    net::ConnectFn connect = [worldd_host, worldd_port]()
        -> std::unique_ptr<cn::ITransport> {
        auto t = std::make_unique<cn::TlsClientTransport>(worldd_host, worldd_port);
        if (!t->ok()) return nullptr;
        return t;
    };

    net::NetThreadConfig ncfg;
    ncfg.world_hello_frame = net::Bytes(wh_frame.begin(), wh_frame.end());
    ncfg.session_key = net::Bytes(grant.session_key.begin(), grant.session_key.end());

    net::NetThreadCore core;
    if (!core.start(std::move(connect), std::move(ncfg))) {
        res.connect_failed = true;
        res.detail = "net thread failed to start";
        return res;
    }

    // --- 3. Per-tick loop: drain the inbound ring (pump), feed the interpolator,
    //        and — if walking — predict + send a MovementIntent. Mirrors world.gd's
    //        _physics_process at 20 Hz. ---------------------------------------------
    remote::RemoteInterpolator interp;

    mv::FlatWorldQuery world(0.0f);
    mv::MovementSnapshot start;
    start.position = {kSpawnWireX, /*y=height*/ kSpawnWireZ, /*z=ground*/ kSpawnWireY};
    start.grounded = true;
    mv::PredictionReconciler reconciler(world, start);

    const float leg_metres = kSquareHalfExtent * 2.0f;
    const float per_tick_m = mv::kRunSpeed * static_cast<float>(mv::kTickSeconds);
    std::uint32_t ticks_per_leg =
        static_cast<std::uint32_t>(std::lround(leg_metres / per_tick_m));
    if (ticks_per_leg == 0) ticks_per_leg = 1;

    // Character-select → ENTER_WORLD sequencer (D-35/#341): after HandshakeOk the
    // session is NOT spawned; we must ENTER_WORLD an owned character and only then does
    // worldd spawn us + relay the AoI peer frames. Mirrors the bot's contract.
    EnterWorldSequencer enter_seq(cfg.character_id, cfg.auto_create, cfg.character_name);

    // Apply one sequencer step: send the next request frame (if any) and fold spawn /
    // failure state into the result.
    auto apply_enter_step = [&](const EnterStep& step) {
        if (!step.frame.empty()) core.send(step.frame, net::SendPriority::kControl);
        if (step.spawned) {
            res.spawned = true;
            res.character_id = enter_seq.character_id();
            res.enter_world_status = enter_seq.last_enter_status();
            res.detail = step.detail;
        } else if (step.failed) {
            res.enter_world_status = enter_seq.last_enter_status();
            res.detail = step.detail;
        }
    };

    // Drain every inbound event currently on the ring. Returns false if the session
    // ended (Disconnect / transport closed / connect failed).
    auto pump = [&](std::uint64_t recv_ms) -> bool {
        net::InboundMessage msg;
        bool alive = true;
        while (core.try_pop_inbound(msg)) {
            switch (msg.kind) {
                case net::InboundKind::kHandshakeOk:
                    // Reached CHARACTER-SELECT (not spawned) — kick off ENTER_WORLD.
                    res.entered_world = true;
                    if (res.detail.empty()) res.detail = "reached character-select (HandshakeOk)";
                    apply_enter_step(enter_seq.on_handshake_ok());
                    break;
                case net::InboundKind::kCharList:
                    apply_enter_step(enter_seq.on_char_list(msg.roster));
                    break;
                case net::InboundKind::kCharCreate:
                    apply_enter_step(enter_seq.on_char_create(msg.char_create));
                    break;
                case net::InboundKind::kEnterWorld:
                    apply_enter_step(enter_seq.on_enter_world(msg.enter_world));
                    break;
                case net::InboundKind::kCharDelete:
                    break;  // the probe never deletes; ignore any stray response
                case net::InboundKind::kMovementState: {
                    ++res.movement_states;
                    // Reconcile our own authoritative state (Y-UP frame from wire).
                    mv::MovementStateIn in_state;
                    in_state.ack_seq = msg.state.ack_seq;
                    in_state.state_flags = msg.state.state_flags;
                    in_state.position = {msg.state.x, msg.state.z, msg.state.y};
                    in_state.orientation = msg.state.orientation;
                    in_state.server_time_ms = msg.state.server_time_ms;
                    reconciler.reconcile(in_state);
                    break;
                }
                case net::InboundKind::kEntityFrame:
                    if (apply_entity_frame(interp, res, msg.opcode, msg.payload, recv_ms)) {
                        ++res.entity_frames;
                    }
                    break;
                case net::InboundKind::kDisconnect:
                    res.disconnected = true;
                    res.disconnect_reason = msg.disc.reason;
                    if (!msg.detail.empty()) res.detail = msg.detail;
                    alive = false;
                    break;
                case net::InboundKind::kTransportClosed:
                    res.transport_closed = true;
                    if (!msg.detail.empty()) res.detail = msg.detail;
                    alive = false;
                    break;
                case net::InboundKind::kConnectFailed:
                    res.connect_failed = true;
                    if (!msg.detail.empty()) res.detail = msg.detail;
                    alive = false;
                    break;
            }
        }
        return alive;
    };

    const std::uint64_t t0 = now_ms();
    std::uint32_t tick = 0;
    bool alive = true;
    while (alive && (now_ms() - t0) < cfg.duration_ms) {
        const std::uint64_t recv_ms = now_ms();
        alive = pump(recv_ms);
        if (!alive) break;

        // Only a SPAWNED session (ENTER_WORLD OK) may move — worldd rejects a
        // MovementIntent sent at character-select (ctx.movement is emplaced on spawn).
        if (res.spawned && cfg.walk) {
            const std::uint64_t client_time_ms = t0 + tick * mv::kTickMillis;
            mv::MovementInput in = input_for_tick(tick, ticks_per_leg);
            mv::MovementIntentOut intent = reconciler.predict(in, client_time_ms);
            if (reconciler.should_emit_intent(client_time_ms, intent.state_flags)) {
                cn::codec::MovementIntent mi;
                mi.seq = intent.seq;
                mi.state_flags = intent.state_flags;
                mi.x = intent.x;   // ground X
                mi.y = intent.z;   // ground Z (client z) → wire y
                mi.z = intent.y;   // height (client y)  → wire z
                mi.orientation = intent.orientation;
                mi.client_time_ms = client_time_ms;
                cn::Bytes payload = cn::codec::encode_movement_intent(mi);
                cn::Bytes frame =
                    cn::encode_world_frame(cn::kOpMovementIntent, intent.seq, payload);
                if (core.send(frame, net::SendPriority::kMovement)) ++res.intents_sent;
            }
            ++tick;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(mv::kTickMillis));
    }

    // Final drain sweep so a late relay frame is captured before teardown.
    for (int i = 0; i < 8 && alive; ++i) {
        alive = pump(now_ms());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    res.tracked_remote = static_cast<std::uint32_t>(interp.tracked_count());
    core.stop();
    return res;
}

}  // namespace meridian::probe
