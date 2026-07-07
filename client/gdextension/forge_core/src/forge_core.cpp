// SPDX-License-Identifier: Apache-2.0
//
// ForgeCore implementation (issue #134). Thin shim over the engine-free cores.

#include "forge_core.h"

#include "forge_terrain_stub.h"
#include "forge_version.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace forge {

ForgeCore::ForgeCore() {}

ForgeCore::~ForgeCore() {}

void ForgeCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("version"), &ForgeCore::version);
	ClassDB::bind_method(D_METHOD("chunk_size_m"), &ForgeCore::chunk_size_m);
	ClassDB::bind_method(D_METHOD("region_size_m"), &ForgeCore::region_size_m);
	ClassDB::bind_method(D_METHOD("heightfield_side"), &ForgeCore::heightfield_side);
	ClassDB::bind_method(D_METHOD("region_tiles_chunk_grid", "region_m"),
			&ForgeCore::region_tiles_chunk_grid);
	ClassDB::bind_method(D_METHOD("terrain_backend_info"), &ForgeCore::terrain_backend_info);
}

String ForgeCore::version() const {
	return String(forge::forge_core_version());
}

double ForgeCore::chunk_size_m() const {
	return forge::terrain::kChunkSizeM;
}

double ForgeCore::region_size_m() const {
	return forge::terrain::kTerrain3DRegionSizeM;
}

int64_t ForgeCore::heightfield_side() const {
	return forge::terrain::kHeightfieldSide;
}

bool ForgeCore::region_tiles_chunk_grid(double region_m) const {
	return forge::terrain::region_tiles_chunk_grid(region_m);
}

Dictionary ForgeCore::terrain_backend_info() const {
	Dictionary d;
	d["backend"] = String("Terrain3D (A-09 stub)");
	d["region_size_m"] = region_size_m();
	d["chunk_size_m"] = chunk_size_m();
	d["heightfield_side"] = heightfield_side();
	// The seam's live assertion: does the pinned region tile the 128 m grid?
	d["aligns"] = region_tiles_chunk_grid(region_size_m());
	return d;
}

} // namespace forge
