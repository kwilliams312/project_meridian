// Project Meridian — engine-free IF-5 pack-manifest verify core (issue #107).
//
// The CLIENT counterpart to worldd's #89 world-DB boot check: at client boot the
// `stream` module mounts a content pack and MUST verify its `pack.manifest.json`
// before the boot flow proceeds (Client SAD §2.3 "verifies pack manifest — engine
// pin, schema version, per-pack content hashes"; §5.2 "IF-5 — pack manifest
// verification"). This header is the ENGINE-FREE core — plain C++17, NO Godot
// types — so it links into the client GDExtension, the headless bot, AND the
// plain unit-test suite exactly like the movement / telemetry cores (Client SAD
// §9.2 "engine-agnostic C++ cores … the bot client and unit tests link without
// Godot"). The thin GDExtension binding lives in meridian_pack_mount.* (#107).
//
// WHY a boot check at all (mirrors worldd #89 / SAD §5.2): the client renders a
// SPECIFIC compile of the content. If the pack is a schema version the client
// cannot read (content-schema drift between the mcc emitter and this client), or
// the manifest is missing/truncated/corrupt (a partial download or a failed
// mount), or its content hash does not match what the realm expects, the client
// must NOT silently load garbage — it fails fast with a clear message and refuses
// to enter (never a silent degrade — SAD §5.2 "clear rejection message plus
// updater hand-off"). This is the client half of the same hash discipline IF-4
// applies server-side (#89).
//
// WHAT this core parses (the EXACT emit-pck `pack.manifest.json`, issue #121 /
// tools/mcc/src/stages/emit_pck.cpp). The producer writes, in a fixed key order:
//   schema                  "meridian/pack-manifest@1"  (the manifest format tag)
//   pack, namespace         the pack's namespace (owns an IF-9 id band)
//   version                 pack semver text
//   content_schema_version  content-schema MAJOR the client checks (int)
//   godot_version           the pinned engine version (PRD R8; client refuses a
//                           mismatch)
//   id_band                 the pack's IF-9 numeric band base
//   content_hash            BLAKE3 of the pack source tree (64 lowercase-hex —
//                           byte-identical to emit-sql's world_manifest hash, the
//                           three-way tie SAD §2.6)
//   mcc_version, built_at   provenance
//   entry_count             number of entries (MUST equal entries.size())
//   entries[]               one object per content/asset resource, each carrying:
//     id (core:...), numeric_id (IF-9), type, resource (res:// path), hash (64-hex)
//
// The verify logic (verify_pack_manifest) is a PURE function over an already-
// parsed manifest — no disk, no Godot — so it is unit-testable WITHOUT a Godot
// runtime. The JSON read (parse_pack_manifest) is a small hand-rolled recursive-
// descent scanner (the repo's dep-free discipline — same as telemetryd's ingest
// reader and the telemetry envelope writer; no third-party JSON library).
//
// M0 SCOPE (honest, per #121): the Godot-native `.pck` container mount
// (ProjectSettings::load_resource_pack) is a follow-up once the pinned engine's
// import pipeline lands. At M0 emit-pck ships the manifest (the testable IF-5
// contract) plus a directory-manifest pack (`pack.contents.jsonl`). So this core
// verifies the MANIFEST INTEGRITY (the M0 deliverable) and can OPTIONALLY verify
// a resource's declared hash against a caller-supplied payload when one is
// available; the actual .pck mount is the thin wrapper's job in the follow-up.

#ifndef MERIDIAN_PACK_MANIFEST_CORE_H
#define MERIDIAN_PACK_MANIFEST_CORE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meridian::pack {

// The manifest-format tag THIS client understands. emit-pck stamps every manifest
// with `"schema": "meridian/pack-manifest@1"`; a manifest declaring any other tag
// is a format this client cannot read (a newer/older emit-pck) -> kMalformed.
inline constexpr const char* kSupportedManifestSchema = "meridian/pack-manifest@1";

// The content-schema MAJOR version THIS client can load. Bumped in lockstep with
// a breaking change to the content schema (mirrors worldd's
// kSupportedContentSchemaVersion, #89). Content Schema v1 is current, so a pack
// whose manifest reports any other `content_schema_version` is one this client
// cannot render -> hard fail (SAD §5.2 "verifies … schema version"). A named
// constant keeps the schema bump a one-line change.
inline constexpr std::uint32_t kSupportedContentSchemaVersion = 1;

