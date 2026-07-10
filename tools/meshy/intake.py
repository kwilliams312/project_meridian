"""Pure normalize/land/sidecar logic for Meshy intake (spec §7.2).

No network I/O and no ``bpy`` — mirrors the shape of
``tools/blender/meridian_export/sidecar.py`` (pure module building the
sidecar dict) so the output passes the same schema + lints
(``schema/content/asset.schema.yaml``, ``validate_content.check_provenance``,
``validate_content.check_budget``). The CLI shell (``__main__.py``) is the
only place that touches the network or the filesystem beyond what's handed to
it here.

AI-tier intake is a structural quarantine, not a policy exception: every
sidecar this module builds carries ``restyle_status: pending`` (Art SAD §3.2)
— the existing L021/asset-schema lints already block ai-tier `pending`
assets from merging, so raw Meshy output cannot ship without a restyle pass
marking it `done`. No new policy is invented here.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BUDGETS_PATH = REPO_ROOT / "tools" / "blender" / "meridian_export" / "budgets.json"

# Asset-name grammar: a single lowercase snake-case segment. Kept intentionally
# narrower than the full art.<category>...<name> id grammar (validate_content's
# ASSET_RE) — Meshy intake always mints a single new leaf id, `art.<name>`.
_NAME_RE = re.compile(r"^[a-z][a-z0-9_]*$")
_NS_RE = re.compile(r"^[a-z][a-z0-9_]{1,31}$")

# class -> geometry prefix, mirrors meridian_export/budgets.json (Art SAD §2.1).
# Loaded from the shared JSON at call time so the two stay in lockstep; this
# fallback only matters if a class is missing a "prefix" row in budgets.json.
_DEFAULT_PREFIX = "sm_"


class IntakeError(ValueError):
    """Raised for malformed CLI input caught before any network/filesystem I/O."""


def load_budgets(path: Path | None = None) -> dict:
    """Load the shared per-class budget table (single source: meridian_export/budgets.json)."""
    return json.loads((path or BUDGETS_PATH).read_text(encoding="utf-8"))["classes"]


def validate_name(name: str) -> None:
    if not _NAME_RE.match(name):
        raise IntakeError(
            f"--name '{name}' must be lowercase snake_case (e.g. 'orc_warrior')"
        )


def validate_namespace(ns: str) -> None:
    if not _NS_RE.match(ns):
        raise IntakeError(f"--ns '{ns}' is not a valid pack namespace")


def geometry_prefix(asset_class: str, budgets: dict) -> str:
    return budgets.get(asset_class, {}).get("prefix", _DEFAULT_PREFIX)


def asset_id(ns: str, name: str) -> str:
    """Build the full `<ns>:art.<name>` asset id."""
    return f"{ns}:art.{name}"


@dataclass
class LandingPaths:
    """Where a Meshy intake lands under the content tree (spec §7.2)."""

    asset_dir: Path  # content/<ns>/assets/art/<name>/
    glb_path: Path  # .../<prefix><name>.glb
    sidecar_path: Path  # .../<name>.asset.yaml
    prompts_path: Path  # .../<name>.prompts.yaml
    source: str  # pack-root-relative source path for the sidecar `source:` field


def landing_paths(
    *, content_root: Path, ns: str, name: str, asset_class: str, budgets: dict
) -> LandingPaths:
    prefix = geometry_prefix(asset_class, budgets)
    asset_dir = content_root / ns / "assets" / "art" / name
    glb_name = f"{prefix}{name}.glb"
    return LandingPaths(
        asset_dir=asset_dir,
        glb_path=asset_dir / glb_name,
        sidecar_path=asset_dir / f"{name}.asset.yaml",
        prompts_path=asset_dir / f"{name}.prompts.yaml",
        source=f"assets/art/{name}/{glb_name}",
    )


def count_glb_triangles(glb_path: Path) -> int:
    """Sum the triangle count across every mesh primitive in a `.glb` (Art SAD §4.2).

    Uses only accessor `.count` metadata (no need to decode the binary buffer):
    an indexed primitive's triangle count is `indices.count // 3`; a
    non-indexed primitive (rare, but valid glTF) falls back to
    `POSITION.count // 3`. Primitives whose `mode` is absent or `4`
    (TRIANGLES, the glTF default) are counted; other draw modes (points,
    lines, strips/fans) are skipped — Meshy exports triangle meshes only.
    """
    from pygltflib import GLTF2

    gltf = GLTF2().load(str(glb_path))
    total = 0
    for mesh in gltf.meshes or []:
        for prim in mesh.primitives or []:
            if prim.mode not in (None, 4):
                continue
            if prim.indices is not None:
                total += gltf.accessors[prim.indices].count // 3
            elif prim.attributes.POSITION is not None:
                total += gltf.accessors[prim.attributes.POSITION].count // 3
    return total


def build_budget_precheck_doc(asset_class: str, lod0_tris: int) -> dict:
    """The minimal doc shape `validate_content.check_budget` needs (class + budget)."""
    return {"class": asset_class, "budget": {"lod0_tris": lod0_tris}}


def build_prompts_doc(
    *,
    task_id: str,
    preview_task_id: str | None,
    request_payload: dict,
    model_version: str,
) -> dict:
    """The prompts-file content: the exact generation request + resulting task id(s).

    Referenced from the sidecar's `provenance.ai.prompts_file` (Art PRD §3.2
    prompt hygiene — an auditable record of exactly what was asked for).
    """
    doc: dict = {"task_id": task_id, "model_version": model_version}
    if preview_task_id is not None:
        doc["preview_task_id"] = preview_task_id
    doc["request"] = dict(request_payload)
    return doc


def build_sidecar(
    *,
    asset_id: str,
    asset_class: str,
    source: str,
    model_version: str,
    prompts_file: str,
    origin_url: str,
    authors: list[str],
    lod0_tris: int,
) -> dict:
    """Build the IF-8 `meridian/asset@1` sidecar for a Meshy intake (spec §7.2).

    Always `source_tier: ai`, `restyle_status: pending` — the intake output is
    structurally quarantined (existing L021/schema lints) until a restyle pass
    marks it `done` (Art PRD §3.4). `ai.tool` records the exact tool+version
    (`meshy@<model-ver>`) so provenance is reproducible.
    """
    return {
        "schema": "meridian/asset@1",
        "id": asset_id,
        "class": asset_class,
        "source": source,
        "license": "CC-BY-4.0",
        "provenance": {
            "source_tier": "ai",
            "authors": list(authors),
            "origin_url": origin_url,
            "ai": {"tool": f"meshy@{model_version}", "prompts_file": prompts_file},
        },
        "budget": {"lod0_tris": lod0_tris},
        "restyle_status": "pending",
    }
