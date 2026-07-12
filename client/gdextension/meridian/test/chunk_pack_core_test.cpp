// Project Meridian — engine-free unit + golden test for the client IF-6/IF-8
// CHUNK-PACK verify core (issue #554, Epic #22 Story A). NO Godot: compiles
// against the plain-C++ core (chunk_pack_core.*) + the vendored mcc BLAKE3, so it
// runs in any C++17 toolchain (Client SAD §9.2). Plain-main style, mirroring the
// pack-manifest / movement tests.
//
// Golden anchor: the CHECKED-IN Story-0 fixture pack (test/fixtures/chunkpack/,
// byte-for-byte `mcc chunk-emit`) — the verifier and the mcc emitter are proven
// to agree on the exact IF-6 manifest + IF-8 asset table + the per-chunk
// both-payloads BLAKE3 framing.
//
// Proves the #554 fail-closed matrix:
//   (a) the real emitted pack -> kOk, all 9 chunks present + intact
//   (b) parse+resolve fills the in-memory index (coords incl. NEGATIVE, aabb,
//       priority, resolved res:// paths, deps)
//   (c) recompute_chunk_hash matches the manifest hash (BLAKE3 reuse + framing)
//   (d) FAIL CLOSED on a corrupted payload (one byte flipped)   -> kHashMismatch
//   (e) FAIL CLOSED on a missing referenced payload (file deleted) -> kMissingAsset
//   (f) FAIL CLOSED on an unresolvable ref (dropped from the table) -> kUnresolvedRef
//   (g) FAIL CLOSED on a malformed manifest (truncated)         -> kManifestMalformed
//
// Exit code 0 = all pass; non-zero = at least one failure.

#include "chunk_pack_core.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace cp = meridian::chunkpack;
namespace fs = std::filesystem;

static int g_fail = 0;
static int g_checks = 0;

static void check(const char *name, bool ok, const std::string &detail = "") {
	++g_checks;
	std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok) {
		++g_fail;
		if (!detail.empty()) std::printf("        %s\n", detail.c_str());
	}
}

// ── Fixture location (relative to THIS source, runnable from any build dir) ────
static std::string this_dir() {
	std::string f = __FILE__;
	const std::size_t slash = f.find_last_of("/\\");
	return (slash == std::string::npos) ? std::string(".") : f.substr(0, slash);
}
static std::string fixture_root() {
	// The pack root the res:// paths are relative to (res://meridian/... maps here).
	return this_dir() + "/fixtures/chunkpack";
}
static std::string zone_dir() {
	return fixture_root() + "/meridian/core/chunks/zone01";
}

static bool read_file(const std::string &path, std::string &out) {
	std::ifstream in(path, std::ios::binary);
	if (!in) return false;
	std::ostringstream ss;
	ss << in.rdbuf();
	out = ss.str();
	return true;
}

// ── res:// -> filesystem provider, rooted at a pack directory ─────────────────
struct FsFileProvider final : cp::IPackFileProvider {
	std::string root;
	explicit FsFileProvider(std::string r) : root(std::move(r)) {}

	std::string to_fs(const std::string &res_path) const {
		static const std::string kScheme = "res://";
		std::string rel = res_path;
		if (rel.compare(0, kScheme.size(), kScheme) == 0) {
			rel = rel.substr(kScheme.size());
		}
		return root + "/" + rel;
	}
	bool exists(const std::string &res_path) const override {
		std::error_code ec;
		return fs::exists(to_fs(res_path), ec);
	}
	bool read(const std::string &res_path, std::string &out) const override {
		return read_file(to_fs(res_path), out);
	}
};

// Recursively copy the fixture pack into a unique temp dir so a test can mutate
// it (flip a byte / delete a file) without touching the checked-in golden.
static std::string make_temp_copy(const std::string &tag) {
	std::error_code ec;
	const auto uniq = std::chrono::steady_clock::now().time_since_epoch().count();
	const fs::path dst = fs::temp_directory_path(ec) /
	                     ("meridian_chunkpack_" + tag + "_" + std::to_string(uniq));
	fs::remove_all(dst, ec);
	fs::create_directories(dst, ec);
	fs::copy(fixture_root(), dst, fs::copy_options::recursive, ec);
	if (ec) return std::string();
	return dst.string();
}

