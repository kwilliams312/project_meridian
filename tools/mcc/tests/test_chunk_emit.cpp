// tools/mcc/tests/test_chunk_emit.cpp — round-trip tests for the chunk-emit stage
// (Tools SAD §3, IF-6, story #553). Self-contained (no GoogleTest), matching the
// test_emit_pck.cpp / test_emit_sql.cpp style. CTest-registered.
//
// Covers the task's required round-trip:
//   (a) the <zone>.chunks.json manifest parses and carries EVERY field the real
//       schema/chunk/chunk-manifest.schema.yaml marks required (top-level +
//       per-chunk entry), with a non-zero origin, negative-index grid bounds
//       (Zone-01 §3.1), chunk_size_m=128, far_ring, and C2 refs / aabb / hash
//       matching the schema's id + contentHash grammars.
//   (b) each <cx>_<cz>.chunk.bin parses via the FlatBuffers verifier as a
//       ServerChunk with a 129×129 f32 Heightfield that is DELIBERATELY NON-FLAT
//       (a flat heightfield would pass a lazy test but hide a flat-vs-sloped bug),
//       and adjacent chunks share their edge exactly (shared-edge convention §3.2).
//   (c) the per-chunk BLAKE3 recomputed INDEPENDENTLY over both payloads matches
//       the manifest hash (proves the emitted hash is real, not stamped).
//   (d) the IF-5 pack contains every referenced asset — every chunk's scene,
//       proxy AND server .chunk.bin ref resolves to a pack entry (Q1(a)).
//   (e) determinism — the whole emit is byte-identical across runs, and the CLI
//       wrapper writes the fixture to disk with the manifest matching the result.
//
// Exit code 0 = all pass; non-zero = at least one failure (CTest reads this).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>  // getpid (unique scratch dir)

#include <yaml-cpp/yaml.h>

#include "flatbuffers/flatbuffers.h"

#include "chunk_generated.h"
#include "hash/blake3.h"
#include "stages/chunk_emit.h"
#include "stages/diagnostics.h"

#ifndef MCC_CHUNK_MANIFEST_SCHEMA
#define MCC_CHUNK_MANIFEST_SCHEMA ""
#endif

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
int g_checks = 0;

void report(bool cond, const std::string& name, const std::string& detail = "") {
    ++g_checks;
    if (cond) {
        std::cout << "  ok   " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "  FAIL " << name << "\n";
        if (!detail.empty()) std::cout << "       " << detail << "\n";
    }
}

mcc::stages::ChunkEmitResult run(const mcc::stages::ChunkEmitOptions& opts) {
    mcc::diag::Diagnostics diags;
    return mcc::stages::chunk_emit(opts, diags);
}

// Independent re-implementation of the stage's per-chunk hash framing, so the
// test proves the manifest hash rather than trusting the stage's own value.
std::string recompute_entry_hash(const std::string& server, const std::string& scene,
                                 const std::string& proxy) {
    mcc::hash::Blake3 h;
    auto frame = [&h](const char* label, const std::string& b) {
        h.update(label, std::string(label).size());
        h.update("\0", 1);
        h.update(b.data(), b.size());
        h.update("\0", 1);
    };
    frame("server", server);
    frame("scene", scene);
    frame("proxy", proxy);
    return "blake3:" + h.hex();
}

const std::regex kResourceRef(R"(^[a-z][a-z0-9_]{1,31}:[a-z0-9_]+(\.[a-z0-9_]+)*$)");
const std::regex kAssetId(R"(^[a-z][a-z0-9_]{1,31}:(art|mus|sfx|amb)\.[a-z0-9_]+(\.[a-z0-9_]+)*$)");
const std::regex kContentHash(R"(^blake3:[0-9a-f]{64}$)");
const std::regex kZoneId(R"(^[a-z][a-z0-9_]{1,31}:zone\.[a-z0-9_]+(\.[a-z0-9_]+)*$)");

// Cross-check a manifest against the REAL schema's `required` lists (top-level +
// per-entry). Shared by the zone01 fixture and the chibi meadow emit.
bool schema_required_ok(const YAML::Node& m) {
    bool ok = true;
    try {
        const YAML::Node schema = YAML::LoadFile(MCC_CHUNK_MANIFEST_SCHEMA);
        for (const auto& req : schema["required"]) {
            const std::string key = req.as<std::string>();
            if (!m[key]) {
                ok = false;
                std::cout << "       missing top-level required: " << key << "\n";
            }
        }
        const YAML::Node entry_req = schema["$defs"]["chunkEntry"]["required"];
        const YAML::Node e0 = m["chunks"][0];
        for (const auto& req : entry_req) {
            const std::string key = req.as<std::string>();
            if (!e0[key]) {
                ok = false;
                std::cout << "       missing chunkEntry required: " << key << "\n";
            }
        }
    } catch (const std::exception& ex) {
        ok = false;
        std::cout << "       schema load failed: " << ex.what() << "\n";
    }
    return ok;
}

// Decode a chunk's ServerChunk heightfield samples (129x129, row-major z-outer).
std::vector<float> heightfield_of(const std::string& chunk_bin) {
    const auto* buf = reinterpret_cast<const uint8_t*>(chunk_bin.data());
    flatbuffers::Verifier v(buf, chunk_bin.size());
    if (!meridian::chunk::VerifyServerChunkBuffer(v)) return {};
    const auto* hf = meridian::chunk::GetServerChunk(buf)->heightfield();
    if (!hf || !hf->samples()) return {};
    std::vector<float> out;
    out.reserve(hf->samples()->size());
    for (flatbuffers::uoffset_t i = 0; i < hf->samples()->size(); ++i)
        out.push_back(hf->samples()->Get(i));
    return out;
}

// The canonical Sprout Meadow emit: the `meadow` profile at zone01's EXISTING
// geometry (origin -384, 3x3 grid => zone-local [-512,-128]), so the server's
// spawn (-320,-320) and bounds constants need no change.
mcc::stages::ChunkEmitOptions meadow_opts() {
    mcc::stages::ChunkEmitOptions o;
    o.zone = "chibi:zone.sprout_meadow";
    o.profile = mcc::stages::HeightProfile::Meadow;
    o.grid = 3;
    o.origin_x = -384.0;
    o.origin_z = -384.0;
    return o;
}

