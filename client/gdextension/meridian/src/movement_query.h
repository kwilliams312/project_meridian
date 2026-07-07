// Project Meridian — world-query seam for the kinematic movement controller
// (issue #101 spike; the full controller is #102).
//
// This header locks the QUERY METHOD decision (docs/movement-spike.md §3): the
// controller determines ground height / collision each tick by sampling the
// per-chunk heightfield — the SAME `f32[129×129]` grid the server validates
// against (Tools SAD §5.2 `ITerrainBackend::export_heightfield`) — NOT via a
// Godot PhysicsServer raycast and NOT via CharacterBody3D::move_and_slide.
//
// Rationale (full weighing in the spike doc §3):
//   • Determinism: a pure math sample of shared data is bit-reproducible across
//     the N-times-per-frame re-simulation reconciliation needs (Client SAD
//     §2.2/§9.2) — Godot's PhysicsServer is frame-coupled and non-deterministic
//     under re-stepping, which is exactly why the SAD rejects CharacterBody3D.
//   • Client/server parity: the server (plain C++/Linux, no Godot — Server SAD
//     §1) CANNOT run Godot physics; it validates z against the heightfield
//     sample (Server SAD §5.5). Sampling the same grid is the ONLY way client
//     prediction and server validation agree by construction.
//   • Min-spec cost: a bilinear sample is a few FLOPs — free against the ≤ 2 ms
//     interp/prediction budget on Low (Client SAD §6.3), vs. per-tick physics
//     queries that add cost and jitter.
//
// This is the SEAM only — signatures + a flat-plane M0 implementation (D-19:
// M0 runs on a flat bootstrap map, bounds-only, no heightfield). #102
// implements the real bilinear sample against IF-6 chunk data at M1.

#ifndef MERIDIAN_MOVEMENT_QUERY_H
#define MERIDIAN_MOVEMENT_QUERY_H

#include "movement_constants.h"

namespace meridian::movement {

// A ground-sample result at a world XZ. Kept engine-agnostic (no Godot types)
// so it links into both the client GDExtension and the engine-free doctest /
// bot builds (Client SAD §9.2 "engine-agnostic cores"), mirroring server code.
struct GroundSample {
	float height   = 0.0f;   // zone-local terrain height (y) at the queried XZ
	bool  walkable = true;   // false over holes / liquid-without-swim / OOB (M1)
};

// The query seam the kinematic controller (#102) calls each tick to resolve the
// ground under the character. Implementations sample the per-chunk heightfield
// (129×129 @ 1 m, Tools SAD §3.3) with bilinear interpolation between the four
// surrounding lattice samples. The controller then clamps the predicted y to
// `height` (walking) or integrates the jump/fall arc toward it (§5 constants),
// and the SERVER validates the resulting z to ±4 m of THIS same sample
// (kHeightTolerance) — same data, same result.
class IWorldQuery {
public:
	virtual ~IWorldQuery() = default;

	// Ground under (x, z) in zone-local metres. This is the SINGLE call shape
	// the controller depends on; swapping the M0 flat plane for the M1
	// heightfield sampler is a drop-in with no controller change.
	virtual GroundSample sample_ground(float x, float z) const = 0;
};

// M0 implementation (D-19): flat bootstrap test map — constant ground plane at
// y = 0, everywhere walkable. Proves the call shape end to end without a
// heightfield. #102/M1 adds `HeightfieldWorldQuery` (bilinear over IF-6 chunk
// data) behind the same interface.
class FlatWorldQuery final : public IWorldQuery {
public:
	explicit FlatWorldQuery(float plane_y = 0.0f) : plane_y_(plane_y) {}

	GroundSample sample_ground(float /*x*/, float /*z*/) const override {
		return GroundSample{plane_y_, true};
	}

private:
	float plane_y_;
};

} // namespace meridian::movement

#endif // MERIDIAN_MOVEMENT_QUERY_H
