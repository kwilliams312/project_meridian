// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — MeridianNetThread GDExtension class (issue #97). The thin Godot
// binding over the engine-free net-thread core (net_thread_core.*): it owns the
// client's IF-2 world session on a DEDICATED std::thread and is the scene's only
// handle to worldd. The socket lives entirely on that thread; the scene talks to it
// exclusively through the lock-free SPSC rings the core exposes.
//
// Mirrors the engine-free-core + thin-wrapper pattern of MeridianLogin (#99) and
// MeridianRemoteInterpolator (#104): ALL threading/queue/decode policy lives in the
// tested core; this class only (a) marshals Godot types in/out and (b) DRAINS the
// inbound ring at the pre-sim sync point, re-emitting each decoded event as a Godot
// signal for the scene.
//
// USAGE (Client SAD §2.2/§6.1 — the `net` module feeding `sim`):
//   1. Log in with MeridianLogin -> grant + session_key + WorldHello frame.
//   2. connect_to_world(host, port, world_hello_frame, session_key) -> spawns the
//      net thread (it connects, sends WorldHello, reads HandshakeOk off-thread).
//   3. Each physics tick, BEFORE simulation, call pump(): it drains every decoded
//      server event that arrived since the last tick and emits it as a signal
//      (handshake_ok / movement_state / entity_frame / disconnected / ...). This is
//      the fixed pre-sim sync point — the ONLY place inbound net state crosses into
//      the deterministic sim.
//   4. Send with send_movement_intent() / send_control() / send_bulk() — the frame
//      is queued at the matching priority and written by the net thread.
//   5. disconnect() (or freeing the object) stops + joins the thread cleanly.

#ifndef MERIDIAN_NET_THREAD_H
#define MERIDIAN_NET_THREAD_H

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "net_thread_core.h"

namespace meridian {

class MeridianNetThread : public godot::RefCounted {
	GDCLASS(MeridianNetThread, godot::RefCounted)

public:
	// GDScript-facing send priorities (mirror net::SendPriority) so a scene can call
	// send(frame, MeridianNetThread.PRIORITY_MOVEMENT) explicitly if it prefers the
	// generic entry point over the named helpers.
	enum SendPriorityCode {
		PRIORITY_CONTROL = 0,   // heartbeat / clock-sync / handshake / disconnect
		PRIORITY_MOVEMENT = 1,  // MovementIntent (real-time gameplay)
		PRIORITY_BULK = 2,      // best-effort (chat / inventory / asset pulls)
	};

protected:
	static void _bind_methods();

public:
	MeridianNetThread();
	~MeridianNetThread();

	// ── Lifecycle ────────────────────────────────────────────────────────────
	// Spawn the net thread and begin the IF-2 world session. `world_hello_frame` is
	// the frame from MeridianLogin.build_world_hello_frame(); `session_key` is the
	// 32-byte SessionGrant.session_key (may be empty at M0 — plaintext wire). The
	// connect/DNS/socket work happens on the new thread, NOT here. Returns false if
	// already connected or `world_hello_frame` is empty.
	bool connect_to_world(const godot::String &host, int port,
			const godot::PackedByteArray &world_hello_frame,
			const godot::PackedByteArray &session_key);

	// Stop + join the net thread and close the socket. Idempotent; also runs on free.
	void disconnect_from_world();

	bool is_running() const;      // thread alive (between connect and teardown/close)
	bool is_in_world() const;     // HandshakeOk seen

	// ── Pre-sim drain (the fixed sync point the main loop calls each tick) ──────
	// Drain every inbound event that arrived since the last call and emit each as a
	// signal. Returns the number of events drained this tick. Call BEFORE simulation.
	int pump();

	// ── Outbound sends (queue at a priority; the net thread writes them) ───────
	// Each takes an already-encoded IF-2 frame body. Returns false if that priority's
	// ring is full (back-pressure) or the thread is not running.
	bool send(const godot::PackedByteArray &frame, int priority);
	bool send_control(const godot::PackedByteArray &frame);
	bool send_movement_intent(const godot::PackedByteArray &frame);
	bool send_bulk(const godot::PackedByteArray &frame);

	// ── Entity-relay decode (the `sim`-layer handoff for the scene) ────────────
	// Decode a raw entity_frame (opcode + FlatBuffer payload, as delivered by the
	// `entity_frame` signal) into a scene-ready Dictionary using the SHARED clientnet
	// entity codec (#301 — the same decode the headless probe uses). Positions are
	// mapped from the wire (Z-UP: x/y ground, z height) to the Godot render frame
	// (Y-UP: y = height), so the scene feeds `position` straight to
	// MeridianRemoteInterpolator / a Node3D. Returns:
	//   { "kind": "enter"|"update"|"leave"|"",   # "" if not an entity opcode / bad
	//     "guid": int, "has_position": bool, "position": Vector3, "orientation": float,
	//     "type_id": int (enter), "reason": int (leave) }
	// A non-entity opcode or an undecodable payload returns { "kind": "" }.
	godot::Dictionary decode_entity_frame(int opcode,
			const godot::PackedByteArray &payload) const;

	// Encode a MovementIntent (from MeridianMovementController.predict()) into a ready-
	// to-send IF-2 frame, mapping the client Y-UP frame (x, y=height, z=ground) to the
	// wire Z-UP frame and wrapping with the shared clientnet codec + wire_frame (#301 —
	// the SAME encode the bot/probe use). `intent` is the predict() Dictionary
	// (seq, state_flags, x, y, z, orientation, client_time_ms). Pass the result to
	// send_movement_intent(). Returns an empty array on a malformed Dictionary.
	godot::PackedByteArray build_movement_intent_frame(const godot::Dictionary &intent) const;

	// ── Diagnostics (atomic counters from the core) ────────────────────────────
	int64_t frames_sent() const;
	int64_t frames_received() const;
	int64_t inbound_dropped() const;
	int64_t send_dropped() const;

private:
	std::unique_ptr<net::NetThreadCore> core_;
};

} // namespace meridian

VARIANT_ENUM_CAST(meridian::MeridianNetThread::SendPriorityCode);

#endif // MERIDIAN_NET_THREAD_H
