# Codex tooling — `Meridian.Yaml.Cst`

CST-preserving YAML layer for the **Codex** content editor, per
[`docs/sad/tools-sad.md` §6.2](../../docs/sad/tools-sad.md) (the editor's content-editing model).
Issue #126.

The user-facing acceptance contract for building the linked Chibi demo across Codex, Forge,
Meshy intake, `mcc`, and the runtime is
[`docs/chibi-mvp-authoring-acceptance-guide.md`](../../docs/chibi-mvp-authoring-acceptance-guide.md).
It labels current capabilities and temporary fallbacks separately from the completed-tool target.

## Why a CST, not a normal parse → re-emit

`mcc fmt` (#155, `tools/mcc/src/stages/format.cpp`) is a **canonical formatter**: it parses
content YAML into a logical value tree and re-emits it under fixed rules. That is exactly the
wrong tool for the editor's save path — it normalizes quoting, folds block scalars, re-attaches
comments heuristically, and rewrites layout. Codex edits files that **humans also hand-edit**
(comments, deliberate formatting, key order), so a save must change **only** the value the user
edited and leave every other byte untouched.

This library provides that surgical-edit substrate. It underpins the NPC/item editors
(#128/#129).

## Language & approach (SAD-grounded)

- **Language: C# / .NET 8 LTS.** The SAD names Codex as C#/.NET 8 + Avalonia (§6) and specifies
  this layer by name — `Meridian.Yaml.Cst` — as a decision (§6.2, §10.1). This is that library.
- **Approach: verbatim byte-splicing over YamlDotNet's low-level token stream.** §6.2 mandates
  building on `Scanner(…, skipComments: false)` rather than YamlDotNet's object (de)serializer
  (which loses comments, key order, and scalar styles).

  The scanner yields every scalar/structural token with a precise **character span**
  (`Start.Index`/`End.Index`) into the source. We build a concrete syntax tree
  (`CstNode`: Mapping / Sequence / Scalar) where each node is anchored to its source span. The
  edit model is a set of non-overlapping **splices** `(span, replacement)` against the original
  text:

  - **Round-trip identity** — zero splices ⇒ output *is* the original string (byte-identical).
  - **Surgical value edit** — `SetValue(path, value)` replaces only the target scalar's span; all
    other bytes (comments, whitespace, punctuation, quotes, key order) are emitted verbatim.
  - **Add / remove key** — insert/delete exactly one entry line, matching sibling indentation;
    surrounding layout is preserved.

  This is strictly stronger than reconstructing an emitter: untouched regions are *literally the
  original bytes*, so there is nothing to reconstruct and nothing to lose.

## Public API

```csharp
var doc = YamlDocument.Load("content/core/npcs/kobold_miner.npc.yaml");

doc.GetValue("stats.health");            // "120"
doc.SetValue("stats.health", "150");     // surgical value edit
doc.RemoveKey("stats.armor");            // delete an entry line
doc.AddKey("stats", "mana", "60");       // append an entry line

doc.Save(path);                          // only edited spans changed; rest byte-identical
string text = doc.ToText();              // same, as a string
```

Path grammar: mapping keys separated by `.`, sequence indices as `[n]`
(e.g. `ai.abilities[0].priority`).

### Guarantees (in §6.2's order of strength)

1. **Semantic identity** on round-trip — the logical value tree is unchanged except for the
   edited field. Scalar style is preserved; a replacement value is quoted only when plain form
   would break syntax or change the resolved YAML type (type stability).
2. **Byte identity for untouched regions** — proven against the whole `content/core` corpus.
3. **Key order preserved**; new keys are appended after the last sibling.

**Escape hatch:** a document the CST cannot faithfully represent raises `YamlCstException`; the
editor opens such a file read-only ("reformat with mcc fmt to edit") rather than risk silent
destruction (§6.2, TS-2).

## Build & test (macOS / Linux / Windows)

```bash
uv sync --locked
dotnet restore tools/codex/Meridian.Codex.sln
dotnet build tools/codex/Meridian.Codex.sln --no-restore -m:1 --disable-build-servers
tools/codex/check-warning-clean.sh
dotnet test tools/codex/Meridian.Codex.sln --no-restore # round-trip, surgical-edit, schema-form, and headless UI coverage
```

Tests load the real `content/core` YAML files as fixtures.

The normal restore is an **online security-audit path**. `NU1900` (audit data
unavailable) and `NU1901`-`NU1904` (a vulnerable dependency was found) fail the
restore instead of being buried among compiler warnings. Fix network/proxy/TLS
access to `https://api.nuget.org/v3/index.json` for `NU1900`; investigate and
upgrade the reported package for `NU1901`-`NU1904`.

For a deliberately disconnected build, first populate the NuGet and uv caches on
a connected machine, then select the offline path explicitly:

```bash
uv sync --locked --offline
dotnet restore tools/codex/Meridian.Codex.sln \
  --ignore-failed-sources -p:NuGetAudit=false
UV_OFFLINE=1 tools/codex/check-warning-clean.sh
```

Disabling `NuGetAudit` is confined to that explicit restore; it is not a warning
suppression and it makes no security claim. Run the online restore before release
or dependency changes. If `uv` reports a cache permission error, repair ownership
of its reported cache directory or point `UV_CACHE_DIR` at a writable directory;
do not skip schema verification.

Every Codex compile runs the schema generator in `--check` mode. Missing Python,
an unusable `uv` cache, or generated-model drift therefore fails the build before
C# compilation. After changing `/schema/content` or the generator, refresh and
verify the checked-in artifacts explicitly:

```bash
uv run python -m tools.schema_gen
uv run python -m tools.schema_gen --check
```

Code/compiler warnings are separate from audit availability and toolchain/cache
failures: the first are fixed in source, the second by restoring online access (or
choosing the documented offline restore), and the third by repairing the local
Python/uv environment. None is allowed to warn and continue.

## Content Pack Workspace

Codex now opens a pack directory (`content/<namespace>/`) as its unit of work.
The **Pack** screen can create a minimal manifest, reopen it, edit namespace and
version contracts, manage exact-pinned dependencies, and persist recent pack
locations. Field diagnostics mirror `pack.schema.yaml` for namespace, semver,
content-schema, compatibility, Godot engine pin, dependency, and license values.

Pack validation evaluates the checked-in Draft 2020-12 `pack.schema.yaml` with
the same merged `common.defs.yaml` and `skeleton.defs.yaml` contract used by
`validate_content.py`. Those files are embedded at build time, so validation is
fully offline at runtime and cannot fetch or silently substitute a remote schema.
Differential tests run both Codex and the TLS-07 reference validator over
pack manifests. Their corpus covers the PyYAML SafeLoader scalar boundaries that
can affect pack fields: exact boolean/null spellings, numeric prefix and exponent
forms, dates/timestamps, quoted controls, and explicit scalar tags. This is a
pack-manifest compatibility boundary, not a claim of general PyYAML equivalence.
This uses [JsonSchema.Net 7.2.3](https://www.nuget.org/packages/JsonSchema.Net/7.2.3)
(MIT license, pinned NuGet package); MIT is compatible with Meridian's
Apache-2.0 distribution. `libmccore` remains the intended shared validation
boundary once its currently deferred JSON Schema layer is implemented.

Existing `pack.yaml` files use `Meridian.Yaml.Cst`: scalar edits replace only the
edited token and dependency-list edits replace only that value node. Comments,
key order, quoting, and every unrelated byte remain unchanged. A clean external
edit reloads automatically; if disk and unsaved local edits both change, Codex
keeps the local form state, marks an external conflict, and blocks Save instead
of overwriting either side.

## Experimental generic schema form

Story #668 adds a reusable Draft 2020-12 form renderer backed directly by the
embedded content schemas and `Meridian.Yaml.Cst`. It supports strings, enums,
booleans, numeric ranges, nested objects, ordered arrays, optional/defaulted
fields, and object `oneOf` branches with destructive-change confirmation. A
schema construct the renderer cannot represent remains visible but read-only
with an instruction to edit that value in YAML.

Until the individual content editors migrate to the renderer, an opt-in internal
preview opens a schema-valid file without changing normal Codex startup:

```bash
dotnet run --project Meridian.Codex/Meridian.Codex.csproj -- \
  --schema-form ability /tmp/example.ability.yaml
```

Accepted schema names are `pack`, `npc`, `item`, and `ability` (or their full
`*.schema.yaml` filenames). Preview a disposable copy: Save writes
CST-preserving edits back to the supplied path.

To exercise the UI from this directory:

```bash
dotnet run --project Meridian.Codex/Meridian.Codex.csproj
```

> The library targets `net8.0` per the SAD. `RollForward=Major` lets the assemblies run cleanly on
> a machine that only ships a newer runtime (e.g. a macOS box with the .NET 9 runtime).
