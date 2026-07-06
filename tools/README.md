# `/tools` — Meridian content pipeline

The authoring-to-runtime toolchain. **Single funnel:** only `mcc` produces runtime
artifacts (Overview §3.3). Tools is the M1 critical path.

**Read with:** [Tools PRD](../docs/prd/tools-prd.md) · [Tools SAD](../docs/sad/tools-sad.md)

## Surfaces

| Surface | What | Stack |
|---------|------|-------|
| `mcc/` | Meridian Content Compiler: YAML → world-DB SQL (IF-4) + client `.pck` (IF-5) | C++20 CLI |
| `blender/` | `meridian_export` art pipeline addon | Python (Blender LTS) |
| Forge | zone editor (lives in `/client` as a Godot editor plugin) | GDScript + `forge_core` |
| Codex | standalone data editors | C# / .NET 8 + Avalonia |

## Reference validator (TLS-07 stopgap, live now)

Until `mcc` subsumes them, two Python tools gate `/content` in CI:

- [`validate_content.py`](validate_content.py) — schema + lint engine (L001–L062)
- [`check_traceability.py`](check_traceability.py) — baseline↔PRD matrix + version sync

```bash
uv run tools/validate_content.py --assets error
uv run tools/check_traceability.py
uv run pytest -q
```