// A BLAKE3 content hash rendered as lowercase hex is exactly 64 chars (32 bytes).
// emit-pck writes content_hash + every entry hash as 64 lowercase-hex; a value of
// any other width or with a non-hex char is a truncated / corrupt manifest
// (mirrors worldd's kContentHashHexLen, #89).
inline constexpr std::size_t kContentHashHexLen = 64;

// One entry from the manifest's `entries[]` — a content/asset resource the client
// would mount. Field names mirror the emit-pck JSON keys exactly (the IF-5
// contract): id / numeric_id / type / resource / hash.
struct PackEntry {
    std::string   id;           // fully-qualified content/asset id (core:...)
    std::uint32_t numeric_id = 0;   // IF-9 runtime id (same as the SQL keys)
    std::string   type;         // npc | item | ability | ... | asset
    std::string   resource;     // res://meridian/<ns>/<...> (by ID, SAD §2.7)
    std::string   hash;         // 64-hex BLAKE3 of the entry's canonical payload
};

// The parsed `pack.manifest.json`. Field names + order mirror emit-pck's fixed-
// key JSON exactly. `parse_ok` distinguishes a JSON that would not scan at all
// (malformed bytes) from one that scanned but is semantically invalid (caught by
// verify): a parse failure sets parse_ok=false and leaves the struct default.
struct PackManifest {
    bool          parse_ok = false;   // did the JSON scan cleanly?
    std::string   manifest_schema;    // "schema" — the manifest format tag
    std::string   pack;               // "pack"
    std::string   pack_namespace;     // "namespace"
    std::string   version;            // "version" (semver text)
    std::uint32_t content_schema_version = 0;  // "content_schema_version"
    std::string   godot_version;      // "godot_version" (engine pin)
    std::uint32_t id_band = 0;        // "id_band"
    std::string   content_hash;       // "content_hash" (64-hex BLAKE3)
    std::string   mcc_version;        // "mcc_version"
    std::string   built_at;           // "built_at"
    std::uint64_t entry_count = 0;    // "entry_count" (MUST equal entries.size())
    bool          has_entry_count = false;   // was the field present at all?
    std::vector<PackEntry> entries;   // "entries" (parsed objects, in file order)
};

// How the boot verify came out (mirrors worldd's BootVerdict, #89). `kOk` is the
// only verdict that lets the client enter; every other is a HARD failure — the
// boot flow halts with a clear message (SAD §5.2: never a silent degrade).
enum class PackVerdict {
    kOk,               // manifest present, well-formed, schema matches (and the
                       // expected/entry hashes matched if any were checked)
    kMalformed,        // structurally invalid: won't parse, wrong manifest schema
                       // tag, a missing/blank required field, a bad-width hash, or
                       // entry_count != entries.size() (truncated / corrupt pack)
    kSchemaMismatch,   // content_schema_version != kSupportedContentSchemaVersion
                       // (a content compile this client cannot render — fail fast)
    kEngineMismatch,   // godot_version != the client's pinned engine (PRD R8; the
                       // pack was built for a different Godot/export-template)
    kHashMismatch,     // the manifest content_hash != the caller's expected pin,
                       // OR a verified entry payload's hash != its declared hash
};

// The outcome of the boot check (mirrors worldd's BootReport, #89): the verdict,
// a human-readable reason (for the log / the fatal boot message), and the
// resolved content identity the client would mount when the manifest is usable.
// `hard_fail` is the single "should the client refuse to enter?" bit the boot
// scene acts on — true unless verdict is kOk.
struct PackReport {
    PackVerdict verdict = PackVerdict::kMalformed;
    bool        hard_fail = true;    // true unless verdict is kOk
    std::string reason;              // one-line explanation for logs / boot UX

    // Resolved content identity (populated once the manifest is well-formed —
    // even on a schema/engine/hash mismatch, so the boot UX can name what it
    // rejected). Empty on a malformed / unparseable manifest.
    std::string content_hash;        // the pack content hash (for realm validation)
    std::string content_version;     // "<namespace>@<version>" — what the client mounted
    std::string godot_version;       // the pack's pinned engine version
    std::uint32_t content_schema_version = 0;
    std::size_t   entry_count = 0;   // number of entries in the manifest
};

// Options for the verify. All optional — when unset, that check is skipped and a
// well-formed, schema-matching manifest is kOk. This mirrors worldd's optional
// operator-pinned hash tie (#89), extended for the client's engine pin.
struct PackVerifyOptions {
    // The client's PINNED Godot engine version (client/ENGINE_VERSION GODOT_VERSION,
    // e.g. "4.6-stable" — pass the same token the pack records, e.g. "4.6"). When
    // set AND non-empty, the manifest's godot_version MUST match it or the pack is
    // kEngineMismatch (PRD R8; SAD §5.2 "engine pin"). Empty -> engine pin not
    // checked (the M0 default until the boot scene wires the real pin in).
    std::string expected_godot_version;

