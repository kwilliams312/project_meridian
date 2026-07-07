// Project Meridian — engine-free IF-5 pack-manifest verify core (issue #107).
// See pack_manifest_core.h for the design + the SAD/#89/#121 citations. Plain
// C++17, NO Godot — links into the GDExtension, the bot, and the unit test.
//
// Clean-room from the Client SAD (§2.3 / §5.2) + the emit-pck manifest shape
// (tools/mcc/src/stages/emit_pck.cpp, #121) + the worldd #89 verify policy. The
// JSON reader is a small hand-rolled recursive-descent scanner in the repo's
// dep-free style (telemetryd/ingest.cpp). No third-party JSON library.

#include "pack_manifest_core.h"

#include <cctype>
#include <cstdint>

namespace meridian::pack {

// ===========================================================================
// A tiny recursive-descent JSON scanner (dep-free; the repo's discipline).
// ===========================================================================
// Supports exactly what emit-pck's pack.manifest.json uses: objects, arrays,
// double-quoted strings (with the RFC 8259 escapes emit-pck emits: \" \\ \n \r
// \t \b \f \uXXXX), non-negative integers, and the object/array nesting of the
// entries list. It is STRICT: any structural surprise fails the scan (parse_ok
// stays false), never a partial accept. emit-pck never emits floats, `true`/
// `false`/`null`, or negative numbers in this manifest, so those are not needed;
// a value shape the scanner does not expect is a parse failure (kMalformed).
namespace {

class JsonScanner {
public:
    explicit JsonScanner(const std::string& s) : s_(s) {}

    // Parse the whole document as a single object into `out`. Returns false on any
    // malformed byte. On success, trailing whitespace is allowed but no trailing
    // non-whitespace content.
    bool parse_manifest(PackManifest& out) {
        skip_ws();
        if (!parse_object_into(out)) return false;
        skip_ws();
        return i_ == s_.size();  // no trailing junk
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;
    bool ok_ = true;

    char peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }
    bool eof() const { return i_ >= s_.size(); }
    void fail() { ok_ = false; }

    void skip_ws() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool expect(char c) {
        skip_ws();
        if (peek() != c) { fail(); return false; }
        ++i_;
        return true;
    }

    // Scan a double-quoted string (assumes the caller has NOT yet consumed the
    // opening quote). Decodes the escapes emit-pck emits. On malformed input
    // (unterminated, bad escape) sets ok_=false and returns "".
    std::string scan_string() {
        std::string out;
        if (!expect('"')) return out;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i_ >= s_.size()) { fail(); return out; }
                const char e = s_[i_++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u': {
                        // \uXXXX — decode the 4 hex digits. emit-pck only ever
                        // emits \u00XX for low control chars; decode the full BMP
                        // codepoint to UTF-8 for correctness.
                        if (i_ + 4 > s_.size()) { fail(); return out; }
                        std::uint32_t cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = s_[i_++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                            else { fail(); return out; }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: fail(); return out;
                }
            } else {
                out += c;
            }
        }
        fail();  // ran off the end without a closing quote
        return out;
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

    // Scan a non-negative integer into `out`. emit-pck emits only non-negative
    // decimal integers for numeric fields (numeric_id, id_band, content_schema_
    // version, entry_count). A leading '-' or a non-digit is a parse failure.
    bool scan_uint(std::uint64_t& out) {
        skip_ws();
        if (!std::isdigit(static_cast<unsigned char>(peek()))) { fail(); return false; }
        std::uint64_t v = 0;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            v = v * 10 + static_cast<std::uint64_t>(s_[i_++] - '0');
        }
        out = v;
        return ok_;
    }

