# Task 2 report — MeridianContentDB + catalog-driven char-create pickers (②/T2, #539 / #477)

## Status: implemented, gates green, PR opened (NOT merged)

Branch: `feat/assembler-t2-contentdb` (from `origin/dev`).

## Step 1 discovery (the pivotal finding)

`emit_pck.cpp` does **not** serialize any type's field data. The M0 pck is a
directory manifest — `pack.manifest.json` + `pack.contents.jsonl` carry only
`id → numeric_id → resource-path → hash`. The `res://.../tables/<type>.bin` paths
are **declarative strings** (no Godot importer at M0). `appearance` and `dye` are
correctly classified (discover.cpp: client-only types, emit no world.sql, flow
into the pck as manifest entries) — but as path/hash entries only. Items carry no
`visual.worn`, catalogs no presets, dyes no color anywhere client-readable.

Therefore, to satisfy the BINDING API, the "minimal mcc emit needed" (spec §3) was
required: a new **`pack.data.json`** emit carrying the appearance/item-worn/dye
FIELDS, keyed by IF-9 numeric id.

Boot path: `MeridianPackMount` mounts `res://meridian/core`. No content was staged
into the client at M0. This story stages the emitted core pack there (first client
content), and `ContentDB` reads it (env `MERIDIAN_PACK_DIR` overrides for dev/CI).

## What changed

- **mcc (scope addition):** `emit_pck.{h,cpp}` emit `pack.data.json` (appearance,
  item.worn, dye — sorted, deterministic, dep-free JSON); `check.cpp` writes it;
  `content-build.sh` + `check-golden.sh` gate it (golden now 5 files); golden
  regenerated; `smoke.c` id_count 81→82; new unit test in `test_emit_pck.cpp`.
- **Content:** authored `content/core/appearance/ardent_male.appearance.yaml`
  (references committed blockout `core:art.char.ardent.male.skeleton/.base`;
  placeholder presets → base asset; no new sidecars). idmap.lock: id 82 appended.
- **Client:** `content/content_db.gd` (autoload `ContentDB`, binding API +
  `numeric_id_for` helper); `content/content_db_verify.gd`; `project.godot`
  autoload; staged pack at `client/project/meridian/core/`; `char_select.gd`
  pickers catalog-driven + content-missing state; `char_select_verify.gd` extended.

## Binding API numeric ids (id_band 0): rusty_pickaxe=20, russet=78, appearance=82.

## Evidence (run in this worktree)

- `uv run pytest -q` → **316 passed**.
- `uv run tools/validate_content.py` → OK (83 files).
- `uv run tools/check_traceability.py` → OK.
- `mcc check ./content` → OK (83 files); `mcc fmt --check` catalog → canonical.
- mcc `ctest` → **9/9 passed** (incl. new emit-pck data_json test, updated smoke).
- `check-golden.sh` → golden match + determinism green.
- `content-build.sh` → determinism + IF-4/IF-5 hash tie green.
- **ContentDB data-contract simulated in Python** against the staged pack:
  catalog(1,0).body_model=core:art.char.ardent.male.base; worn(20).attach.socket
  =main_hand; dye_color(78)=#8a4b2d; model_path resolves; sentinels empty. ALL PASS.

## NOT verified here (lead must rerun)

