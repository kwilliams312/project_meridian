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
#include <filesystem>
#include <fstream>
#include <iostream>
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

    // Cross-check against the REAL schema's `required` lists (top-level + entry).
    bool schema_ok = true;
    try {
        const YAML::Node schema = YAML::LoadFile(MCC_CHUNK_MANIFEST_SCHEMA);
        for (const auto& req : schema["required"]) {
            const std::string key = req.as<std::string>();
            if (!m[key]) {
                schema_ok = false;
                std::cout << "       missing top-level required: " << key << "\n";
            }
        }
        const YAML::Node entry_req = schema["$defs"]["chunkEntry"]["required"];
        const YAML::Node e0 = m["chunks"][0];
        for (const auto& req : entry_req) {
            const std::string key = req.as<std::string>();
            if (!e0[key]) {
                schema_ok = false;
                std::cout << "       missing chunkEntry required: " << key << "\n";
            }
        }
    } catch (const std::exception& ex) {
        schema_ok = false;
        std::cout << "       schema load failed: " << ex.what() << "\n";
    }
    report(schema_ok, "every schema-required property is emitted (top-level + entry)");

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
        triples &= fs::exists(chunk_dir / (rec.tag + ".scn"));
        triples &= fs::exists(chunk_dir / (rec.tag + ".proxy.scn"));
    }
    report(triples, "wrote every per-chunk .chunk.bin/.scn/.proxy.scn");

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

}  // namespace

int main() {
    std::cout << "mcc chunk-emit (IF-6 procedural Zone-01 fixture) round-trip tests\n\n";
    test_manifest_wellformed();
    test_chunk_bin_flatbuffer();
    test_hash_roundtrip();
    test_pack_completeness();
    test_determinism_and_disk();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    if (g_failures) {
        std::cout << "FAIL: " << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}