    // Skip a JSON value of any shape (used for UNKNOWN top-level keys — forward
    // compatibility). Handles string / number / object / array / true / false /
    // null well enough to resynchronize the scanner past the value.
    bool skip_value() {
        skip_ws();
        const char c = peek();
        if (c == '"') { scan_string(); return ok_; }
        if (c == '{') return skip_container('{', '}');
        if (c == '[') return skip_container('[', ']');
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            if (c == '-') ++i_;
            // number: digits, optional '.', optional exponent
            while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.' ||
                   peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-') {
                ++i_;
            }
            return true;
        }
        if (match_literal("true") || match_literal("false") || match_literal("null")) {
            return true;
        }
        fail();
        return false;
    }

    bool match_literal(const char* lit) {
        std::size_t n = 0;
        while (lit[n] != '\0') ++n;
        if (s_.compare(i_, n, lit) == 0) { i_ += n; return true; }
        return false;
    }

    // Skip a balanced container starting at the opening bracket (respects nested
    // containers and strings, so braces inside strings don't confuse the depth).
    bool skip_container(char open, char close) {
        if (peek() != open) { fail(); return false; }
        ++i_;
        int depth = 1;
        while (i_ < s_.size() && depth > 0) {
            const char c = s_[i_];
            if (c == '"') { scan_string(); if (!ok_) return false; continue; }
            if (c == open) ++depth;
            else if (c == close) --depth;
            ++i_;
        }
        if (depth != 0) { fail(); return false; }
        return true;
    }

    // Parse one entry object { "id":.., "numeric_id":.., "type":.., "resource":..,
    // "hash":.. } into `e`. Unknown keys inside an entry are skipped (forward-
    // compat), same as the top level.
    bool parse_entry(PackEntry& e) {
        if (!expect('{')) return false;
        skip_ws();
        if (peek() == '}') { ++i_; return ok_; }  // empty object (verify catches it)
        while (true) {
            const std::string key = scan_string();
            if (!ok_) return false;
            if (!expect(':')) return false;
            if (key == "id") {
                e.id = scan_string();
            } else if (key == "numeric_id") {
                std::uint64_t v = 0;
                scan_uint(v);
                e.numeric_id = static_cast<std::uint32_t>(v);
            } else if (key == "type") {
                e.type = scan_string();
            } else if (key == "resource") {
                e.resource = scan_string();
            } else if (key == "hash") {
                e.hash = scan_string();
            } else {
                skip_value();  // unknown entry key — forward compatibility
            }
            if (!ok_) return false;
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == '}') { ++i_; return ok_; }
            fail();
            return false;
        }
    }

    // Parse the entries array into `out.entries`.
    bool parse_entries(PackManifest& out) {
        if (!expect('[')) return false;
        skip_ws();
        if (peek() == ']') { ++i_; return ok_; }  // empty array (valid: 0 entries)
        while (true) {
            PackEntry e;
            if (!parse_entry(e)) return false;
            out.entries.push_back(std::move(e));
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == ']') { ++i_; return ok_; }
            fail();
            return false;
        }
    }

    // Parse the top-level object into `out`, dispatching known keys to fields and
    // skipping unknown ones.
    bool parse_object_into(PackManifest& out) {
        if (!expect('{')) return false;
        skip_ws();
        if (peek() == '}') { ++i_; return ok_; }  // empty object (verify catches it)
        while (true) {
            const std::string key = scan_string();
            if (!ok_) return false;
            if (!expect(':')) return false;

            if (key == "schema") {
                out.manifest_schema = scan_string();
            } else if (key == "pack") {
                out.pack = scan_string();
            } else if (key == "namespace") {
                out.pack_namespace = scan_string();
            } else if (key == "version") {
                out.version = scan_string();
            } else if (key == "content_schema_version") {
                std::uint64_t v = 0;
                scan_uint(v);
                out.content_schema_version = static_cast<std::uint32_t>(v);
            } else if (key == "godot_version") {
                out.godot_version = scan_string();
            } else if (key == "id_band") {
                std::uint64_t v = 0;
                scan_uint(v);
                out.id_band = static_cast<std::uint32_t>(v);
            } else if (key == "content_hash") {
                out.content_hash = scan_string();
            } else if (key == "mcc_version") {
                out.mcc_version = scan_string();
            } else if (key == "built_at") {
                out.built_at = scan_string();
            } else if (key == "entry_count") {
                std::uint64_t v = 0;
                scan_uint(v);
                out.entry_count = v;
                out.has_entry_count = true;
            } else if (key == "entries") {
                if (!parse_entries(out)) return false;
            } else {
                skip_value();  // unknown top-level key — forward compatibility
            }
            if (!ok_) return false;
            skip_ws();
            if (peek() == ',') { ++i_; continue; }
            if (peek() == '}') { ++i_; return ok_; }
            fail();
            return false;
        }
    }
};

