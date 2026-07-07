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
#include <godot_cpp/variant/dictionary.hpp>

#include "meridian/clientnet/tls_client.h"
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
