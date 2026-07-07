// Project Meridian — placeholder GDExtension class (issue #158 bootstrap).
//
// MeridianClient is a trivial exported class whose only job in this bootstrap PR
// is to prove the client GDExtension compiles and links against the pinned
// godot-cpp (client/godot-cpp @ 4.6-stable). It exposes a single method,
// get_version(), returning a build/version string. The real hot-path modules
// (net / sim / stream / datastore, Client SAD §2) land in later issues
// (e.g. the C++ movement controller, #102); this class is scaffolding only.

#ifndef MERIDIAN_CLIENT_H
#define MERIDIAN_CLIENT_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace meridian {

class MeridianClient : public godot::Object {
	GDCLASS(MeridianClient, godot::Object)

protected:
	static void _bind_methods();

public:
	MeridianClient();
	~MeridianClient();

	// Returns the pinned engine/toolchain version this build targets.
	// Kept in sync with client/ENGINE_VERSION by convention.
	godot::String get_version() const;

	// Movement-spike proof (#101): exposes the locked shared movement constants
	// to GDScript/tests so the constants header + query seam are demonstrably
	// compiled and linked into the GDExtension. NOT the controller (that's #102).
	// Returns { tick_hz, run_speed, walk_speed, jump_speed, gravity,
	//           speed_tol, height_tol, ground_y_at_origin } as a Dictionary.
	godot::Dictionary get_movement_constants() const;
};

} // namespace meridian

#endif // MERIDIAN_CLIENT_H