// A well-formed content/entry hash is exactly kContentHashHexLen lowercase-hex
// chars (mirrors worldd's is_well_formed_hash, #89). emit-pck renders BLAKE3 as
// lowercase hex; a wrong width or non-hex char means a truncated / corrupt value.
bool is_well_formed_hash(const std::string& h) {
    if (h.size() != kContentHashHexLen) return false;
    for (const char c : h) {
        const bool lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lower_hex) return false;
    }
    return true;
}

}  // namespace

// ===========================================================================
// pack_verdict_name
// ===========================================================================
const char* pack_verdict_name(PackVerdict v) {
    switch (v) {
        case PackVerdict::kOk:             return "ok";
        case PackVerdict::kMalformed:      return "malformed";
        case PackVerdict::kSchemaMismatch: return "schema-mismatch";
        case PackVerdict::kEngineMismatch: return "engine-mismatch";
        case PackVerdict::kHashMismatch:   return "hash-mismatch";
    }
    return "unknown";
}

// ===========================================================================
// parse_pack_manifest
// ===========================================================================
PackManifest parse_pack_manifest(const std::string& json) {
    PackManifest m;
    JsonScanner scanner(json);
    m.parse_ok = scanner.parse_manifest(m);
    if (!m.parse_ok) {
        // Leave the struct otherwise default — verify reports kMalformed. Do NOT
        // trust partially-scanned fields from a document that did not close.
        m = PackManifest{};
        m.parse_ok = false;
    }
    return m;
}

