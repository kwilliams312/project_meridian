"""CLI driver: schema -> generated C++/C# files, with a --check drift guard.

    uv run python -m tools.schema_gen.generate            # regenerate in place
    uv run python -m tools.schema_gen.generate --check     # fail if stale (CI)

`--check` re-renders in memory and diffs against the checked-in files without
writing, exiting non-zero on drift — the same guard style as the determinism
golden corpus. `render()` is the pure schema->text function the tests use.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import emit_cpp, emit_csharp
from .ir import build_model

# Repo layout: tools/schema_gen/generate.py -> repo root is parents[2].
_THIS = Path(__file__).resolve()
REPO_ROOT = _THIS.parents[2]
SCHEMA_DIR = REPO_ROOT / "schema" / "content"
GENERATED_ROOT = _THIS.parent / "generated"

# target name -> (emitter, output path relative to GENERATED_ROOT).
TARGETS: dict[str, tuple] = {
    "cpp": (emit_cpp.emit, Path("cpp") / "content_types.gen.hpp"),
    "csharp": (emit_csharp.emit, Path("csharp") / "ContentTypes.g.cs"),
}


def render(schema_dir: Path) -> dict[str, str]:
    """Return {target: generated-text} for every target. Pure; no I/O writes."""
    model = build_model(schema_dir)
    return {name: emit(model) for name, (emit, _out) in TARGETS.items()}


def _output_path(target: str) -> Path:
    return GENERATED_ROOT / TARGETS[target][1]


def generate_all(schema_dir: Path = SCHEMA_DIR) -> dict[str, Path]:
    """Render and write every target; return {target: written-path}."""
    rendered = render(schema_dir)
    written: dict[str, Path] = {}
    for target, text in rendered.items():
        out = _output_path(target)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8")
        written[target] = out
    return written


def _check(schema_dir: Path) -> int:
    rendered = render(schema_dir)
    drift: list[str] = []
    for target, text in rendered.items():
        out = _output_path(target)
        current = out.read_text(encoding="utf-8") if out.exists() else ""
        if current != text:
            drift.append(str(out.relative_to(REPO_ROOT)))
    if drift:
        print(
            "schema_gen drift — checked-in generated files are stale:", file=sys.stderr
        )
        for path in drift:
            print(f"  {path}", file=sys.stderr)
        print(
            "Run: uv run python -m tools.schema_gen.generate",
            file=sys.stderr,
        )
        return 1
    print("schema_gen: generated files are up to date.")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Do not write; exit non-zero if checked-in output is stale.",
    )
    parser.add_argument(
        "--schema-dir",
        type=Path,
        default=SCHEMA_DIR,
        help="Content schema directory (default: /schema/content).",
    )
    args = parser.parse_args(argv)

    if args.check:
        return _check(args.schema_dir)

    written = generate_all(args.schema_dir)
    for target, path in written.items():
        print(f"schema_gen: wrote {target:7s} -> {path.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
