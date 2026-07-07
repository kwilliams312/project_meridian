"""Schema-driven struct generator (Tools SAD §2.1 / §6.1, issue #117).

Parses the Content Schema v1 (/schema/content/*.schema.yaml) and emits a typed
content model for the pipeline's consumers:

  - C++20  header  -> tools/schema_gen/generated/cpp/content_types.gen.hpp   (mcc/worldd)
  - C#     source  -> tools/schema_gen/generated/csharp/ContentTypes.g.cs    (Codex editor)

One generator, one schema, two targets — the single source of truth the SAD
calls for. Output is deterministic; a drift guard (tests/schema_gen) keeps the
checked-in generated files in sync with the schema.
"""

from .generate import GENERATED_ROOT, TARGETS, generate_all, render

__all__ = ["GENERATED_ROOT", "TARGETS", "generate_all", "render"]
