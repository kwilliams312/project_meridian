// Project Meridian — client remote-entity interpolator GDExtension node
// (issue #104). Thin Godot wrapper around the engine-free core
// (remote_interpolation.*): it marshals Godot Vector3/GUIDs into the core's
// plain structs and back, exposing entity-lifecycle + clock-sync feed methods
// and a per-frame interpolated-position query to the scene.
//
// This is the REMOTE-side `sim` surface (Client SAD §2.2 "Interpolation clock"):
// the scene feeds it the EntityEnter/Update/Leave the `net` module decodes and
// the ClockSync round-trips, then each frame asks for every visible remote
// entity's smoothed position to write to its Node3D. All the determinism-
// critical logic lives in the core so it is unit-testable without a Godot
// runtime (SAD §9.2); this class holds only glue. Distinct from #102
// MeridianMovementController, which predicts the LOCAL player.

#ifndef MERIDIAN_REMOTE_INTERPOLATOR_NODE_H
#define MERIDIAN_REMOTE_INTERPOLATOR_NODE_H

#include "remote_interpolation.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace meridian {

class MeridianRemoteInterpolator : public godot::RefCounted {
	GDCLASS(MeridianRemoteInterpolator, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	MeridianRemoteInterpolator();
	~MeridianRemoteInterpolator();

	// --- Clock sync (fed from ClockSync #65 round-trips the `net` module times) ---
	// Returns true if the sample was accepted (passed the outlier gate).
	bool on_clock_sync(uint64_t client_send_ms, uint64_t server_reply_ms,
	                   uint64_t client_recv_ms);
	int64_t clock_offset_ms() const;
	int64_t clock_rtt_ms() const;
	bool has_clock_estimate() const;

	// --- Entity lifecycle (fed from #87 AoI relay via EntityEnter/Update/Leave) ---
	void on_entity_enter(uint64_t guid, const godot::Vector3 &position,
	                     float orientation, uint64_t server_time_ms);
	void on_entity_update(uint64_t guid, const godot::Vector3 &position,
	                      float orientation, uint64_t server_time_ms);
	void on_entity_leave(uint64_t guid);

	bool is_tracked(uint64_t guid) const;
	uint32_t tracked_count() const;

	// --- Rendering query ---
	// Sample entity `guid`'s interpolated position for a frame whose CLIENT-clock
	// time is `client_now_ms`. Returns the position to draw. Callers that need to
	// know whether the sample is fresh (interpolated) vs. extrapolated/frozen (for
	// presentation-fade) use sample_entity() below.
	godot::Vector3 get_interpolated_position(uint64_t guid, uint64_t client_now_ms) const;

	// Full sample: Dictionary { kind:int, x,y,z:float, orientation:float } where
	// kind is the remote::SampleKind (0 empty,1 interp,2 extrap,3 held).
	godot::Dictionary sample_entity(uint64_t guid, uint64_t client_now_ms) const;

private:
	remote::RemoteInterpolator interp_;
};

} // namespace meridian

#endif // MERIDIAN_REMOTE_INTERPOLATOR_NODE_H
