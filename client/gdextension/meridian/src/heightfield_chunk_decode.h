// Project Meridian — decode the Heightfield out of a shipped `.chunk.bin` (#557).
//
// Turns the IF-6 `ServerChunk` FlatBuffer (schema/chunk/chunk.fbs) — the SAME
// bytes worldd validates movement against (Q1(a)) — into an engine-free
// HeightfieldChunk the HeightfieldWorldQuery can bilinear-sample. This is the ONE
// place the FlatBuffers dependency lives; the sampler + query it feeds stay
// dependency-free (heightfield_query.*), so only THIS translation unit needs the
// flatc bindings + the FlatBuffers runtime (the login-core discipline in the
// client test CMake).

#ifndef MERIDIAN_HEIGHTFIELD_CHUNK_DECODE_H
#define MERIDIAN_HEIGHTFIELD_CHUNK_DECODE_H

#include "heightfield_query.h"

#include <cstddef>

namespace meridian::movement {

// Decode the Heightfield table (schema/chunk/chunk.fbs §3.2) out of a shipped
// `<cx>_<cz>.chunk.bin` (`data`/`len`) into `out`. Fail-closed (like the #554
// chunk-pack verify): verifies the FlatBuffers "MCHK" identifier + structure,
// requires `coord` + `heightfield`, and requires exactly side*side samples with
// side >= 2 before trusting the buffer. On ANY failure it returns false and leaves
// `out` untouched; on success it returns true with `out` fully populated.
bool decode_heightfield_chunk(const void* data, std::size_t len,
                              HeightfieldChunk& out);

}  // namespace meridian::movement

#endif  // MERIDIAN_HEIGHTFIELD_CHUNK_DECODE_H
