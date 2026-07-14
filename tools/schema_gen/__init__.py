"""Schema-driven struct generator (Tools SAD §2.1 / §6.1, issue #117).

Parses the Content Schema v1 (/schema/content/*.schema.yaml) and emits a typed
content model for the pipeline's consumers:

  - C++20  header  -> tools/schema_gen/generated/cpp/content_types.gen.hpp   (mcc/worldd)
  - C#     source  -> tools/schema_gen/generated/csharp/ContentTypes.g.cs    (Codex editor)

One generator, one schema, two targets — the single source of truth the SAD
calls for. Output is deterministic; a drift guard (tests/schema_gen) keeps the
checked-in generated files in sync with the schema.
"""

from importlib import import_module
from typing import Any

__all__ = ["GENERATED_ROOT", "TARGETS", "generate_all", "render"]


def __getattr__(name: str) -> Any:
    """Load the generator API without pre-importing its executable module.

    Eagerly importing :mod:`tools.schema_gen.generate` here makes
    ``python -m tools.schema_gen.generate`` find the target module in
    ``sys.modules`` before runpy executes it. Besides producing a warning, that
    ordering is explicitly documented by Python as unpredictable. Keep the
    package-level convenience API, but resolve it only when a caller asks for
    one of the exported names.
    """

    if name not in __all__:
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    generator = import_module(".generate", __name__)
    return getattr(generator, name)
