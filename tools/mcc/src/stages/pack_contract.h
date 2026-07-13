// tools/mcc/src/stages/pack_contract.h — pack contract checks (Tools SAD §2.4,
// pack-contract spec §3): the idmap append-only lint and the breaking-change
// `mcc diff` classifier. These formalize the "ids are append-only, never
// renumbered" discipline (roster.h) as a tool-enforced content contract and give
// the client<->server compatibility gate its authoring-time signal.
//
// Kept in PARITY with tools/validate_content.py's L016 append-only lint: both
// tools flag the same renumber/reuse footprints so the mcc-parity CI job holds.

#ifndef MCC_STAGES_PACK_CONTRACT_H
#define MCC_STAGES_PACK_CONTRACT_H

#include <ostream>
#include <string>

#include "stages/check.h"        // DiagFormat
#include "stages/diagnostics.h"
#include "stages/idmap.h"

namespace mcc::stages {

// L016 — idmap.lock append-only discipline (spec §3). Scan ONE parsed idmap for
// the renumber/reuse footprints that violate append-only ordering, appending an
// L016 error per violation to `diags` (rel = the lock's repo-relative path):
//   * a local index assigned to two distinct live ids (a renumber collision);
//   * a live index that reuses an index frozen in `retired`;
//   * an id present in both `map` and `retired`;
//   * index 0 (reserved as the null/unset id).
// Deterministic: ids are visited in sorted (std::map) order. The clean corpus
// produces no L016 — this only fires on a hand-edited renumber/reuse.
void scan_idmap_append_only(const idmap::IdMap& m, const std::string& rel,
                            diag::Diagnostics& diags);

// `mcc diff <old-pack> <new-pack>` — classify every change between two content
// packs (each a content root dir) as ADDITIVE (new ids, new optional fields) or
// BREAKING (a removed id, a renumbered id, a removed field/capability, a changed
// id type), per spec §3. Writes an actionable report to `out` naming the exact
// ids/fields. A BREAKING change whose new `compatibility_version` was not bumped
// above the old one is itself reported as an actionable error (the boot-gate
// contract). Returns:
//   0  — additive-only (or identical): the new pack boots freely against the old;
//   1  — at least one breaking change (non-zero exit is the CI/authoring gate);
//   2  — a content root could not be scanned (usage/environment error).
int diff_packs(const std::string& old_dir, const std::string& new_dir,
               DiagFormat format, std::ostream& out, std::ostream& err);

}  // namespace mcc::stages

#endif  // MCC_STAGES_PACK_CONTRACT_H
