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

	ClassDB::bind_method(D_METHOD("build_char_list_request_frame"),
	                     &MeridianNetThread::build_char_list_request_frame);
	ClassDB::bind_method(
	    D_METHOD("build_char_create_request_frame", "name", "race", "char_class"),
	    &MeridianNetThread::build_char_create_request_frame);
	ClassDB::bind_method(D_METHOD("build_char_delete_request_frame", "character_id"),
	                     &MeridianNetThread::build_char_delete_request_frame);
	ClassDB::bind_method(D_METHOD("build_enter_world_request_frame", "character_id"),
	                     &MeridianNetThread::build_enter_world_request_frame);

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
	// char_list carries an Array of Dictionaries {id,name,race,char_class,level}.
	ADD_SIGNAL(MethodInfo("char_list", PropertyInfo(Variant::ARRAY, "characters")));
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
					chars.push_back(row);
				}
				emit_signal("char_list", chars);
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

// ── Character-select frame builders (D-35 / #286 / #341) ─────────────────────
// Seq is a per-message counter the server echoes; it is not correlated by the
// client (responses are matched by opcode/signal), so a fixed non-zero seq is fine.

PackedByteArray MeridianNetThread::build_char_list_request_frame() const {
	net::Bytes payload = cn::codec::encode_char_list_request();
	return to_pba(cn::encode_world_frame(cn::kOpCharListReq, /*seq=*/1, payload));
}

PackedByteArray MeridianNetThread::build_char_create_request_frame(
		const String &name, int race, int char_class) const {
	cn::codec::CharCreateRequest req;
	req.name = to_std(name);
	req.race = static_cast<std::uint8_t>(race);
	req.char_class = static_cast<std::uint8_t>(char_class);
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
