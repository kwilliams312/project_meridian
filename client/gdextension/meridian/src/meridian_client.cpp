#include "meridian_client.h"

#include "movement_constants.h"
#include "movement_query.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace meridian {

// Bootstrap version string. Bumped alongside the client/ENGINE_VERSION pin.
static const char *MERIDIAN_CLIENT_VERSION = "meridian-client 0.0.1 (godot 4.7-stable)";

MeridianClient::MeridianClient() {}

MeridianClient::~MeridianClient() {}

void MeridianClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_version"), &MeridianClient::get_version);
	ClassDB::bind_method(D_METHOD("get_movement_constants"), &MeridianClient::get_movement_constants);
}

String MeridianClient::get_version() const {
	return String(MERIDIAN_CLIENT_VERSION);
}

Dictionary MeridianClient::get_movement_constants() const {
	namespace mv = meridian::movement;

	// Exercise the query seam so the M0 flat-plane implementation (D-19) is
	// linked and demonstrably callable with the decided call shape.
	mv::FlatWorldQuery world;
	const mv::GroundSample g = world.sample_ground(0.0f, 0.0f);

	Dictionary d;
	d["tick_hz"]              = mv::kServerTickHz;
	d["tick_seconds"]        = mv::kTickSeconds;
	d["intent_max_hz"]       = mv::kMovementIntentMaxHz;
	d["run_speed"]           = mv::kRunSpeed;
	d["walk_speed"]          = mv::kWalkSpeed;
	d["jump_speed"]          = mv::kJumpSpeed;
	d["gravity"]             = mv::kGravity;
	d["speed_tolerance"]     = mv::kSpeedTolerance;
	d["height_tolerance"]    = mv::kHeightTolerance;
	d["run_speed_via_fn"]    = mv::server_speed(mv::MoveMode::Run);
	d["max_packet_disp"]     = mv::kMaxPacketDisplacement;
	d["heightfield_side"]    = mv::kHeightfieldSide;
	d["ground_y_at_origin"]  = g.height;   // proves sample_ground() links & runs
	return d;
}

} // namespace meridian
