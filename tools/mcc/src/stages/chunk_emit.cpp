// tools/mcc/src/stages/chunk_emit.cpp — chunk-emit stage (Tools SAD §3, IF-6).
//
// Procedural Zone-01 fixture emit (story #553). See chunk_emit.h for the full
// contract, the per-chunk hash framing, the Godot mesh-encoding coupling, and the
// determinism guarantee. This file bakes the FlatBuffers `ServerChunk` payloads
// through the flatc-generated bindings (chunk_generated.h), bakes the client
// terrain `ArrayMesh` from the same heightfield (#875), renders the IF-6 manifest
// + IF-8 asset table + the IF-5 client pack, and (via the CLI wrapper) writes them
// to disk.

#include "stages/chunk_emit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"

#include "chunk_generated.h"   // flatc --cpp of schema/chunk/chunk.fbs (build-time)
#include "hash/blake3.h"
#include "stages/idmap.h"

namespace mcc::stages {

namespace fs = std::filesystem;

namespace {

// ---- Small formatting helpers (deterministic, no locale) -------------------

// Escape a string for a double-quoted JSON string literal (RFC 8259). Mirrors
// emit_pck.cpp's json_quote so the pack manifests share one escaping discipline.
std::string json_quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[(c >> 4) & 0xF];
                    out += hex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

// A fixed-precision real for JSON numbers. %.4f is deterministic on every
// platform (no locale, no shortest-round-trip ambiguity) — good enough for a
// metres-scale fixture and byte-stable across builds.
std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return std::string(buf);
}

// The namespace segment of a fully-qualified id ("core:zone.zone01" -> "core").
std::string ns_of(const std::string& id) {
    const std::size_t colon = id.find(':');
    return colon == std::string::npos ? std::string() : id.substr(0, colon);
}

// The trailing dotted segment of a zone id ("core:zone.zone01" -> "zone01"),
// used for the manifest filename and the res:// chunk directory.
std::string zone_bare_of(const std::string& zone_id) {
    const std::size_t dot = zone_id.rfind('.');
    if (dot == std::string::npos) return std::string();
    return zone_id.substr(dot + 1);
}

// Id-/filename-safe tag for a signed chunk index pair. Negative indices cannot
// carry a '-' (it is outside the id grammar [a-z0-9_]), so a negative value is
// spelled "n<abs>": (-1,2) -> "n1_2". Deterministic and reversible-by-eye.
std::string coord_tag(int cx, int cz) {
    auto part = [](int v) -> std::string {
        if (v < 0) return "n" + std::to_string(-v);
        return std::to_string(v);
    };
    return part(cx) + "_" + part(cz);
}

// ---- Little-endian byte writers + base64 (Godot PackedByteArray payloads) ---
//
// Godot serializes an ArrayMesh surface's buffers as base64 PackedByteArray
// literals holding LITTLE-ENDIAN binary. These writers emit LE explicitly (byte
// by byte, never a raw memcpy of the host layout) so the emitted bytes are
// identical on a big-endian host — determinism is a P0 here (header, DETERMINISM).

void put_f32(std::string& b, float v) {
    std::uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));  // bit pattern; the shifts below impose LE order
    b.push_back(static_cast<char>(u & 0xFF));
    b.push_back(static_cast<char>((u >> 8) & 0xFF));
    b.push_back(static_cast<char>((u >> 16) & 0xFF));
    b.push_back(static_cast<char>((u >> 24) & 0xFF));
}

void put_u16(std::string& b, std::uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>((v >> 8) & 0xFF));
}

