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

import base64
import hashlib
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


# Default generation request as a share of the class ceiling. Meshy tracks
# target_polycount closely (issue #627: a 30k request produced 30664 tris), so
# a ceiling-equal request routinely overshoots the L070 budget lint. Requesting
# ~80% of the ceiling leaves headroom for that overshoot while staying as close
# to the budget as the class allows.
TARGET_POLYCOUNT_HEADROOM = 0.8


def class_polycount_ceiling(asset_class: str, budgets: dict) -> int:
    """The lod0_tris ceiling for a class (single source: budgets.json).

    Raises IntakeError for a class that carries no lod0_tris budget — better to
    refuse before any network call than to guess a polycount for it.
    """
    entry = budgets.get(asset_class) or {}
    ceiling = entry.get("lod0_tris")
    if ceiling is None:
        raise IntakeError(
            f"--class '{asset_class}' has no lod0_tris budget in budgets.json"
        )
    return int(ceiling)


def derive_target_polycount(
    asset_class: str, budgets: dict, *, override: int | None = None
) -> int:
    """Resolve the Meshy target_polycount to request for a class (issue #627).

    Default: ~80% of the class's lod0_tris ceiling (see TARGET_POLYCOUNT_HEADROOM
    for why headroom is needed). An explicit override is honored but NEVER
    allowed to exceed the ceiling: Meshy tracks the request closely, so an
    over-ceiling request would guarantee an over-budget landing that the
    L070 pre-check then refuses — reject it up front with a clear error instead
    of silently clamping (mirrors the tool's other refuse-loudly gates).
    """
    ceiling = class_polycount_ceiling(asset_class, budgets)
    if override is not None:
        if override < 1:
            raise IntakeError(
                f"--target-polycount {override} must be a positive integer"
            )
        if override > ceiling:
            raise IntakeError(
                f"--target-polycount {override} exceeds the '{asset_class}' "
                f"lod0_tris ceiling of {ceiling} — Meshy tracks the requested "
                f"count closely, so this would land over budget and be refused"
            )
        return override
    return max(1, int(ceiling * TARGET_POLYCOUNT_HEADROOM))


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


# Local-image formats Meshy's image-to-3D accepts as base64 data URIs
# (docs.meshy.ai image-to-3d: image_url is "Public URL or base64 data URI
# (.jpg, .jpeg, .png)").
_IMAGE_MIME_BY_SUFFIX = {
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
}


def image_ref_to_url(image_ref: str) -> str:
    """Resolve a `--image` argument to what the Meshy API accepts as `image_url`.

    * `http(s)://...` (or an already-encoded `data:` URI) passes through.
    * An existing local `.png`/`.jpg`/`.jpeg` file is base64-encoded into a
      `data:<mime>;base64,...` URI (the documented alternative to a public URL).
    * Anything else — missing file, unsupported extension — is an IntakeError,
      raised before any network call.
    """
    if image_ref.startswith(("http://", "https://", "data:")):
        return image_ref
    path = Path(image_ref)
    if not path.is_file():
        raise IntakeError(
            f"--image '{image_ref}' is neither a URL nor an existing local file"
        )
    mime = _IMAGE_MIME_BY_SUFFIX.get(path.suffix.lower())
    if mime is None:
        raise IntakeError(
            f"--image '{image_ref}' has unsupported extension '{path.suffix}' — "
            f"Meshy accepts {', '.join(sorted(_IMAGE_MIME_BY_SUFFIX))} for local files"
        )
    encoded = base64.b64encode(path.read_bytes()).decode("ascii")
    return f"data:{mime};base64,{encoded}"


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
    prompt hygiene — an auditable record of exactly what was asked for). One
    exception to verbatim recording: a base64 data-URI `image_url` (local-file
    image-to-3D) is replaced by its byte length + SHA-256 digest — megabytes of
    base64 in a YAML companion would be useless for review, and the digest
    still pins exactly which image was submitted.
    """
    doc: dict = {"task_id": task_id, "model_version": model_version}
    if preview_task_id is not None:
        doc["preview_task_id"] = preview_task_id
    request = dict(request_payload)
    image_url = request.get("image_url")
    if isinstance(image_url, str) and image_url.startswith("data:"):
        digest = hashlib.sha256(image_url.encode("ascii")).hexdigest()
        request["image_url"] = (
            f"<data-uri omitted: {len(image_url)} chars, sha256:{digest}>"
        )
    doc["request"] = request
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
