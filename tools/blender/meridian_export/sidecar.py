"""Pure-Python sidecar generation + convention checks for meridian_export.

This module is deliberately free of any ``bpy`` import so it is unit-testable
without a Blender install (Art SAD §2.1, AR2 — the addon and CI share one
implementation of the rules). The ``bpy``-dependent operator (``__init__.py``)
extracts a plain :class:`MeshInfo` from the selected object and hands it here;
everything below operates on that snapshot.

Responsibilities:
  * build the IF-8 ``meridian/asset@1`` sidecar dict that PASSES
    ``schema/content/asset.schema.yaml`` and the #142 provenance/budget lints
    (validate_content.py L021/L022/L070-072);
  * compute the budget block from the mesh (LOD0 tri count, largest texture
    dimension) against ``budgets.json`` — the same data CI uses;
  * enforce the naming + pivot/scale conventions the Art SAD §2.1/§6.1 require,
    surfacing violations as structured warnings.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

BUDGETS_PATH = Path(__file__).with_name("budgets.json")

# Addon version, embedded into sidecars as a provenance-adjacent marker and used
# by CI's stale-export guard (Art SAD §2.1). Bump on any rule change.
ADDON_VERSION = "0.1.0"

# Object-name prefix -> IF-8 class. `sk_` (skeletal) and `sm_` (static) are the
# geometry prefixes an exportable mesh can carry (Art PRD §4.2). The finer class
# (kit_piece vs prop, character vs armor) cannot be read from the prefix alone,
# so the operator passes an explicit `asset_class`; the prefix is validated
# against the class's expected prefix from budgets.json.
GEOMETRY_PREFIXES = ("sm_", "sk_")

# Asset-ID grammar (Baseline §5.3 / content-schema): art.<category>.<...>.<name>,
# lowercase snake segments. Mirrors the ASSET_RE in validate_content.py minus the
# namespace, since the namespace is supplied separately.
_ID_LOCAL_RE = re.compile(r"^art(?:\.[a-z0-9_]+)+$")
_NS_RE = re.compile(r"^[a-z][a-z0-9_]{1,31}$")
_SOURCE_RE = re.compile(r"^assets/[A-Za-z0-9_./-]+$")
_OBJ_NAME_RE = re.compile(r"^(?:sm|sk|t|a|fx)_[a-z0-9_]+$")

# Per kit-piece category, the contracted pivot location (Art PRD §6.1). Used to
# describe the expected origin in warnings; the numeric 1 mm check runs in the
# operator against real object geometry, this table drives the human message.
KIT_PIVOT_RULES = {
    "floor": "back-left-bottom corner",
    "wall": "bottom-left edge",
    "prop": "bottom-center",
    "cliff": "approximate ground-contact centroid",
}

PIVOT_TOLERANCE_M = 0.001  # 1 mm (Art SAD §2.1)
GRID_MODULE_M = 0.5  # 50 cm master grid (Art PRD §6.1)


def load_budgets(path: Path | None = None) -> dict:
    """Load the shared per-class budget table (budgets.json)."""
    return json.loads((path or BUDGETS_PATH).read_text(encoding="utf-8"))["classes"]


@dataclass
class MeshInfo:
    """Plain snapshot of a Blender object, extracted by the operator.

    Everything the pure logic needs, with no ``bpy`` types — the operator reads
    these off the real object; tests construct them directly.
    """

    name: str  # object/mesh name, e.g. "sm_env_zone01_kit_wall_stone_a"
    tri_count: int  # LOD0 triangle count
    # Largest authored texture dimension across the object's material images, in
    # px; None when undeterminable (procedural-only, no image textures).
    texture_max_px: int | None = None
    material_set_count: int | None = None  # unique material sets on the object
    # Transform state, for scale/pivot enforcement.
    scale: tuple[float, float, float] = (1.0, 1.0, 1.0)
    transform_applied: bool = True
    has_negative_scale: bool = False
    # Pivot offset from the category-contracted origin, in metres (per axis).
    # None when the object's class carries no pivot rule (e.g. characters).
    pivot_offset_m: tuple[float, float, float] | None = None
    kit_category: str | None = None  # floor|wall|prop|cliff, drives pivot rule
    # AABB extents in metres, for the 50 cm grid check (kit pieces).
    aabb_size_m: tuple[float, float, float] | None = None


@dataclass
class ProvenanceInput:
    """Contributor-supplied provenance form values (the operator's panel)."""

    source_tier: str = "original"  # original | ai | cc0 | cc_by
    authors: list[str] = field(default_factory=list)
    license: str = (
        "CC-BY-4.0"  # CC-BY-4.0 for original, CC0-1.0/CC-BY-4.0 for 3rd-party
    )
    origin_url: str | None = None
    license_verified_on: str | None = None
    attribution: str | None = None
    ai_tool: str | None = None
    ai_prompts_file: str | None = None
    transform_notes: str | None = None


@dataclass
class Warning:
    """A convention violation surfaced to the artist (non-fatal by default)."""

    code: str  # e.g. "NAME", "SCALE", "PIVOT", "GRID", "BUDGET"
    message: str


def check_naming(mesh: MeshInfo, asset_class: str, budgets: dict) -> list[Warning]:
    """Naming enforcement (Art SAD §2.1): prefix + snake-case stem."""
    warnings: list[Warning] = []
    name = mesh.name
    if not _OBJ_NAME_RE.match(name):
        warnings.append(
            Warning(
                "NAME",
                f"object name '{name}' must be lowercase snake_case with an "
                f"sm_/sk_/t_/a_/fx_ prefix (Art PRD §4.2)",
            )
        )
        return warnings
    expected_prefix = budgets.get(asset_class, {}).get("prefix")
    if expected_prefix and not name.startswith(expected_prefix):
        warnings.append(
            Warning(
                "NAME",
                f"object name '{name}' should use the '{expected_prefix}' prefix "
                f"for class '{asset_class}' (Art SAD §2.1)",
            )
        )
    return warnings


def check_transform(mesh: MeshInfo) -> list[Warning]:
    """Scale/transform enforcement (Art SAD §2.1): 1 BU = 1 m, applied, no mirror."""
    warnings: list[Warning] = []
    if not mesh.transform_applied:
        warnings.append(
            Warning(
                "SCALE",
                f"'{mesh.name}' has unapplied transforms — apply all transforms "
                f"before export (Art SAD §2.1)",
            )
        )
    if any(abs(s - 1.0) > 1e-4 for s in mesh.scale):
        warnings.append(
            Warning(
                "SCALE",
                f"'{mesh.name}' object scale is {mesh.scale}, expected (1, 1, 1) — "
                f"1 Blender unit = 1 m (Art PRD §4.2)",
            )
        )
    if mesh.has_negative_scale:
        warnings.append(
            Warning(
                "SCALE",
                f"'{mesh.name}' has a negative (mirrored) scale — not allowed "
                f"(Art SAD §2.1)",
            )
        )
    return warnings


def check_pivot(mesh: MeshInfo) -> list[Warning]:
    """Pivot + 50 cm grid enforcement for kit pieces (Art SAD §2.1 / §6.1)."""
    warnings: list[Warning] = []
    if mesh.kit_category is None:
        return warnings  # non-kit classes have no pivot contract
    rule = KIT_PIVOT_RULES.get(mesh.kit_category)
    if rule is None:
        warnings.append(
            Warning(
                "PIVOT",
                f"'{mesh.name}' kit_category '{mesh.kit_category}' has no pivot "
                f"rule (expected one of {', '.join(KIT_PIVOT_RULES)})",
            )
        )
        return warnings
    if mesh.pivot_offset_m is not None and any(
        abs(o) > PIVOT_TOLERANCE_M for o in mesh.pivot_offset_m
    ):
        warnings.append(
            Warning(
                "PIVOT",
                f"'{mesh.name}' origin is {mesh.pivot_offset_m} m from the "
                f"{mesh.kit_category} pivot ({rule}); must be within "
                f"{PIVOT_TOLERANCE_M * 1000:.0f} mm (Art PRD §6.1)",
            )
        )
    if mesh.aabb_size_m is not None:
        for axis, size in zip("xyz", mesh.aabb_size_m):
            # bounds must land on the 50 cm grid within 1 mm.
            if (
                abs((size / GRID_MODULE_M) - round(size / GRID_MODULE_M))
                * GRID_MODULE_M
                > PIVOT_TOLERANCE_M
            ):
                warnings.append(
                    Warning(
                        "GRID",
                        f"'{mesh.name}' {axis}-extent {size:.4f} m is off the "
                        f"{GRID_MODULE_M * 100:.0f} cm grid (Art PRD §6.1)",
                    )
                )
    return warnings


def compute_budget(mesh: MeshInfo, asset_class: str, budgets: dict) -> dict:
    """Compute the sidecar `budget` block from the mesh (Art SAD §4.2).

    Only fields determinable from the object are emitted; the schema block is
    optional and additionalProperties:false, so we never emit unknown keys.
    """
    block: dict[str, int] = {"lod0_tris": int(mesh.tri_count)}
    if mesh.texture_max_px is not None:
        block["texture_max_px"] = int(mesh.texture_max_px)
    caps = budgets.get(asset_class, {})
    # material_sets only meaningful (and capped) for kit pieces.
    if mesh.material_set_count is not None and "material_sets" in caps:
        block["material_sets"] = int(mesh.material_set_count)
    return block


def check_budget(budget: dict, asset_class: str, budgets: dict) -> list[Warning]:
    """Flag any declared budget field over its class ceiling (mirrors L070-072)."""
    warnings: list[Warning] = []
    caps = budgets.get(asset_class, {})
    for metric in ("lod0_tris", "texture_max_px", "material_sets", "vram_mb"):
        declared = budget.get(metric)
        ceiling = caps.get(metric)
        if declared is not None and ceiling is not None and declared > ceiling:
            warnings.append(
                Warning(
                    "BUDGET",
                    f"{metric} {declared} exceeds the {ceiling} ceiling for class "
                    f"'{asset_class}' ({caps.get('cite', '')})",
                )
            )
    return warnings


def _build_provenance(prov: ProvenanceInput) -> dict:
    """Assemble the provenance block per source_tier (schema + L021 shape)."""
    block: dict = {
        "source_tier": prov.source_tier,
        "authors": list(prov.authors),
    }
    if prov.origin_url:
        block["origin_url"] = prov.origin_url
    if prov.license_verified_on:
        block["license_verified_on"] = prov.license_verified_on
    if prov.attribution:
        block["attribution"] = prov.attribution
    if prov.transform_notes:
        block["transform_notes"] = prov.transform_notes
    if prov.source_tier == "ai" and (prov.ai_tool or prov.ai_prompts_file):
        block["ai"] = {
            "tool": prov.ai_tool or "",
            "prompts_file": prov.ai_prompts_file or "",
        }
    return block


def build_sidecar(
    *,
    asset_id: str,
    asset_class: str,
    source: str,
    mesh: MeshInfo,
    provenance: ProvenanceInput,
    import_hints: dict | None = None,
    budgets: dict | None = None,
) -> dict:
    """Build the full IF-8 sidecar dict for an asset.

    Returns a dict that validates against asset.schema.yaml and passes the #142
    lints (L021/L022 provenance, L070-072 budget). `asset_id` is the full
    ``<ns>:art.<...>`` id; `source` is pack-root-relative (``assets/...``).
    """
    budgets = budgets if budgets is not None else load_budgets()
    doc: dict = {
        "schema": "meridian/asset@1",
        "id": asset_id,
        "class": asset_class,
        "source": source,
        "license": provenance.license,
        "provenance": _build_provenance(provenance),
    }
    budget_block = compute_budget(mesh, asset_class, budgets)
    if budget_block:
        doc["budget"] = budget_block
    if import_hints:
        doc["import_hints"] = import_hints
    # Tiers other than `original` cannot merge as pending (Art SAD §3.2); the
    # export sets `done` so a fresh export from a clean source is mergeable, and
    # `not_applicable` for original work.
    if provenance.source_tier == "original":
        doc["restyle_status"] = "not_applicable"
    else:
        doc["restyle_status"] = "done"
    return doc


def collect_warnings(
    mesh: MeshInfo, asset_class: str, budget: dict, budgets: dict | None = None
) -> list[Warning]:
    """Run every convention/budget check for an object, in report order."""
    budgets = budgets if budgets is not None else load_budgets()
    warnings: list[Warning] = []
    warnings += check_naming(mesh, asset_class, budgets)
    warnings += check_transform(mesh)
    warnings += check_pivot(mesh)
    warnings += check_budget(budget, asset_class, budgets)
    return warnings


def validate_id(asset_id: str) -> str | None:
    """Return an error message if `asset_id` is malformed, else None."""
    if ":" not in asset_id:
        return f"asset id '{asset_id}' must be namespaced (<ns>:art....)"
    ns, local = asset_id.split(":", 1)
    if not _NS_RE.match(ns):
        return f"namespace '{ns}' is not a valid pack namespace"
    if not _ID_LOCAL_RE.match(local):
        return f"asset id local part '{local}' must match art.<category>...<name>"
    return None


def validate_source(source: str) -> str | None:
    """Return an error message if `source` is not a valid pack-root-relative path."""
    if not _SOURCE_RE.match(source):
        return (
            f"source '{source}' must be pack-root-relative under assets/ "
            f"(pattern ^assets/[A-Za-z0-9_./-]+$)"
        )
    return None
