#include "meridian_client.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace meridian {

// Bootstrap version string. Bumped alongside the client/ENGINE_VERSION pin.
static const char *MERIDIAN_CLIENT_VERSION = "meridian-client 0.0.1 (godot 4.6-stable)";

MeridianClient::MeridianClient() {}

MeridianClient::~MeridianClient() {}

void MeridianClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_version"), &MeridianClient::get_version);
}

String MeridianClient::get_version() const {
	return String(MERIDIAN_CLIENT_VERSION);
}

} // namespace meridian