// (a) manifest well-formed + schema-required-field coverage ------------------
void test_manifest_wellformed() {
    std::cout << "test_manifest_wellformed (IF-6 <zone>.chunks.json)\n";
    const mcc::stages::ChunkEmitResult res = run({});
    report(res.ok, "chunk_emit reports ok");
    report(res.chunks.size() == 9, "3x3 grid -> 9 chunks",
           "got " + std::to_string(res.chunks.size()));

    YAML::Node m;
    bool parsed = true;
    try { m = YAML::Load(res.manifest_json); } catch (...) { parsed = false; }
    report(parsed && m.IsMap(), "manifest parses as a JSON/YAML mapping");

    report(m["format_version"].as<int>(0) == 1, "format_version == 1");
    report(std::regex_match(m["zone"].as<std::string>(""), kZoneId),
           "zone is a valid zone id", m["zone"].as<std::string>(""));
    report(m["chunk_size_m"].as<int>(0) == 128, "chunk_size_m == 128");
    // Non-zero origin (contract: Zone-01 sits at x ≈ −300, not the world origin).
    const double ox = m["origin"]["x"].as<double>(0.0);
    const double oz = m["origin"]["z"].as<double>(0.0);
    report(ox != 0.0 && oz != 0.0, "origin is non-zero",
           "x=" + std::to_string(ox) + " z=" + std::to_string(oz));
    // Negative-index grid bounds (exercises the §3.1 negative-coordinate path).
    report(m["grid"]["min_cx"].as<int>(0) < 0 && m["grid"]["min_cz"].as<int>(0) < 0,
           "grid bounds span negative indices",
           "min_cx=" + std::to_string(m["grid"]["min_cx"].as<int>(0)));
    report(m["far_ring"].as<int>(-1) >= 0, "far_ring present");
    report(m["chunks"].IsSequence() && m["chunks"].size() == 9, "9 chunk entries");

    report(schema_required_ok(m), "every schema-required property is emitted (top-level + entry)");

    // Per-entry grammar: refs, hash, aabb, deps, priority.
    bool refs_ok = true, hash_ok = true, aabb_ok = true, deps_ok = true;
    for (const auto& e : m["chunks"]) {
        refs_ok &= std::regex_match(e["scene"].as<std::string>(""), kResourceRef);
        refs_ok &= std::regex_match(e["proxy"].as<std::string>(""), kResourceRef);
        refs_ok &= std::regex_match(e["server"].as<std::string>(""), kResourceRef);
        hash_ok &= std::regex_match(e["hash"].as<std::string>(""), kContentHash);
        aabb_ok &= (e["aabb"]["min"]["y"].IsDefined() && e["aabb"]["max"]["y"].IsDefined());
        for (const auto& d : e["deps"]) deps_ok &= std::regex_match(d.as<std::string>(""), kAssetId);
    }
    report(refs_ok, "every scene/proxy/server ref matches the resourceRef grammar (C2)");
    report(hash_ok, "every entry hash matches blake3:<64hex>");
    report(aabb_ok, "every entry carries an aabb with y bounds");
    report(deps_ok, "every dep matches the IF-8 assetId grammar");
}

// (b) FlatBuffer verify + non-flat heightfield + shared edge -----------------
void test_chunk_bin_flatbuffer() {
    std::cout << "test_chunk_bin_flatbuffer (ServerChunk 129x129 non-flat)\n";
    const mcc::stages::ChunkEmitResult res = run({});

    bool all_verify = true, all_side = true, all_nonflat = true, all_coord = true;
    for (const auto& rec : res.chunks) {
        const auto* buf = reinterpret_cast<const uint8_t*>(rec.chunk_bin.data());
        flatbuffers::Verifier v(buf, rec.chunk_bin.size());
        if (!meridian::chunk::VerifyServerChunkBuffer(v)) { all_verify = false; continue; }
        const auto* sc = meridian::chunk::GetServerChunk(buf);
        const auto* hf = sc->heightfield();
        all_side &= (hf && hf->side() == 129 && hf->samples() &&
                     hf->samples()->size() == 129u * 129u);
        all_coord &= (sc->coord() && sc->coord()->cx() == rec.cx && sc->coord()->cz() == rec.cz);
        if (hf && hf->samples()) {
            float lo = hf->samples()->Get(0), hi = lo;
            bool any_nan = false;
            for (flatbuffers::uoffset_t i = 0; i < hf->samples()->size(); ++i) {
                const float s = hf->samples()->Get(i);
                if (std::isnan(s)) any_nan = true;
                lo = std::min(lo, s);
                hi = std::max(hi, s);
            }
            // NON-FLAT: a real span between min and max, and no NaN sentinels in a
            // well-formed shipped chunk (chunk-payload.md hole-sentinel rule).
            all_nonflat &= (!any_nan && (hi - lo) > 0.5f);
        }
    }
    report(all_verify, "every .chunk.bin passes the FlatBuffers verifier");
    report(all_side, "every heightfield is 129x129 (16641 f32 samples)");
    report(all_coord, "every ServerChunk.coord matches its (cx,cz)");
    report(all_nonflat, "every heightfield is non-flat (span > 0.5 m, no NaN)");

    // Shared-edge convention (§3.2): the east column (x=128) of a chunk equals the
    // west column (x=0) of its +X neighbour, row for row.
    auto find = [&](int cx, int cz) -> const mcc::stages::ChunkRecord* {
        for (const auto& r : res.chunks) if (r.cx == cx && r.cz == cz) return &r;
        return nullptr;
    };
    const mcc::stages::ChunkRecord* left = find(-1, -1);
    const mcc::stages::ChunkRecord* right = find(0, -1);
    bool shared_edge = (left && right);
    if (shared_edge) {
        const auto* lsc = meridian::chunk::GetServerChunk(
            reinterpret_cast<const uint8_t*>(left->chunk_bin.data()));
        const auto* rsc = meridian::chunk::GetServerChunk(
            reinterpret_cast<const uint8_t*>(right->chunk_bin.data()));
        for (int z = 0; z < 129; ++z) {
            const float le = lsc->heightfield()->samples()->Get(z * 129 + 128);
            const float rw = rsc->heightfield()->samples()->Get(z * 129 + 0);
            if (le != rw) { shared_edge = false; break; }
        }
    }
    report(shared_edge, "adjacent chunks share their edge exactly (shared-edge convention)");
}

// ---- .tscn parsing helpers (independent of the stage's own writers) --------

// The stage renders reals as %.4f; mirror that to compare a formatted field.
std::string num_str(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return std::string(buf);
}

// Value of a `"key": <value>,` entry inside the _surfaces dict, unquoted/untrimmed.
std::string tscn_field(const std::string& scn, const std::string& key) {
    const std::string needle = "\"" + key + "\": ";
    const std::size_t at = scn.find(needle);
    if (at == std::string::npos) return "";
    const std::size_t start = at + needle.size();
    std::size_t end = scn.find('\n', start);
    if (end == std::string::npos) end = scn.size();
    std::string v = scn.substr(start, end - start);
    while (!v.empty() && (v.back() == ',' || v.back() == '\r')) v.pop_back();
    return v;
}

