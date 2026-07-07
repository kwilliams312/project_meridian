// tools/mcc/src/stages/emit_sql.h — emit-sql stage (Tools SAD §2.6, IF-4).
//
// After discover/parse -> validate -> link (refs resolved + IF-9 numeric ids
// allocated), emit-sql walks the linked content model and produces the world DB
// DML: a deterministic SQL script that populates the content tables
// (schema/sql/world/*.sql) with every entity keyed by its IF-9 numeric id, PLUS
// the one `world_manifest` row that worldd's boot (#89) reads + verifies.
//
// CONTRACT (SAD §2.6 + schema/sql/world/ is the IF-4 contract):
//   * DML is a FULL deterministic dump: tables in DDL declaration order, rows
//     ordered by primary key, no wall-clock timestamps, no AUTO_INCREMENT
//     reliance (all keys come from IF-9). Same source + same mcc => byte-identical
//     SQL (SAD §5 determinism; nondeterminism is a P0 tools bug).
//   * String content ids (core:npc.x) are NEVER emitted as keys — they are
//     resolved to their IF-9 numeric id via the link stage's idmap. Content refs
//     (*Ref) and asset refs (art/mus/sfx/amb) become numeric *_id columns.
//   * FK ordering: the emitted script brackets the inserts with
//     SET FOREIGN_KEY_CHECKS=0 ... =1 (mirroring the schema loader's
//     00_manifest.sql / 90_gossip.sql preamble/epilogue) so forward references
//     between tables load cleanly regardless of insert order.
//   * world_manifest row: exactly the seven columns worldd reads — pack_namespace,
//     pack_version, id_band, content_hash (BLAKE3 of the canonical source tree,
//     64 lowercase hex), schema_version (content_schema_version), mcc_version,
//     built_at. content_hash uses the vendored BLAKE3 (hash/blake3.h); worldd
//     only requires 64 lowercase-hex + schema_version == its supported major (1).
//
// The stage is a pure function over already-parsed inputs (no disk writes here;
// the CLI decides where the string goes — stdout or --out <file>). It appends
// diagnostics for any content it cannot map (e.g. a ref with no idmap entry).

#ifndef MCC_STAGES_EMIT_SQL_H
#define MCC_STAGES_EMIT_SQL_H

#include <string>

#include "stages/diagnostics.h"
#include "stages/link.h"
#include "stages/model.h"

namespace mcc::stages {

// Build metadata stamped into the world_manifest row. `built_at` is passed in
// (not read from the wall clock) so the emit is reproducible: the CLI defaults it
// to a fixed epoch and lets a caller override with --built-at for a real build.
struct EmitSqlOptions {
    std::string mcc_version = "0.0.0";               // compiler version -> mcc_version
    // Deterministic build timestamp (DATETIME text "YYYY-MM-DD HH:MM:SS"). NOT the
    // wall clock — a fixed default keeps double-build output byte-identical; a real
    // nightly build passes its own value via --built-at (documented in the CLI).
    std::string built_at = "1970-01-01 00:00:00";
};

// The result of an emit-sql run: the SQL text plus row/table counts for the
// summary line. `ok` is false if any diagnostic error was appended.
struct EmitSqlResult {
    std::string sql;             // the full deterministic DML script
    std::size_t manifest_rows = 0;   // world_manifest rows emitted (one per pack)
    std::size_t content_rows = 0;    // total content-table rows emitted
    bool ok = true;
};

// Emit the world DB DML for a linked content model. `linked` supplies the per-pack
// idmaps (string id -> numeric id) used to key every row and resolve every ref.
// Diagnostics are appended for unmappable refs/ids (rule "E001"). Deterministic:
// packs, entities, and rows are emitted in sorted (id) order.
EmitSqlResult emit_sql(const model::ContentModel& model, const LinkResult& linked,
                       const EmitSqlOptions& opts, diag::Diagnostics& diags);

}  // namespace mcc::stages

#endif  // MCC_STAGES_EMIT_SQL_H
