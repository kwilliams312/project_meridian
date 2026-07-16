// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — MeridianNetThread GDExtension binding (issue #97). Thin Godot
// glue over the engine-free net-thread core (net_thread_core.*): marshals Godot
// types in/out and drains the inbound SPSC ring at the pre-sim sync point (pump()),
// re-emitting each decoded server event as a Godot signal. All threading / queue /
// decode policy lives in the tested core. See meridian_net_thread.h.

#include "meridian_net_thread.h"

#include <cstring>
#include <string>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>  // MethodInfo / ADD_SIGNAL
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "meridian/clientnet/codec.h"
#include "meridian/clientnet/tls_client.h"
#include "meridian/clientnet/wire_frame.h"
#include "net_thread_messages.h"

using namespace godot;

namespace meridian {

namespace cn = meridian::clientnet;

namespace {

std::string to_std(const String &s) {
	return std::string(s.utf8().get_data());
}

// Copy a Godot PackedByteArray into a net::Bytes (std::vector<uint8_t>).
net::Bytes to_bytes(const PackedByteArray &a) {
	net::Bytes b;
	const int64_t n = a.size();
	if (n > 0) {
		b.resize(static_cast<std::size_t>(n));
		std::memcpy(b.data(), a.ptr(), static_cast<std::size_t>(n));
	}
	return b;
}

// Copy a net::Bytes into a Godot PackedByteArray.
PackedByteArray to_pba(const net::Bytes &b) {
	PackedByteArray a;
	a.resize(static_cast<int64_t>(b.size()));
	if (!b.empty()) {
		std::memcpy(a.ptrw(), b.data(), b.size());
	}
	return a;
}

}  // namespace

MeridianNetThread::MeridianNetThread() : core_(std::make_unique<net::NetThreadCore>()) {}

MeridianNetThread::~MeridianNetThread() {
	// Ensure the net thread is stopped + joined before the core is destroyed.
	if (core_) {
		core_->stop();
	}
}

void MeridianNetThread::_bind_methods() {
	ClassDB::bind_method(
	    D_METHOD("connect_to_world", "host", "port", "world_hello_frame", "session_key"),
	    &MeridianNetThread::connect_to_world);
	ClassDB::bind_method(D_METHOD("disconnect_from_world"),
	                     &MeridianNetThread::disconnect_from_world);
	ClassDB::bind_method(D_METHOD("is_running"), &MeridianNetThread::is_running);
	ClassDB::bind_method(D_METHOD("is_in_world"), &MeridianNetThread::is_in_world);
	ClassDB::bind_method(D_METHOD("pump"), &MeridianNetThread::pump);

	ClassDB::bind_method(D_METHOD("send", "frame", "priority"), &MeridianNetThread::send);
	ClassDB::bind_method(D_METHOD("send_control", "frame"), &MeridianNetThread::send_control);
	ClassDB::bind_method(D_METHOD("send_movement_intent", "frame"),
	                     &MeridianNetThread::send_movement_intent);
	ClassDB::bind_method(D_METHOD("send_bulk", "frame"), &MeridianNetThread::send_bulk);

	ClassDB::bind_method(D_METHOD("decode_entity_frame", "opcode", "payload"),
	                     &MeridianNetThread::decode_entity_frame);
	ClassDB::bind_method(D_METHOD("build_movement_intent_frame", "intent"),
	                     &MeridianNetThread::build_movement_intent_frame);

	ClassDB::bind_method(
	    D_METHOD("build_cast_request_frame", "ability_id", "target_guid", "client_time_ms"),
	    &MeridianNetThread::build_cast_request_frame);
	ClassDB::bind_method(D_METHOD("decode_cast_frame", "opcode", "payload"),
	                     &MeridianNetThread::decode_cast_frame);

	ClassDB::bind_method(D_METHOD("build_release_request_frame"),
	                     &MeridianNetThread::build_release_request_frame);
	ClassDB::bind_method(D_METHOD("build_resurrect_request_frame"),
	                     &MeridianNetThread::build_resurrect_request_frame);
	ClassDB::bind_method(D_METHOD("decode_death_frame", "opcode", "payload"),
	                     &MeridianNetThread::decode_death_frame);

	ClassDB::bind_method(D_METHOD("build_char_list_request_frame"),
	                     &MeridianNetThread::build_char_list_request_frame);
	ClassDB::bind_method(
	    D_METHOD("build_char_create_request_frame", "name", "race", "char_class",
	             "hair", "face", "skin"),
	    &MeridianNetThread::build_char_create_request_frame);
	ClassDB::bind_method(D_METHOD("build_char_delete_request_frame", "character_id"),
	                     &MeridianNetThread::build_char_delete_request_frame);
	ClassDB::bind_method(D_METHOD("build_enter_world_request_frame", "character_id"),
	                     &MeridianNetThread::build_enter_world_request_frame);

	ClassDB::bind_method(D_METHOD("build_gossip_hello_frame", "npc_guid"),
	                     &MeridianNetThread::build_gossip_hello_frame);
	ClassDB::bind_method(D_METHOD("build_quest_accept_frame", "quest_id", "giver_guid"),
	                     &MeridianNetThread::build_quest_accept_frame);
	ClassDB::bind_method(
	    D_METHOD("build_quest_turn_in_frame", "quest_id", "turn_in_guid", "choice_index"),
	    &MeridianNetThread::build_quest_turn_in_frame);
	ClassDB::bind_method(D_METHOD("build_quest_log_request_frame"),
	                     &MeridianNetThread::build_quest_log_request_frame);
	ClassDB::bind_method(D_METHOD("decode_quest_frame", "opcode", "payload"),
	                     &MeridianNetThread::decode_quest_frame);

	ClassDB::bind_method(D_METHOD("build_loot_request_frame", "corpse_guid"),
	                     &MeridianNetThread::build_loot_request_frame);
	ClassDB::bind_method(D_METHOD("build_loot_take_frame", "corpse_guid", "slot", "money"),
	                     &MeridianNetThread::build_loot_take_frame);
	ClassDB::bind_method(D_METHOD("build_loot_release_frame", "corpse_guid"),
	                     &MeridianNetThread::build_loot_release_frame);
	ClassDB::bind_method(
	    D_METHOD("build_vendor_buy_frame", "vendor_id", "item_template_id", "quantity"),
	    &MeridianNetThread::build_vendor_buy_frame);
	ClassDB::bind_method(
	    D_METHOD("build_vendor_sell_frame", "vendor_id", "backpack_slot", "quantity"),
	    &MeridianNetThread::build_vendor_sell_frame);
	ClassDB::bind_method(D_METHOD("build_vendor_buyback_frame", "buyback_slot"),
	                     &MeridianNetThread::build_vendor_buyback_frame);
	ClassDB::bind_method(D_METHOD("build_trainer_learn_frame", "npc_guid", "ability_id"),
	                     &MeridianNetThread::build_trainer_learn_frame);
	ClassDB::bind_method(D_METHOD("build_equipment_change_frame", "action", "slot"),
	                     &MeridianNetThread::build_equipment_change_frame);
	ClassDB::bind_method(D_METHOD("decode_econ_frame", "opcode", "payload"),
	                     &MeridianNetThread::decode_econ_frame);

	ClassDB::bind_method(
	    D_METHOD("build_chat_message_frame", "channel", "target", "text"),
	    &MeridianNetThread::build_chat_message_frame);
	ClassDB::bind_method(D_METHOD("decode_chat_frame", "opcode", "payload"),
	                     &MeridianNetThread::decode_chat_frame);

	ClassDB::bind_method(D_METHOD("frames_sent"), &MeridianNetThread::frames_sent);
	ClassDB::bind_method(D_METHOD("frames_received"), &MeridianNetThread::frames_received);
	ClassDB::bind_method(D_METHOD("inbound_dropped"), &MeridianNetThread::inbound_dropped);
	ClassDB::bind_method(D_METHOD("send_dropped"), &MeridianNetThread::send_dropped);

	// GDScript-facing priority ordinals.
	BIND_ENUM_CONSTANT(PRIORITY_CONTROL);
	BIND_ENUM_CONSTANT(PRIORITY_MOVEMENT);
	BIND_ENUM_CONSTANT(PRIORITY_BULK);

	// Inbound events, re-emitted from pump() (the pre-sim sync point).
	ADD_SIGNAL(MethodInfo("handshake_ok"));
	ADD_SIGNAL(MethodInfo("movement_state", PropertyInfo(Variant::DICTIONARY, "state")));
	ADD_SIGNAL(MethodInfo("entity_frame", PropertyInfo(Variant::INT, "opcode"),
	                      PropertyInfo(Variant::INT, "seq"),
	                      PropertyInfo(Variant::PACKED_BYTE_ARRAY, "payload")));
	ADD_SIGNAL(MethodInfo("disconnected", PropertyInfo(Variant::INT, "reason"),
	                      PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("transport_closed", PropertyInfo(Variant::STRING, "detail")));
	ADD_SIGNAL(MethodInfo("connect_failed", PropertyInfo(Variant::STRING, "detail")));

	// Character-select round-trips (D-35 / #286 / #341), re-emitted from pump().
	// char_list carries an Array of Dictionaries {id,name,race,char_class,level}
	// plus a typed status (world.fbs CharListStatus: 0 OK, 1 INTERNAL). status!=0
	// means the roster could NOT be read (server/DB fault, #479) — an empty
	// `characters` is a LOAD FAILURE, not "zero characters".
	ADD_SIGNAL(MethodInfo("char_list", PropertyInfo(Variant::ARRAY, "characters"),
	                      PropertyInfo(Variant::INT, "status")));
	ADD_SIGNAL(MethodInfo("char_create_result", PropertyInfo(Variant::INT, "status"),
	                      PropertyInfo(Variant::INT, "character_id")));
	ADD_SIGNAL(MethodInfo("char_delete_result", PropertyInfo(Variant::INT, "status")));
	// enter_world_result: EnterWorldStatus (0=OK spawned, 1=NOT_FOUND, 2=NO_CHARACTER,
	// 3=INTERNAL). The scene switches to the world scene only on OK.
	ADD_SIGNAL(MethodInfo("enter_world_result", PropertyInfo(Variant::INT, "status")));
}

bool MeridianNetThread::connect_to_world(const String &host, int port,
		const PackedByteArray &world_hello_frame, const PackedByteArray &session_key) {
	if (!core_ || core_->running()) {
		return false;
	}
	net::NetThreadConfig cfg;
	cfg.world_hello_frame = to_bytes(world_hello_frame);
	cfg.session_key = to_bytes(session_key);
	if (cfg.world_hello_frame.empty()) {
		return false;
	}

	const std::string host_s = to_std(host);
	const std::uint16_t port_u = static_cast<std::uint16_t>(port);
	// The connect runs ON the net thread — the socket is never created on the main
	// thread. worldd's IF-2 world listener is TLS 1.3 (server/worldd/main.cpp: cert/key
	// are "Required to serve", and the proven headless bot connects to worldd via TLS),
	// so the client MUST speak TLS too — a plain-TCP connect is rejected by worldd's
	// handshake ("wrong version number"). (#301: this replaces the earlier plain
	// TcpTransport, whose "world channel is plain TCP at M0" assumption never matched
	// the real worldd deployment — the client could not enter the world without it.)
	net::ConnectFn connect = [host_s, port_u]() -> std::unique_ptr<cn::ITransport> {
		auto t = std::make_unique<cn::TlsClientTransport>(host_s, port_u);
		if (!t->ok()) {
			return nullptr;
		}
		return t;
	};
	return core_->start(std::move(connect), std::move(cfg));
}

void MeridianNetThread::disconnect_from_world() {
	if (core_) {
		core_->stop();
	}
}

bool MeridianNetThread::is_running() const { return core_ && core_->running(); }
bool MeridianNetThread::is_in_world() const { return core_ && core_->entered_world(); }

int MeridianNetThread::pump() {
	if (!core_) {
		return 0;
	}
	int drained = 0;
	net::InboundMessage msg;
	while (core_->try_pop_inbound(msg)) {
		++drained;
		switch (msg.kind) {
			case net::InboundKind::kHandshakeOk:
				emit_signal("handshake_ok");
				break;
			case net::InboundKind::kMovementState: {
				Dictionary d;
				d["entity_guid"] = static_cast<int64_t>(msg.state.entity_guid);
				d["ack_seq"] = static_cast<int64_t>(msg.state.ack_seq);
				d["state_flags"] = static_cast<int64_t>(msg.state.state_flags);
				d["x"] = msg.state.x;
				d["y"] = msg.state.y;
				d["z"] = msg.state.z;
				d["orientation"] = msg.state.orientation;
				d["server_time_ms"] = static_cast<int64_t>(msg.state.server_time_ms);
				emit_signal("movement_state", d);
				break;
			}
			case net::InboundKind::kEntityFrame:
				emit_signal("entity_frame", static_cast<int64_t>(msg.opcode),
				            static_cast<int64_t>(msg.seq), to_pba(msg.payload));
				break;
			case net::InboundKind::kDisconnect:
				emit_signal("disconnected", static_cast<int64_t>(msg.disc.reason),
				            String(msg.detail.c_str()));
				break;
			case net::InboundKind::kTransportClosed:
				emit_signal("transport_closed", String(msg.detail.c_str()));
				break;
			case net::InboundKind::kConnectFailed:
				emit_signal("connect_failed", String(msg.detail.c_str()));
				break;
			case net::InboundKind::kCharList: {
				Array chars;
				for (const auto &c : msg.roster.characters) {
					Dictionary row;
					row["id"] = static_cast<int64_t>(c.character_id);
					row["name"] = String(c.name.c_str());
					row["race"] = static_cast<int64_t>(c.race);
					row["class"] = static_cast<int64_t>(c.char_class);
					row["level"] = static_cast<int64_t>(c.level);
					// Persisted appearance (②/T4, #541): char-select re-assembles the
					// preview from this on roster selection. Present only when the row
					// carried the record (contract ① T5); shape matches assemble()'s
					// appearance {v,hair,face,skin} (unified with the EntityEnter path
					// below — story #550).
					if (c.has_appearance) {
						Dictionary appearance;
						appearance["v"] = static_cast<int64_t>(c.appearance.version);
						appearance["hair"] = static_cast<int64_t>(c.appearance.hair);
						appearance["face"] = static_cast<int64_t>(c.appearance.face);
						appearance["skin"] = static_cast<int64_t>(c.appearance.skin);
						row["appearance"] = appearance;
					}
					chars.push_back(row);
				}
				emit_signal("char_list", chars,
				            static_cast<int64_t>(msg.roster.status));
				break;
			}
			case net::InboundKind::kCharCreate:
				emit_signal("char_create_result",
				            static_cast<int64_t>(msg.char_create.status),
				            static_cast<int64_t>(msg.char_create.character_id));
				break;
			case net::InboundKind::kCharDelete:
				emit_signal("char_delete_result",
				            static_cast<int64_t>(msg.char_delete.status));
				break;
			case net::InboundKind::kEnterWorld:
				emit_signal("enter_world_result",
				            static_cast<int64_t>(msg.enter_world.status));
				break;
		}
	}
	return drained;
}

bool MeridianNetThread::send(const PackedByteArray &frame, int priority) {
	if (!core_ || priority < 0 ||
	    priority >= static_cast<int>(net::kSendPriorityCount)) {
		return false;
	}
	return core_->send(to_bytes(frame), static_cast<net::SendPriority>(priority));
}

bool MeridianNetThread::send_control(const PackedByteArray &frame) {
	return core_ && core_->send(to_bytes(frame), net::SendPriority::kControl);
}
bool MeridianNetThread::send_movement_intent(const PackedByteArray &frame) {
	return core_ && core_->send(to_bytes(frame), net::SendPriority::kMovement);
}
bool MeridianNetThread::send_bulk(const PackedByteArray &frame) {
	return core_ && core_->send(to_bytes(frame), net::SendPriority::kBulk);
}

Dictionary MeridianNetThread::decode_entity_frame(int opcode,
		const PackedByteArray &payload) const {
	Dictionary d;
	d["kind"] = String("");
	const net::Bytes buf = to_bytes(payload);
	// Wire (Z-UP: x/y ground, z height) -> Godot render frame (Y-UP: y = height).
	auto to_godot = [](float wx, float wy, float wz) {
		return Vector3(wx, /*y=height*/ wz, /*z=ground*/ wy);
	};

	if (opcode == cn::kOpEntityEnter) {
		auto e = cn::codec::decode_entity_enter(buf);
		if (!e) return d;
		d["kind"] = String("enter");
		d["guid"] = static_cast<int64_t>(e->entity_guid);
		d["type_id"] = static_cast<int64_t>(e->type_id);
		d["char_class"] = static_cast<int64_t>(e->char_class);  // #328: class→color
		d["has_position"] = true;
		d["position"] = to_godot(e->x, e->y, e->z);
		d["orientation"] = e->orientation;
		// Vitals (#430/#431 HUD contract): the unit-frame block the event bus routes
		// to the player/target frames (UI-01). Server-authoritative — display only.
		d["health"] = static_cast<int64_t>(e->health);
		d["max_health"] = static_cast<int64_t>(e->max_health);
		d["power"] = static_cast<int64_t>(e->power);
		d["max_power"] = static_cast<int64_t>(e->max_power);
		d["power_type"] = static_cast<int64_t>(e->power_type);
		d["level"] = static_cast<int64_t>(e->level);
		d["name"] = String(e->name.c_str());
		// Visual-assembly fields (②/T4, #541): present ONLY for player entities (the
		// codec sets has_appearance when the Appearance table rode the frame). NPCs /
		// pre-#538 servers omit them, so world.gd's `d.has("appearance")` gate keeps the
		// capsule path untouched (design §6). Shapes match AssembledCharacter.assemble():
		// appearance {v,hair,face,skin}; equipment [{slot, item_template,
		// dyes:[{channel, dye_id}]}] — the per-channel dye shape (⑤/S3, #570) the
		// mask-tint assembler needs to bind each RGB dye-mask region to its colour.
		if (e->has_appearance) {
			d["race"] = static_cast<int64_t>(e->race);
			d["sex"] = static_cast<int64_t>(e->sex);
			Dictionary appearance;
			appearance["v"] = static_cast<int64_t>(e->appearance.version);
			appearance["hair"] = static_cast<int64_t>(e->appearance.hair);
			appearance["face"] = static_cast<int64_t>(e->appearance.face);
			appearance["skin"] = static_cast<int64_t>(e->appearance.skin);
			d["appearance"] = appearance;
			Array equipment;
			for (const auto &ev : e->equipment) {
				Dictionary slot;
				slot["slot"] = static_cast<int64_t>(ev.slot);
				slot["item_template"] = static_cast<int64_t>(ev.item_template);
				// AssembledCharacter._apply_dyes takes an Array of {channel, dye_id}
				// dicts (⑤/S3, #570) — the dye `channel` (0=primary/1=secondary/
				// 2=accent) selects which RGB region of the piece's dye mask this
				// colour tints. Forwarding the channel (not a bare id) is the whole
				// point of the mask-tint path; dropping it flattens every dye onto the
				// primary region.
				Array dyes;
				for (const auto &dc : ev.dyes) {
					Dictionary dye;
					dye["channel"] = static_cast<int64_t>(dc.channel);
					dye["dye_id"] = static_cast<int64_t>(dc.dye_id);
					dyes.push_back(dye);
				}
				slot["dyes"] = dyes;
				equipment.push_back(slot);
			}
			d["equipment"] = equipment;
		}
	} else if (opcode == cn::kOpVitalsUpdate) {
		// VITALS_UPDATE (#430/#431): the HUD health/power/level delta. Forwarded raw
		// by the net thread (net_thread_core's default S→C path), decoded here into a
		// scene-ready Dictionary the event bus publishes to the unit frames.
		auto v = cn::codec::decode_vitals_update(buf);
		if (!v) return d;
		d["kind"] = String("vitals");
		d["guid"] = static_cast<int64_t>(v->entity_guid);
		d["has_position"] = false;
		d["health"] = static_cast<int64_t>(v->health);
		d["max_health"] = static_cast<int64_t>(v->max_health);
		d["power"] = static_cast<int64_t>(v->power);
		d["max_power"] = static_cast<int64_t>(v->max_power);
		d["power_type"] = static_cast<int64_t>(v->power_type);
		d["level"] = static_cast<int64_t>(v->level);
	} else if (opcode == cn::kOpXpGained) {
		// XP_GAINED (CHR-03, #531): the HUD XP-bar delta. Forwarded raw by the net thread
		// (net_thread_core's default S→C path), decoded here into a scene-ready Dictionary
		// the event bus publishes to the XP bar. Server-authoritative — display only.
		auto x = cn::codec::decode_xp_gained(buf);
		if (!x) return d;
		d["kind"] = String("xp");
		d["guid"] = static_cast<int64_t>(x->player_guid);
		d["has_position"] = false;
		d["xp_gained"] = static_cast<int64_t>(x->xp_gained);
		d["level"] = static_cast<int64_t>(x->level);
		d["xp_total"] = static_cast<int64_t>(x->xp_total);
		d["xp_to_next"] = static_cast<int64_t>(x->xp_to_next);
	} else if (opcode == cn::kOpLevelUp) {
		// LEVEL_UP (CHR-03, #531): the player dinged. Decoded into the new level + the
		// stat growth (new health / secondary-resource caps) the event bus routes to the
		// level-up presentation + the player unit frame. Server-authoritative.
		auto l = cn::codec::decode_level_up(buf);
		if (!l) return d;
		d["kind"] = String("level_up");
		d["guid"] = static_cast<int64_t>(l->player_guid);
		d["has_position"] = false;
		d["old_level"] = static_cast<int64_t>(l->old_level);
		d["new_level"] = static_cast<int64_t>(l->new_level);
		d["max_health"] = static_cast<int64_t>(l->max_health);
		d["max_resource"] = static_cast<int64_t>(l->max_resource);
	} else if (opcode == cn::kOpEntityUpdate) {
		auto u = cn::codec::decode_entity_update(buf);
		if (!u) return d;
		d["kind"] = String("update");
		d["guid"] = static_cast<int64_t>(u->entity_guid);
		d["has_position"] = u->has_position();
		d["position"] = to_godot(u->x, u->y, u->z);
		d["orientation"] = u->orientation;
	} else if (opcode == cn::kOpEntityLeave) {
		auto l = cn::codec::decode_entity_leave(buf);
		if (!l) return d;
		d["kind"] = String("leave");
		d["guid"] = static_cast<int64_t>(l->entity_guid);
		d["has_position"] = false;
		d["reason"] = static_cast<int64_t>(l->reason);
	} else if (opcode == cn::kOpEquipmentVisualUpdate) {
		auto u = cn::codec::decode_equipment_visual_update(buf);
		if (!u) return d;
		d["kind"] = String("equipment_visual");
		d["guid"] = static_cast<int64_t>(u->entity_guid);
		d["has_position"] = false;
		Array equipment;
		for (const auto &ev : u->equipment) {
			Dictionary row;
			row["slot"] = static_cast<int64_t>(ev.slot);
			row["item_template"] = static_cast<int64_t>(ev.item_template);
			Array dyes;
			for (const auto &dc : ev.dyes) {
				Dictionary dye;
				dye["channel"] = static_cast<int64_t>(dc.channel);
				dye["dye_id"] = static_cast<int64_t>(dc.dye_id);
				dyes.push_back(dye);
			}
			row["dyes"] = dyes;
			equipment.push_back(row);
		}
		d["equipment"] = equipment;
	}
	return d;  // kind stays "" for a non-entity opcode / undecodable payload
}

PackedByteArray MeridianNetThread::build_movement_intent_frame(
		const Dictionary &intent) const {
	// The predict() Dictionary is in the client Y-UP frame (x, y=height, z=ground);
	// map to the wire Z-UP frame exactly as the bot/probe do at the wire boundary.
	if (!intent.has("seq") || !intent.has("x") || !intent.has("y") || !intent.has("z")) {
		return PackedByteArray();
	}
	cn::codec::MovementIntent mi;
	mi.seq = static_cast<std::uint32_t>(static_cast<int64_t>(intent["seq"]));
	mi.state_flags = static_cast<std::uint32_t>(
	    static_cast<int64_t>(intent.get("state_flags", 0)));
	const float cx = static_cast<float>(static_cast<double>(intent["x"]));
	const float cy = static_cast<float>(static_cast<double>(intent["y"]));  // height
	const float cz = static_cast<float>(static_cast<double>(intent["z"]));  // ground Z
	mi.x = cx;   // ground X
	mi.y = cz;   // wire y = client ground Z
	mi.z = cy;   // wire z = client height
	mi.orientation = static_cast<float>(static_cast<double>(intent.get("orientation", 0.0)));
	mi.client_time_ms =
	    static_cast<std::uint64_t>(static_cast<int64_t>(intent.get("client_time_ms", 0)));

	net::Bytes payload = cn::codec::encode_movement_intent(mi);
	net::Bytes frame = cn::encode_world_frame(cn::kOpMovementIntent, mi.seq, payload);
	return to_pba(frame);
}

// ── Combat frame builder + decode (CMB-01, D-10, #432) ───────────────────────
// The action bar builds CAST_REQUEST on a press (send_bulk); the server's ACCEPT /
// REJECT / resolution arrive as raw `entity_frame`s decoded by decode_cast_frame.

PackedByteArray MeridianNetThread::build_cast_request_frame(
		int ability_id, int64_t target_guid, int64_t client_time_ms) const {
	cn::codec::CastRequest req;
	req.ability_id = static_cast<std::uint32_t>(ability_id);
	req.target_guid = static_cast<std::uint64_t>(target_guid);
	req.client_time_ms = static_cast<std::uint64_t>(client_time_ms);
	net::Bytes payload = cn::codec::encode_cast_request(req);
	return to_pba(cn::encode_world_frame(cn::kOpCastRequest, /*seq=*/1, payload));
}

Dictionary MeridianNetThread::decode_cast_frame(int opcode,
		const PackedByteArray &payload) const {
	Dictionary d;
	d["kind"] = String("");
	const net::Bytes buf = to_bytes(payload);

	if (opcode == cn::kOpCastStart) {
		auto r = cn::codec::decode_cast_start(buf);
		if (!r) return d;
		d["kind"] = String("cast_start");
		d["ability_id"] = static_cast<int64_t>(r->ability_id);
		d["cast_ms"] = static_cast<int64_t>(r->cast_ms);
		d["server_time_ms"] = static_cast<int64_t>(r->server_time_ms);
	} else if (opcode == cn::kOpCastFailed) {
		auto r = cn::codec::decode_cast_failed(buf);
		if (!r) return d;
		d["kind"] = String("cast_failed");
		d["ability_id"] = static_cast<int64_t>(r->ability_id);
		d["reason"] = static_cast<int64_t>(r->reason);  // CastFailReason
		d["gcd_remaining_ms"] = static_cast<int64_t>(r->gcd_remaining_ms);
	} else if (opcode == cn::kOpCastResult) {
		auto r = cn::codec::decode_cast_result(buf);
		if (!r) return d;
		d["kind"] = String("cast_result");
		d["ability_id"] = static_cast<int64_t>(r->ability_id);
		d["caster_guid"] = static_cast<int64_t>(r->caster_guid);
		d["target_guid"] = static_cast<int64_t>(r->target_guid);
		d["outcome"] = static_cast<int64_t>(r->outcome);  // AttackOutcome
		d["amount"] = static_cast<int64_t>(r->amount);
		d["is_heal"] = r->is_heal;
		d["target_health"] = static_cast<int64_t>(r->target_health);
		d["target_dead"] = r->target_dead;
		d["server_time_ms"] = static_cast<int64_t>(r->server_time_ms);
	} else if (opcode == cn::kOpKnownAbilities) {
		auto r = cn::codec::decode_known_abilities(buf);
		if (!r) return d;
		d["kind"] = String("known_abilities");
		Array abilities;
		for (const auto &a : r->abilities) {
			Dictionary row;
			row["ability_id"] = static_cast<int64_t>(a.ability_id);
			row["cast_ms"] = static_cast<int64_t>(a.cast_ms);
			row["triggers_gcd"] = a.triggers_gcd;
			row["resource_type"] = static_cast<int64_t>(a.resource_type);  // AbilityResource
			row["resource_cost"] = static_cast<int64_t>(a.resource_cost);
			row["range_m"] = a.range_m;
			abilities.push_back(row);
		}
		d["abilities"] = abilities;
	}
	return d;  // kind stays "" for a non-combat opcode / undecodable payload
}

// ── Death / ghost / resurrect frame builders + decode (CMB-03, #359/#532) ────
// The death overlay builds RELEASE_REQUEST (early graveyard release) and the ghost
// presentation builds RESURRECT_REQUEST (resurrect at the corpse); both are empty C→S
// bodies (send_control). The server's DEATH_STATE / GHOST_STATE / RESURRECT_RESULT arrive
// as raw `entity_frame`s decoded by decode_death_frame. Presentation-only (never predicted).

PackedByteArray MeridianNetThread::build_release_request_frame() const {
	net::Bytes payload = cn::codec::encode_release_request();
	return to_pba(cn::encode_world_frame(cn::kOpReleaseRequest, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_resurrect_request_frame() const {
	net::Bytes payload = cn::codec::encode_resurrect_request();
	return to_pba(cn::encode_world_frame(cn::kOpResurrectRequest, /*seq=*/1, payload));
}

Dictionary MeridianNetThread::decode_death_frame(int opcode,
		const PackedByteArray &payload) const {
	Dictionary d;
	d["kind"] = String("");
	const net::Bytes buf = to_bytes(payload);
	// Wire (Z-UP: x/y ground, z height) -> Godot render frame (Y-UP: y = height), the SAME
	// mapping decode_entity_frame uses, so the scene can compare corpse/graveyard positions
	// against the local player's render position directly.
	auto to_godot = [](float wx, float wy, float wz) {
		return Vector3(wx, /*y=height*/ wz, /*z=ground*/ wy);
	};

	if (opcode == cn::kOpDeathState) {
		auto s = cn::codec::decode_death_state(buf);
		if (!s) return d;
		d["kind"] = String("death");
		d["victim_guid"] = static_cast<int64_t>(s->victim_guid);
		d["killer_guid"] = static_cast<int64_t>(s->killer_guid);
		d["corpse_guid"] = static_cast<int64_t>(s->corpse_guid);
		d["corpse_position"] = to_godot(s->corpse_x, s->corpse_y, s->corpse_z);
		d["auto_release_ms"] = static_cast<int64_t>(s->auto_release_ms);
	} else if (opcode == cn::kOpGhostState) {
		auto g = cn::codec::decode_ghost_state(buf);
		if (!g) return d;
		d["kind"] = String("ghost");
		d["player_guid"] = static_cast<int64_t>(g->player_guid);
		d["graveyard_position"] = to_godot(g->graveyard_x, g->graveyard_y, g->graveyard_z);
		d["corpse_guid"] = static_cast<int64_t>(g->corpse_guid);
	} else if (opcode == cn::kOpResurrectResult) {
		auto r = cn::codec::decode_resurrect_result(buf);
		if (!r) return d;
		d["kind"] = String("resurrect_result");
		d["player_guid"] = static_cast<int64_t>(r->player_guid);
		d["status"] = static_cast<int64_t>(r->status);  // ResurrectStatus
		d["health"] = static_cast<int64_t>(r->health);
		d["max_health"] = static_cast<int64_t>(r->max_health);
	}
	return d;  // kind stays "" for a non-death opcode / undecodable payload
}

// ── Character-select frame builders (D-35 / #286 / #341) ─────────────────────
// Seq is a per-message counter the server echoes; it is not correlated by the
// client (responses are matched by opcode/signal), so a fixed non-zero seq is fine.

PackedByteArray MeridianNetThread::build_char_list_request_frame() const {
	net::Bytes payload = cn::codec::encode_char_list_request();
	return to_pba(cn::encode_world_frame(cn::kOpCharListReq, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_char_create_request_frame(
		const String &name, int race, int char_class,
		int hair, int face, int skin) const {
	cn::codec::CharCreateRequest req;
	req.name = to_std(name);
	req.race = static_cast<std::uint8_t>(race);
	req.char_class = static_cast<std::uint8_t>(char_class);
	// Appearance set (CHR-01 #435). version stays 1 (only v1 exists at M1); hair/
	// face/skin are the 1-based preset ids the player picked (MeridianAppearance).
	req.appearance.hair = static_cast<std::uint8_t>(hair);
	req.appearance.face = static_cast<std::uint8_t>(face);
	req.appearance.skin = static_cast<std::uint8_t>(skin);
	net::Bytes payload = cn::codec::encode_char_create_request(req);
	return to_pba(cn::encode_world_frame(cn::kOpCharCreateReq, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_char_delete_request_frame(
		int64_t character_id) const {
	net::Bytes payload = cn::codec::encode_char_delete_request(
	    static_cast<std::uint64_t>(character_id));
	return to_pba(cn::encode_world_frame(cn::kOpCharDeleteReq, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_enter_world_request_frame(
		int64_t character_id) const {
	net::Bytes payload = cn::codec::encode_enter_world_request(
	    static_cast<std::uint64_t>(character_id));
	return to_pba(cn::encode_world_frame(cn::kOpEnterWorldReq, /*seq=*/1, payload));
}

// ── Quest / gossip frame builders (QST-01 / NPC-01/02, #371/#372/#433) ───────
// Client → server intents. Seq is a per-message counter the server echoes; the client
// matches replies by opcode (decode_quest_frame), so a fixed non-zero seq is fine
// (mirrors the char-select builders above).

PackedByteArray MeridianNetThread::build_gossip_hello_frame(int64_t npc_guid) const {
	cn::codec::GossipHello in;
	in.npc_guid = static_cast<std::uint64_t>(npc_guid);
	net::Bytes payload = cn::codec::encode_gossip_hello(in);
	return to_pba(cn::encode_world_frame(cn::kOpGossipHello, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_quest_accept_frame(int quest_id,
		int64_t giver_guid) const {
	cn::codec::QuestAccept in;
	in.quest_id = static_cast<std::uint32_t>(quest_id);
	in.giver_guid = static_cast<std::uint64_t>(giver_guid);
	net::Bytes payload = cn::codec::encode_quest_accept(in);
	return to_pba(cn::encode_world_frame(cn::kOpQuestAccept, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_quest_turn_in_frame(int quest_id,
		int64_t turn_in_guid, int choice_index) const {
	cn::codec::QuestTurnIn in;
	in.quest_id = static_cast<std::uint32_t>(quest_id);
	in.turn_in_guid = static_cast<std::uint64_t>(turn_in_guid);
	in.choice_index = static_cast<std::int32_t>(choice_index);
	net::Bytes payload = cn::codec::encode_quest_turn_in(in);
	return to_pba(cn::encode_world_frame(cn::kOpQuestTurnIn, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_quest_log_request_frame() const {
	// The C→S request is an EMPTY QuestLog table body (world.fbs QuestLog "sent on …
	// resync"); worldd replies with the populated snapshot under the same opcode.
	net::Bytes payload = cn::codec::encode_quest_log(cn::codec::QuestLog{});
	return to_pba(cn::encode_world_frame(cn::kOpQuestLog, /*seq=*/1, payload));
}

Dictionary MeridianNetThread::decode_quest_frame(int opcode,
		const PackedByteArray &payload) const {
	Dictionary d;
	d["kind"] = String("");
	const net::Bytes buf = to_bytes(payload);

	if (opcode == cn::kOpGossipMenu) {
		auto m = cn::codec::decode_gossip_menu(buf);
		if (!m) return d;
		d["kind"] = String("gossip_menu");
		d["npc_guid"] = static_cast<int64_t>(m->npc_guid);
		Array options;
		for (const auto &o : m->options) {
			Dictionary row;
			row["kind"] = static_cast<int64_t>(o.kind);            // GossipOptionKind
			row["target_id"] = static_cast<int64_t>(o.target_id);  // quest id (quest kinds)
			options.push_back(row);
		}
		d["options"] = options;
	} else if (opcode == cn::kOpQuestAcceptResult) {
		auto r = cn::codec::decode_quest_accept_result(buf);
		if (!r) return d;
		d["kind"] = String("quest_accept_result");
		d["quest_id"] = static_cast<int64_t>(r->quest_id);
		d["status"] = static_cast<int64_t>(r->status);  // QuestAcceptStatus
	} else if (opcode == cn::kOpQuestProgress) {
		auto p = cn::codec::decode_quest_progress(buf);
		if (!p) return d;
		d["kind"] = String("quest_progress");
		d["quest_id"] = static_cast<int64_t>(p->quest_id);
		d["objective_index"] = static_cast<int64_t>(p->objective_index);
		d["type"] = static_cast<int64_t>(p->type);  // QuestObjectiveType
		d["have"] = static_cast<int64_t>(p->have);
		d["need"] = static_cast<int64_t>(p->need);
		d["complete"] = p->complete;
	} else if (opcode == cn::kOpQuestTurnInResult) {
		auto r = cn::codec::decode_quest_turn_in_result(buf);
		if (!r) return d;
		d["kind"] = String("quest_turn_in_result");
		d["quest_id"] = static_cast<int64_t>(r->quest_id);
		d["status"] = static_cast<int64_t>(r->status);  // QuestTurnInStatus
		d["reward_xp"] = static_cast<int64_t>(r->reward_xp);
		d["reward_money"] = static_cast<int64_t>(r->reward_money);
		d["new_level"] = static_cast<int64_t>(r->new_level);
		Array items;
		for (const auto &it : r->reward_items) {
			Dictionary row;
			row["item_id"] = static_cast<int64_t>(it.item_id);
			row["count"] = static_cast<int64_t>(it.count);
			items.push_back(row);
		}
		d["reward_items"] = items;
	} else if (opcode == cn::kOpQuestLog) {
		auto log = cn::codec::decode_quest_log(buf);
		if (!log) return d;
		d["kind"] = String("quest_log");
		Array quests;
		for (const auto &q : log->quests) {
			Dictionary qd;
			qd["quest_id"] = static_cast<int64_t>(q.quest_id);
			qd["level"] = static_cast<int64_t>(q.level);
			qd["complete"] = q.complete;
			Array objs;
			for (const auto &o : q.objectives) {
				Dictionary od;
				od["type"] = static_cast<int64_t>(o.type);
				od["target_id"] = static_cast<int64_t>(o.target_id);
				od["have"] = static_cast<int64_t>(o.have);
				od["need"] = static_cast<int64_t>(o.need);
				od["complete"] = o.complete;
				objs.push_back(od);
			}
			qd["objectives"] = objs;
			// Reward PREVIEW (#443): flat XP + copper, always-granted items, one-of choices.
			qd["reward_xp"] = static_cast<int64_t>(q.reward_xp);
			qd["reward_money"] = static_cast<int64_t>(q.reward_money);
			Array rewards;
			for (const auto &it : q.reward_items) {
				Dictionary row;
				row["item_id"] = static_cast<int64_t>(it.item_id);
				row["count"] = static_cast<int64_t>(it.count);
				rewards.push_back(row);
			}
			qd["reward_items"] = rewards;
			Array choices;
			for (const auto &it : q.choice_items) {
				Dictionary row;
				row["item_id"] = static_cast<int64_t>(it.item_id);
				row["count"] = static_cast<int64_t>(it.count);
				choices.push_back(row);
			}
			qd["choice_items"] = choices;
			quests.push_back(qd);
		}
		d["quests"] = quests;
	}
	return d;  // kind stays "" for a non-quest/gossip opcode / undecodable payload
}

// ── Loot / vendor / trainer frame builders (ITM-02/ECO-01/NPC-02, #369/370/372/441) ─
// Client → server intents. Seq is a per-message counter the server echoes; the client
// matches replies by opcode (decode_econ_frame), so a fixed non-zero seq is fine
// (mirrors the quest/gossip + char-select builders above).

PackedByteArray MeridianNetThread::build_loot_request_frame(int64_t corpse_guid) const {
	cn::codec::LootRequest in;
	in.corpse_guid = static_cast<std::uint64_t>(corpse_guid);
	net::Bytes payload = cn::codec::encode_loot_request(in);
	return to_pba(cn::encode_world_frame(cn::kOpLootRequest, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_loot_take_frame(int64_t corpse_guid, int slot,
		bool money) const {
	cn::codec::LootTake in;
	in.corpse_guid = static_cast<std::uint64_t>(corpse_guid);
	in.slot = static_cast<std::uint32_t>(slot);
	in.money = money;
	net::Bytes payload = cn::codec::encode_loot_take(in);
	return to_pba(cn::encode_world_frame(cn::kOpLootTake, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_loot_release_frame(int64_t corpse_guid) const {
	cn::codec::LootRelease in;
	in.corpse_guid = static_cast<std::uint64_t>(corpse_guid);
	net::Bytes payload = cn::codec::encode_loot_release(in);
	return to_pba(cn::encode_world_frame(cn::kOpLootRelease, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_vendor_buy_frame(int vendor_id,
		int item_template_id, int quantity) const {
	cn::codec::VendorBuyRequest in;
	in.vendor_id = static_cast<std::uint32_t>(vendor_id);
	in.item_template_id = static_cast<std::uint32_t>(item_template_id);
	in.quantity = static_cast<std::uint32_t>(quantity);
	net::Bytes payload = cn::codec::encode_vendor_buy_request(in);
	return to_pba(cn::encode_world_frame(cn::kOpVendorBuyReq, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_vendor_sell_frame(int vendor_id,
		int backpack_slot, int quantity) const {
	cn::codec::VendorSellRequest in;
	in.vendor_id = static_cast<std::uint32_t>(vendor_id);
	in.backpack_slot = static_cast<std::uint16_t>(backpack_slot);
	in.quantity = static_cast<std::uint32_t>(quantity);
	net::Bytes payload = cn::codec::encode_vendor_sell_request(in);
	return to_pba(cn::encode_world_frame(cn::kOpVendorSellReq, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_vendor_buyback_frame(int buyback_slot) const {
	cn::codec::VendorBuybackRequest in;
	in.buyback_slot = static_cast<std::uint16_t>(buyback_slot);
	net::Bytes payload = cn::codec::encode_vendor_buyback_request(in);
	return to_pba(cn::encode_world_frame(cn::kOpVendorBuybackReq, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_trainer_learn_frame(int64_t npc_guid,
		int ability_id) const {
	cn::codec::TrainerLearn in;
	in.npc_guid = static_cast<std::uint64_t>(npc_guid);
	in.ability_id = static_cast<std::uint32_t>(ability_id);
	net::Bytes payload = cn::codec::encode_trainer_learn(in);
	return to_pba(cn::encode_world_frame(cn::kOpTrainerLearn, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_equipment_change_frame(int action, int slot) const {
	cn::codec::EquipmentChangeRequest in;
	in.action = static_cast<std::uint8_t>(action);
	in.slot = static_cast<std::uint16_t>(slot);
	net::Bytes payload = cn::codec::encode_equipment_change_request(in);
	return to_pba(cn::encode_world_frame(cn::kOpEquipmentChangeReq, /*seq=*/1, payload));
}

Dictionary MeridianNetThread::decode_econ_frame(int opcode,
		const PackedByteArray &payload) const {
	Dictionary d;
	d["kind"] = String("");
	const net::Bytes buf = to_bytes(payload);

	if (opcode == cn::kOpInventorySnapshot) {
		auto r = cn::codec::decode_inventory_snapshot(buf);
		if (!r) return d;
		d["kind"] = String("inventory_snapshot");
		d["money"] = static_cast<int64_t>(r->money);
		d["backpack_slots"] = static_cast<int64_t>(r->backpack_slots);
		Array items;
		for (const auto &it : r->items) {
			Dictionary row;
			row["slot"] = static_cast<int64_t>(it.slot);
			row["item_template_id"] = static_cast<int64_t>(it.item_template_id);
			row["count"] = static_cast<int64_t>(it.count);
			row["quality"] = static_cast<int64_t>(it.quality);
			row["binding"] = static_cast<int64_t>(it.binding);
			items.push_back(row);
		}
		d["items"] = items;
		Array equipment;
		for (const auto &it : r->equipment) {
			Dictionary row;
			row["slot"] = static_cast<int64_t>(it.slot);
			row["item_template_id"] = static_cast<int64_t>(it.item_template_id);
			row["quality"] = static_cast<int64_t>(it.quality);
			row["binding"] = static_cast<int64_t>(it.binding);
			equipment.push_back(row);
		}
		d["equipment"] = equipment;
	} else if (opcode == cn::kOpEquipmentChangeResult) {
		auto r = cn::codec::decode_equipment_change_result(buf);
		if (!r) return d;
		d["kind"] = String("equipment_change_result");
		d["status"] = static_cast<int64_t>(r->status);
		d["action"] = static_cast<int64_t>(r->action);
		d["slot"] = static_cast<int64_t>(r->slot);
		d["equipped_slot"] = static_cast<int64_t>(r->equipped_slot);
	} else if (opcode == cn::kOpVendorList) {
		auto r = cn::codec::decode_vendor_list(buf);
		if (!r) return d;
		d["kind"] = String("vendor_list");
		d["vendor_id"] = static_cast<int64_t>(r->vendor_id);
		Array items;
		for (const auto &it : r->items) {
			Dictionary row;
			row["item_template_id"] = static_cast<int64_t>(it.item_template_id);
			row["price"] = static_cast<int64_t>(it.price);
			row["quality"] = static_cast<int64_t>(it.quality);
			row["stock"] = static_cast<int64_t>(it.stock);
			items.push_back(row);
		}
		d["items"] = items;
	} else if (opcode == cn::kOpLootResponse) {
		auto r = cn::codec::decode_loot_response(buf);
		if (!r) return d;
		d["kind"] = String("loot_response");
		d["corpse_guid"] = static_cast<int64_t>(r->corpse_guid);
		d["status"] = static_cast<int64_t>(r->status);  // LootStatus
		d["copper"] = static_cast<int64_t>(r->copper);
		Array items;
		for (const auto &it : r->items) {
			Dictionary row;
			row["slot"] = static_cast<int64_t>(it.slot);
			row["item_template_id"] = static_cast<int64_t>(it.item_template_id);
			row["count"] = static_cast<int64_t>(it.count);
			row["quality"] = static_cast<int64_t>(it.quality);
			row["quest_item"] = it.quest_item;
			items.push_back(row);
		}
		d["items"] = items;
	} else if (opcode == cn::kOpLootResult) {
		auto r = cn::codec::decode_loot_result(buf);
		if (!r) return d;
		d["kind"] = String("loot_result");
		d["corpse_guid"] = static_cast<int64_t>(r->corpse_guid);
		d["slot"] = static_cast<int64_t>(r->slot);
		d["status"] = static_cast<int64_t>(r->status);  // LootTakeStatus
		d["item_template_id"] = static_cast<int64_t>(r->item_template_id);
		d["count"] = static_cast<int64_t>(r->count);
		d["copper"] = static_cast<int64_t>(r->copper);
	} else if (opcode == cn::kOpLootClosed) {
		auto r = cn::codec::decode_loot_closed(buf);
		if (!r) return d;
		d["kind"] = String("loot_closed");
		d["corpse_guid"] = static_cast<int64_t>(r->corpse_guid);
	} else if (opcode == cn::kOpVendorBuyResult) {
		auto r = cn::codec::decode_vendor_buy_result(buf);
		if (!r) return d;
		d["kind"] = String("vendor_buy_result");
		d["status"] = static_cast<int64_t>(r->status);  // VendorBuyStatus
		d["vendor_id"] = static_cast<int64_t>(r->vendor_id);
		d["item_template_id"] = static_cast<int64_t>(r->item_template_id);
		d["quantity"] = static_cast<int64_t>(r->quantity);
		d["item_guid"] = static_cast<int64_t>(r->item_guid);
		d["total_price"] = static_cast<int64_t>(r->total_price);
		d["balance"] = static_cast<int64_t>(r->balance);
	} else if (opcode == cn::kOpVendorSellResult) {
		auto r = cn::codec::decode_vendor_sell_result(buf);
		if (!r) return d;
		d["kind"] = String("vendor_sell_result");
		d["status"] = static_cast<int64_t>(r->status);  // VendorSellStatus
		d["backpack_slot"] = static_cast<int64_t>(r->backpack_slot);
		d["item_template_id"] = static_cast<int64_t>(r->item_template_id);
		d["quantity"] = static_cast<int64_t>(r->quantity);
		d["total_credit"] = static_cast<int64_t>(r->total_credit);
		d["balance"] = static_cast<int64_t>(r->balance);
		d["buyback_slot"] = static_cast<int64_t>(r->buyback_slot);
	} else if (opcode == cn::kOpVendorBuybackResult) {
		auto r = cn::codec::decode_vendor_buyback_result(buf);
		if (!r) return d;
		d["kind"] = String("vendor_buyback_result");
		d["status"] = static_cast<int64_t>(r->status);  // VendorBuybackStatus
		d["item_template_id"] = static_cast<int64_t>(r->item_template_id);
		d["quantity"] = static_cast<int64_t>(r->quantity);
		d["item_guid"] = static_cast<int64_t>(r->item_guid);
		d["price"] = static_cast<int64_t>(r->price);
		d["balance"] = static_cast<int64_t>(r->balance);
		d["buyback_slot"] = static_cast<int64_t>(r->buyback_slot);  // echoed (#453/#471)
	} else if (opcode == cn::kOpTrainerList) {
		auto r = cn::codec::decode_trainer_list(buf);
		if (!r) return d;
		d["kind"] = String("trainer_list");
		d["npc_guid"] = static_cast<int64_t>(r->npc_guid);
		Array entries;
		for (const auto &e : r->entries) {
			Dictionary row;
			row["ability_id"] = static_cast<int64_t>(e.ability_id);
			row["cost"] = static_cast<int64_t>(e.cost);
			row["required_class"] = static_cast<int64_t>(e.required_class);
			row["required_level"] = static_cast<int64_t>(e.required_level);
			row["state"] = static_cast<int64_t>(e.state);  // TrainableState
			entries.push_back(row);
		}
		d["entries"] = entries;
	} else if (opcode == cn::kOpTrainerLearnResult) {
		auto r = cn::codec::decode_trainer_learn_result(buf);
		if (!r) return d;
		d["kind"] = String("trainer_learn_result");
		d["npc_guid"] = static_cast<int64_t>(r->npc_guid);
		d["ability_id"] = static_cast<int64_t>(r->ability_id);
		d["status"] = static_cast<int64_t>(r->status);  // TrainerLearnStatus
		d["cost"] = static_cast<int64_t>(r->cost);
		d["new_balance"] = static_cast<int64_t>(r->new_balance);
	}
	return d;  // kind stays "" for a non-econ opcode / undecodable payload
}

// ── Chat frame builder + decode (SOC-01, #367/#434) ──────────────────────────
// The chat panel sends CHAT_MESSAGE on a line (send_bulk); the server's CHAT_DELIVER /
// CHAT_REJECTED arrive as raw `entity_frame`s decoded by decode_chat_frame. Seq is a
// per-message counter the server echoes; the client matches by opcode, so a fixed
// non-zero seq is fine (mirrors the quest/econ builders).

PackedByteArray MeridianNetThread::build_chat_message_frame(
		int channel, const String &target, const String &text) const {
	cn::codec::ChatMessage msg;
	msg.channel = static_cast<std::uint16_t>(channel);
	msg.target = to_std(target);
	msg.text = to_std(text);
	net::Bytes payload = cn::codec::encode_chat_message(msg);
	return to_pba(cn::encode_world_frame(cn::kOpChatMessage, /*seq=*/1, payload));
}

Dictionary MeridianNetThread::decode_chat_frame(int opcode,
		const PackedByteArray &payload) const {
	Dictionary d;
	d["kind"] = String("");
	const net::Bytes buf = to_bytes(payload);

	if (opcode == cn::kOpChatDeliver) {
		auto r = cn::codec::decode_chat_deliver(buf);
		if (!r) return d;
		d["kind"] = String("chat_deliver");
		d["channel"] = static_cast<int64_t>(r->channel);  // ChatChannel
		d["sender_guid"] = static_cast<int64_t>(r->sender_guid);
		d["sender_name"] = String::utf8(r->sender_name.c_str());
		d["text"] = String::utf8(r->text.c_str());
	} else if (opcode == cn::kOpChatRejected) {
		auto r = cn::codec::decode_chat_rejected(buf);
		if (!r) return d;
		d["kind"] = String("chat_rejected");
		d["channel"] = static_cast<int64_t>(r->channel);  // ChatChannel
		d["reason"] = static_cast<int64_t>(r->reason);    // ChatRejectReason
		d["target"] = String::utf8(r->target.c_str());
	}
	return d;  // kind stays "" for a non-chat opcode / undecodable payload
}

int64_t MeridianNetThread::frames_sent() const {
	return core_ ? static_cast<int64_t>(core_->frames_sent()) : 0;
}
int64_t MeridianNetThread::frames_received() const {
	return core_ ? static_cast<int64_t>(core_->frames_received()) : 0;
}
int64_t MeridianNetThread::inbound_dropped() const {
	return core_ ? static_cast<int64_t>(core_->inbound_dropped()) : 0;
}
int64_t MeridianNetThread::send_dropped() const {
	return core_ ? static_cast<int64_t>(core_->send_dropped()) : 0;
}

} // namespace meridian