// The base64 payload inside `PackedByteArray("....")` for a given key.
std::string tscn_packed_bytes_b64(const std::string& scn, const std::string& key) {
    const std::string raw = tscn_field(scn, key);  // PackedByteArray("....")
    const std::size_t q1 = raw.find('"');
    const std::size_t q2 = raw.rfind('"');
    if (q1 == std::string::npos || q2 == std::string::npos || q2 <= q1) return "";
    return raw.substr(q1 + 1, q2 - q1 - 1);
}

// Independent base64 decoder — the test must not borrow the stage's encoder.
std::string b64_decode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (const char c : in) {
        const int v = val(c);
        if (v < 0) continue;  // '=' padding / whitespace
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buf >> bits) & 0xFF));
        }
    }
    return out;
}

// Bounds-checked readers: a scene MISSING the buffer entirely (e.g. the old
// BoxMesh emit) must make the checks FAIL cleanly, never crash the test binary.
float le_f32(const std::string& b, std::size_t off) {
    if (off + 4 > b.size()) return std::numeric_limits<float>::quiet_NaN();
    const std::uint32_t u = static_cast<std::uint8_t>(b[off]) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 1])) << 8) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 2])) << 16) |
                            (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[off + 3])) << 24);
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}

std::uint16_t le_u16(const std::string& b, std::size_t off) {
    if (off + 2 > b.size()) return 0xFFFF;  // out of range -> fails the range check
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[off]) |
                                      (static_cast<std::uint16_t>(static_cast<std::uint8_t>(b[off + 1])) << 8));
}

