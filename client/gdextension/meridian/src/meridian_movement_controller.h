// Project Meridian — client kinematic movement controller GDExtension node
// (issue #102). Thin Godot wrapper around the engine-free core
// (movement_controller.*): it marshals Godot input/state into the core's plain
// structs and back, and exposes tick / reconcile / intent methods to the scene.
//
// This is the `sim`-module surface the scene drives at the fixed sim tick
// (Client SAD §2.2 fixed-tick prediction, §6.1 main-thread sim). All the
// determinism-critical logic lives in the core so it is unit-testable without a
// Godot runtime (SAD §9.2); this class holds only glue + a FlatWorldQuery (M0,
// D-19). It extends the #158 scaffold alongside MeridianClient.

#ifndef MERIDIAN_MOVEMENT_CONTROLLER_NODE_H
#define MERIDIAN_MOVEMENT_CONTROLLER_NODE_H

#include "movement_controller.h"
#include "movement_query.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>

namespace meridian {

class MeridianMovementController : public godot::RefCounted {
	GDCLASS(MeridianMovementController, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	MeridianMovementController();
	~MeridianMovementController();

	// Seed the confirmed + predicted state (e.g. from EntityEnter spawn pos).
	void reset(const godot::Vector3 &position, float orientation);

	// Sample-and-predict ONE fixed sim tick. `move` is (x=strafe, z=forward) in
	// [-1,1] world axes; `walk`/`jump` are the toggles. Returns the predicted
	// MovementIntent as a Dictionary (seq, state_flags, x/y/z, orientation,
	// client_time_ms) for the `net` module to encode. Advances the predicted
	// state visible via get_predicted_position().
	godot::Dictionary predict(const godot::Vector3 &move, bool walk, bool jump,
	                          float orientation, uint64_t client_time_ms);

	// Apply an authoritative MovementState (decoded by `net`): discard acked
	// inputs and re-simulate unacked ones from the server position. Returns the
	// reconciled predicted position.
	godot::Vector3 reconcile(uint32_t ack_seq, const godot::Vector3 &server_position,
	                         float orientation);

	// Rate-cap gate for intent emission (≤ 10/s + on state change).
	bool should_emit_intent(uint64_t client_time_ms, uint32_t state_flags);

	// Client-visible predicted state accessors.
	godot::Vector3 get_predicted_position() const;
	godot::Vector3 get_predicted_velocity() const;
	bool is_grounded() const;
	uint32_t pending_input_count() const;

private:
	movement::FlatWorldQuery world_;                              // M0 (D-19)
	std::unique_ptr<movement::PredictionReconciler> reconciler_;  // core

	void ensure_reconciler();
};

} // namespace meridian

#endif // MERIDIAN_MOVEMENT_CONTROLLER_NODE_H
