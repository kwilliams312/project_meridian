// tools/mcc/src/stages/link.h — link stage (Tools SAD §2.3).
//
// After discover/parse + validate, the link stage:
//   1. Resolves the content reference graph — every `*Ref` (a string content id)
//      is resolved to its defining entity; a dangling ref (target missing) is an
//      L011 error with the offending file + json-path (SAD §2.3, parity with the
//      check-stage diagnostics).
//   2. Builds the BACKLINK index — the reverse graph (who refers to X) that
//      Codex find-usages, orphan-detection lint, and the incremental dependency
//      graph consume (SAD §2.3, §2.8).
//   3. Allocates IF-9 numeric ids via the per-pack idmap.lock allocator
//      (idmap.h): deterministic, append-only, stable across builds (SAD §2.4).
//
// The stage is a pure function over the parsed model: it appends diagnostics and
// returns a LinkResult (the graph + per-pack idmaps). Writing idmap.lock to disk
// is a separate, opt-in step (`--allocate-ids`; CI runs read-only, SAD §2.3).

#ifndef MCC_STAGES_LINK_H
#define MCC_STAGES_LINK_H

#include <map>
#include <string>
#include <vector>

#include "stages/diagnostics.h"
#include "stages/idmap.h"
#include "stages/model.h"

namespace mcc::stages {

// One resolved edge in the reference graph: `from` (a content id) refers to `to`
// (a fully-qualified content id) at `where` (a json-path) in file `file`.
struct RefEdge {
    std::string from;    // owning entity id (fully qualified, e.g. "core:quest.x")
    std::string to;      // referenced id (fully qualified, e.g. "core:npc.y")
    std::string file;    // owning file rel path (for diagnostics)
    std::string where;   // json-path of the reference (e.g. "$.giver")
};

// The output of the link stage.
struct LinkResult {
    // Every resolved (non-dangling) edge, in discovery order.
    std::vector<RefEdge> edges;
    // Backlink index: referenced id -> sorted list of ids that refer to it.
    // (Orphans — content with no backlinks — are simply absent as keys.)
    std::map<std::string, std::vector<std::string>> backlinks;
    // Per-pack idmaps after allocation, keyed by namespace (e.g. "core").
    std::map<std::string, idmap::IdMap> idmaps;
    // Number of dangling references found (each also emitted as an L011 error).
    std::size_t dangling_count = 0;
};

// Run the link stage over a parsed+validated model, appending diagnostics.
//
// `content_dir` is the scanned /content root: idmap.lock files are read from
// (and, when `allocate` is true, written to) /content/<namespace>/idmap.lock.
// When `allocate` is false the allocator still runs in memory (so ids are known
// to later stages) but nothing is written and any lock drift is reported as an
// L015 error (the CI read-only contract, SAD §2.3 / §2.2 L015).
//
// `emit_dangling` controls whether a dangling reference is reported as an L011
// error here. In the full DAG the validate stage has already emitted L011 for
// the corpus, so the `link_content` orchestrator passes false to avoid a
// duplicate diagnostic; a standalone `link` (or a unit test exercising link in
// isolation) passes true. Either way `LinkResult::dangling_count` is populated
// and dangling edges are excluded from the graph + backlinks.
//
// Returns the LinkResult; the caller's exit code comes from `diags`.
LinkResult link(const model::ContentModel& model, const std::string& content_dir,
                bool allocate, diag::Diagnostics& diags, bool emit_dangling = true);

}  // namespace mcc::stages

#endif  // MCC_STAGES_LINK_H