// (b2) the RENDERED scene is a real terrain ArrayMesh, not a BoxMesh ----------
//
// The regression this locks down (#875): chunk-emit used to render ONE BoxMesh per
// chunk, so mounting chunks drew giant boxes rather than terrain. The heightfield
// existed only in the server payload. These checks assert the .tscn now carries an
// ArrayMesh whose geometry IS the chunk's heightfield — decoded straight out of the
// emitted text and cross-checked against the .chunk.bin samples, so a mesh that
// merely *looks* structurally valid but doesn't follow the terrain still fails.
void test_scene_is_terrain_arraymesh() {
    std::cout << "test_scene_is_terrain_arraymesh (#875: real terrain mesh, not a BoxMesh)\n";
    const mcc::stages::ChunkEmitResult res = run({});
    report(!res.chunks.empty(), "have chunks to inspect");
    if (res.chunks.empty()) return;

    bool no_box = true, is_array = true, has_material = true, counts_ok = true;
    for (const auto& rec : res.chunks) {
        for (const std::string* scn : {&rec.scn, &rec.proxy_scn}) {
            no_box &= (scn->find("BoxMesh") == std::string::npos);
            is_array &= (scn->find("[sub_resource type=\"ArrayMesh\"") != std::string::npos);
            // A material SLOT the chunk can reference (T3 fills in the chibi look).
            has_material &= (scn->find("\"material\": SubResource(") != std::string::npos);
            has_material &= (scn->find("[sub_resource type=\"StandardMaterial3D\"") != std::string::npos);
        }
        // The full-detail surface is the heightfield 1:1; the proxy is decimated 4x.
        counts_ok &= (tscn_field(rec.scn, "vertex_count") == "16641");     // 129*129
        counts_ok &= (tscn_field(rec.scn, "index_count") == "98304");      // 128*128*2 tris
        counts_ok &= (tscn_field(rec.proxy_scn, "vertex_count") == "1089");  // 33*33
        counts_ok &= (tscn_field(rec.proxy_scn, "index_count") == "6144");   // 32*32*2 tris
    }
    report(no_box, "no chunk scene ships a BoxMesh any more (the #875 regression)");
    report(is_array, "every chunk scene ships an ArrayMesh sub-resource");
    report(has_material, "every surface carries a referenceable material slot");
    report(counts_ok, "full mesh = 129x129 verts / 32768 tris; proxy decimated to 33x33");

    // The Godot array-format bits the emitter writes. Byte-for-byte what Godot
    // 4.7-stable (client/ENGINE_VERSION) itself serialises for a
    // VERTEX|NORMAL|TANGENT|TEX_UV|INDEX surface: 4119 | (1 << 35). If an engine
    // bump changes the mesh array format, THIS is the tripwire — regenerate against
    // the new engine rather than hand-editing the constant.
    report(tscn_field(res.chunks[0].scn, "format") == "34359742487",
           "surface format matches Godot 4.7's own VERTEX|NORMAL|TANGENT|TEX_UV|INDEX bits",
           "got " + tscn_field(res.chunks[0].scn, "format"));
    report(tscn_field(res.chunks[0].scn, "primitive") == "3",
           "primitive == 3 (PRIMITIVE_TRIANGLES)");

    // ---- The geometry IS the heightfield -----------------------------------
    // Decode the vertex buffer and compare every vertex's Y against the SAME
    // sample in the emitted .chunk.bin. This is the check that would have caught
    // the box: a BoxMesh has no per-sample heights at all.
    const auto& rec = res.chunks[0];
    const auto* sc = meridian::chunk::GetServerChunk(
        reinterpret_cast<const uint8_t*>(rec.chunk_bin.data()));
    const auto* hf = sc->heightfield();

    const std::string vd = b64_decode(tscn_packed_bytes_b64(rec.scn, "vertex_data"));
    const std::size_t vcount = 16641;
    report(vd.size() == vcount * 12 + vcount * 8,
           "vertex_data size == 129*129 * (12B pos + 8B normal/tangent)",
           "got " + std::to_string(vd.size()));

    bool heights_match = true, xz_match = true;
    float mesh_lo = 1e30f, mesh_hi = -1e30f;
    std::size_t mismatches = 0;
    for (int lz = 0; lz < 129 && heights_match; ++lz) {
        for (int lx = 0; lx < 129; ++lx) {
            const std::size_t vi = static_cast<std::size_t>(lz) * 129 + lx;
            const float vx = le_f32(vd, vi * 12 + 0);
            const float vy = le_f32(vd, vi * 12 + 4);
            const float vz = le_f32(vd, vi * 12 + 8);
            // Mesh-local XZ: the node transform carries the chunk's world corner.
            if (vx != static_cast<float>(lx) || vz != static_cast<float>(lz)) xz_match = false;
            const float sample = hf->samples()->Get(static_cast<flatbuffers::uoffset_t>(vi));
            if (vy != sample) {
                if (++mismatches <= 3)
                    std::cout << "       height mismatch at (" << lx << "," << lz << "): mesh="
                              << vy << " heightfield=" << sample << "\n";
                heights_match = false;
            }
            mesh_lo = std::min(mesh_lo, vy);
            mesh_hi = std::max(mesh_hi, vy);
        }
    }
    report(xz_match, "every vertex sits on the 1 m heightfield lattice (mesh-local XZ)");
    report(heights_match, "EVERY vertex Y equals the .chunk.bin heightfield sample (exact)");

    // Sampled height: the mesh is genuinely non-flat and spans the same range the
    // manifest AABB advertises.
    report((mesh_hi - mesh_lo) > 0.5f, "mesh surface is non-flat (Y span > 0.5 m)",
           "span=" + std::to_string(mesh_hi - mesh_lo));
    report(mesh_lo == rec.min_y && mesh_hi == rec.max_y,
           "mesh Y span matches the chunk's manifest AABB Y bounds",
           "mesh=[" + std::to_string(mesh_lo) + "," + std::to_string(mesh_hi) + "] manifest=[" +
               std::to_string(rec.min_y) + "," + std::to_string(rec.max_y) + "]");

    // The surface's own declared AABB must bound the real geometry.
    const std::string aabb = tscn_field(rec.scn, "aabb");
    const std::string want_aabb = "AABB(0, " + num_str(rec.min_y) + ", 0, 128.0000, " +
                                  num_str(rec.max_y - rec.min_y) + ", 128.0000)";
    report(aabb == want_aabb, "surface aabb bounds the chunk footprint and the real height span",
           "got " + aabb + " want " + want_aabb);

    // Normals: an independent octahedron DECODE (mirroring Godot's
    // Vector3::octahedron_decode) must recover unit vectors that point up. A
    // mis-encoded normal block is the most likely silent breakage — the scene still
    // loads, the terrain just lights wrong — so decode rather than trust.
    bool normals_up = (vd.size() == vcount * 20);
    float worst_y = 1.0f;
    for (std::size_t i = 0; normals_up && i < vcount; ++i) {
        const std::size_t off = vcount * 12 + i * 8;
        const float fx = static_cast<float>(le_u16(vd, off + 0)) / 65535.0f * 2.0f - 1.0f;
        const float fy = static_cast<float>(le_u16(vd, off + 2)) / 65535.0f * 2.0f - 1.0f;
        float nx = fx, ny = fy, nz = 1.0f - std::fabs(fx) - std::fabs(fy);
        const float t = std::max(0.0f, std::min(-nz, 1.0f));
        nx += (nx >= 0.0f ? -t : t);
        ny += (ny >= 0.0f ? -t : t);
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        ny /= len;
        worst_y = std::min(worst_y, ny);
        // Gentle terrain: every normal is firmly world-up (+Y).
        if (!(ny > 0.9f)) normals_up = false;
    }
    report(normals_up, "every decoded octahedral normal is unit-length and points up (+Y)",
           "worst normal.y=" + std::to_string(worst_y));

    // Indices reference real vertices and form whole triangles.
    const std::string id = b64_decode(tscn_packed_bytes_b64(rec.scn, "index_data"));
    report(id.size() == 98304u * 2, "index_data is 98304 u16 indices",
           "got " + std::to_string(id.size()));
    // Non-empty guard: an absent index buffer must FAIL here, not pass vacuously
    // because the loop below never runs.
    bool idx_in_range = !id.empty() && (id.size() % 6 == 0);
    for (std::size_t i = 0; i + 1 < id.size(); i += 2) {
        if (le_u16(id, i) >= vcount) { idx_in_range = false; break; }
    }
    report(idx_in_range, "every index is in range (a closed, well-formed triangle list)");

    // WINDING — the terrain must FACE UP. Godot's front faces are clockwise: for an
    // up-facing surface the right-hand-rule normal (B-A)x(C-A) points DOWN (-Y).
    // (Ground truth: Godot's own PlaneMesh, declared normal +Y, winds exactly so.)
    // Get this wrong and the mesh is INVISIBLE — it loads fine and reads back
    // correct arrays, but backface culling drops every triangle for a camera above
    // it. Nothing else in this suite can see that, so assert it per triangle.
    bool winding_ok = !id.empty();
    std::size_t bad_tris = 0;
    const std::size_t nidx = id.size() / 2;
    for (std::size_t t = 0; t + 2 < nidx; t += 3) {
        const std::size_t ia = le_u16(id, (t + 0) * 2);
        const std::size_t ib = le_u16(id, (t + 1) * 2);
        const std::size_t ic = le_u16(id, (t + 2) * 2);
        if (ia >= vcount || ib >= vcount || ic >= vcount) { winding_ok = false; break; }
        const float ax = le_f32(vd, ia * 12 + 0), az = le_f32(vd, ia * 12 + 8);
        const float bx = le_f32(vd, ib * 12 + 0), bz = le_f32(vd, ib * 12 + 8);
        const float cx = le_f32(vd, ic * 12 + 0), cz = le_f32(vd, ic * 12 + 8);
        // Only the Y component of the cross product matters for facing.
        const float ux = bx - ax, uz = bz - az;
        const float wx = cx - ax, wz = cz - az;
        // (u x w).y — must be negative for an up-facing Godot triangle.
        const float ny_rh = uz * wx - ux * wz;
        if (!(ny_rh < 0.0f)) { ++bad_tris; winding_ok = false; }
    }
    report(winding_ok,
           "every triangle is wound to FACE UP (Godot front-face convention; a flipped "
           "winding renders the terrain invisible)",
           "wrong-facing triangles=" + std::to_string(bad_tris));

    // UVs span the chunk 0..1 so a ground material can tile over it.
    const std::string ad = b64_decode(tscn_packed_bytes_b64(rec.scn, "attribute_data"));
    report(ad.size() == vcount * 8, "attribute_data is one UV (2xf32) per vertex");
    report(le_f32(ad, 0) == 0.0f && le_f32(ad, 4) == 0.0f &&
               le_f32(ad, (vcount - 1) * 8) == 1.0f && le_f32(ad, (vcount - 1) * 8 + 4) == 1.0f,
           "UVs run (0,0) at the chunk's min corner to (1,1) at its max corner");
}

