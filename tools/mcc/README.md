# `mcc` — Meridian Content Compiler (TLS-01)

C++20 static CLI, zero Godot dependency except resource import. One compiler, two outputs
(TD-06): world-DB SQL (IF-4) for the server + client `.pck` packs (IF-5). Runs on Windows
x64 and Linux x64 from one CMake tree.

**Contract:** [Tools SAD §2](../../docs/sad/tools-sad.md) · **Schemas:** [`/schema/content`](../../schema/content)

Pipeline is a DAG of pure stages: `discover/parse → validate → link → bake → emit-sql / emit-pck`.
CLI (v1): `mcc build [--full|--watch] | check | fmt | diff | pack | install | uninstall | migrate | idmap verify|reassign`.

Determinism is a hard requirement — same source + same `mcc` ⇒ byte-identical SQL and
content-identical `.pck` (double-build hash-compared in CI).

## Implemented so far (#118)

**discover / parse** (Tools SAD §2.1) and the **structural lint layer** of **validate**
(§2.2). `mcc check` walks a content root, parses every file with yaml-cpp, and runs the
structural lints; `mcc build` runs `check` first and aborts if it fails.

- `mcc check [dir]` — default `dir` is `./content`. Exit `0` clean, `1` on any error.
- `mcc check --diag-format=json` — structured diagnostics for Codex / Forge / CI (§2.2).

### Validation-as-you-type — single-file mode (#130, SAD §6.3 / TLS-06)

For the tight editor loop, validate **one** file in isolation instead of the
whole corpus:

- `mcc check --file <path> [dir] [--diag-format=json]` — parse + the
  single-file-computable lints (`L001` filename/envelope, `L003` id type
  segment, `PARSE`) over exactly one file. **Fast** — no directory walk, no
  corpus load; sub-100ms per file (measured ~3–4ms incl. process spawn on the
  Zone-01 corpus), and O(1) as the tree grows to the R2 10k–50k-entity scale
  where a full `check` is O(N). The optional `[dir]` (default `./content`) is
  used **only** as root context to make the diagnostic path repo-root-relative,
  matching a full `check`'s paths.
- `mcc check --file <path> --watch` — re-validate on every save, streaming a
  fresh diagnostics render to stdout until Ctrl-C (the M1 half of the TLS-06
  loop; the fs-event + Forge-attached session upgrade is M2, SAD §6.5).

**Graceful cross-file degradation.** A single file cannot resolve references
into other files, so the cross-file rules are **not** run as errors — they are
reported as **deferred `info` notes** (never a false `L011`/`L002`/`L010`
error):

