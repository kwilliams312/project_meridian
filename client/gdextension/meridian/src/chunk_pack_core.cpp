// Project Meridian — engine-free client CHUNK-PACK verify core (issue #554).
// See chunk_pack_core.h for the full fail-closed contract. This file holds a
// small, strict, dependency-free JSON reader (the repo's hand-rolled discipline,
// same as pack_manifest_core.* / the telemetry cores — no third-party JSON lib),
// the parse+resolve stage, and the presence+integrity verify stage. The per-
// chunk integrity hash REUSES the vendored mcc BLAKE3 (tools/mcc/src/hash) —
// there is no second BLAKE3 in the tree (issue directive).

#include "chunk_pack_core.h"

#include "hash/blake3.h"  // tools/mcc/src/hash — the ONE vendored BLAKE3 (reused)

#include <cctype>
#include <cstdlib>
#include <map>
#include <utility>

namespace meridian::chunkpack {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// A minimal, strict, recursive-descent JSON value model + parser. Supports the
// subset the two fixture files use — objects, arrays, strings, numbers, `true`,
// `false`, `null` — and rejects any structural surprise (a partial/corrupt file
// must not parse into something plausible). No third-party dep, no locale.
// ─────────────────────────────────────────────────────────────────────────────
enum class JKind { Null, Bool, Num, Str, Arr, Obj };

struct JValue {
	JKind kind = JKind::Null;
	bool bval = false;
	double num = 0.0;
	std::string str;
	std::vector<JValue> arr;
	// Object members kept in insertion order; lookups are linear (small objects).
	std::vector<std::pair<std::string, JValue>> obj;

	const JValue* find(const std::string& key) const {
		for (const auto& kv : obj) {
			if (kv.first == key) return &kv.second;
		}
		return nullptr;
	}
	bool is_obj() const { return kind == JKind::Obj; }
	bool is_arr() const { return kind == JKind::Arr; }
	bool is_str() const { return kind == JKind::Str; }
	bool is_num() const { return kind == JKind::Num; }
	bool is_null() const { return kind == JKind::Null; }
};

class JsonParser {
public:
	explicit JsonParser(const std::string& s) : s_(s) {}

	// Parse a whole document. Returns false on any malformed byte / trailing junk.
	bool parse(JValue& out) {
		skip_ws();
		if (!parse_value(out)) return false;
		skip_ws();
		return pos_ == s_.size();  // no trailing garbage
	}

private:
	const std::string& s_;
	std::size_t pos_ = 0;

	bool eof() const { return pos_ >= s_.size(); }
	char peek() const { return s_[pos_]; }

	void skip_ws() {
		while (!eof()) {
			const char c = s_[pos_];
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
				++pos_;
			} else {
				break;
			}
		}
	}

	bool match(char c) {
		if (eof() || s_[pos_] != c) return false;
		++pos_;
		return true;
	}

	bool parse_value(JValue& out) {
		skip_ws();
		if (eof()) return false;
		const char c = peek();
		switch (c) {
			case '{': return parse_object(out);
			case '[': return parse_array(out);
			case '"': {
				out.kind = JKind::Str;
				return parse_string(out.str);
			}
			case 't': case 'f': return parse_bool(out);
			case 'n': return parse_null(out);
			default:
				if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
				return false;
		}
	}

	bool parse_object(JValue& out) {
		out.kind = JKind::Obj;
		if (!match('{')) return false;
		skip_ws();
		if (match('}')) return true;  // empty object
		for (;;) {
			skip_ws();
			std::string key;
			if (eof() || peek() != '"') return false;
			if (!parse_string(key)) return false;
			skip_ws();
			if (!match(':')) return false;
			JValue v;
			if (!parse_value(v)) return false;
			out.obj.emplace_back(std::move(key), std::move(v));
			skip_ws();
			if (match(',')) continue;
			if (match('}')) return true;
			return false;
		}
	}

	bool parse_array(JValue& out) {
		out.kind = JKind::Arr;
		if (!match('[')) return false;
		skip_ws();
		if (match(']')) return true;  // empty array
		for (;;) {
			JValue v;
			if (!parse_value(v)) return false;
			out.arr.push_back(std::move(v));
			skip_ws();
			if (match(',')) continue;
			if (match(']')) return true;
			return false;
		}
	}

