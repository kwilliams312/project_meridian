// Project Meridian — Heightfield decode from a shipped `.chunk.bin` (#557).

#include "heightfield_chunk_decode.h"

#include "chunk_generated.h"   // flatc --cpp of schema/chunk/chunk.fbs (build-time)

#include <cstdint>
#include <utility>

namespace meridian::movement {

bool decode_heightfield_chunk(const void* data, std::size_t len, HeightfieldChunk& out) {
	if (data == nullptr || len == 0) return false;

	// Fail closed: reject anything that is not a verified, correctly-identified
	// ServerChunk buffer before touching a field (same posture as #554).
	const auto* bytes = static_cast<const std::uint8_t*>(data);
	flatbuffers::Verifier verifier(bytes, len);
	if (!meridian::chunk::VerifyServerChunkBuffer(verifier)) return false;
	if (!meridian::chunk::ServerChunkBufferHasIdentifier(data)) return false;

	const meridian::chunk::ServerChunk* sc = meridian::chunk::GetServerChunk(data);
	if (sc == nullptr) return false;

	const meridian::chunk::Heightfield* hf = sc->heightfield();
	if (hf == nullptr) return false;
	const auto* samples = hf->samples();
	if (samples == nullptr) return false;

	const std::uint16_t side = hf->side();
	if (side < 2) return false;
	const std::size_t expected = static_cast<std::size_t>(side) * side;
	if (samples->size() != expected) return false;

	HeightfieldChunk c;
	if (const meridian::chunk::ChunkCoord* coord = sc->coord()) {
		c.cx = coord->cx();
		c.cz = coord->cz();
	}
	c.side = side;
	c.spacing_m = hf->spacing_m();
	c.samples.resize(expected);
	for (std::size_t i = 0; i < expected; ++i) {
		c.samples[i] = samples->Get(static_cast<flatbuffers::uoffset_t>(i));
	}

	out = std::move(c);
	return true;
}

}  // namespace meridian::movement