- each content reference → one `L011` **info** note ("not resolved single-file —
  run `mcc check` to confirm"), carrying the field's json-path in `where`;
- `L002` (pack-namespace ownership) and `L010` (duplicate id) → one deferred
  `info` note each.

`info` notes never fail the run: exit stays `0` unless a real single-file
**error** (L001/L003/PARSE) is found. This is exactly the contract an editor /
LSP layer consumes to show live errors while the author types, without
false-flagging every not-yet-saved reference.

### JSON diagnostic shape (the editor/LSP contract)

`--diag-format=json` (both full `check` and `--file`) emits one stable object:

```json
{
  "schema": "mcc-diagnostics@1",
  "mode": "file",                     // "check" (full corpus) | "file" (single-file)
  "ok": true,
  "error_count": 0,
  "warning_count": 0,
  "info_count": 2,
  "diagnostics": [
    { "rule": "L011", "severity": "info",
      "file": "content/core/quests/x.quest.yaml",
      "where": "$.giver",             // schema json-path → maps to a form control
      "line": 0, "col": 0,            // 1-based source coords; 0 = unknown
      "message": "reference 'core:npc.y' not resolved single-file — run 'mcc check' to confirm it exists" }
  ]
}
```

`severity` is `error` | `warning` | `info` (only `error` affects `ok`/exit code).
The shape is **additive**: the `#118` fields (`ok`, `error_count`,
`warning_count`, `diagnostics[].rule/severity/file/where/message`) are unchanged;
`schema`, `mode`, `info_count`, and per-diagnostic `line`/`col` are new. Consumers
ignore unknown keys.

**Lints (parity with `tools/validate_content.py`, the reference validator):**

| Rule | Checks |
|------|--------|
| `PARSE` | malformed / empty / non-mapping YAML (reported, never a crash) |
| `L001` | filename ↔ declared envelope `schema:` type |
| `L002` | id namespace == owning pack's namespace |
| `L003` | id type segment == the file's schema type |
| `L010` | no duplicate ids across the tree |
| `L011` | every content ref (`npc.`/`item.`/…) resolves to a defined id |

`mcc check` and `validate_content.py` produce **identical** messages on these rules
(verified message-for-message on schema-valid fixtures).

**fmt** (Tools PRD §2.1 / SAD §6.2) — the canonical YAML formatter. `mcc fmt`
rewrites a content file (or a whole tree) into one canonical form so diffs are
semantic, not cosmetic; `mcc fmt --check` is the pre-merge / pre-commit gate
(exit non-zero on drift, writes nothing). Two property-tested guarantees:
**idempotent** (`fmt(fmt(x)) == fmt(x)`) and **semantics-preserving**
(`parse(fmt(x))` node-identical to `parse(x)`, resolved scalar types included).
Canonical rules — envelope-first key order, two-space block indent, preserved
flow style for leaf collections, explicit-quote preservation, comment
preservation, single trailing newline — are documented in
[FORMAT.md](FORMAT.md).

- `mcc fmt [path]` — format in place (default `./content`).
- `mcc fmt --check [path]` — report drift, exit `1` if any file is non-canonical.

**emit-sql** (Tools PRD §2.2 / SAD §2.6, IF-4) — emits the **world DB SQL**: the
deterministic DML that populates the content tables (`schema/sql/world/*.sql`)
with every entity keyed by its IF-9 numeric id, plus the one `world_manifest` row
that `worldd`'s boot (issue #89) reads and verifies. This is what makes real
content flow `mcc → world DB → worldd serves it`.

- `mcc emit-sql [dir]` — emit the SQL to **stdout** (diagnostics to stderr).
- `mcc emit-sql [dir] --out world.sql` — write the SQL to a file (diagnostics to
  stdout). `mcc build` writes `build/world.sql` this way.
- `mcc emit-sql [dir] --built-at "2026-07-06 03:00:00"` — stamp a real build
  timestamp into `world_manifest.built_at`. The default is a **fixed epoch**
  (`1970-01-01 00:00:00`) so double-build output is byte-identical (SAD §5
  determinism); pass a real value for a nightly build.

emit-sql consumes the **existing `idmap.lock`** read-only — run `mcc link` or
`mcc build` (which allocate ids) first; an out-of-date lock is an `L015` error.

**Content → SQL mapping** (mirrors `schema/sql/world/*.sql`, the co-reviewed IF-4
DDL contract): nested scalar objects flatten to prefixed columns (`stats.*` →
`stat_*`), `intRange {min,max}` → `<field>_min`/`_max`, `position {x,y,z}` →
`pos_x/y/z`, arrays of objects → child tables keyed `(parent_id, ordinal)`,
`oneOf` variant arrays (`effects`, `objectives`, loot entries) → one discriminated
child table, and every `*Ref` / asset ref → its IF-9 numeric `*_id` column. The
dump is bracketed by `SET FOREIGN_KEY_CHECKS = 0 … = 1` (like the schema loader)
so forward references load cleanly. Tables emit in DDL declaration order; rows in
primary-key order; no wall-clock timestamps → stable diffs.

**Content hash** (`world_manifest.content_hash`): **BLAKE3** of the pack's
canonical source tree (each parsed content/asset/pack file, in sorted rel-path
order, contributing `rel_path\0<canonical-yaml>\0`), rendered as 64 lowercase-hex
chars — exactly the shape `worldd`'s `verify_world_manifest` accepts
(`kContentHashHexLen = 64`, `schema_version == kSupportedContentSchemaVersion = 1`).
BLAKE3 is **vendored** (`src/hash/blake3.{h,cpp}`, no external hashing dep, SAD
§2.6 decision table) and byte-verified against the published BLAKE3 test vectors.

**chunk-emit** (Tools SAD §3, IF-6, #553) — emits a **procedural Zone-01 fixture
pack**: the v0 slice of the real mcc chunk stage in *fixture mode*, so client
stories A–E (chunk streamer, epic #22) can build + test against a
contract-accurate pack before Forge v1 (#26) / the chunk-exporter (#315) land. It
goes through the **real** schemas (`schema/chunk/chunk.fbs` `ServerChunk` +
`chunk-manifest.schema.yaml`) and the **real** vendored BLAKE3, so the fixture is
byte-shaped exactly like production — it can never rot or diverge from real mcc.

- `mcc chunk-emit` — emit the IF-6 manifest to **stdout**.
- `mcc chunk-emit --out <dir>` — write the whole fixture under
  `<dir>/meridian/<ns>/chunks/<zone>/`.
- `mcc chunk-emit --grid 4 --zone core:zone.zone01 --origin-x -512 --origin-z -512`
  — tune the N×N grid / zone / non-zero grid origin.

For the default `core:zone.zone01`, a **3×3** grid centred on negative indices
(Zone-01 spawns at x ≈ −300, §3.1) it produces, in one directory:

- **`zone01.chunks.json`** — the IF-6 manifest (validates against
  `chunk-manifest.schema.yaml`): non-zero origin, `chunk_size_m` 128, grid bounds,
  `far_ring`, and one sparse entry per chunk with C2 asset-ID refs
  (`scene`/`proxy`/`server`), an `aabb`, a load-order `priority`, `deps`, and the
  per-chunk **BLAKE3-256** (`blake3:<64hex>`) over **both** payloads.
- **`<cx>_<cz>.chunk.bin`** — a real `ServerChunk` FlatBuffer (baked via the
  flatc-generated `chunk.fbs` bindings) whose 129×129 f32 `Heightfield` is
  deliberately **non-flat** (a ramp + shallow bowl, a pure function of zone-local
  world coords so shared edges join exactly) — the contract-critical artifact the
  client reads the heightfield from (Q1(a)); a flat-vs-sloped bug is catchable.
- **`<cx>_<cz>.tscn` + `<cx>_<cz>.proxy.tscn`** — minimal but valid Godot text
  scenes (a `MeshInstance3D` whose box Y-extent tracks the chunk's height span, so
  the placeholder is visibly non-flat per chunk). They carry the `.tscn` extension
  (not `.scn`) so Godot's ResourceLoader routes them to the TEXT loader and can
  instance them as-shipped — a text payload under a `.scn` name is rejected by the
  BINARY loader (#579). A full baked terrain mesh is Forge/#315 real-content work,
  out of scope for the v0 fixture.
- **`zone01.assets.json`** — the IF-8 asset-ID table so the C2 refs resolve
  (id → band-0 local index → IF-9 numeric id, allocated lexicographically).
- **`pack.manifest.json` + `pack.contents.jsonl`** — the IF-5 client pack (same
  `meridian/pack-manifest@1` shape as **emit-pck**) that **includes the server
  `.chunk.bin`** per chunk (Q1(a)) alongside the scene/proxy/dep entries.

Deterministic: identical options ⇒ identical bytes (pure integer/float geometry,
no libm transcendentals, no wall-clock). The round-trip ctest (`mcc-chunk-emit-unit`)
proves the manifest carries every schema-required field, each `.chunk.bin` passes
the FlatBuffers verifier as a non-flat 129×129 `ServerChunk`, the recomputed
per-chunk BLAKE3 matches the manifest, and the pack covers every referenced asset.

**Deferred to later M0 tasks** (reported as stubs / not yet run): full JSON Schema 2020-12
validation, L004 intRange, L020 asset-sidecar existence, the semantic lints
(L034/L035/L052/L062), and the `bake` / `emit-pck` stages. Because JSON
Schema validation is deferred, a file the reference validator would reject at the `SCHEMA`
layer (and skip) still reaches mcc's structural lints — the two agree wherever the reference
actually reaches the structural layer.

## Dependencies: yaml-cpp + FlatBuffers

The parse stage uses [yaml-cpp](https://github.com/jbeder/yaml-cpp) (Tools SAD §2). CMake
prefers the system package via `find_package(yaml-cpp)` (Homebrew: `brew install yaml-cpp`;
most distros and vcpkg ship a config), and falls back to a pinned `FetchContent` build
(`0.8.0`) so CI runners without the system library still build hermetically. Both paths
expose the same `yaml-cpp::yaml-cpp` target.

The **chunk-emit** stage (IF-6) bakes the real `schema/chunk/chunk.fbs` `ServerChunk`
payload, so mcc also needs **FlatBuffers** — the `flatc` compiler (run at build time to
generate `chunk_generated.h`, no committed generated code) plus the header-only C++
runtime. Same find-first / fetch-fallback strategy as `server/libs/proto` (Homebrew:
`brew install flatbuffers`; pinned FetchContent tag `v25.12.19` as the CI safety net).
BLAKE3 is vendored (`src/hash/blake3.{h,cpp}`); no other new dependencies.

## Build

```sh
cmake --preset default        # configure into tools/mcc/build/
cmake --build build           # -> tools/mcc/build/mcc (+ mcc-tests)
./build/mcc check ../../content
./build/mcc check --file ../../content/core/items/rusty_pickaxe.item.yaml ../../content --diag-format=json
./build/mcc fmt --check ../../content   # canonical-form gate
./build/mcc link ../../content --report # allocate IF-9 ids + print the idmap
./build/mcc emit-sql ../../content --out world.sql   # IF-4 world DB SQL + manifest
./build/mcc chunk-emit --out fixtures                # IF-6 procedural Zone-01 fixture pack
ctest --test-dir build --output-on-failure   # unit tests (fmt, single-file, link, emit-sql, chunk-emit)
```

### emit-sql tests + the DB-backed integration test

`ctest` runs `mcc-emit-sql-unit` (BLAKE3 vs published vectors, well-formed SQL, a
`world_manifest` row with a 64-hex hash + `schema_version = 1`, determinism, and
`*Ref` → numeric-id resolution) always. The `mcc-emit-sql-db` test **loads the
real world DDL + the emitted SQL into a live MariaDB** and asserts the content
tables and `world_manifest` are populated — it is **env-guarded** (parity with the
worldd DB tests) and SKIPs unless `MERIDIAN_DB_*` (or `MERIDIAN_WORLDDB_*`) point
at a MariaDB with a `mariadb`/`mysql` client on `PATH`:

```sh
export MERIDIAN_DB_HOST=127.0.0.1 MERIDIAN_DB_PORT=3306 MERIDIAN_DB_USER=root
ctest --test-dir build -R mcc-emit-sql-db --output-on-failure
```