// ===========================================================================
// verify_pack_manifest — PURE (no Godot, no disk)
// ===========================================================================
PackReport verify_pack_manifest(const PackManifest& manifest,
                                const PackVerifyOptions& opts) {
    PackReport rep;

    // (1) Parsed at all + the manifest schema tag is one this client reads. A byte-
    // malformed manifest (parse_ok=false) or a wrong/absent schema tag is a pack
    // this client cannot trust -> kMalformed (mirrors worldd's missing/malformed
    // gate, #89, folded into one "not a manifest I understand" verdict).
    if (!manifest.parse_ok) {
        rep.verdict = PackVerdict::kMalformed;
        rep.hard_fail = true;
        rep.reason = "pack.manifest.json did not parse (truncated / corrupt / not JSON)";
        return rep;
    }
    if (manifest.manifest_schema != kSupportedManifestSchema) {
        rep.verdict = PackVerdict::kMalformed;
        rep.hard_fail = true;
        rep.reason = "pack manifest declares schema '" + manifest.manifest_schema +
                     "' but this client reads '" + std::string(kSupportedManifestSchema) +
                     "' (unknown manifest format — corrupt or an incompatible mcc)";
        return rep;
    }

    // (2) Required scalar fields present + non-blank; content_hash 64-hex;
    // entry_count present and consistent; every entry well-formed. Any of these
    // failing is a truncated / corrupt manifest (SAD §5.2 refuses to mount).
    if (manifest.pack_namespace.empty()) {
        rep.verdict = PackVerdict::kMalformed;
        rep.hard_fail = true;
        rep.reason = "pack manifest has an empty namespace (corrupt manifest)";
        return rep;
    }
    if (manifest.version.empty()) {
        rep.verdict = PackVerdict::kMalformed;
        rep.hard_fail = true;
        rep.reason = "pack manifest '" + manifest.pack_namespace +
                     "' has an empty version (corrupt manifest)";
        return rep;
    }
    if (!is_well_formed_hash(manifest.content_hash)) {
        rep.verdict = PackVerdict::kMalformed;
        rep.hard_fail = true;
        rep.reason = "pack manifest '" + manifest.pack_namespace +
                     "' has a malformed content_hash (expected 64 lowercase-hex "
                     "chars) — truncated / corrupt content pack";
        return rep;
    }
    if (!manifest.has_entry_count || manifest.entry_count != manifest.entries.size()) {
        rep.verdict = PackVerdict::kMalformed;
        rep.hard_fail = true;
        rep.reason = "pack manifest '" + manifest.pack_namespace + "' entry_count (" +
                     (manifest.has_entry_count ? std::to_string(manifest.entry_count)
                                               : std::string("absent")) +
                     ") does not match the number of entries (" +
                     std::to_string(manifest.entries.size()) +
                     ") — truncated / corrupt manifest";
        return rep;
    }
    for (const PackEntry& e : manifest.entries) {
        if (e.id.empty() || e.resource.empty() || !is_well_formed_hash(e.hash)) {
            rep.verdict = PackVerdict::kMalformed;
            rep.hard_fail = true;
            rep.reason = "pack manifest '" + manifest.pack_namespace +
                         "' has a malformed entry (id='" + e.id + "', resource='" +
                         e.resource + "') — missing id/resource or a bad-width hash";
            return rep;
        }
    }

    // The manifest is well-formed. Resolve the content identity now, so the boot
    // UX can name what it accepted OR rejected in the mismatch cases below.
    rep.content_hash = manifest.content_hash;
    rep.content_version = manifest.pack_namespace + "@" + manifest.version;
    rep.godot_version = manifest.godot_version;
    rep.content_schema_version = manifest.content_schema_version;
    rep.entry_count = manifest.entries.size();

    // (3) Content schema this client can render? (fail fast — mirrors worldd #89's
    // schema-version gate). A compile whose content-schema major this client does
    // not implement cannot be loaded, no matter how well-formed it is.
    if (manifest.content_schema_version != kSupportedContentSchemaVersion) {
        rep.verdict = PackVerdict::kSchemaMismatch;
        rep.hard_fail = true;
        rep.reason = "pack '" + manifest.pack_namespace + "' reports content_schema_version " +
                     std::to_string(manifest.content_schema_version) +
                     " but this client renders content-schema v" +
                     std::to_string(kSupportedContentSchemaVersion) +
                     " — update the client, or rebuild the pack with a matching mcc";
        return rep;
    }

    // (4) Engine pin (PRD R8; SAD §5.2 "engine pin"). Only when the caller passed a
    // pin — the pack must be built for the same Godot / export-template version.
    if (!opts.expected_godot_version.empty() &&
        manifest.godot_version != opts.expected_godot_version) {
        rep.verdict = PackVerdict::kEngineMismatch;
        rep.hard_fail = true;
        rep.reason = "pack '" + manifest.pack_namespace + "' was built for Godot '" +
                     manifest.godot_version + "' but this client is pinned to '" +
                     opts.expected_godot_version +
                     "' — mount refused (engine/export-template mismatch, PRD R8)";
        return rep;
    }

    // (5) Realm/operator content-hash pin (SAD §5.2 "the realm accepts only on
    // content-hash match"). Only when a pin is set AND itself well-formed — a
    // malformed pin is operator-config noise, not a content fault, so it is ignored
    // (no-pin) rather than failing boot (same policy as worldd #89). A real
    // disagreement is a hard fail: the client is loading a different compile than
    // the realm expects (clear rejection, never a silent degrade).
    if (opts.expected_content_hash && is_well_formed_hash(*opts.expected_content_hash) &&
        *opts.expected_content_hash != rep.content_hash) {
        rep.verdict = PackVerdict::kHashMismatch;
        rep.hard_fail = true;
        rep.reason = "pack '" + manifest.pack_namespace + "' content_hash " +
                     rep.content_hash + " does not match the expected hash " +
                     *opts.expected_content_hash +
                     " — mount refused (wrong content compile for this realm)";
        return rep;
    }

    rep.verdict = PackVerdict::kOk;
    rep.hard_fail = false;
    rep.reason = "pack manifest ok";
    return rep;
}

PackReport verify_pack_manifest_json(const std::string& json,
                                     const PackVerifyOptions& opts) {
    return verify_pack_manifest(parse_pack_manifest(json), opts);
}

// ===========================================================================
// verify_entry_hash — OPTIONAL per-resource check
// ===========================================================================
PackVerdict verify_entry_hash(const PackManifest& manifest,
                              const std::string& resource_path,
                              const std::string& actual_hash) {
    for (const PackEntry& e : manifest.entries) {
        if (e.resource == resource_path) {
            if (!is_well_formed_hash(actual_hash)) return PackVerdict::kHashMismatch;
            return e.hash == actual_hash ? PackVerdict::kOk : PackVerdict::kHashMismatch;
        }
    }
    return PackVerdict::kMalformed;  // no such resource in the manifest
}

}  // namespace meridian::pack