	bool parse_string(std::string& out) {
		if (!match('"')) return false;
		out.clear();
		while (!eof()) {
			const char c = s_[pos_++];
			if (c == '"') return true;
			if (c == '\\') {
				if (eof()) return false;
				const char e = s_[pos_++];
				switch (e) {
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'u': {
						if (pos_ + 4 > s_.size()) return false;
						std::uint32_t cp = 0;
						for (int i = 0; i < 4; ++i) {
							const char h = s_[pos_++];
							cp <<= 4;
							if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
							else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
							else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
							else return false;
						}
						append_utf8(out, cp);
						break;
					}
					default: return false;
				}
			} else {
				out += c;
			}
		}
		return false;  // unterminated string
	}

	static void append_utf8(std::string& out, std::uint32_t cp) {
		if (cp < 0x80) {
			out += static_cast<char>(cp);
		} else if (cp < 0x800) {
			out += static_cast<char>(0xC0 | (cp >> 6));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			out += static_cast<char>(0xE0 | (cp >> 12));
			out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}

	bool parse_number(JValue& out) {
		const std::size_t start = pos_;
		if (match('-')) { /* optional sign */ }
		bool any = false;
		while (!eof() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
		if (match('.')) {
			while (!eof() && s_[pos_] >= '0' && s_[pos_] <= '9') { ++pos_; any = true; }
		}
		if (!eof() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
			++pos_;
			if (!eof() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
			while (!eof() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
		}
		if (!any) return false;
		out.kind = JKind::Num;
		out.num = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
		return true;
	}

	bool parse_bool(JValue& out) {
		if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; out.kind = JKind::Bool; out.bval = true; return true; }
		if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; out.kind = JKind::Bool; out.bval = false; return true; }
		return false;
	}

	bool parse_null(JValue& out) {
		if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; out.kind = JKind::Null; return true; }
		return false;
	}
};

// ── Small helpers ────────────────────────────────────────────────────────────

bool is_lower_hex(const std::string& s, std::size_t n) {
	if (s.size() != n) return false;
	for (const char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	return true;
}

// A manifest per-chunk hash is "blake3:" + 64 lowercase-hex.
bool is_well_formed_chunk_hash(const std::string& h) {
	static const std::string kPrefix = "blake3:";
	if (h.compare(0, kPrefix.size(), kPrefix) != 0) return false;
	return is_lower_hex(h.substr(kPrefix.size()), kBlake3HexLen);
}

// Read an integer field that JSON stored as a number (chunk_size_m, coords, …).
bool as_int(const JValue& v, int& out) {
	if (!v.is_num()) return false;
	out = static_cast<int>(v.num);
	return true;
}

ChunkPackReport fail(ChunkPackVerdict verdict, std::string reason, ChunkIndex index = {}) {
	ChunkPackReport r;
	r.verdict = verdict;
	r.ok = false;
	r.hard_fail = true;
	r.reason = std::move(reason);
	r.index = std::move(index);
	r.chunk_count = r.index.chunks.size();
	return r;
}

std::string coord_str(int cx, int cz) {
	return "(" + std::to_string(cx) + "," + std::to_string(cz) + ")";
}

// Parse a Vec3 object {x,y,z}. Returns false on any missing/typed-wrong member.
bool parse_vec3(const JValue& v, Vec3& out) {
	if (!v.is_obj()) return false;
	const JValue* x = v.find("x");
	const JValue* y = v.find("y");
	const JValue* z = v.find("z");
	if (!x || !y || !z || !x->is_num() || !y->is_num() || !z->is_num()) return false;
	out.x = x->num;
	out.y = y->num;
	out.z = z->num;
	return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// recompute_chunk_hash — byte-identical to mcc chunk_emit `entry_hash_of`.
// ─────────────────────────────────────────────────────────────────────────────
std::string recompute_chunk_hash(const std::string& server_bytes,
                                 const std::string& scene_bytes,
                                 const std::string& proxy_bytes) {
	mcc::hash::Blake3 h;
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

// ─────────────────────────────────────────────────────────────────────────────
// build_and_resolve — parse both JSONs + resolve every ref (no disk).
// ─────────────────────────────────────────────────────────────────────────────
ChunkPackReport build_and_resolve(const std::string& chunks_json,
                                  const std::string& assets_json) {
	// (a) Parse the IF-8 asset-map first — the resolution table the manifest
	//     refs point into. A malformed map means we cannot resolve anything.
	JValue amap;
	if (!JsonParser(assets_json).parse(amap) || !amap.is_obj()) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "assets.json (IF-8 asset map) did not parse as a JSON object — "
		            "truncated or corrupt");
	}
	{
		const JValue* schema = amap.find("schema");
		if (!schema || !schema->is_str() || schema->str != kSupportedAssetMapSchema) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            std::string("assets.json schema tag is not '") +
			                kSupportedAssetMapSchema + "' — unreadable asset-map format");
		}
	}
	const JValue* aentries = amap.find("entries");
	if (!aentries || !aentries->is_arr()) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "assets.json has no 'entries' array");
	}

	// Build id -> AssetEntry. Every entry must carry a non-blank id + res path and
	// a well-formed bare 64-hex hash (a corrupt map is a hard fail, not a silent
	// partial table that would then mis-resolve).
	std::map<std::string, AssetEntry> table;
	for (const JValue& e : aentries->arr) {
		if (!e.is_obj()) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "assets.json entry is not an object");
		}
		AssetEntry a;
		const JValue* id = e.find("id");
		const JValue* res = e.find("resource");
		const JValue* hash = e.find("hash");
		const JValue* type = e.find("type");
		const JValue* nid = e.find("numeric_id");
		if (!id || !id->is_str() || id->str.empty()) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "assets.json entry missing a non-blank 'id'");
		}
		if (!res || !res->is_str() || res->str.empty()) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "assets.json entry '" + id->str + "' missing a non-blank 'resource'");
		}
		if (!hash || !hash->is_str() || !is_lower_hex(hash->str, kBlake3HexLen)) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "assets.json entry '" + id->str + "' has a bad-width / non-hex 'hash'");
		}
		a.id = id->str;
		a.res_path = res->str;
		a.hash = hash->str;
		a.type = (type && type->is_str()) ? type->str : std::string();
		if (nid && nid->is_num()) a.numeric_id = static_cast<std::uint32_t>(nid->num);
		table[a.id] = std::move(a);
	}

	// (b) Parse the IF-6 chunk manifest.
	JValue man;
	if (!JsonParser(chunks_json).parse(man) || !man.is_obj()) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "chunks.json (IF-6 manifest) did not parse as a JSON object — "
		            "truncated or corrupt");
	}

	ChunkIndex index;
	const JValue* fv = man.find("format_version");
	if (!fv || !as_int(*fv, index.format_version)) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "chunks.json missing integer 'format_version'");
	}
	if (index.format_version != kSupportedManifestFormatVersion) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "chunks.json format_version " + std::to_string(index.format_version) +
		                " != supported major " + std::to_string(kSupportedManifestFormatVersion));
	}
	const JValue* zone = man.find("zone");
	if (!zone || !zone->is_str() || zone->str.empty()) {
		return fail(ChunkPackVerdict::kManifestMalformed, "chunks.json missing non-blank 'zone'");
	}
	index.zone = zone->str;

	const JValue* csm = man.find("chunk_size_m");
	if (!csm || !as_int(*csm, index.chunk_size_m) || index.chunk_size_m < 1) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "chunks.json missing/invalid 'chunk_size_m'", index);
	}
	const JValue* origin = man.find("origin");
	if (!origin || !origin->is_obj()) {
		return fail(ChunkPackVerdict::kManifestMalformed, "chunks.json missing 'origin'", index);
	}
	{
		const JValue* ox = origin->find("x");
		const JValue* oz = origin->find("z");
		if (!ox || !oz || !ox->is_num() || !oz->is_num()) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunks.json 'origin' missing numeric x/z", index);
		}
		index.origin.x = ox->num;
		index.origin.z = oz->num;
	}
	const JValue* grid = man.find("grid");
	if (!grid || !grid->is_obj()) {
		return fail(ChunkPackVerdict::kManifestMalformed, "chunks.json missing 'grid'", index);
	}
	{
		const JValue* a = grid->find("min_cx");
		const JValue* b = grid->find("min_cz");
		const JValue* c = grid->find("max_cx");
		const JValue* d = grid->find("max_cz");
		if (!a || !b || !c || !d ||
		    !as_int(*a, index.min_cx) || !as_int(*b, index.min_cz) ||
		    !as_int(*c, index.max_cx) || !as_int(*d, index.max_cz)) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunks.json 'grid' missing integer bounds", index);
		}
	}
	const JValue* fr = man.find("far_ring");
	if (fr && !as_int(*fr, index.far_ring)) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "chunks.json 'far_ring' is not an integer", index);
	}

	const JValue* chunks = man.find("chunks");
	if (!chunks || !chunks->is_arr()) {
		return fail(ChunkPackVerdict::kManifestMalformed,
		            "chunks.json missing 'chunks' array", index);
	}

	// Resolve one logical ref through the asset table. On success fills `out`;
	// on failure returns the missing id so the caller can name it.
	auto resolve = [&table](const std::string& id, ResolvedRef& out) -> bool {
		const auto it = table.find(id);
		if (it == table.end()) return false;
		out.ref = id;
		out.res_path = it->second.res_path;
		return true;
	};

	for (const JValue& ce : chunks->arr) {
		if (!ce.is_obj()) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunks.json chunk entry is not an object", index);
		}
		ResolvedChunk rc;
		const JValue* cx = ce.find("cx");
		const JValue* cz = ce.find("cz");
		if (!cx || !cz || !as_int(*cx, rc.cx) || !as_int(*cz, rc.cz)) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunks.json chunk entry missing integer cx/cz", index);
		}
		const JValue* hash = ce.find("hash");
		if (!hash || !hash->is_str() || !is_well_formed_chunk_hash(hash->str)) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunk " + coord_str(rc.cx, rc.cz) +
			                " has a missing / bad 'hash' (expected blake3:<64hex>)",
			            index);
		}
		rc.hash = hash->str;

		const JValue* aabb = ce.find("aabb");
		const JValue* aabb_min = aabb && aabb->is_obj() ? aabb->find("min") : nullptr;
		const JValue* aabb_max = aabb && aabb->is_obj() ? aabb->find("max") : nullptr;
		if (!aabb_min || !aabb_max ||
		    !parse_vec3(*aabb_min, rc.aabb.min) || !parse_vec3(*aabb_max, rc.aabb.max)) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunk " + coord_str(rc.cx, rc.cz) + " has a missing / malformed 'aabb'",
			            index);
		}

		const JValue* prio = ce.find("priority");
		if (prio) {
			if (!as_int(*prio, rc.priority)) {
				return fail(ChunkPackVerdict::kManifestMalformed,
				            "chunk " + coord_str(rc.cx, rc.cz) + " 'priority' is not an integer",
				            index);
			}
			rc.has_priority = true;
		}

		// scene (required, resolvable)
		const JValue* scene = ce.find("scene");
		if (!scene || !scene->is_str() || scene->str.empty()) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunk " + coord_str(rc.cx, rc.cz) + " missing 'scene' ref", index);
		}
		if (!resolve(scene->str, rc.scene)) {
			return fail(ChunkPackVerdict::kUnresolvedRef,
			            "chunk " + coord_str(rc.cx, rc.cz) + " scene ref '" + scene->str +
			                "' is not in the IF-8 asset table",
			            index);
		}

		// server (required, resolvable) — Q1(a): the server .chunk.bin ships in the client pack
		const JValue* server = ce.find("server");
		if (!server || !server->is_str() || server->str.empty()) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunk " + coord_str(rc.cx, rc.cz) + " missing 'server' ref", index);
		}
		if (!resolve(server->str, rc.server)) {
			return fail(ChunkPackVerdict::kUnresolvedRef,
			            "chunk " + coord_str(rc.cx, rc.cz) + " server ref '" + server->str +
			                "' is not in the IF-8 asset table",
			            index);
		}

		// proxy (required key; value may be null == explicit no-proxy, amendment C3)
		const JValue* proxy = ce.find("proxy");
		if (!proxy) {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunk " + coord_str(rc.cx, rc.cz) + " missing 'proxy' key (use null for none)",
			            index);
		}
		if (proxy->is_null()) {
			rc.has_proxy = false;
		} else if (proxy->is_str() && !proxy->str.empty()) {
			if (!resolve(proxy->str, rc.proxy)) {
				return fail(ChunkPackVerdict::kUnresolvedRef,
				            "chunk " + coord_str(rc.cx, rc.cz) + " proxy ref '" + proxy->str +
				                "' is not in the IF-8 asset table",
				            index);
			}
			rc.has_proxy = true;
		} else {
			return fail(ChunkPackVerdict::kManifestMalformed,
			            "chunk " + coord_str(rc.cx, rc.cz) + " 'proxy' must be an id string or null",
			            index);
		}

		// deps (optional; each must resolve — but is a prefetch-only ref, not a
		// completeness requirement here, so presence is checked in stage 2 NOT at all)
		const JValue* deps = ce.find("deps");
		if (deps) {
			if (!deps->is_arr()) {
				return fail(ChunkPackVerdict::kManifestMalformed,
				            "chunk " + coord_str(rc.cx, rc.cz) + " 'deps' is not an array", index);
			}
			for (const JValue& d : deps->arr) {
				if (!d.is_str() || d.str.empty()) {
					return fail(ChunkPackVerdict::kManifestMalformed,
					            "chunk " + coord_str(rc.cx, rc.cz) + " has a blank dep ref", index);
				}
				ResolvedRef dr;
				if (!resolve(d.str, dr)) {
					return fail(ChunkPackVerdict::kUnresolvedRef,
					            "chunk " + coord_str(rc.cx, rc.cz) + " dep ref '" + d.str +
					                "' is not in the IF-8 asset table",
					            index);
				}
				rc.deps.push_back(std::move(dr));
			}
		}

		index.chunks.push_back(std::move(rc));
	}

	ChunkPackReport r;
	r.verdict = ChunkPackVerdict::kOk;
	r.ok = true;
	r.hard_fail = false;
	r.reason = "resolved " + std::to_string(index.chunks.size()) + " chunks";
	r.index = std::move(index);
	r.chunk_count = r.index.chunks.size();
	r.verified_chunks = 0;  // resolution only — presence/integrity not yet checked
	return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// verify_chunk_pack — parse+resolve, then presence + integrity (fail closed).