int main() {
	std::printf("chunk_pack_core_test — IF-6/IF-8 fail-closed packaging (#554)\n");

	std::string chunks_json, assets_json;
	const bool got_chunks = read_file(zone_dir() + "/zone01.chunks.json", chunks_json);
	const bool got_assets = read_file(zone_dir() + "/zone01.assets.json", assets_json);
	check("fixture: zone01.chunks.json present", got_chunks, zone_dir());
	check("fixture: zone01.assets.json present", got_assets, zone_dir());
	if (!got_chunks || !got_assets) {
		std::printf("\nFATAL: fixture missing — cannot run.\n");
		return 1;
	}

	// ── (b) parse + resolve fills the in-memory index ─────────────────────────
	{
		const cp::ChunkPackReport r = cp::build_and_resolve(chunks_json, assets_json);
		check("resolve: build_and_resolve -> ok", r.ok,
		      cp::chunk_pack_verdict_name(r.verdict) + std::string(" / ") + r.reason);
		check("resolve: format_version == 1", r.index.format_version == 1);
		check("resolve: zone == core:zone.zone01", r.index.zone == "core:zone.zone01",
		      r.index.zone);
		check("resolve: chunk_size_m == 128", r.index.chunk_size_m == 128);
		check("resolve: origin == (-384,-384)",
		      r.index.origin.x == -384.0 && r.index.origin.z == -384.0);
		check("resolve: grid == [-1..1]",
		      r.index.min_cx == -1 && r.index.min_cz == -1 &&
		          r.index.max_cx == 1 && r.index.max_cz == 1);
		check("resolve: 9 chunks", r.index.chunks.size() == 9,
		      std::to_string(r.index.chunks.size()));

		// Negative coords exercised, and every ref resolved to a real res:// path.
		bool saw_negative = false;
		bool paths_ok = true;
		bool deps_ok = true;
		for (const cp::ResolvedChunk &c : r.index.chunks) {
			if (c.cx < 0 || c.cz < 0) saw_negative = true;
			if (c.server.res_path.find("res://meridian/core/chunks/zone01/") != 0 ||
			    c.server.res_path.rfind(".chunk.bin") == std::string::npos) {
				paths_ok = false;
			}
			if (c.scene.res_path.rfind(".scn") == std::string::npos) paths_ok = false;
			if (!c.has_proxy || c.proxy.res_path.rfind(".proxy.scn") == std::string::npos) {
				paths_ok = false;
			}
			// Every chunk prefetches the one shared ground dep (resolves in the table).
			if (c.deps.size() != 1 || c.deps[0].ref != "core:art.terrain.zone01_ground") {
				deps_ok = false;
			}
			if (!c.has_priority) paths_ok = false;  // fixture stamps a priority on all
		}
		check("resolve: negative chunk coords present", saw_negative);
		check("resolve: server .chunk.bin / scene .scn / proxy .proxy.scn paths resolved",
		      paths_ok);
		check("resolve: shared ground dep resolved on every chunk", deps_ok);
	}

	// ── (c) recompute_chunk_hash matches the manifest (BLAKE3 reuse + framing) ─
	{
		std::string sv, sc, px;
		const bool ok = read_file(zone_dir() + "/n1_n1.chunk.bin", sv) &&
		                read_file(zone_dir() + "/n1_n1.scn", sc) &&
		                read_file(zone_dir() + "/n1_n1.proxy.scn", px);
		check("hash: read chunk (-1,-1) payloads", ok);
		const std::string h = cp::recompute_chunk_hash(sv, sc, px);
		check("hash: recomputed == manifest golden for (-1,-1)",
		      h == "blake3:32cd7902da378b811051c7e1589c8d492e30ec5bfdd377ddbea857e522247c77",
		      h);
	}

	// ── (a) HAPPY PATH: full verify against the real pack -> kOk, all intact ───
	{
		const FsFileProvider files(fixture_root());
		const cp::ChunkPackReport r = cp::verify_chunk_pack(chunks_json, assets_json, files);
		check("verify: happy path -> kOk", r.verdict == cp::ChunkPackVerdict::kOk,
		      cp::chunk_pack_verdict_name(r.verdict) + std::string(" / ") + r.reason);
		check("verify: ok flag true + hard_fail false", r.ok && !r.hard_fail);
		check("verify: verified_chunks == chunk_count == 9",
		      r.verified_chunks == 9 && r.chunk_count == 9,
		      std::to_string(r.verified_chunks) + "/" + std::to_string(r.chunk_count));
	}

	// ── (d) FAIL CLOSED: corrupt one payload byte -> kHashMismatch ────────────
	{
		const std::string tmp = make_temp_copy("corrupt");
		check("corrupt: temp copy made", !tmp.empty(), tmp);
		if (!tmp.empty()) {
			// Flip one byte in a server payload.
			const std::string victim = tmp + "/meridian/core/chunks/zone01/0_0.chunk.bin";
			std::string bytes;
			read_file(victim, bytes);
			check("corrupt: victim non-empty", !bytes.empty());
			bytes[bytes.size() / 2] ^= 0x01;
			{ std::ofstream o(victim, std::ios::binary | std::ios::trunc); o.write(bytes.data(),
			  static_cast<std::streamsize>(bytes.size())); }

			const FsFileProvider files(tmp);
			const cp::ChunkPackReport r = cp::verify_chunk_pack(chunks_json, assets_json, files);
			check("corrupt: verdict == kHashMismatch",
			      r.verdict == cp::ChunkPackVerdict::kHashMismatch,
			      cp::chunk_pack_verdict_name(r.verdict) + std::string(" / ") + r.reason);
			check("corrupt: hard_fail true, ok false", r.hard_fail && !r.ok);
			check("corrupt: reason names the offending chunk (0,0)",
			      r.reason.find("(0,0)") != std::string::npos, r.reason);
			std::error_code ec; fs::remove_all(tmp, ec);
		}
	}

	// ── (e) FAIL CLOSED: delete a referenced payload -> kMissingAsset ─────────
	{
		const std::string tmp = make_temp_copy("missing");
		check("missing: temp copy made", !tmp.empty(), tmp);
		if (!tmp.empty()) {
			std::error_code ec;
			const std::string victim = tmp + "/meridian/core/chunks/zone01/0_0.scn";
			fs::remove(victim, ec);
			check("missing: victim deleted", !fs::exists(victim, ec));

			const FsFileProvider files(tmp);
			const cp::ChunkPackReport r = cp::verify_chunk_pack(chunks_json, assets_json, files);
			check("missing: verdict == kMissingAsset",
			      r.verdict == cp::ChunkPackVerdict::kMissingAsset,
			      cp::chunk_pack_verdict_name(r.verdict) + std::string(" / ") + r.reason);
			check("missing: hard_fail true, ok false", r.hard_fail && !r.ok);
			check("missing: reason names the missing path",
			      r.reason.find("0_0.scn") != std::string::npos, r.reason);
			fs::remove_all(tmp, ec);
		}
	}

	// ── (f) FAIL CLOSED: an unresolvable ref (dropped from the table) ─────────
	{
		// Break the id of one asset entry so chunk (0,0)'s server ref no longer
		// resolves — the manifest still references the original id.
		std::string broken_assets = assets_json;
		const std::string id = "core:chunk.zone01.0_0.server";
		const std::size_t at = broken_assets.find("\"" + id + "\"");
		check("unresolved: asset id present to mutate", at != std::string::npos);
		if (at != std::string::npos) {
			broken_assets.replace(at, id.size() + 2, "\"" + id + "_GONE\"");
			const FsFileProvider files(fixture_root());
			const cp::ChunkPackReport r = cp::verify_chunk_pack(chunks_json, broken_assets, files);
			check("unresolved: verdict == kUnresolvedRef",
			      r.verdict == cp::ChunkPackVerdict::kUnresolvedRef,
			      cp::chunk_pack_verdict_name(r.verdict) + std::string(" / ") + r.reason);
			check("unresolved: hard_fail true, ok false", r.hard_fail && !r.ok);
			check("unresolved: reason names the dangling ref",
			      r.reason.find(id) != std::string::npos, r.reason);
		}
	}

	// ── (g) FAIL CLOSED: a malformed (truncated) manifest -> kManifestMalformed ─
	{
		const std::string truncated = chunks_json.substr(0, 120);  // cut mid-structure
		const FsFileProvider files(fixture_root());
		const cp::ChunkPackReport r = cp::verify_chunk_pack(truncated, assets_json, files);
		check("malformed: verdict == kManifestMalformed",
		      r.verdict == cp::ChunkPackVerdict::kManifestMalformed,
		      cp::chunk_pack_verdict_name(r.verdict) + std::string(" / ") + r.reason);
		check("malformed: hard_fail true, ok false", r.hard_fail && !r.ok);
	}

	std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
	return g_fail == 0 ? 0 : 1;
}
