# schema_gen — schema-driven struct generator (#117)

Generates a **typed content model** from the Content Schema v1
(`/schema/content/*.schema.yaml`) so the pipeline's tools and editor work with
typed structs instead of untyped YAML maps. One generator, one schema, two
targets — the single source of truth the Tools SAD calls for:

| Target | Output | Consumer | SAD |
|--------|--------|----------|-----|
| **C++20** | [`generated/cpp/content_types.gen.hpp`](generated/cpp/content_types.gen.hpp) | `mcc` / `worldd` typed decode (`namespace mcc::content`) | §2.1 "one C++ struct per schema type, generated from /schema/content … the same generator emits the Server's table-loading structs" |
| **C#** | [`generated/csharp/ContentTypes.g.cs`](generated/csharp/ContentTypes.g.cs) | Codex editor `Models/` (`namespace Meridian.Codex.Models`) | §6.1 `Models/ # generated from /schema/content (same generator as mcc's structs, C# target)` |

Today `mcc` parses content into untyped `yaml-cpp` nodes
(`tools/mcc/src/stages/model.h`: *"Full typed per-schema structs … arrive with
schema-generated decoding in a later M0 task"*). This is that generator; the
generated header is the typed model a future decoder targets.

## How the schema maps to types

| Schema construct | C++ | C# |
|---|---|---|
| `string` / `integer` / `number` / `boolean` | `std::string` / `std::int64_t` / `double` / `bool` | `string` / `long` / `double` / `bool` |
| enum (local or shared `$def`) | `enum class` | `enum` |
| `$ref` to `npcRef` / `itemRef` / `artRef` / … | strong id typedef (`NpcRef{std::string id;}`) | `readonly record struct NpcRef(string Id)` |
| shared struct `$def` (`intRange`, `position`) | nested `struct` (`IntRange`) | nested `record` |
| inline object | nested `struct` named `<Owner><Field>` | nested `record` |
| array | `std::vector<T>` | `IReadOnlyList<T>` |
| `oneOf` of tagged variants (quest objectives, ability effects) | one tagged `struct`: discriminator enum + optional variant fields | tagged `record` |
| required vs optional | plain member vs `std::optional<T>` | non-nullable `required` vs nullable `T?` |

Typed id references make the schema README's rule structural — *"an `item:`
field cannot accept an NPC ID"* — a `VendorRef` is not an `ItemRef`.

## Regenerate

```bash
uv run python -m tools.schema_gen           # rewrite the generated files in place
uv run python -m tools.schema_gen --check    # CI drift guard: exit 1 if stale
```

Output is **deterministic** (schema-declaration field order, stable derived
names, no host/wall-clock state) — regenerating twice is byte-identical.

## Drift guard

The checked-in generated files must match the schema. Two pytest tests enforce
this (run by `content-ci.yml`'s `uv run pytest -q` step):

- `tests/schema_gen/test_schema_gen.py::test_checked_in_output_matches_schema_no_drift`
- `tests/schema_gen/test_schema_gen.py::test_check_cli_passes_when_in_sync`

If you edit a schema under `/schema/content`, run
`uv run python -m tools.schema_gen` and commit the regenerated files, exactly
like the determinism golden corpus.

## Layout

```
tools/schema_gen/
  ir.py           # schema -> language-neutral IR (all schema-shape knowledge)
  emit_cpp.py     # IR -> C++20 header
  emit_csharp.py  # IR -> C# records
  generate.py     # CLI driver + --check drift guard
  generated/      # checked-in output (cpp/, csharp/)
tests/schema_gen/
  fixtures/       # widget.schema.yaml (controlled), npc_consumer.cpp (compile proof)
  test_schema_gen.py    # IR/output assertions, determinism, drift
  test_cpp_consumer.py  # compiles the header + a real-content consumer under C++20
```