// ─────────────────────────────────────────────────────────────────────────────
ChunkPackReport verify_chunk_pack(const std::string& chunks_json,
                                  const std::string& assets_json,
                                  const IPackFileProvider& files) {
	ChunkPackReport r = build_and_resolve(chunks_json, assets_json);
	if (!r.ok) return r;  // malformed / unresolved — already a hard fail

	for (const ResolvedChunk& c : r.index.chunks) {
		// (4) Presence — the two shipped payloads (client scene[+proxy] + server bin).
		if (!files.exists(c.server.res_path)) {
			return fail(ChunkPackVerdict::kMissingAsset,
			            "chunk " + coord_str(c.cx, c.cz) + " server payload missing from pack: " +
			                c.server.res_path,
			            std::move(r.index));
		}
		if (!files.exists(c.scene.res_path)) {
			return fail(ChunkPackVerdict::kMissingAsset,
			            "chunk " + coord_str(c.cx, c.cz) + " scene payload missing from pack: " +
			                c.scene.res_path,
			            std::move(r.index));
		}
		if (c.has_proxy && !files.exists(c.proxy.res_path)) {
			return fail(ChunkPackVerdict::kMissingAsset,
			            "chunk " + coord_str(c.cx, c.cz) + " proxy payload missing from pack: " +
			                c.proxy.res_path,
			            std::move(r.index));
		}

		// (5) Integrity — recompute the both-payloads BLAKE3 from the bytes on
		//     disk and match the manifest hash. A read failure here is a missing
		//     asset (presence passed but the byte read failed — still fail closed).
		std::string server_bytes, scene_bytes, proxy_bytes;
		if (!files.read(c.server.res_path, server_bytes) ||
		    !files.read(c.scene.res_path, scene_bytes) ||
		    (c.has_proxy && !files.read(c.proxy.res_path, proxy_bytes))) {
			return fail(ChunkPackVerdict::kMissingAsset,
			            "chunk " + coord_str(c.cx, c.cz) + " payload became unreadable during verify",
			            std::move(r.index));
		}
		const std::string actual = recompute_chunk_hash(server_bytes, scene_bytes, proxy_bytes);
		if (actual != c.hash) {
			return fail(ChunkPackVerdict::kHashMismatch,
			            "chunk " + coord_str(c.cx, c.cz) + " integrity FAIL — manifest hash " +
			                c.hash + " != recomputed " + actual + " (tampered / corrupt payload)",
			            std::move(r.index));
		}
		++r.verified_chunks;
	}

	r.verdict = ChunkPackVerdict::kOk;
	r.ok = true;
	r.hard_fail = false;
	r.reason = "verified " + std::to_string(r.verified_chunks) + "/" +
	           std::to_string(r.chunk_count) + " chunks — pack complete + intact";
	return r;
}

const char* chunk_pack_verdict_name(ChunkPackVerdict v) {
	switch (v) {
		case ChunkPackVerdict::kOk: return "ok";
		case ChunkPackVerdict::kManifestMalformed: return "manifest-malformed";
		case ChunkPackVerdict::kUnresolvedRef: return "unresolved-ref";
		case ChunkPackVerdict::kMissingAsset: return "missing-asset";
		case ChunkPackVerdict::kHashMismatch: return "hash-mismatch";
	}
	return "unknown";
}

}  // namespace meridian::chunkpack