// base64 (RFC 4648, standard alphabet + '=' padding) — the encoding Godot's
// text-scene PackedByteArray literals use.
std::string base64(const std::string& raw) {
    static const char* kAlpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((raw.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < raw.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint8_t>(raw[i]) << 16) |
                                (static_cast<std::uint8_t>(raw[i + 1]) << 8) |
                                static_cast<std::uint8_t>(raw[i + 2]);
        out.push_back(kAlpha[(n >> 18) & 0x3F]);
        out.push_back(kAlpha[(n >> 12) & 0x3F]);
        out.push_back(kAlpha[(n >> 6) & 0x3F]);
        out.push_back(kAlpha[n & 0x3F]);
    }
    if (i < raw.size()) {
        const std::size_t rem = raw.size() - i;  // 1 or 2
        std::uint32_t n = static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[i])) << 16;
        if (rem == 2) n |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw[i + 1])) << 8;
        out.push_back(kAlpha[(n >> 18) & 0x3F]);
        out.push_back(kAlpha[(n >> 12) & 0x3F]);
        out.push_back(rem == 2 ? kAlpha[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

// ---- Godot vertex-attribute compression ------------------------------------
//
// Godot 4 stores normals/tangents OCTAHEDRON-encoded as a pair of unorm16s (not
// raw floats). These mirror Vector3::octahedron_encode /
// octahedron_tangent_encode and the (uint16_t)CLAMP(v * 65535, 0, 65535) quantise
// in RenderingServer's surface packer. Verified byte-for-byte against what Godot
// 4.7.stable itself writes for a known mesh (see the tests).
//
// Only sqrt/fabs are used anywhere in the mesh path — both are IEEE-754
// correctly-rounded, so unlike the transcendentals (sin/exp) they are
// bit-identical across platforms and keep the emit deterministic.
void octahedron_encode(float x, float y, float z, float& ox, float& oy) {
    const float s = std::fabs(x) + std::fabs(y) + std::fabs(z);
    // Callers always pass a normalised, non-degenerate vector; guard anyway so a
    // zero vector can never produce a NaN in a shipped payload.
    if (s == 0.0f) { ox = 0.5f; oy = 0.5f; return; }
    x /= s;
    y /= s;
    z /= s;
    if (z >= 0.0f) {
        ox = x;
        oy = y;
    } else {
        ox = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        oy = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
    }
    ox = ox * 0.5f + 0.5f;
    oy = oy * 0.5f + 0.5f;
}

std::uint16_t unorm16(float v) {
    const float c = v * 65535.0f;
    if (!(c > 0.0f)) return 0;          // also catches NaN
    if (c >= 65535.0f) return 65535;
    return static_cast<std::uint16_t>(c);  // truncates, matching Godot's cast
}

// Append Godot's 8-byte normal+tangent block for one vertex: unorm16 pairs
// (normal.xy, tangent.xy). `sign` is the tangent's binormal sign (±1).
void put_normal_tangent(std::string& b, float nx, float ny, float nz, float tx, float ty,
                        float tz, float sign) {
    float ox = 0.0f, oy = 0.0f;
    octahedron_encode(nx, ny, nz, ox, oy);
    put_u16(b, unorm16(ox));
    put_u16(b, unorm16(oy));

    octahedron_encode(tx, ty, tz, ox, oy);
    // octahedron_tangent_encode: bias y off zero, fold to [0.5,1], then flip for
    // a negative sign — the sign rides in the encoded y.
    const float bias = 1.0f / 32767.0f;
    if (oy < bias) oy = bias;
    oy = oy * 0.5f + 0.5f;
    if (sign < 0.0f) oy = 1.0f - oy;
    put_u16(b, unorm16(ox));
    put_u16(b, unorm16(oy));
}

// ---- Heightfield generation ------------------------------------------------

// The deliberately NON-FLAT fixture surface: a gentle eastward ramp plus a
// shallow parabolic bowl, evaluated as a PURE FUNCTION of zone-local world
// coordinates. Because height depends only on (wx, wz), a chunk's shared edge
// (local row/col 128) evaluates identically to the neighbour's row/col 0 — the
// shared-edge convention (§3.2) holds for free, with no seam. Pure float math
// (no libm) keeps it bit-deterministic across platforms.
float fixture_height(double wx, double wz, double grid_min_x, double grid_min_z,
                     double center_x, double center_z, int chunk_size_m) {
    const float ramp = 0.08f * static_cast<float>(wx - grid_min_x);   // rises eastward
    const float ndx = static_cast<float>((wx - center_x) / chunk_size_m);
    const float ndz = static_cast<float>((wz - center_z) / chunk_size_m);
    const float bowl = 2.5f * (ndx * ndx + ndz * ndz);               // shallow parabola
    (void)grid_min_z;  // z-ramp intentionally omitted: the x-ramp + radial bowl already slope both axes
    return 8.0f + ramp + bowl;
}

// §3.2: 128 m @ 1 m spacing + the shared edge column/row.
constexpr int kChunkSide = 129;

// A chunk's heightfield PADDED by one sample ring (side+2 squared, local coords
// [-1 .. side]). The pad exists purely for the render mesh: a central-difference
// normal at the chunk's edge needs the neighbour's first sample, and because
// height is a pure function of world coords the padded ring evaluates to EXACTLY
// the neighbour chunk's own samples. So per-chunk normals agree across a shared
// edge and the terrain lights seamlessly — no cross-chunk data flow needed. The
// server payload uses only the inner side×side window (`padded_at`).
struct PaddedField {
    std::vector<float> v;  // (side+2)^2, row-major over padded coords
    int side = kChunkSide;

    float at(int lx, int lz) const {  // lx,lz in [-1 .. side]
        return v[static_cast<std::size_t>(lz + 1) * (side + 2) + (lx + 1)];
    }
};

PaddedField build_padded_field(const ChunkRecord& rec, double origin_x, double origin_z,
                               double grid_min_x, double grid_min_z, double center_x,
                               double center_z, int chunk_size_m) {
    PaddedField f;
    const int n = kChunkSide + 2;
    f.v.reserve(static_cast<std::size_t>(n) * n);
    for (int lz = -1; lz <= kChunkSide; ++lz) {
        const double wz = origin_z + static_cast<double>(rec.cz) * chunk_size_m + lz;
        for (int lx = -1; lx <= kChunkSide; ++lx) {
            const double wx = origin_x + static_cast<double>(rec.cx) * chunk_size_m + lx;
            f.v.push_back(fixture_height(wx, wz, grid_min_x, grid_min_z, center_x, center_z,
                                         chunk_size_m));
        }
    }
    return f;
}

// Build one chunk's ServerChunk FlatBuffer (§3.4). format_version=1, the AoI
// `coord`, the required 129×129 f32 Heightfield (row-major, z-outer x-inner), and
// the terrain heightfield-collider params. The remaining server slices
// (colliders/navmesh/liquids/markers/volumes) are empty at v0 — a bootstrap
// terrain-only fixture — but the required contract fields are all present so the
// buffer passes the FlatBuffers verifier and worldd's loader. Also fills the
// chunk's Y AABB bounds from the generated samples.
std::string build_chunk_bin(ChunkRecord& rec, const PaddedField& field, double origin_x,
                            double origin_z, int chunk_size_m) {
    const int side = kChunkSide;
    std::vector<float> samples;
    samples.reserve(static_cast<std::size_t>(side) * side);

    float min_y = 0.0f;
    float max_y = 0.0f;
    bool first = true;
    // Row-major: z outer (north–south), x inner (east–west) — i = z*129 + x. The
    // inner window of the padded field: identical values to the pre-pad emit.
    for (int lz = 0; lz < side; ++lz) {
        for (int lx = 0; lx < side; ++lx) {
            const float h = field.at(lx, lz);
            samples.push_back(h);
            if (first || h < min_y) min_y = h;
            if (first || h > max_y) max_y = h;
            first = false;
        }
    }

    // Zone-local AABB (metres): XZ from the chunk's grid cell, Y from the samples.
    rec.min_x = static_cast<float>(origin_x + static_cast<double>(rec.cx) * chunk_size_m);
    rec.max_x = rec.min_x + static_cast<float>(chunk_size_m);
    rec.min_z = static_cast<float>(origin_z + static_cast<double>(rec.cz) * chunk_size_m);
    rec.max_z = rec.min_z + static_cast<float>(chunk_size_m);
    rec.min_y = min_y;
    rec.max_y = max_y;

    flatbuffers::FlatBufferBuilder fbb(1 << 18);
    const auto samples_off = fbb.CreateVector<float>(samples);
    const auto hf = meridian::chunk::CreateHeightfield(fbb, static_cast<uint16_t>(side),
                                                       1.0f, samples_off);
    const auto hfc = meridian::chunk::CreateHeightfieldCollider(fbb, 0.04f, 0.0f);
    const meridian::chunk::ChunkCoord coord(rec.cx, rec.cz);

    meridian::chunk::ServerChunkBuilder sb(fbb);
    sb.add_format_version(1);
    sb.add_coord(&coord);
    sb.add_heightfield(hf);
    sb.add_hf_collider(hfc);
    const auto root = sb.Finish();
    meridian::chunk::FinishServerChunkBuffer(fbb, root);  // adds the "MCHK" file_identifier

    return std::string(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                       fbb.GetSize());
}

// ---- Godot scene generation ------------------------------------------------

// The Godot ArrayMesh surface format bits this emitter writes:
//   VERTEX(1) | NORMAL(2) | TANGENT(4) | TEX_UV(16) | INDEX(4096) = 4119,
//   OR'd with the mesh array-format VERSION in the high bits (1 << 35).
// Godot ALWAYS pairs TANGENT with NORMAL (they share one packed 8-byte block), so
// tangent is not optional once normals are present. This constant is what Godot
// 4.7.stable emits for such a surface; it is asserted against the engine's own
// output in the tests, which is the tripwire if an engine bump changes the layout.
constexpr std::uint64_t kMeshFormat = (static_cast<std::uint64_t>(1) << 35) | 4119ULL;

// One built ArrayMesh surface, in the exact buffers Godot's text scene wants.
struct MeshSurface {
    std::string vertex_data;     // positions (3×f32) for all verts, then normal+tangent (4×u16)
    std::string attribute_data;  // UV (2×f32) per vertex
    std::string index_data;      // u16 triangle indices
    int vertex_count = 0;
    int index_count = 0;
    float min_y = 0.0f;
    float max_y = 0.0f;
};

// Turn the chunk's heightfield into a real triangulated surface.
//
// Vertices are MESH-LOCAL: x/z run 0..chunk_size_m (the node transform below puts
// the chunk at its zone-local corner), y is the sample height itself — so the node
// sits at y=0 and vertex y IS world height, keeping the mesh and the server
// heightfield trivially comparable.
//
// `stride` decimates the sample grid (1 = every sample, the exact heightfield;
// 4 = every 4th, the coarse proxy). Normals always use full-resolution central
// differences off the PADDED field, so they describe the true surface and match
// across chunk edges regardless of stride.
MeshSurface build_surface(const PaddedField& field, int stride, int chunk_size_m) {
    const int side = field.side;
    const float spacing = static_cast<float>(chunk_size_m) / static_cast<float>(side - 1);
    const int n = (side - 1) / stride + 1;  // verts per axis

    MeshSurface s;
    s.vertex_count = n * n;
    std::string normals;  // built alongside, appended after all positions
    s.vertex_data.reserve(static_cast<std::size_t>(s.vertex_count) * 12);
    normals.reserve(static_cast<std::size_t>(s.vertex_count) * 8);
    s.attribute_data.reserve(static_cast<std::size_t>(s.vertex_count) * 8);

    bool first = true;
    for (int j = 0; j < n; ++j) {
        const int lz = j * stride;
        for (int i = 0; i < n; ++i) {
            const int lx = i * stride;
            const float h = field.at(lx, lz);
            if (first || h < s.min_y) s.min_y = h;
            if (first || h > s.max_y) s.max_y = h;
            first = false;

            put_f32(s.vertex_data, static_cast<float>(lx) * spacing);
            put_f32(s.vertex_data, h);
            put_f32(s.vertex_data, static_cast<float>(lz) * spacing);

            // Central differences on the padded field (valid even at lx/lz 0 and
            // side-1, which is exactly what the pad ring is for).
            const float dhdx = (field.at(lx + 1, lz) - field.at(lx - 1, lz)) / (2.0f * spacing);
            const float dhdz = (field.at(lx, lz + 1) - field.at(lx, lz - 1)) / (2.0f * spacing);
            // Surface normal of a height field h(x,z): (-dh/dx, 1, -dh/dz), normalised.
            float nx = -dhdx, ny = 1.0f, nz = -dhdz;
            const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= nlen;
            ny /= nlen;
            nz /= nlen;
            // Tangent along +X (the U axis): (1, dh/dx, 0), normalised.
            float tx = 1.0f, ty = dhdx, tz = 0.0f;
            const float tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
            tx /= tlen;
            ty /= tlen;
            tz /= tlen;
            put_normal_tangent(normals, nx, ny, nz, tx, ty, tz, /*sign=*/1.0f);

            // UV spans [0,1] across the chunk so a tiling ground material can scale
            // it however it likes (T3 supplies the real material).
            put_f32(s.attribute_data, static_cast<float>(lx) / static_cast<float>(side - 1));
            put_f32(s.attribute_data, static_cast<float>(lz) / static_cast<float>(side - 1));
        }
    }
    s.vertex_data += normals;

    // Two triangles per quad, wound so the surface FACES UP.
    //
    // Godot's front faces are clockwise, which in practice means: for an
    // upward-facing surface the RIGHT-HAND-rule normal (B-A)×(C-A) points DOWN
    // (-Y). Ground truth is Godot's own PlaneMesh — an up-facing plane whose
    // declared normal is +Y emits triangles whose right-hand normal is -Y. Get this
    // backwards and the terrain is silently invisible: it still loads, still reads
    // back correct arrays, and backface culling simply hides every triangle from a
    // camera above it. The tests assert the per-triangle orientation for exactly
    // this reason.
    s.index_data.reserve(static_cast<std::size_t>(n - 1) * (n - 1) * 12);
    for (int j = 0; j + 1 < n; ++j) {
        for (int i = 0; i + 1 < n; ++i) {
            const std::uint16_t v00 = static_cast<std::uint16_t>(j * n + i);
            const std::uint16_t v10 = static_cast<std::uint16_t>(j * n + i + 1);
            const std::uint16_t v01 = static_cast<std::uint16_t>((j + 1) * n + i);
            const std::uint16_t v11 = static_cast<std::uint16_t>((j + 1) * n + i + 1);
            put_u16(s.index_data, v00);
            put_u16(s.index_data, v10);
            put_u16(s.index_data, v01);
            put_u16(s.index_data, v10);
            put_u16(s.index_data, v11);
            put_u16(s.index_data, v01);
            s.index_count += 6;
        }
    }
    return s;
}

// A VALID Godot text scene (.tscn) the streamer can instance: a MeshInstance3D
// holding an ArrayMesh baked from the chunk's own heightfield — the real terrain
// surface, replacing the v0 BoxMesh placeholder (#875). The node is translated to
// the chunk's zone-local XZ corner at y=0, so the mesh's vertex heights are world
// heights and neighbouring chunks tile exactly along their shared edge.
//
// The surface carries a StandardMaterial3D slot (a neutral placeholder; the chibi
// ground material is T3's job) keyed to the chunk's existing shared terrain-ground
// dep id. It is an embedded sub-resource rather than an ext_resource because that
// dep has no on-disk payload at v0 — a dangling ext_resource would fail the load.
//
// The proxy is the same surface decimated 4× per axis (33×33 verts vs 129×129) —
// a genuinely coarser LOD stand-in, not just a flag change.
std::string build_scene(const ChunkRecord& rec, const PaddedField& field, int chunk_size_m,
                        const std::string& material_id, bool proxy) {
    const int stride = proxy ? 4 : 1;
    const MeshSurface m = build_surface(field, stride, chunk_size_m);
    const std::string node_name = (proxy ? "ChunkProxy_" : "Chunk_") + rec.tag;

    std::ostringstream s;
    s << "[gd_scene load_steps=3 format=3]\n\n";

    // Placeholder ground material (T3 replaces the look; the SLOT is the contract).
    s << "[sub_resource type=\"StandardMaterial3D\" id=\"TerrainGround\"]\n";
    s << "resource_name = " << json_quote(material_id) << "\n";
    s << "albedo_color = Color(0.5000, 0.5000, 0.5000, 1.0000)\n";
    s << "roughness = 1.0000\n";
    s << "metallic = 0.0000\n";
    s << "\n";

    // Mesh-local AABB: XZ is the chunk footprint, Y the surface's own span.
    s << "[sub_resource type=\"ArrayMesh\" id=\"TerrainMesh\"]\n";
    s << "_surfaces = [{\n";
    s << "\"aabb\": AABB(0, " << num(m.min_y) << ", 0, " << num(chunk_size_m) << ", "
      << num(m.max_y - m.min_y) << ", " << num(chunk_size_m) << "),\n";
    s << "\"attribute_data\": PackedByteArray(" << json_quote(base64(m.attribute_data)) << "),\n";
    s << "\"format\": " << kMeshFormat << ",\n";
    s << "\"index_count\": " << m.index_count << ",\n";
    s << "\"index_data\": PackedByteArray(" << json_quote(base64(m.index_data)) << "),\n";
    s << "\"material\": SubResource(\"TerrainGround\"),\n";
    s << "\"primitive\": 3,\n";  // Mesh.PRIMITIVE_TRIANGLES
    s << "\"uv_scale\": Vector4(0, 0, 0, 0),\n";
    s << "\"vertex_count\": " << m.vertex_count << ",\n";
    s << "\"vertex_data\": PackedByteArray(" << json_quote(base64(m.vertex_data)) << ")\n";
    s << "}]\n\n";

    s << "[node name=\"" << node_name << "\" type=\"MeshInstance3D\"]\n";
    s << "transform = Transform3D(1, 0, 0, 0, 1, 0, 0, 0, 1, " << num(rec.min_x) << ", "
      << num(0.0) << ", " << num(rec.min_z) << ")\n";
    s << "mesh = SubResource(\"TerrainMesh\")\n";
    return s.str();
}

// ---- BLAKE3 helpers --------------------------------------------------------

// BLAKE3 of a byte range, 64 lowercase-hex (bare — the per-resource form).
std::string blake3_bare(const std::string& bytes) {
    return hash::blake3_hex(bytes.data(), bytes.size());
}

// The per-chunk manifest hash: BLAKE3 over BOTH payloads, each framed
// "<label>\0<bytes>\0" so no concatenation collision is possible. Rendered
// "blake3:<hex>" per the manifest contentHash grammar.
std::string entry_hash_of(const std::string& server_bytes, const std::string& scene_bytes,
                          const std::string& proxy_bytes) {
    hash::Blake3 h;
    auto frame = [&h](const char* label, const std::string& b) {
        h.update(label, std::string(label).size());
        h.update("\0", 1);
        h.update(b.data(), b.size());
        h.update("\0", 1);
    };
    frame("server", server_bytes);
    frame("scene", scene_bytes);
    frame("proxy", proxy_bytes);
    return "blake3:" + h.hex();
}

}  // namespace

ChunkEmitResult chunk_emit(const ChunkEmitOptions& opts, diag::Diagnostics& diags) {
    ChunkEmitResult result;
    result.zone = opts.zone;
    result.ns = ns_of(opts.zone);
    result.zone_bare = zone_bare_of(opts.zone);

    // ---- Validate options ---------------------------------------------------
    if (opts.grid < 1) {
        diags.error("E300", opts.zone, "$.grid",
                    "chunk-emit: grid must be >= 1 (got " + std::to_string(opts.grid) + ")");
        result.ok = false;
        return result;
    }
    if (opts.chunk_size_m < 1) {
        diags.error("E300", opts.zone, "$.chunk_size_m",
                    "chunk-emit: chunk_size_m must be >= 1");
        result.ok = false;
        return result;
    }
    if (result.ns.empty() || result.zone_bare.empty() ||
        opts.zone.find(":zone.") == std::string::npos) {
        diags.error("E300", opts.zone, "$.zone",
                    "chunk-emit: zone must be a fully-qualified zone id "
                    "(e.g. core:zone.zone01)");
        result.ok = false;
        return result;
    }

    const std::string& ns = result.ns;
    const std::string& zb = result.zone_bare;

    // ---- Grid geometry ------------------------------------------------------
    // Centre the N×N grid on negative indices so the fixture exercises the
    // negative-coordinate path (Zone-01 spawns at x ≈ −300, §3.1): for N the
    // indices run [-floor(N/2) .. -floor(N/2)+N-1]. With the default -384 origin
    // this yields zone-local coords around −512..−128 for a 3×3 grid.
    const int half = opts.grid / 2;
    const int min_c = -half;
    const int max_c = min_c + opts.grid - 1;
    const double grid_min_x = opts.origin_x + static_cast<double>(min_c) * opts.chunk_size_m;
    const double grid_min_z = opts.origin_z + static_cast<double>(min_c) * opts.chunk_size_m;
    const double extent = static_cast<double>(opts.grid) * opts.chunk_size_m;
    const double center_x = grid_min_x + extent / 2.0;
    const double center_z = grid_min_z + extent / 2.0;

    // A single shared terrain-ground asset every chunk prefetches (deps half of
    // walk C4). A real IF-8 asset id (type art) so it validates as an assetId.
    const std::string shared_dep = ns + ":art.terrain." + zb + "_ground";

    // ---- Generate every chunk (row-major by (cz, cx)) -----------------------
    for (int cz = min_c; cz <= max_c; ++cz) {
        for (int cx = min_c; cx <= max_c; ++cx) {
            ChunkRecord rec;
            rec.cx = cx;
            rec.cz = cz;
            rec.tag = coord_tag(cx, cz);
            rec.scene_ref = ns + ":chunk." + zb + "." + rec.tag + ".scene";
            rec.proxy_ref = ns + ":chunk." + zb + "." + rec.tag + ".proxy";
            rec.server_ref = ns + ":chunk." + zb + "." + rec.tag + ".server";
            rec.deps = {shared_dep};
            // Load-order hint: grid-centre chunk first (0), rising by Chebyshev
            // distance so nearer-centre chunks stream in earlier (walk C4).
            rec.priority = std::max(std::abs(cx - 0), std::abs(cz - 0));

            // One padded heightfield evaluation feeds BOTH the server payload (its
            // inner window) and the render mesh (which needs the pad ring for
            // seam-free normals).
            const PaddedField field =
                build_padded_field(rec, opts.origin_x, opts.origin_z, grid_min_x, grid_min_z,
                                   center_x, center_z, opts.chunk_size_m);
            rec.chunk_bin =
                build_chunk_bin(rec, field, opts.origin_x, opts.origin_z, opts.chunk_size_m);
            // build_scene needs the chunk's zone-local corner, which build_chunk_bin
            // fills into rec — so it must run first.
            rec.scn = build_scene(rec, field, opts.chunk_size_m, shared_dep, /*proxy=*/false);
            rec.proxy_scn = build_scene(rec, field, opts.chunk_size_m, shared_dep, /*proxy=*/true);
            rec.entry_hash = entry_hash_of(rec.chunk_bin, rec.scn, rec.proxy_scn);

            result.chunks.push_back(std::move(rec));
        }
    }

    // ---- IF-8 asset-ID table ------------------------------------------------
    // Collect every logical ref (server/scene/proxy per chunk + shared deps),
    // allocate band-0 local indices in LEXICOGRAPHIC id order (idmap discipline,
    // SAD §2.4), and record each entry's kind + by-ID res:// path + per-resource
    // BLAKE3. res:// is logical (independent of --out); ext follows the payload.
    struct AssetMeta {
        std::string type;
        std::string res_path;
        std::string hash;
    };
    std::map<std::string, AssetMeta> asset_meta;  // id -> meta (sorted by id)
    const std::string res_root = "res://meridian/" + ns + "/chunks/" + zb + "/";
    for (const ChunkRecord& rec : result.chunks) {
        asset_meta[rec.server_ref] = {"chunk_server", res_root + rec.tag + ".chunk.bin",
                                      blake3_bare(rec.chunk_bin)};
        // Godot routes `.scn` to the BINARY scene loader, which rejects our
        // TEXT-format payloads ("Unrecognized binary resource file", #579). mcc
        // is C++ and cannot emit Godot's binary `.scn`, so the fixture ships the
        // text scenes with the `.tscn` extension the TEXT loader reads directly —
        // making the fixture loadable as-shipped (no staging rename). The payload
        // BYTES are unchanged (build_scene already renders a valid `.tscn`), so the
        // per-resource BLAKE3 and the per-chunk hash are identical to before.
        asset_meta[rec.scene_ref] = {"chunk_scene", res_root + rec.tag + ".tscn",
                                     blake3_bare(rec.scn)};
        asset_meta[rec.proxy_ref] = {"chunk_proxy", res_root + rec.tag + ".proxy.tscn",
                                     blake3_bare(rec.proxy_scn)};
    }
    // Shared logical deps (no on-disk payload at v0): a stable id-derived hash so
    // the pack entry still verifies. res:// follows the by-ID asset layout.
    {
        AssetMeta m;
        m.type = "asset";
        m.res_path = "res://meridian/" + ns + "/art/terrain/" + zb + "_ground.res";
        m.hash = blake3_bare(shared_dep);
        asset_meta[shared_dep] = m;
    }

    // Allocate local indices in lexicographic id order (std::map is already sorted).
    std::uint32_t next_idx = 1;
    for (const auto& [id, meta] : asset_meta) {
        ChunkAsset a;
        a.id = id;
        a.local_index = next_idx;
        a.numeric_id = idmap::numeric_id(/*band=*/0, next_idx);
        a.type = meta.type;
        a.res_path = meta.res_path;
        a.hash = meta.hash;
        result.assets.push_back(std::move(a));
        ++next_idx;
    }
    // result.assets is already in numeric-id order (== local-index == lexicographic).

    // ---- Pack content hash --------------------------------------------------
    // A deterministic BLAKE3 over the sorted (id\0hash\0) of every pack entry —
    // the fixture pack's aggregate content hash (the emit-pck three-way-tie role;
    // here it ties the pack manifest to exactly the resources it lists).
    {
        hash::Blake3 h;
        for (const ChunkAsset& a : result.assets) {
            h.update(a.id.data(), a.id.size());
            h.update("\0", 1);
            h.update(a.hash.data(), a.hash.size());
            h.update("\0", 1);
        }
        result.content_hash = h.hex();
    }

    // ---- Render <zone>.chunks.json (IF-6 manifest) --------------------------
    {
        std::ostringstream j;
        j << "{\n";
        j << "  \"format_version\": 1,\n";
        j << "  \"zone\": " << json_quote(opts.zone) << ",\n";
        j << "  \"chunk_size_m\": " << opts.chunk_size_m << ",\n";
        j << "  \"origin\": { \"x\": " << num(opts.origin_x) << ", \"z\": "
          << num(opts.origin_z) << " },\n";
        j << "  \"grid\": { \"min_cx\": " << min_c << ", \"min_cz\": " << min_c
          << ", \"max_cx\": " << max_c << ", \"max_cz\": " << max_c << " },\n";
        j << "  \"far_ring\": " << opts.far_ring << ",\n";
        j << "  \"chunks\": [";
        for (std::size_t i = 0; i < result.chunks.size(); ++i) {
            const ChunkRecord& r = result.chunks[i];
            j << (i == 0 ? "\n" : ",\n");
            j << "    {\n";
            j << "      \"cx\": " << r.cx << ", \"cz\": " << r.cz << ",\n";
            j << "      \"hash\": " << json_quote(r.entry_hash) << ",\n";
            j << "      \"scene\": " << json_quote(r.scene_ref) << ",\n";
            j << "      \"proxy\": " << json_quote(r.proxy_ref) << ",\n";
            j << "      \"server\": " << json_quote(r.server_ref) << ",\n";
            j << "      \"deps\": [";
            for (std::size_t d = 0; d < r.deps.size(); ++d) {
                j << (d == 0 ? "" : ", ") << json_quote(r.deps[d]);
            }
            j << "],\n";
            j << "      \"priority\": " << r.priority << ",\n";
            j << "      \"aabb\": { \"min\": { \"x\": " << num(r.min_x) << ", \"y\": "
              << num(r.min_y) << ", \"z\": " << num(r.min_z) << " }, \"max\": { \"x\": "
              << num(r.max_x) << ", \"y\": " << num(r.max_y) << ", \"z\": " << num(r.max_z)
              << " } }\n";
            j << "    }";
        }
        j << (result.chunks.empty() ? "" : "\n  ") << "]\n";
        j << "}\n";
        result.manifest_json = j.str();
    }

    // ---- Render <zone>.assets.json (IF-8 asset-ID table) --------------------
    {
        std::ostringstream j;
        j << "{\n";
        j << "  \"schema\": \"meridian/chunk-assetmap@1\",\n";
        j << "  \"zone\": " << json_quote(opts.zone) << ",\n";
        j << "  \"namespace\": " << json_quote(ns) << ",\n";
        j << "  \"band\": 0,\n";
        j << "  \"entry_count\": " << result.assets.size() << ",\n";
        j << "  \"entries\": [";
        for (std::size_t i = 0; i < result.assets.size(); ++i) {
            const ChunkAsset& a = result.assets[i];
            j << (i == 0 ? "\n" : ",\n");
            j << "    { \"id\": " << json_quote(a.id) << ", \"local_index\": " << a.local_index
              << ", \"numeric_id\": " << a.numeric_id << ", \"type\": " << json_quote(a.type)
              << ", \"resource\": " << json_quote(a.res_path) << ", \"hash\": "
              << json_quote(a.hash) << " }";
        }
        j << (result.assets.empty() ? "" : "\n  ") << "]\n";
        j << "}\n";
        result.assets_json = j.str();
    }

    // ---- Render pack.manifest.json + M0 pack.contents.jsonl (IF-5) ----------
    // Same meridian/pack-manifest@1 shape emit-pck (#121) produces, so the client
    // pack reader handles both. INCLUDES the server .chunk.bin per chunk (Q1(a)).
    {
        std::ostringstream j;
        j << "{\n";
        j << "  \"schema\": \"meridian/pack-manifest@1\",\n";
        j << "  \"pack\": " << json_quote(ns) << ",\n";
        j << "  \"namespace\": " << json_quote(ns) << ",\n";
        j << "  \"version\": \"0.0.0\",\n";
        j << "  \"content_schema_version\": 1,\n";
        j << "  \"godot_version\": " << json_quote(opts.godot_version) << ",\n";
        j << "  \"id_band\": 0,\n";
        j << "  \"content_hash\": " << json_quote(result.content_hash) << ",\n";
        j << "  \"mcc_version\": " << json_quote(opts.mcc_version) << ",\n";
        j << "  \"built_at\": " << json_quote(opts.built_at) << ",\n";
        j << "  \"entry_count\": " << result.assets.size() << ",\n";
        j << "  \"entries\": [";
        for (std::size_t i = 0; i < result.assets.size(); ++i) {
            const ChunkAsset& a = result.assets[i];
            j << (i == 0 ? "\n" : ",\n");
            j << "    { \"id\": " << json_quote(a.id) << ", \"numeric_id\": " << a.numeric_id
              << ", \"type\": " << json_quote(a.type) << ", \"resource\": "
              << json_quote(a.res_path) << ", \"hash\": " << json_quote(a.hash) << " }";
        }
        j << (result.assets.empty() ? "" : "\n  ") << "]\n";
        j << "}\n";
        result.pack_manifest_json = j.str();

        std::ostringstream p;
        for (const ChunkAsset& a : result.assets) {
            p << "{\"id\":" << json_quote(a.id) << ",\"numeric_id\":" << a.numeric_id
              << ",\"resource\":" << json_quote(a.res_path) << ",\"hash\":"
              << json_quote(a.hash) << "}\n";
        }
        result.pack_contents_jsonl = p.str();
    }

    if (!diags.ok()) result.ok = false;
    return result;
}

int chunk_emit_run(const ChunkEmitOptions& opts, const std::string& out_dir,
                   DiagFormat format, std::ostream& out, std::ostream& err) {
    diag::Diagnostics diags;
    const ChunkEmitResult res = chunk_emit(opts, diags);

    std::ostream& diag_out = out_dir.empty() ? err : out;
    if (format == DiagFormat::Json) {
        diag::render_json(diags, diag_out);
    } else {
        std::string stats =
            "Emitted chunk fixture '" + res.zone + "': " +
            std::to_string(res.chunks.size()) + " chunks, " +
            std::to_string(res.assets.size()) + " assets (content_hash " +
            (res.content_hash.size() >= 12 ? res.content_hash.substr(0, 12)
                                           : res.content_hash) +
            "...).";
        diag::render_text(diags, stats, diag_out);
    }

    if (!res.ok || !diags.ok()) {
        err << "mcc chunk-emit: emit failed — no fixture written\n";
        return 1;
    }

    if (out_dir.empty()) {
        // No output dir: the IF-6 manifest (the primary contract) goes to stdout.
        out << res.manifest_json;
        return 0;
    }

    std::error_code ec;
    const fs::path chunk_dir =
        fs::path(out_dir) / "meridian" / res.ns / "chunks" / res.zone_bare;
    fs::create_directories(chunk_dir, ec);
    if (ec) {
        err << "mcc chunk-emit: could not create output dir: " << chunk_dir.string() << '\n';
        return 2;
    }

    auto write = [&](const fs::path& p, const std::string& bytes) -> bool {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) {
            err << "mcc chunk-emit: could not write " << p.string() << '\n';
            return false;
        }
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(f);
    };

    if (!write(chunk_dir / (res.zone_bare + ".chunks.json"), res.manifest_json)) return 2;
    if (!write(chunk_dir / (res.zone_bare + ".assets.json"), res.assets_json)) return 2;
    if (!write(chunk_dir / "pack.manifest.json", res.pack_manifest_json)) return 2;
    if (!write(chunk_dir / "pack.contents.jsonl", res.pack_contents_jsonl)) return 2;
    for (const ChunkRecord& r : res.chunks) {
        if (!write(chunk_dir / (r.tag + ".chunk.bin"), r.chunk_bin)) return 2;
        if (!write(chunk_dir / (r.tag + ".tscn"), r.scn)) return 2;
        if (!write(chunk_dir / (r.tag + ".proxy.tscn"), r.proxy_scn)) return 2;
    }

    out << "  chunk-emit: wrote " << res.chunks.size() << "-chunk fixture -> "
        << chunk_dir.string() << "/" << res.zone_bare << ".chunks.json\n";
    return 0;
}

}  // namespace mcc::stages
