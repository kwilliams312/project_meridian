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

**Deferred to later M0 tasks** (reported as stubs / not yet run): full JSON Schema 2020-12
validation, L004 intRange, L020 asset-sidecar existence, the semantic lints
(L034/L035/L052/L062), and the `link → bake → emit-sql / emit-pck` stages. Because JSON
Schema validation is deferred, a file the reference validator would reject at the `SCHEMA`
layer (and skip) still reaches mcc's structural lints — the two agree wherever the reference
actually reaches the structural layer.

## Dependency: yaml-cpp

The parse stage uses [yaml-cpp](https://github.com/jbeder/yaml-cpp) (Tools SAD §2). CMake
prefers the system package via `find_package(yaml-cpp)` (Homebrew: `brew install yaml-cpp`;
most distros and vcpkg ship a config), and falls back to a pinned `FetchContent` build
(`0.8.0`) so CI runners without the system library still build hermetically. Both paths
expose the same `yaml-cpp::yaml-cpp` target. Zero other new dependencies.

## Build

```sh
cmake --preset default        # configure into tools/mcc/build/
cmake --build build           # -> tools/mcc/build/mcc
./build/mcc check ../../content
```