// (b3) the render mesh joins seamlessly across a shared chunk edge ------------
// Height is a pure function of world coords, so the east column of a chunk must
// equal the west column of its +X neighbour — for the MESH, not just the payload.
void test_mesh_shared_edge() {
    std::cout << "test_mesh_shared_edge (render mesh tiles without a seam)\n";
    const mcc::stages::ChunkEmitResult res = run({});
    auto find = [&](int cx, int cz) -> const mcc::stages::ChunkRecord* {
        for (const auto& r : res.chunks) if (r.cx == cx && r.cz == cz) return &r;
        return nullptr;
    };
    const auto* left = find(-1, -1);
    const auto* right = find(0, -1);
    report(left && right, "found the (-1,-1) and (0,-1) neighbours");
    if (!left || !right) return;

    const std::string lvd = b64_decode(tscn_packed_bytes_b64(left->scn, "vertex_data"));
    const std::string rvd = b64_decode(tscn_packed_bytes_b64(right->scn, "vertex_data"));
    const std::size_t vcount = 16641;

    // Both meshes must actually HAVE a full vertex buffer, or the comparisons below
    // would compare nothing and pass vacuously.
    bool edge_ok = (lvd.size() == vcount * 20) && (rvd.size() == vcount * 20);
    bool normals_ok = edge_ok;
    for (int lz = 0; edge_ok && lz < 129; ++lz) {
        // left's east column (lx=128) vs right's west column (lx=0)
        const std::size_t li = static_cast<std::size_t>(lz) * 129 + 128;
        const std::size_t ri = static_cast<std::size_t>(lz) * 129 + 0;
        if (le_f32(lvd, li * 12 + 4) != le_f32(rvd, ri * 12 + 4)) edge_ok = false;
        // Normals must agree too, or the terrain lights with a visible seam — this
        // is what the padded-field central differences buy.
        for (int k = 0; k < 4; ++k) {
            if (le_u16(lvd, vcount * 12 + li * 8 + k * 2) !=
                le_u16(rvd, vcount * 12 + ri * 8 + k * 2)) { normals_ok = false; break; }
        }
    }
    report(edge_ok, "shared-edge vertex heights match exactly across neighbouring meshes");
    report(normals_ok, "shared-edge NORMALS match exactly (no lighting seam)");
}

// (c) manifest hash is the real recomputed BLAKE3 over both payloads ---------
void test_hash_roundtrip() {
    std::cout << "test_hash_roundtrip (per-chunk BLAKE3 over both payloads)\n";
    const mcc::stages::ChunkEmitResult res = run({});
    const YAML::Node m = YAML::Load(res.manifest_json);

    bool all_match = true;
    for (std::size_t i = 0; i < res.chunks.size(); ++i) {
        const auto& rec = res.chunks[i];
        const std::string recomputed =
            recompute_entry_hash(rec.chunk_bin, rec.scn, rec.proxy_scn);
        const std::string manifest_hash = m["chunks"][i]["hash"].as<std::string>("");
        if (recomputed != rec.entry_hash || recomputed != manifest_hash) {
            all_match = false;
            std::cout << "       chunk " << rec.tag << " recomputed=" << recomputed
                      << " manifest=" << manifest_hash << "\n";
        }
    }
    report(all_match, "recomputed per-chunk hash matches the manifest (over BOTH payloads)");

    // A payload change MUST change the hash (the invalidation guarantee, walk C5).
    if (!res.chunks.empty()) {
        std::string tampered = res.chunks[0].chunk_bin;
        tampered[64] ^= 0x01;  // flip a byte in the heightfield region
        const std::string h1 =
            recompute_entry_hash(res.chunks[0].chunk_bin, res.chunks[0].scn, res.chunks[0].proxy_scn);
        const std::string h2 =
            recompute_entry_hash(tampered, res.chunks[0].scn, res.chunks[0].proxy_scn);
        report(h1 != h2, "a payload byte flip changes the entry hash");
    }
}

// (d) pack contains every referenced asset incl. the server chunk -----------
void test_pack_completeness() {
    std::cout << "test_pack_completeness (IF-5 pack covers every ref)\n";
    const mcc::stages::ChunkEmitResult res = run({});

    const YAML::Node pack = YAML::Load(res.pack_manifest_json);
    report(pack["schema"].as<std::string>("") == "meridian/pack-manifest@1",
           "pack declares meridian/pack-manifest@1");
    std::set<std::string> pack_ids;
    int server_entries = 0;
    for (const auto& e : pack["entries"]) {
        pack_ids.insert(e["id"].as<std::string>(""));
        if (e["type"].as<std::string>("") == "chunk_server") ++server_entries;
    }
    report(static_cast<int>(pack["entry_count"].as<int>(0)) ==
               static_cast<int>(pack["entries"].size()),
           "pack entry_count matches the entry list length");
    report(server_entries == static_cast<int>(res.chunks.size()),
           "one server .chunk.bin entry per chunk (Q1(a))",
           "server entries=" + std::to_string(server_entries));

    bool all_refs_present = true;
    for (const auto& rec : res.chunks) {
        for (const std::string& ref : {rec.scene_ref, rec.proxy_ref, rec.server_ref}) {
            if (!pack_ids.count(ref)) {
                all_refs_present = false;
                std::cout << "       missing pack entry for ref: " << ref << "\n";
            }
        }
    }
    report(all_refs_present, "every manifest scene/proxy/server ref resolves to a pack entry");

    // Client scenes ship as TEXT `.tscn` (not `.scn`): a text payload under a
    // `.scn` name is routed to Godot's BINARY loader and rejected (#579). Assert
    // the asset-table resource paths carry the loadable extension.
    auto ends_with = [](const std::string& s, const std::string& suf) {
        return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
    };
    bool ext_ok = true;
    for (const auto& a : res.assets) {
        if (a.type == "chunk_scene") ext_ok &= ends_with(a.res_path, ".tscn") && !ends_with(a.res_path, ".proxy.tscn");
        if (a.type == "chunk_proxy") ext_ok &= ends_with(a.res_path, ".proxy.tscn");
    }
    report(ext_ok, "chunk_scene -> .tscn and chunk_proxy -> .proxy.tscn (loadable text scenes, #579)");

    // The IF-8 asset table covers exactly the same ids with band-0 numeric ids.
    const YAML::Node assets = YAML::Load(res.assets_json);
    bool numeric_ok = !res.assets.empty();
    for (const auto& a : res.assets) numeric_ok &= (a.numeric_id != 0 && a.numeric_id == a.local_index);
    report(numeric_ok, "IF-8 asset table: band-0 numeric ids == local indices (non-zero)");
    report(static_cast<int>(assets["entry_count"].as<int>(-1)) ==
               static_cast<int>(res.assets.size()),
           "asset table entry_count matches");
}

