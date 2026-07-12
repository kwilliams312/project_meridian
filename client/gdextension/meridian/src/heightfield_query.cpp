// Project Meridian — HeightfieldWorldQuery + the shared bilinear sampler (#557).
// Engine-free (Client SAD §9.2): plain C++17, no Godot, no FlatBuffers.

#include "heightfield_query.h"

#include <cmath>
#include <limits>

namespace meridian::movement {

HeightfieldSampleResult sample_heightfield_bilinear(const float* samples,
                                                    std::uint16_t side,
                                                    float spacing_m,
                                                    float local_x,
                                                    float local_z) {
	HeightfieldSampleResult r;
	if (samples == nullptr || side < 2 || spacing_m <= 0.0f) {
		r.hole = true;
		r.height = std::numeric_limits<float>::quiet_NaN();
		return r;
	}
	const int last = static_cast<int>(side) - 1;   // max valid index (side-1)

	// LOCAL metres -> lattice coordinate.
	const float gx = local_x / spacing_m;
	const float gz = local_z / spacing_m;

	// Base index clamped so the +1 neighbour is always in-grid. At the far/shared
	// edge (gx == side-1) the base clamps to side-2 and fx becomes 1.0, so the
	// result is exactly the edge sample.
	int ix = static_cast<int>(std::floor(gx));
	int iz = static_cast<int>(std::floor(gz));
	if (ix < 0) ix = 0;
	if (ix > last - 1) ix = last - 1;
	if (iz < 0) iz = 0;
	if (iz > last - 1) iz = last - 1;

	const float fx = gx - static_cast<float>(ix);
	const float fz = gz - static_cast<float>(iz);

	const std::size_t s = side;
	const float h00 = samples[static_cast<std::size_t>(iz) * s + ix];
	const float h10 = samples[static_cast<std::size_t>(iz) * s + ix + 1];
	const float h01 = samples[static_cast<std::size_t>(iz + 1) * s + ix];
	const float h11 = samples[static_cast<std::size_t>(iz + 1) * s + ix + 1];

	// A NaN corner marks a hole / un-sculpted cell (schema/chunk-payload.md
	// sentinel) — not walkable. Report it rather than propagating a poisoned lerp.
	if (std::isnan(h00) || std::isnan(h10) || std::isnan(h01) || std::isnan(h11)) {
		r.hole = true;
		r.height = std::numeric_limits<float>::quiet_NaN();
		return r;
	}

	const float h0 = h00 + (h10 - h00) * fx;   // interpolate along +x at iz
	const float h1 = h01 + (h11 - h01) * fx;   // interpolate along +x at iz+1
	r.height = h0 + (h1 - h0) * fz;            // interpolate along +z
	r.hole = false;
	return r;
}

HeightfieldWorldQuery::HeightfieldWorldQuery(float origin_x, float origin_z,
                                             float chunk_size_m)
    : origin_x_(origin_x), origin_z_(origin_z), chunk_size_m_(chunk_size_m) {}

void HeightfieldWorldQuery::add_chunk(HeightfieldChunk chunk) {
	for (auto& c : chunks_) {
		if (c.cx == chunk.cx && c.cz == chunk.cz) {
			c = std::move(chunk);
			return;
		}
	}
	chunks_.push_back(std::move(chunk));
}

const HeightfieldChunk* HeightfieldWorldQuery::find_chunk(int cx, int cz) const {
	for (const auto& c : chunks_) {
		if (c.cx == cx && c.cz == cz) return &c;
	}
	return nullptr;
}

GroundSample HeightfieldWorldQuery::sample_ground(float x, float z) const {
	// World XZ -> chunk cell. floor() so negative coords map correctly: the grid
	// spans negative indices (schema/chunk.fbs §3.1 — Zone-01 spawns at x ≈ -300).
	const float cell_x = (x - origin_x_) / chunk_size_m_;
	const float cell_z = (z - origin_z_) / chunk_size_m_;
	const int cx = static_cast<int>(std::floor(cell_x));
	const int cz = static_cast<int>(std::floor(cell_z));

	const HeightfieldChunk* c = find_chunk(cx, cz);
	if (c == nullptr) {
		// No resident chunk covers this XZ -> out of bounds -> not walkable.
		return GroundSample{0.0f, false};
	}
	// Guard a malformed resident grid (short sample vector) rather than reading
	// past the buffer — a decode that slipped through is a hole, not a crash.
	const std::size_t need = static_cast<std::size_t>(c->side) * c->side;
	if (c->side < 2 || c->samples.size() < need) {
		return GroundSample{0.0f, false};
	}

	// Local offset within the chunk, in metres, in [0, chunk_size_m].
	const float local_x = (x - origin_x_) - static_cast<float>(cx) * chunk_size_m_;
	const float local_z = (z - origin_z_) - static_cast<float>(cz) * chunk_size_m_;

	const HeightfieldSampleResult r = sample_heightfield_bilinear(
	    c->samples.data(), c->side, c->spacing_m, local_x, local_z);

	GroundSample g;
	g.walkable = !r.hole;
	g.height = r.hole ? 0.0f : r.height;   // no meaningful ground over a hole
	return g;
}

}  // namespace meridian::movement
