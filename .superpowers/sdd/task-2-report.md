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
