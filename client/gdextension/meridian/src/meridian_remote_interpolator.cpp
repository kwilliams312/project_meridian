// Project Meridian — MeridianRemoteInterpolator GDExtension node (issue #104).
// Thin Godot glue over the engine-free core (remote_interpolation.*).

#include "meridian_remote_interpolator.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace meridian {

namespace rem = meridian::remote;

MeridianRemoteInterpolator::MeridianRemoteInterpolator() {}
MeridianRemoteInterpolator::~MeridianRemoteInterpolator() {}

void MeridianRemoteInterpolator::_bind_methods() {
	ClassDB::bind_method(
	    D_METHOD("on_clock_sync", "client_send_ms", "server_reply_ms", "client_recv_ms"),
	    &MeridianRemoteInterpolator::on_clock_sync);
	ClassDB::bind_method(D_METHOD("clock_offset_ms"),
	                     &MeridianRemoteInterpolator::clock_offset_ms);
	ClassDB::bind_method(D_METHOD("clock_rtt_ms"),
	                     &MeridianRemoteInterpolator::clock_rtt_ms);
	ClassDB::bind_method(D_METHOD("has_clock_estimate"),
	                     &MeridianRemoteInterpolator::has_clock_estimate);

	ClassDB::bind_method(
	    D_METHOD("on_entity_enter", "guid", "position", "orientation", "server_time_ms"),
	    &MeridianRemoteInterpolator::on_entity_enter);
	ClassDB::bind_method(
	    D_METHOD("on_entity_update", "guid", "position", "orientation", "server_time_ms"),
	    &MeridianRemoteInterpolator::on_entity_update);
	ClassDB::bind_method(D_METHOD("on_entity_leave", "guid"),
	                     &MeridianRemoteInterpolator::on_entity_leave);

	ClassDB::bind_method(D_METHOD("is_tracked", "guid"),
	                     &MeridianRemoteInterpolator::is_tracked);
	ClassDB::bind_method(D_METHOD("tracked_count"),
	                     &MeridianRemoteInterpolator::tracked_count);

	ClassDB::bind_method(
	    D_METHOD("get_interpolated_position", "guid", "client_now_ms"),
	    &MeridianRemoteInterpolator::get_interpolated_position);
	ClassDB::bind_method(D_METHOD("sample_entity", "guid", "client_now_ms"),
	                     &MeridianRemoteInterpolator::sample_entity);
}

// --- Clock sync ---

bool MeridianRemoteInterpolator::on_clock_sync(uint64_t client_send_ms,
                                               uint64_t server_reply_ms,
                                               uint64_t client_recv_ms) {
	return interp_.on_clock_sync(client_send_ms, server_reply_ms, client_recv_ms);
}

int64_t MeridianRemoteInterpolator::clock_offset_ms() const {
	return interp_.clock().offset_ms();
}
int64_t MeridianRemoteInterpolator::clock_rtt_ms() const {
	return interp_.clock().rtt_ms();
}
bool MeridianRemoteInterpolator::has_clock_estimate() const {
	return interp_.clock().has_estimate();
}

// --- Entity lifecycle ---

void MeridianRemoteInterpolator::on_entity_enter(uint64_t guid,
                                                 const Vector3 &position,
                                                 float orientation,
                                                 uint64_t server_time_ms) {
	interp_.on_enter(guid, rem::Vec3{position.x, position.y, position.z},
	                 orientation, server_time_ms);
}

void MeridianRemoteInterpolator::on_entity_update(uint64_t guid,
                                                  const Vector3 &position,
                                                  float orientation,
                                                  uint64_t server_time_ms) {
	interp_.on_update(guid, rem::Vec3{position.x, position.y, position.z},
	                  orientation, server_time_ms);
}

void MeridianRemoteInterpolator::on_entity_leave(uint64_t guid) {
	interp_.on_leave(guid);
}

bool MeridianRemoteInterpolator::is_tracked(uint64_t guid) const {
	return interp_.is_tracked(guid);
}
uint32_t MeridianRemoteInterpolator::tracked_count() const {
	return static_cast<uint32_t>(interp_.tracked_count());
}

// --- Rendering query ---

Vector3 MeridianRemoteInterpolator::get_interpolated_position(
    uint64_t guid, uint64_t client_now_ms) const {
	const rem::SampleResult s = interp_.sample_entity(guid, client_now_ms);
	return Vector3(s.position.x, s.position.y, s.position.z);
}

Dictionary MeridianRemoteInterpolator::sample_entity(uint64_t guid,
                                                     uint64_t client_now_ms) const {
	const rem::SampleResult s = interp_.sample_entity(guid, client_now_ms);
	Dictionary d;
	d["kind"]        = static_cast<int>(s.kind);
	d["x"]           = s.position.x;
	d["y"]           = s.position.y;
	d["z"]           = s.position.z;
	d["orientation"] = s.orientation;
	return d;
}

} // namespace meridian