    // The realm/operator-pinned "this is the compile we deployed" content hash
    // (SAD §5.2: "the realm accepts only on content-hash match"). When set AND
    // itself 64-hex, the manifest content_hash MUST equal it or the pack is
    // kHashMismatch. A malformed pin is ignored (treated as no pin) rather than
    // failing boot, since it is operator config, not content — same policy as
    // worldd's #89. Empty/nullopt -> no pin, the hash tie is not checked.
    std::optional<std::string> expected_content_hash;
};

// PURE verify (no disk, no Godot — unit-testable). Validate a parsed manifest
// against this client's contract, in fail-fast order (mirrors worldd #89):
//   (1) parsed at all + manifest schema tag matches   -> else kMalformed
//   (2) required scalar fields present + non-blank; content_hash is 64-hex;
//       entry_count present and == entries.size(); every entry well-formed
//       (non-blank id, non-blank resource, 64-hex entry hash)  -> else kMalformed
//   (3) content_schema_version == kSupportedContentSchemaVersion -> else
//       kSchemaMismatch (fail fast — a compile this client cannot render)
//   (4) if opts.expected_godot_version set, manifest godot_version equals it ->
//       else kEngineMismatch (the engine pin, PRD R8)
//   (5) if opts.expected_content_hash set (and well-formed), manifest
//       content_hash equals it -> else kHashMismatch (the realm content tie)
// The resolved identity (hash / "<ns>@<version>" / engine / schema / entry_count)
// is filled once the manifest is well-formed, so the boot UX can name the pack it
// accepted OR rejected.
PackReport verify_pack_manifest(const PackManifest& manifest,
                                const PackVerifyOptions& opts = {});

// Parse `pack.manifest.json` bytes into a PackManifest. A small hand-rolled
// recursive-descent JSON scanner (no third-party dep — this core links into the
// engine-free test with zero deps, the repo's discipline). Strict: a byte-level
// malformed document sets parse_ok=false (verify then reports kMalformed). It
// reads exactly the emit-pck key set; UNKNOWN top-level keys are ignored (forward
// compatibility — a newer emit-pck minor that adds a key must not break an older
// client, mirroring the schema envelope's minor-tolerance, Tools SAD §2.1).
PackManifest parse_pack_manifest(const std::string& json);

// Convenience: parse + verify in one call (the common boot path). Equivalent to
// verify_pack_manifest(parse_pack_manifest(json), opts). A parse failure yields a
// kMalformed report with a parse-error reason.
PackReport verify_pack_manifest_json(const std::string& json,
                                     const PackVerifyOptions& opts = {});

// OPTIONAL per-entry hash check (SAD §2.7 "per-resource BLAKE3; the client
// verifies each mounted resource"). Compare an ALREADY-COMPUTED resource hash to
// the entry whose `resource` path matches `resource_path`. Returns:
//   kOk           — the entry exists and its declared hash equals actual_hash
//   kHashMismatch — the entry exists but the hashes differ (tampered / corrupt
//                   resource) — also returned if actual_hash is not 64-hex
//   kMalformed    — no entry with that resource path (not in the manifest)
//
// WHY the hash is passed in, not computed here (honest M0 boundary): emit-pck's
// per-entry `hash` is BLAKE3 over a FRAMED canonicalization of the entry's SOURCE
// payload (`rel_path\0canon\0`, emit_pck.cpp resource_hash_of), NOT a raw BLAKE3
// of the mounted resource bytes — and at M0 the imported .pck resource bytes do
// not exist yet (the .pck mount is the follow-up per #121). So the engine-free
// core does NOT re-derive the hash (it cannot, without the source canon + the
// pinned importer); it compares a hash the CALLER computed with the matching
// construction. This keeps the core dependency-free (no BLAKE3 vendored) and
// truthful: the manifest-integrity check (verify_pack_manifest) is the M0
// deliverable; per-resource re-hash verification lands with the real .pck mount.
PackVerdict verify_entry_hash(const PackManifest& manifest,
                              const std::string& resource_path,
                              const std::string& actual_hash);

// Human-readable name for a verdict (logs / test diagnostics; mirrors worldd's
// boot_verdict_name, #89).
const char* pack_verdict_name(PackVerdict v);

}  // namespace meridian::pack

#endif  // MERIDIAN_PACK_MANIFEST_CORE_H
