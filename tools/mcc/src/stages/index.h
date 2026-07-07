// tools/mcc/src/stages/index.h — the ID index / typed reference pickers /
// backlinks surface (Tools SAD §2.3, §6 Codex `IdIndexVm`, TLS-03).
//
// #119's link stage builds the reference graph, the backlink index, and the
// IF-9 idmap internally. This stage SURFACES that in-memory model as a stable,
// queryable API — the data an editor (Forge/Codex) needs to:
//
//   * "resolve this id"            -> IdIndex::resolve(id)     (id -> type, numeric_id, file)
//   * "list all items"            -> IdIndex::by_type(type)    (the grouped id universe)
//   * "pick a valid ref of type X" -> IdIndex::pickable(type)  (typed reference picker)
//   * "what references this entity" -> IdIndex::backlinks(id)   (find-usages / reverse graph)
//
// The type expectation that makes a picker "typed" is grounded in the schema's
// ref grammar (schema/content/common.defs.yaml: npcRef/itemRef/…): every `*Ref`
// field encodes its EXPECTED target type in the id prefix (`npc.`, `item.`, …),
// which is the SAME grammar the link stage's `kRefRe` resolves. So a picker for
// an `itemRef` field returns exactly the ids of type `item` — a picker cannot
// offer an npc where an item is expected (schema README §References line 17).
//
// The index is a pure function of a linked ContentModel; it does not touch disk
// or emit diagnostics. Its JSON serialization is deterministic (ids sorted) so
// it is a stable editor-consumable contract, testable byte-for-byte.

#ifndef MCC_STAGES_INDEX_H
#define MCC_STAGES_INDEX_H

#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "stages/link.h"
#include "stages/model.h"

namespace mcc::stages {

// One entry in the ID index: a fully-qualified content/asset id with everything
// an editor needs to display + resolve it. `type` is the id's grammar prefix
// (npc, item, quest, ability, loot, vendor, spawn, zone, art, mus, sfx, amb).
struct IndexEntry {
    std::string id;           // fully-qualified, e.g. "core:item.rusty_pickaxe"
    std::string type;         // grammar type token, e.g. "item"
    std::string namespace_;   // owning pack namespace, e.g. "core"
    std::uint32_t numeric_id = 0;  // IF-9 numeric runtime id (0 = unmapped)
    std::string file;         // source file rel path (for jump-to-definition)
    bool is_asset = false;    // true for art/mus/sfx/amb (IF-8 sidecar) ids
};

// One referrer in a find-usages / backlink answer: WHO references the target,
// and WHERE (the referencing field's json-path in the referrer's file). One
// referrer entity may reference the same target from several fields — each is a
// distinct Referrer row so an editor can offer "jump to each usage".
struct Referrer {
    std::string from;    // the referring entity id (e.g. "core:quest.culling_the_kobolds")
    std::string file;    // the referring file rel path
    std::string where;   // json-path of the reference (e.g. "$.giver")
};

// The whole queryable index over one linked content root. Built once from a
// ContentModel + its LinkResult, then answered against in O(log n) / O(1).
class IdIndex {
public:
    // Build the index from a linked model. `model` supplies the id universe +
    // source files + types; `linked` supplies the IF-9 numeric ids and the
    // reference edges (for backlinks-with-path). Deterministic: entries are
    // stored sorted by id.
    static IdIndex build(const model::ContentModel& model, const LinkResult& linked);

    // All entries, sorted by id (the full universe: content + assets).
    const std::vector<IndexEntry>& entries() const { return entries_; }

    // The set of ids of a given type, sorted (e.g. by_type("item") -> every
    // item id). Empty when no id of that type exists. This is "list all X".
    std::vector<std::string> by_type(const std::string& type) const;

    // The DISTINCT types present, sorted (e.g. {ability, amb, art, item, …}).
    std::vector<std::string> types() const;

    // Resolve an id (bare or fully-qualified; a bare id defaults to the sole
    // pack namespace when unambiguous) to its entry, or nullopt if unknown.
    std::optional<IndexEntry> resolve(const std::string& id) const;

    // Typed reference picker: the VALID target ids for a ref FIELD whose schema
    // ref type is `ref_type` (one of npc/item/quest/ability/loot/vendor/zone or
    // art/mus/sfx/amb — the schema `*Ref` $defs). Returns exactly the ids of the
    // matching type, sorted — the candidates a picker UI would offer. For a
    // content-ref grammar type this is identical to by_type; the separate entry
    // point documents the schema-grounded intent (a picker, not a raw listing)
    // and normalizes the schema ref-def name ("itemRef" | "item" both accepted).
    std::vector<std::string> pickable(const std::string& ref_type) const;

    // Find-usages / backlinks: who references `id`, each with the referencing
    // field path. Sorted by (from, where) for determinism. Empty for an
    // unreferenced id (an orphan of the reference graph) OR an unknown id — use
    // resolve() to distinguish "exists but unreferenced" from "does not exist".
    std::vector<Referrer> backlinks(const std::string& id) const;

private:
    std::vector<IndexEntry> entries_;                 // sorted by id
    std::map<std::string, IndexEntry> by_id_;         // id -> entry
    std::map<std::string, std::vector<std::string>> by_type_;  // type -> sorted ids
    std::map<std::string, std::vector<Referrer>> backlinks_;   // target id -> sorted referrers
    std::vector<std::string> namespaces_;             // pack namespaces present, sorted
};

// ---- JSON serialization (the editor-consumable contract, deterministic) -----
//
// Each emitter writes a stable, pretty-printed JSON document to `out`. Ids are
// sorted; there is no wall-clock or environment state, so a re-run over
// unchanged content is byte-identical (SAD §5 determinism).

// The full index: {"schema","namespace(s)","type_count","id_count","types":{...},
// "ids":[{id,type,namespace,numeric_id,file,is_asset}, …]}. The `types` map
// groups id lists by type (list-all-X); `ids` is the flat resolvable universe.
void render_index_json(const IdIndex& index, std::ostream& out);

// A typed picker result for `ref_type`: {"schema","ref_type","type",
// "count","candidates":[{id,numeric_id,file}, …]}. `ok` is false (still valid
// JSON) when the ref type is unknown.
void render_pickable_json(const IdIndex& index, const std::string& ref_type,
                          std::ostream& out);

// A backlinks / find-usages result for `id`: {"schema","id","resolved"(bool),
// entry fields when resolved, "referrer_count","referrers":[{from,file,where}, …]}.
void render_backlinks_json(const IdIndex& index, const std::string& id,
                           std::ostream& out);

// ---- CLI orchestration (behind `mcc index` / `mcc pickable` / `mcc refs`) ---
//
// Each runs discover+parse+validate+link read-only over `content_dir` (never
// writes idmap.lock — inspection, not a build), builds the index, and renders
// the answer. `as_json` selects the JSON contract (stdout) vs a human table.
// Diagnostics from the front half render to `err`. Returns 0 on success, 1 when
// the front half reported an error (the index would be untrustworthy), 2 when
// `content_dir` cannot be scanned. `refs`/`pickable` additionally return 0 even
// when the id/type has no results (an empty answer is a valid answer).

int index_content(const std::string& content_dir, bool as_json, std::ostream& out,
                  std::ostream& err);

int pickable_content(const std::string& content_dir, const std::string& ref_type,
                     bool as_json, std::ostream& out, std::ostream& err);

int refs_content(const std::string& content_dir, const std::string& id, bool as_json,
                 std::ostream& out, std::ostream& err);

}  // namespace mcc::stages

#endif  // MCC_STAGES_INDEX_H
