// SPDX-License-Identifier: Apache-2.0
//
// Project Meridian — ForgeCore: the Godot-facing shim for the forge_core
// GDExtension (issue #134, M0 EditorPlugin-skeleton de-risk).
//
// This is the ONE class the Forge editor plugin (addons/meridian_forge/) calls
// into — the thin editor-facing layer over the engine-free cores (forge_version.*,
// forge_terrain_stub.*). Its whole job in this skeleton is to prove the
// EditorPlugin↔GDExtension bridge end-to-end (Tools SAD §5, §8 M0 exit; PRD R7:
// "heavy logic stays in forge_core behind a thin editor-facing layer to shrink the
// API-churn surface"). Real heavy ops (chunk export, Recast bake, the full
// ITerrainBackend) land in later Forge issues (SAD §5.1–§5.4).
//
// A plain RefCounted (not a Node): the dock instantiates it as a value object,
// calls version()/terrain seam queries, and drops it — no scene-tree lifetime.

#ifndef FORGE_CORE_FORGE_CORE_H
#define FORGE_CORE_FORGE_CORE_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace forge {

class ForgeCore : public godot::RefCounted {
	GDCLASS(ForgeCore, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	ForgeCore();
	~ForgeCore();

	// Proof-of-bridge value: the forge_core build/version string. The dock renders
	// this, so a correct string in the editor proves the plugin loaded the native
	// library and called across the boundary. (forge_version.*)
	godot::String version() const;

	// --- ITerrainBackend region-alignment seam (Tools SAD §5.2 op 3) ------------
	// The one *meaningful* native call: the pure-logic half of the region-alignment
	// query both terrain backends share (forge_terrain_stub.*).

	// The Meridian chunk grid pitch in metres (128 m, SAD §3.2).
	double chunk_size_m() const;

	// The region size the chosen Terrain3D backend pins (A-09): 128 m == one chunk.
	double region_size_m() const;

	// The per-chunk server heightfield side (129 = 128 m span + shared edge, op 4).
	int64_t heightfield_side() const;

	// Does a terrain region of `region_m` metres tile the 128 m chunk grid?
	bool region_tiles_chunk_grid(double region_m) const;

	// A single dictionary the dock can render to show the seam is live end-to-end:
	// { backend, region_size_m, chunk_size_m, heightfield_side, aligns }.
	godot::Dictionary terrain_backend_info() const;
};

} // namespace forge

#endif // FORGE_CORE_FORGE_CORE_H
