// Project Meridian — client kinematic movement controller GDExtension node
// (issue #102). Thin Godot wrapper around the engine-free core
// (movement_controller.*): it marshals Godot input/state into the core's plain
// structs and back, and exposes tick / reconcile / intent methods to the scene.
//
// This is the `sim`-module surface the scene drives at the fixed sim tick
// (Client SAD §2.2 fixed-tick prediction, §6.1 main-thread sim). All the
// determinism-critical logic lives in the core so it is unit-testable without a
// Godot runtime (SAD §9.2); this class holds only glue + the active IWorldQuery
// ground backend. The backend defaults to the M0 flat plane (FlatWorldQuery,
// D-19) and is swapped IN PLACE to the M1 HeightfieldWorldQuery (#557 Story D) —
// behind the SAME IWorldQuery seam, no caller change — the moment the world scene
// (#558 Story E) mounts a zone pack and feeds it the shipped per-chunk
// `.chunk.bin` heightfields. It extends the #158 scaffold alongside MeridianClient.

#ifndef MERIDIAN_MOVEMENT_CONTROLLER_NODE_H
#define MERIDIAN_MOVEMENT_CONTROLLER_NODE_H

#include "heightfield_query.h"
#include "movement_controller.h"
#include "movement_query.h"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
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

	// Advance the reconciliation error-offset decay by `dt_ms` of render time
	// (#103). Call once per frame, AFTER draining server acks (reconcile) at the
	// net-thread pre-sim sync point, so a small correction slides toward the
	// authoritative sim over the decay window instead of popping.
	void advance_smoothing(uint64_t dt_ms);

	// Client-visible predicted state accessors.
	godot::Vector3 get_predicted_position() const;   // raw authoritative-correct SIM
	// The RENDER position: corrected sim + decaying error offset (#103). The local
	// player node should render THIS; get_predicted_position() stays the sim truth.
	godot::Vector3 get_render_position() const;
	godot::Vector3 get_predicted_velocity() const;
	bool is_grounded() const;
	uint32_t pending_input_count() const;

	// Reconciliation smoothing observability (#103).
	bool is_smoothing() const;               // a small-error correction is decaying
	float last_error_magnitude() const;      // |error| of the last reconcile (m)
	bool last_reconcile_snapped() const;     // did the last reconcile snap?

	// ── M1 heightfield ground backend (#557 Story D → #558 Story E) ───────────
	// Swap the flat M0 plane for a real HeightfieldWorldQuery over the IF-6 zone
	// grid (origin + chunk size from the zone manifest). Called ONCE by the world
	// scene after a fail-closed pack verify, BEFORE feeding chunks. The reconciler
	// is rebuilt against the new backend, carrying the current predicted state, so
	// an in-flight session keeps its position across the swap. Idempotent per zone
	// geometry: a second call installs a FRESH (empty) query — feed the chunks again.
	void use_heightfield_zone(float origin_x, float origin_z, float chunk_size_m);

	// Make one chunk's shipped `.chunk.bin` heightfield resident on the active
	// heightfield backend (decoded via heightfield_chunk_decode — the SAME bytes
	// worldd validates against, Q1(a)). `cx,cz` is the IF-6 grid cell. Returns false
	// (no-op) when no heightfield zone is active or the bytes fail to decode
	// (fail-closed — never a silent bad grid). Replaces any chunk already at (cx,cz).
	bool add_heightfield_chunk(int cx, int cz, const godot::PackedByteArray &chunk_bin);

	// The active ground backend's sample at world XZ: { "height": float,
	// "walkable": bool }. `walkable` is false over a hole OR when no resident chunk
	// covers XZ (the chunk has not streamed in yet). The world scene reads this to
	// (a) spawn the player ON terrain and (b) HOLD an entity at its server position
	// until the ground under it is resident (never drop it through — #558).
	godot::Dictionary sample_ground(float x, float z) const;

	// Convenience: is the ground under (x, z) resident + walkable right now?
	bool has_ground_at(float x, float z) const;

	// True once use_heightfield_zone installed a heightfield backend (else the M0
	// flat plane is still active). Number of resident heightfield chunks.
	bool is_heightfield_active() const;
	int  heightfield_chunk_count() const;

private:
	// The active ground query the reconciler samples each tick. Owns a FlatWorldQuery
	// at construction (M0, D-19); use_heightfield_zone() swaps in a HeightfieldWorldQuery
	// (M1, #557). Held by unique_ptr so the address is STABLE — the reconciler keeps a
	// reference to *world_, and adding chunks mutates the query in place without
	// invalidating it. Declared before reconciler_ so it outlives it (destruction order).
	std::unique_ptr<movement::IWorldQuery>          world_;
	// Non-owning alias to *world_ when the active backend is a heightfield (else null),
	// so add_heightfield_chunk can feed it without a downcast.
	movement::HeightfieldWorldQuery                *heightfield_ = nullptr;
	std::unique_ptr<movement::PredictionReconciler> reconciler_;  // core

	void ensure_reconciler();
};

} // namespace meridian

#endif // MERIDIAN_MOVEMENT_CONTROLLER_NODE_H
