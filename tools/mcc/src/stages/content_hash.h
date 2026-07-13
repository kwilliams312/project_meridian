// tools/mcc/src/stages/content_hash.h — the per-pack content hash (IF-4 + IF-5).
//
// The Tools SAD (§2.6, §2.7) defines ONE content hash per pack: BLAKE3 of the
// pack's *canonical source tree*. It appears in TWO emitted artifacts and MUST
// be byte-identical in both — this is the three-way content-hash tie (server DB
// world_manifest.content_hash, client pack.manifest.json content_hash, source
// tag) the deployment view depends on (§2.6: "The same content_hash appears in
// the IF-5 pack manifest"):
//
//   * emit-sql (IF-4) writes it into the world_manifest row worldd verifies.
//   * emit-pck (IF-5) writes it into pack.manifest.json the client verifies.
//
// If the two stages computed the hash independently, any drift (a different
// file-ordering, a different canonicalization, an off-by-one in the framing)
// would silently break pack verification — a P0 tools bug (SAD §5). So the
// computation lives here, once, and both stages call it. This is the single
// source of truth for "what content produced this build".
//
// CANONICAL FORM (must never change without a schema-version bump): the hash is
// taken over every parsed file whose namespace is `ns`, iterated in sorted
// rel_path order, each contributing the framed bytes
//   "<rel_path>\0<canonical-yaml>\0"
// where <canonical-yaml> is the file re-serialized through YAML::Emitter (so the
// hash is whitespace/comment/quote-style insensitive and ties to exactly what
// the compiler parsed, not the on-disk byte soup). Pack manifests contribute via
// their declared `namespace_`; content + asset files via their id prefix.

#ifndef MCC_STAGES_CONTENT_HASH_H
#define MCC_STAGES_CONTENT_HASH_H

#include <map>
#include <ostream>
#include <string>

#include "stages/model.h"

namespace mcc::stages {

// Compute the per-namespace content hash for every pack in `model`. Returns a
// map: namespace -> 64-lowercase-hex BLAKE3 of that pack's canonical source
// tree. A namespace with no parseable files does not appear in the map; callers
// substitute the all-zero hash for a pack whose manifest was found but whose
// tree hashed to nothing. Deterministic: input files are sorted by rel_path, so
// the same source tree always yields the same digest on every platform.
std::map<std::string, std::string> compute_pack_hashes(const model::ContentModel& model);

// `mcc content-hash [dir] [--json]` — surface the per-pack content_hash on stdout
// (the pack-level digest over the canonicalized content set, spec §3, the
// client<->server agreement token). Runs discover + parse over `content_dir`,
// then prints one namespace -> 64-hex-digest line per pack (or a JSON object with
// --json). This is the same digest emit-sql/emit-pck stamp into their manifests
// (the three-way tie), exposed directly so it can be diffed/pinned. Because the
// pack manifest is part of the hashed tree, a change to any pack field — including
// the new compatibility_version / theme — flows into the digest. Deterministic:
// stable across runs on unchanged content, and changes iff the content changes.
// Returns 0 on success, 2 when `content_dir` cannot be scanned.
int content_hash_report(const std::string& content_dir, bool as_json, std::ostream& out,
                        std::ostream& err);

}  // namespace mcc::stages

#endif  // MCC_STAGES_CONTENT_HASH_H