// (e) determinism + disk round-trip ------------------------------------------
void test_determinism_and_disk() {
    std::cout << "test_determinism_and_disk\n";
    const mcc::stages::ChunkEmitResult a = run({});
    const mcc::stages::ChunkEmitResult b = run({});
    report(a.manifest_json == b.manifest_json, "manifest is byte-identical across runs");
    report(a.pack_manifest_json == b.pack_manifest_json, "pack manifest is byte-identical across runs");
    report(a.content_hash == b.content_hash, "pack content_hash is stable across runs");
    bool bins_equal = a.chunks.size() == b.chunks.size();
    for (std::size_t i = 0; bins_equal && i < a.chunks.size(); ++i)
        bins_equal &= (a.chunks[i].chunk_bin == b.chunks[i].chunk_bin);
    report(bins_equal, "every .chunk.bin is byte-identical across runs");

    // CLI wrapper writes the fixture to disk; the on-disk manifest == the result.
    const fs::path base =
        fs::temp_directory_path() / ("mcc_chunk_emit_test_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    std::ostringstream out, err;
    mcc::stages::ChunkEmitOptions opts;
    const int rc = mcc::stages::chunk_emit_run(opts, base.string(),
                                               mcc::stages::DiagFormat::Text, out, err);
    report(rc == 0, "chunk_emit_run to disk returns 0", err.str());

    const fs::path chunk_dir = base / "meridian" / "core" / "chunks" / "zone01";
    report(fs::exists(chunk_dir / "zone01.chunks.json"), "wrote zone01.chunks.json");
    report(fs::exists(chunk_dir / "zone01.assets.json"), "wrote zone01.assets.json");
    report(fs::exists(chunk_dir / "pack.manifest.json"), "wrote pack.manifest.json");
    // Every per-chunk artifact triple exists.
    bool triples = true;
    for (const auto& rec : a.chunks) {
        triples &= fs::exists(chunk_dir / (rec.tag + ".chunk.bin"));
        triples &= fs::exists(chunk_dir / (rec.tag + ".tscn"));
        triples &= fs::exists(chunk_dir / (rec.tag + ".proxy.tscn"));
    }
    report(triples, "wrote every per-chunk .chunk.bin/.tscn/.proxy.tscn");

    std::ifstream f(chunk_dir / "zone01.chunks.json", std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    report(ss.str() == a.manifest_json, "on-disk manifest matches the in-memory result");

    // The on-disk .chunk.bin also verifies as a ServerChunk (full disk round-trip).
    if (!a.chunks.empty()) {
        std::ifstream cf(chunk_dir / (a.chunks[0].tag + ".chunk.bin"), std::ios::binary);
        std::stringstream cbuf;
        cbuf << cf.rdbuf();
        const std::string bytes = cbuf.str();
        flatbuffers::Verifier v(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        report(meridian::chunk::VerifyServerChunkBuffer(v),
               "on-disk .chunk.bin passes the FlatBuffers verifier");
    }
    fs::remove_all(base, ec);
}

// (f) the `meadow` height profile + the Sprout Meadow emit (#876) ------------
//
// THE LOAD-BEARING TEST of epic #872's "zero server changes" premise. worldd's
// ground model is the CONSTANT plane z=0 (movement_validation.cpp:45
// `ground_sample() -> kFlatGroundZ`) and R5 rejects any move whose
// |z - ground| exceeds kHeightTolerance = 4.0 m (movement_constants.h:202).
// The client walks the real heightfield, so every metre of terrain height is a
// metre of R5 error budget spent. A meadow authored within +-3 m of z=0 passes
// R5 today, unmodified, with 1 m of headroom for jumps/slope-follow jitter.
// If this assertion ever fails, players standing on the meadow get their moves
// REJECTED — the epic would then need the (much larger) server-side chunk
// ingestion work of epic #874 before it could ship.
void test_meadow_profile_height_budget() {
    std::cout << "test_meadow_profile_height_budget (the +-3 m R5 budget, #876)\n";
    const mcc::stages::ChunkEmitResult res = run(meadow_opts());
    report(res.ok, "meadow emit reports ok");
    report(res.chunks.size() == 9, "3x3 Sprout Meadow grid -> 9 chunks",
           "got " + std::to_string(res.chunks.size()));
    if (res.chunks.empty()) return;

    // Mirror of the server's own numbers, asserted here rather than assumed.
    constexpr float kServerHeightTolerance = 4.0f;  // movement_constants.h:202
    constexpr float kServerFlatGroundZ = 0.0f;      // movement_constants.h:233
    constexpr float kBudget = 3.0f;                 // the authored budget (#876)
    static_assert(kBudget < kServerHeightTolerance, "meadow budget must fit inside R5");

    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();
    bool any_nan = false;
    bool all_decoded = true;
    // Steepest 1 m step anywhere in the field — "gentle rolling hills", not spikes.
    float max_step = 0.0f;
    for (const auto& rec : res.chunks) {
        const std::vector<float> h = heightfield_of(rec.chunk_bin);
        if (h.size() != 129u * 129u) { all_decoded = false; continue; }
        for (int lz = 0; lz < 129; ++lz) {
            for (int lx = 0; lx < 129; ++lx) {
                const float s = h[static_cast<std::size_t>(lz) * 129 + lx];
                if (std::isnan(s)) any_nan = true;
                lo = std::min(lo, s);
                hi = std::max(hi, s);
                if (lx > 0)
                    max_step = std::max(max_step, std::fabs(s - h[static_cast<std::size_t>(lz) * 129 + (lx - 1)]));
                if (lz > 0)
                    max_step = std::max(max_step, std::fabs(s - h[static_cast<std::size_t>(lz - 1) * 129 + lx]));
            }
        }
    }
    report(all_decoded, "every meadow .chunk.bin verifies with a 129x129 heightfield");
    report(!any_nan, "no NaN samples in the meadow heightfield");

    const std::string span = "min=" + std::to_string(lo) + " max=" + std::to_string(hi);
    // ⛔ THE CONSTRAINT. Both bounds, against the server's own ground plane.
    report(std::fabs(lo - kServerFlatGroundZ) <= kBudget &&
               std::fabs(hi - kServerFlatGroundZ) <= kBudget,
           "EVERY meadow sample is within +-3 m of z=0 (R5 passes with no server change)", span);
    report(std::fabs(lo) <= kServerHeightTolerance && std::fabs(hi) <= kServerHeightTolerance,
           "meadow stays inside the server's R5 kHeightTolerance (4.0 m) with headroom", span);

    // Gentle + genuinely rolling: real hills AND real hollows about z=0, no cliffs.
    report((hi - lo) > 2.0f, "meadow is genuinely non-flat (span > 2 m)", span);
    report(lo < -0.5f && hi > 0.5f, "meadow rolls both above and below z=0 (centred, not offset)",
           span);
    report(max_step < 0.5f, "no 1 m step exceeds 0.5 m (gentle rolling hills, not spikes)",
           "max_step=" + std::to_string(max_step));

    // The AABB the manifest publishes must reflect those same real bounds.
    bool aabb_ok = true;
    for (const auto& rec : res.chunks)
        aabb_ok &= (rec.min_y >= -kBudget && rec.max_y <= kBudget && rec.min_y <= rec.max_y);
    report(aabb_ok, "every chunk's published AABB y-range sits inside the +-3 m budget");
}

// The Sprout Meadow emit itself: right zone, right geometry, valid manifest,
// deterministic, seam-free — and the zone01 fixture profile is untouched.
void test_meadow_emit_sprout_meadow() {
    std::cout << "test_meadow_emit_sprout_meadow (chibi:zone.sprout_meadow, #876)\n";
    const mcc::stages::ChunkEmitResult res = run(meadow_opts());
    if (!res.ok || res.chunks.empty()) { report(false, "meadow emit produced chunks"); return; }

    report(res.zone == "chibi:zone.sprout_meadow" && res.ns == "chibi" &&
               res.zone_bare == "sprout_meadow",
           "emits under the chibi namespace as zone sprout_meadow",
           res.ns + " / " + res.zone_bare);

    YAML::Node m;
    bool parsed = true;
    try { m = YAML::Load(res.manifest_json); } catch (const std::exception&) { parsed = false; }
    report(parsed && m.IsMap(), "meadow manifest parses");
    if (!parsed) return;
    report(std::regex_match(m["zone"].as<std::string>(""), kZoneId), "manifest zone matches the zone-id grammar");
    report(schema_required_ok(m), "meadow manifest carries every schema-required property");
    report(m["chunks"].IsSequence() && m["chunks"].size() == 9, "9 meadow chunk entries");

    // Authored at zone01's EXISTING geometry so spawn/bounds need no change: a
    // 3x3 grid at origin -384 spans zone-local [-512, -128] on both axes, which
    // covers the server's spawn placeholder kZoneSpawnXY = (-320, -320).
    report(m["origin"]["x"].as<double>(0) == -384.0 && m["origin"]["z"].as<double>(0) == -384.0,
           "origin is -384 (zone01's geometry — spawn/bounds unchanged)");
    report(m["grid"]["min_cx"].as<int>(0) == -1 && m["grid"]["max_cx"].as<int>(0) == 1 &&
               m["grid"]["min_cz"].as<int>(0) == -1 && m["grid"]["max_cz"].as<int>(0) == 1,
           "grid indices span [-1,1] on both axes");
    report(m["chunk_size_m"].as<int>(0) == 128, "chunk_size_m == 128");

    // The play area around spawn (-320,-320) is covered by a real chunk.
    const double gmin = -384.0 + (-1) * 128.0;  // -512
    const double gmax = gmin + 3 * 128.0;       // -128
    report(gmin <= -320.0 && -320.0 <= gmax,
           "the emitted grid covers the spawn point (-320,-320)",
           "grid spans [" + std::to_string(gmin) + ", " + std::to_string(gmax) + "]");

    bool hash_ok = true, deps_ok = true;
    for (const auto& e : m["chunks"]) {
        hash_ok &= std::regex_match(e["hash"].as<std::string>(""), kContentHash);
        for (const auto& d : e["deps"]) deps_ok &= std::regex_match(d.as<std::string>(""), kAssetId);
    }
    report(hash_ok, "every meadow entry hash matches blake3:<64hex>");
    report(deps_ok, "meadow deps (chibi:art.terrain.sprout_meadow_ground) match the assetId grammar");

    // Determinism: re-emitting the meadow is byte-identical (pack verification).
    const mcc::stages::ChunkEmitResult again = run(meadow_opts());
    bool bytes_equal = again.manifest_json == res.manifest_json &&
                       again.pack_manifest_json == res.pack_manifest_json &&
                       again.content_hash == res.content_hash &&
                       again.chunks.size() == res.chunks.size();
    for (std::size_t i = 0; bytes_equal && i < res.chunks.size(); ++i)
        bytes_equal &= (again.chunks[i].chunk_bin == res.chunks[i].chunk_bin &&
                        again.chunks[i].scn == res.chunks[i].scn &&
                        again.chunks[i].proxy_scn == res.chunks[i].proxy_scn);
    report(bytes_equal, "re-emitting the meadow is byte-identical (deterministic)");

    // Seam-free: height is a pure function of world coords, so a chunk's east
    // edge column must equal its neighbour's west edge column EXACTLY.
    const mcc::stages::ChunkRecord* left = nullptr;
    const mcc::stages::ChunkRecord* right = nullptr;
    for (const auto& rec : res.chunks) {
        if (rec.cx == -1 && rec.cz == 0) left = &rec;
        if (rec.cx == 0 && rec.cz == 0) right = &rec;
    }
    report(left && right, "found the (-1,0) and (0,0) meadow neighbours");
    if (left && right) {
        const std::vector<float> lh = heightfield_of(left->chunk_bin);
        const std::vector<float> rh = heightfield_of(right->chunk_bin);
        bool edge_ok = lh.size() == 129u * 129u && rh.size() == 129u * 129u;
        for (int lz = 0; edge_ok && lz < 129; ++lz)
            edge_ok &= (lh[static_cast<std::size_t>(lz) * 129 + 128] ==
                        rh[static_cast<std::size_t>(lz) * 129 + 0]);
        report(edge_ok, "meadow chunks share their edge samples exactly (no seam)");
    }

    // The profile is OPT-IN: the default zone01 fixture emit is byte-for-byte
    // unchanged by this story (its golden/staged pack must not move).
    mcc::stages::ChunkEmitOptions dflt;
    report(dflt.profile == mcc::stages::HeightProfile::Fixture,
           "the default height profile is still `fixture` (zone01 output unchanged)");
    const mcc::stages::ChunkEmitResult fixture = run({});
    bool differs = !fixture.chunks.empty() && !res.chunks.empty() &&
                   fixture.chunks[0].chunk_bin != res.chunks[0].chunk_bin;
    report(differs, "the meadow profile actually produces a different surface than `fixture`");

    // And the fixture profile still blows the R5 budget (it was never authored to
    // fit) — which is exactly why `meadow` had to exist rather than reusing it.
    const std::vector<float> fh = heightfield_of(fixture.chunks[0].chunk_bin);
    bool fixture_exceeds = false;
    for (const float s : fh) if (std::fabs(s) > 3.0f) fixture_exceeds = true;
    report(fixture_exceeds,
           "sanity: the `fixture` profile does NOT fit the +-3 m budget (why `meadow` exists)");
}

// ---- #877 T3: the meadow's SURFACE STYLE (mesh stride + ground material) -----
//
// Locks in the two decisions T3 made before staging, because the staged pack is
// policed byte-for-byte: changing either silently would churn committed data.
void test_meadow_surface_style() {
    std::cout << "test_meadow_surface_style (#877: stride-2 render mesh + chibi ground)\n";
    const mcc::stages::ChunkEmitResult res = run(meadow_opts());
    if (!res.ok || res.chunks.empty()) { report(false, "meadow emit produced chunks"); return; }

    // ---- The stride decision (#877, deferred from #875) ---------------------
    // meadow renders at stride 2 (65x65 verts / 8192 tris); the proxy stays at
    // stride 4 (33x33), so the far-ring LOD remains genuinely coarser than the
    // full mesh rather than collapsing into a copy of it.
    bool counts_ok = true, proxy_ok = true;
    for (const auto& rec : res.chunks) {
        counts_ok &= (tscn_field(rec.scn, "vertex_count") == "4225");    // 65*65
        counts_ok &= (tscn_field(rec.scn, "index_count") == "24576");    // 64*64*2 tris
        proxy_ok &= (tscn_field(rec.proxy_scn, "vertex_count") == "1089");  // 33*33
        proxy_ok &= (tscn_field(rec.proxy_scn, "index_count") == "6144");   // 32*32*2 tris
    }
    report(counts_ok, "every meadow chunk renders at stride 2 (65x65 verts, 8192 tris)",
           "got " + tscn_field(res.chunks[0].scn, "vertex_count") + " verts");
    report(proxy_ok, "the proxy stays 4x-decimated (33x33) — a real coarser LOD, not a copy");
    report(res.chunks[0].scn != res.chunks[0].proxy_scn,
           "full mesh and proxy are genuinely different payloads");

    // ---- The chibi ground material -----------------------------------------
    // Flat + bright + non-shiny: the documented chibi look ("flat body colors").
    bool mat_ok = true;
    for (const auto& rec : res.chunks) {
        for (const std::string* scn : {&rec.scn, &rec.proxy_scn}) {
            mat_ok &= (scn->find("[sub_resource type=\"StandardMaterial3D\" id=\"TerrainGround\"]")
                       != std::string::npos);
            // Keyed to the shared IF-8 terrain-ground dep id (the T1 slot contract).
            mat_ok &= (scn->find("resource_name = \"chibi:art.terrain.sprout_meadow_ground\"")
                       != std::string::npos);
            mat_ok &= (scn->find("albedo_color = Color(0.4900, 0.7800, 0.3100, 1.0000)")
                       != std::string::npos);
            mat_ok &= (scn->find("metallic_specular = 0.0000") != std::string::npos);
            mat_ok &= (scn->find("\"material\": SubResource(\"TerrainGround\")") != std::string::npos);
            // The material is EMBEDDED, never an ext_resource: the shared ground dep
            // has no on-disk payload, and a dangling ext_resource would fail the load.
            mat_ok &= (scn->find("[ext_resource") == std::string::npos);
        }
    }
    report(mat_ok, "every meadow surface carries the embedded chibi ground material "
                   "(bright flat green, specular killed), keyed to the shared dep id");

    // The fixture profile keeps the neutral grey — T3 moved ONLY the meadow.
    const mcc::stages::ChunkEmitResult fixture = run({});
    report(!fixture.chunks.empty() &&
               fixture.chunks[0].scn.find("albedo_color = Color(0.5000, 0.5000, 0.5000, 1.0000)")
                   != std::string::npos &&
               fixture.chunks[0].scn.find("metallic_specular") == std::string::npos,
           "the `fixture` profile keeps its neutral grey and omits the default-valued "
           "metallic_specular (its bytes are unchanged by #877)");
    report(!fixture.chunks.empty() &&
               tscn_field(fixture.chunks[0].scn, "vertex_count") == "16641",
           "the `fixture` profile still renders the heightfield 1:1 at stride 1 (129x129)");

    // ---- The fidelity the stride decision actually bought --------------------
    // Stride is a RENDER-only knob: the .chunk.bin heightfield the client
    // ground-samples stays full 129x129. So the deviation between the drawn surface
    // and the sampled truth IS the visible float/sink budget. Recompute it from the
    // EMITTED bytes (decode the mesh, compare against the emitted heightfield at the
    // lattice points the mesh dropped) rather than trusting the design note.
    const auto& rec = res.chunks[0];
    const std::vector<float> hf = heightfield_of(rec.chunk_bin);
    report(hf.size() == 129u * 129u,
           "the meadow .chunk.bin heightfield is STILL full-res 129x129 at stride 2 "
           "(ground-sample fidelity is stride-invariant)",
           "got " + std::to_string(hf.size()) + " samples");

    const std::string vd = b64_decode(tscn_packed_bytes_b64(rec.scn, "vertex_data"));
    constexpr int kStride = 2;
    constexpr int kN = 65;  // (129-1)/2 + 1
    bool on_lattice = true;
    float max_dev = 0.0f;
    if (vd.size() >= static_cast<std::size_t>(kN) * kN * 12) {
        // Every retained vertex must sit EXACTLY on its heightfield sample.
        for (int j = 0; j < kN && on_lattice; ++j) {
            for (int i = 0; i < kN; ++i) {
                const std::size_t vi = static_cast<std::size_t>(j) * kN + i;
                const float vy = le_f32(vd, vi * 12 + 4);
                const float s = hf[static_cast<std::size_t>(j * kStride) * 129 + (i * kStride)];
                if (vy != s) { on_lattice = false; break; }
            }
        }
        // At the samples the mesh DROPPED (odd lattice points), the drawn surface is
        // the edge midpoint — measure how far that is from the true height.
        for (int j = 0; j < 129; j += kStride) {
            for (int i = 1; i < 128; i += kStride) {
                const float lo = hf[static_cast<std::size_t>(j) * 129 + (i - 1)];
                const float hi = hf[static_cast<std::size_t>(j) * 129 + (i + 1)];
                const float truth = hf[static_cast<std::size_t>(j) * 129 + i];
                max_dev = std::max(max_dev, std::fabs((lo + hi) * 0.5f - truth));
            }
        }
    }
    report(on_lattice, "every retained stride-2 vertex still sits exactly on its "
                       "heightfield sample (decimation drops samples, never moves them)");
    // The measured worst case across the grid is ~7 mm; 5 cm is a generous ceiling
    // that still fails loudly if someone coarsens the stride without re-deciding.
    report(max_dev < 0.05f,
           "the drawn stride-2 surface stays within 5 cm of the sampled heightfield "
           "(imperceptible float/sink; measured ~7 mm)",
           "max_dev=" + std::to_string(max_dev) + " m");
}

}  // namespace

int main() {
    std::cout << "mcc chunk-emit (IF-6 procedural Zone-01 fixture) round-trip tests\n\n";
    test_manifest_wellformed();
    test_chunk_bin_flatbuffer();
    test_scene_is_terrain_arraymesh();
    test_mesh_shared_edge();
    test_hash_roundtrip();
    test_pack_completeness();
    test_determinism_and_disk();
    test_meadow_profile_height_budget();
    test_meadow_emit_sprout_meadow();
    test_meadow_surface_style();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cout << "FAIL: " << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