No Godot editor in this environment (needs the pinned engine via fetch-deps + a
GUI seed for the #283 MoltenVK workaround). The headless `*_verify.gd` runs are
for the lead. Canonical invocation (content_db_verify needs NO GDExtension build):

```
$GODOT --headless --path client/project --script res://content/content_db_verify.gd
$GODOT --headless --path client/project --script res://scenes/charselect/char_select_verify.gd  # needs framework (MeridianNetThread)
```

## Concerns / notes for review

- **Staged pack drift:** `client/project/meridian/core/*` is a committed copy of the
  mcc emit (M0 stand-in — no automated client content-staging exists yet). Regenerate
  via `scripts/content-build.sh` then copy `build/content-out/pck/meridian/core/*`.
- `model_path(id)` takes an untyped param to honor "asset_id_or_numeric" (accepts a
  String content id OR int numeric) — a superset of the written `: int` signature.
- TDD note: the Godot verify could not be run RED-first here; the data contract was
  instead proven via the Python simulation above. Verify logic is correct-by-construction.

## Addendum — lead-verification fix (commit 731a0c0)

Lead's real-engine run found `content_db_verify.gd` failed to PARSE: 8x
"Cannot infer the type" — `var x := <expr>` where the expression is Variant
(all calls through the deliberately-untyped `db` binding return Variant; my
Python simulation validated the DATA, not the GDScript — that GDScript was
unverified here and is now flagged as such).

Fixed: all eight locals (cat, pick, w, russet, c, by_id, numeric, ok) now
explicitly typed. Defensively made the two `ContentDB.catalog()` call sites in
char_select.gd / char_select_verify.gd explicit (`var cat: Dictionary = ...`).

Audited (every GDScript added/modified in this story):
- content_db_verify.gd — fixed (8 sites); only `var _fails := 0` remains (int literal).
- content_db.gd — all `:=` infer from typed expressions (OS.get_environment→String,
  same-type String ternary, typed helpers →bool/String, FileAccess.open→FileAccess,
  String()/int() casts). JSON.parse_string results already plain `var x =`.
  MeridianRoster referenced identically to char_select.gd (global class_name) —
  no change, per lead (standalone-run cache issue, environmental).
- char_select.gd / char_select_verify.gd — no `:=`-on-Variant in added lines after fix.

Still cannot execute Godot in this env — the pushed branch is the gate for the
lead's re-run of both verify scripts.

## Addendum 2 — autoload-identifier fix (commit d1485c7)

Engine gate progressed to compile and found `Identifier not found: ContentDB`
(content_db_verify.gd:34): standalone `--script` mode never initializes
autoloads. The same latent break existed in char_select.gd (bare `ContentDB`),
which would have failed compile when char_select_verify loads the scene —
this project had ZERO autoloads before this story, which is why the verify
harness and autoloads had never collided.

Fix (keeps the real-app autoload path identical):
- `content_db.gd`: + `class_name MeridianContentDB`; + static `instance()`
  (autoload registers itself in `_init`; verify runs lazily create a shared
  instance); boot load moved `_ready` → `_init` (tree-independent).
- `content_db_verify.gd`: `const ContentDbScript := preload(...)` +
  `ContentDbScript.new()` (per lead instruction); `db.free()` before quit.
- `char_select.gd` / `char_select_verify.gd`: preload-by-path access
  (`ContentDb.instance()` / `ContentDbScript.instance()`) — chosen over the
  bare class name because a freshly-added class_name is invisible to a stale
  `.godot/global_script_class_cache.cfg` (the parse trap run-client.sh
  documents); preload is immune to both failure modes.
- `project.godot`: autoload comment documents the access pattern for T3/T4.

Audited: zero bare `ContentDB` identifier references remain in any .gd
(grep-verified); only the project.godot registration (real-app boot) and
path-based preloads. New constructs re-checked against GDScript 4 rules:
`static var` (4.1+, engine pin 4.7), unqualified `new()` in a static func
(legal — constructor is a static member), no-arg `_init` (required for
autoload/.new()), Variant returns assigned only to explicit-typed locals.

Documented invocation is unchanged (both scripts):
`$GODOT --headless --path client/project --script res://<script>` — the
content_db_verify header now states the full form. GDScript remains
UNVERIFIED by execution in this env; the pushed branch is the lead's gate.

Possible benign noise for the gate run: a "leaked instance" warning at exit
from char_select_verify (the lazily-created ContentDB Node is a
process-lifetime singleton, never freed) — does not affect the exit code.

## Addendum 3 — review fixes (commit 352570a)

Per .superpowers/sdd/task-2-review.md (Approved, two Important + one Minor):

**I-1 (race_overrides lossy):** render_worn now serializes overrides through the
SAME shared renderers as the base worn block (`render_model_list`/`render_hides`)
— full `{models:[{model,mirror}], hides:[]}` shape, keys sorted for determinism.
RED-first: extended the mcc data fixture with a non-empty override
(`emberkin: {models:[{model,mirror:x}], hides:[hands]}`) + byte-faithful
round-trip assertion → 3 checks FAILED before the fix, all green after
(emit-pck unit now 40/40). ContentDB's worn() passes the dict through verbatim
(surfacing automatic); its doc states the full override shape and
content_db_verify asserts `worn().has("race_overrides")`.

**I-2 (staged-pack staleness gate):** check-golden.sh (PR-gated via content-ci)
gained check (3): compares `client/project/meridian/core/{pack.manifest.json,
pack.contents.jsonl,pack.data.json}` against the fresh emit, failing with the
regeneration command in the message. `--update-golden` now refreshes golden AND
staged copy in lockstep (atomic pair). Negative-tested in this worktree: a
corrupted staged file fails the gate with the actionable message; restored →
green.

**N-1 (empty sheath_socket):** omitted when unauthored instead of `""`;
fixture item without sheath_socket asserts key omission (row-bounded check).

Golden + staged regenerated via --update-golden: **byte-identical to before**
(committed content has no overrides; both wearables author sheath_socket) —
no golden churn, gates prove currency.

Evidence: mcc ctest 9/9 · check-golden (3 checks incl. new staged gate) green ·
`uv run pytest -q` 316 passed · validate_content OK (83 files).
GDScript delta (verify assertion + doc comment) audited against GDScript 4
typing rules (`w.has(...)` on a typed Dictionary local — parse-safe); remains
execution-unverified in this env, per the standing boundary.
