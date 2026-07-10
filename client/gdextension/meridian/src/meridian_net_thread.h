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

	// ── Combat frame builder + decode (CMB-01, D-10, #432) ─────────────────────
	// The action bar sends CAST_REQUEST on an ability press (target_guid = 0 means
	// self / no target; the server resolves legality) and decodes the server's ACCEPT
	// (CAST_START), REJECT (CAST_FAILED), and resolution (CAST_RESULT) — the SAME
	// decode seam as decode_entity_frame / decode_econ_frame. The client predicts the
	// optimistic GCD but NEVER the attack-table outcome (Principle 1).
	//   cast_request → CAST_START (accept) | CAST_FAILED (reject) → CAST_RESULT (resolve)
	godot::PackedByteArray build_cast_request_frame(int ability_id, int64_t target_guid,
			int64_t client_time_ms) const;

	// Decode a raw combat S→C frame (opcode + FlatBuffer payload, as delivered by the
	// `entity_frame` signal) into a scene-ready Dictionary the event bus publishes to the
	// action bar / cast bar view-models. Returns:
	//   { "kind": "cast_start"|"cast_failed"|"cast_result"|"",   # "" if not a combat op
	//     ...per-kind fields (see meridian_net_thread.cpp) }
	// A non-combat opcode or an undecodable payload returns { "kind": "" }, so the scene
	// can try the other decode seams first and fall through to this one.
	godot::Dictionary decode_cast_frame(int opcode,
			const godot::PackedByteArray &payload) const;

	// ── Character-select frame builders (D-35 / #286 / #341) ───────────────────
	// Each returns a ready-to-send IF-2 frame; pass to send_bulk() (char management)
	// or send_control() (ENTER_WORLD). The server's reply is drained by pump() and
	// re-emitted as the matching signal: char_list / char_create_result /
	// char_delete_result / enter_world_result. Char management requires an
	// authenticated (post-HandshakeOk / character-select) session.
	godot::PackedByteArray build_char_list_request_frame() const;
	godot::PackedByteArray build_char_create_request_frame(const godot::String &name,
			int race, int char_class) const;
	godot::PackedByteArray build_char_delete_request_frame(int64_t character_id) const;
	godot::PackedByteArray build_enter_world_request_frame(int64_t character_id) const;

	// ── Quest / gossip frame builders (QST-01 / NPC-01/02, #371/#372/#433) ──────
	// Each returns a ready-to-send IF-2 frame; pass to send_bulk(). The server's typed
	// reply is drained by pump() as an `entity_frame` (raw opcode+payload) and decoded
	// by decode_quest_frame() below (mirrors the entity-relay decode seam). The client
	// only issues intents + renders results — it never predicts quest state (Principle 1).
	//   gossip_hello   → GOSSIP_MENU (+ TRAINER_LIST for a trainer NPC)
	//   quest_accept   → QUEST_ACCEPT_RESULT (+ QUEST_LOG resync on OK)
	//   quest_turn_in  → QUEST_TURN_IN_RESULT (+ QUEST_LOG resync on OK); choice_index
	//                    is the reward pick (>= 0) or -1 for a no-choice quest
	//   quest_log_request → QUEST_LOG (the active-quest snapshot; an empty-table request)
	godot::PackedByteArray build_gossip_hello_frame(int64_t npc_guid) const;
	godot::PackedByteArray build_quest_accept_frame(int quest_id, int64_t giver_guid) const;
	godot::PackedByteArray build_quest_turn_in_frame(int quest_id, int64_t turn_in_guid,
			int choice_index) const;
	godot::PackedByteArray build_quest_log_request_frame() const;

	// Decode a raw quest/gossip S→C frame (opcode + FlatBuffer payload, as delivered by
	// the `entity_frame` signal) into a scene-ready Dictionary the event bus publishes
	// to the quest/gossip view-models. Returns:
	//   { "kind": "gossip_menu"|"quest_accept_result"|"quest_progress"|
	//             "quest_turn_in_result"|"quest_log"|"",   # "" if not a quest/gossip op
	//     ...per-kind fields (see meridian_net_thread.cpp) }
	// A non-quest/gossip opcode or an undecodable payload returns { "kind": "" }, so the
	// scene can try decode_entity_frame() first and fall through to this decoder.
	godot::Dictionary decode_quest_frame(int opcode,
			const godot::PackedByteArray &payload) const;

	// ── Loot / vendor / trainer frame builders (ITM-02/ECO-01/NPC-02, #369/370/372/441) ─
	// Each returns a ready-to-send IF-2 frame; pass to send_bulk(). The server's typed
	// reply is drained by pump() as an `entity_frame` (raw opcode+payload) and decoded by
	// decode_econ_frame() below (mirrors the quest/gossip decode seam). The client only
	// issues intents + renders results — it never rolls loot or prices an item (Principle 1).
	//   loot_request  → LOOT_RESPONSE (money + the slots this looter sees)
	//   loot_take     → LOOT_RESULT (a slot take, or the money pile when money = true)
	//   loot_release  → LOOT_CLOSED (close the window)
	//   vendor_buy    → VENDOR_BUY_RESULT     (minted item + debited copper + balance)
	//   vendor_sell   → VENDOR_SELL_RESULT    (credited copper + a buyback slot + balance)
	//   vendor_buyback→ VENDOR_BUYBACK_RESULT (re-minted item + re-debited copper + balance)
	//   trainer_learn → TRAINER_LEARN_RESULT  (learned + debited copper, or a typed reason)
	// There is NO trainer-list request builder: worldd PUSHES TRAINER_LIST alongside
	// GOSSIP_MENU when the player opens gossip on a trainer NPC (world_dispatch.cpp).
	godot::PackedByteArray build_loot_request_frame(int64_t corpse_guid) const;
	godot::PackedByteArray build_loot_take_frame(int64_t corpse_guid, int slot,
			bool money) const;
	godot::PackedByteArray build_loot_release_frame(int64_t corpse_guid) const;
	godot::PackedByteArray build_vendor_buy_frame(int vendor_id, int item_template_id,
			int quantity) const;
	godot::PackedByteArray build_vendor_sell_frame(int vendor_id, int backpack_slot,
			int quantity) const;
	godot::PackedByteArray build_vendor_buyback_frame(int buyback_slot) const;
	godot::PackedByteArray build_trainer_learn_frame(int64_t npc_guid, int ability_id) const;

	// Decode a raw loot/vendor/trainer S→C frame (opcode + FlatBuffer payload, as delivered
	// by the `entity_frame` signal) into a scene-ready Dictionary the event bus publishes to
	// the loot/vendor/trainer/bags view-models. Returns:
	//   { "kind": "loot_response"|"loot_result"|"loot_closed"|"vendor_buy_result"|
	//             "vendor_sell_result"|"vendor_buyback_result"|"trainer_list"|
	//             "trainer_learn_result"|"",   # "" if not a loot/vendor/trainer op
	//     ...per-kind fields (see meridian_net_thread.cpp) }
	// A non-econ opcode or an undecodable payload returns { "kind": "" }, so the scene can
	// try decode_entity_frame() / decode_quest_frame() first and fall through to this.
	godot::Dictionary decode_econ_frame(int opcode,
			const godot::PackedByteArray &payload) const;

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
