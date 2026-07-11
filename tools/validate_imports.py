#!/usr/bin/env python3
"""Godot import-settings validator (Art SAD §2.3/§4.2, issue #138).

The Godot-side counterpart to the Blender export (#137) and the sidecar
provenance/budget validator (#142). This checks that an asset + its IF-8 sidecar
`import_hints` conform to the **import preset** for its class — the canonical
per-class Godot import settings in `client/import-presets/presets.json`. Off-preset
is a hard error, matching the #142 posture.

Scope boundary (does NOT duplicate validate_content.py): #142's L021/L022 cover
provenance/license and L070-072 cover per-asset triangle/VRAM *budgets*. This module
covers import *settings* only — compression mode, mipmaps, colorspace, texture size
caps at import, model LOD-import flags, lightmap-UV2/occluder policy — and, when a
Godot `.import` file exists for a source, validates that file against the class
template too.

Rule ids (I-band, Art SAD §2.3 import preset table):

  I001  sidecar `class` maps to a known import preset
  I002  source file extension is one the class preset imports
  I003  declared texture dimension is within the preset's import size cap for the class
  I004  import_hints.lod_policy is one the class preset permits
  I005  import_hints.lightmap_uv2 matches the preset (statics need UV2; char/foliage must not)
  I006  import_hints.occluder is set for classes whose preset converts occluder nodes
  I007  a texture_set source carries a recognized channel suffix (sRGB/linear assignment)
  I010  a committed `.import` file's key params match the class template (drift guard, §8.1)
  I020  a character_model glb's joint-node name set is exactly the canonical
        skeleton.defs.yaml bone set (missing/extra listed by name) — spec ④ §5
  I021  an armor_model glb's skins bind only canonical joint names — spec ④ §5
  I022  character_model body meshes follow geo_<region>_lod<N> naming and cover
        all 8 geoset regions at lod0 — spec ④ §3/§5
  I023  a skeletal asset with meshes ships a LOD chain (lod0 + lod1+) unless
        import_hints.lod_policy is 'single' — Art PRD §2.1, spec ④ §5
  IPRESET  the preset templates themselves are well-formed and cover the asset classes

I020-I023 parse committed .glb files structurally with pygltflib (names only —
no geometry decode, no Blender). They activate only when skeleton.defs.yaml's
boneName enum is populated: an empty/absent enum means "no bone-name check yet"
(contract ①'s empty-means-skip), so branches predating the rig work stay green.
creature_model is explicitly out of scope (spec ④ §5 — NPC placeholders).

Usage:
  python3 tools/validate_imports.py [--root <repo-root>] [--imports warn|error|ignore]

It is also invoked in-process by validate_content.py so the existing content-CI
`validate_content.py` step runs these checks with no workflow edit.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

import yaml

DEFAULT_PRESETS = "client/import-presets/presets.json"
DEFAULT_SKELETON_DEFS = "schema/content/skeleton.defs.yaml"

# IF-8 art classes this validator governs (mesh/texture/audio import). Content
# classes without an import-settings dimension (none currently) are simply absent
# from every preset and reported by I001 if a sidecar names one.
GOVERNED_CLASSES = (
    "character_model",
    "creature_model",
    "weapon_model",
    "armor_model",
    "kit_piece",
    "prop",
    "foliage",
    "hero_landmark",
    "texture_set",
    "icon",
    "vfx",
    "sfx",
    "ui_sound",
    "music_stem",
    "music_stinger",
    "ambience_bed",
    "ambience_emitter",
)

# Classes whose scene preset requires lightmap UV2 on import (static, lightmapped on
# the Low baked-GI path, PRD §2.4/§4.2). Kept explicit rather than read purely from
# the preset so the rule is greppable and the message can name the PRD cite.
LIGHTMAP_UV2_REQUIRED = ("kit_piece", "hero_landmark", "prop")
# Static classes that must NOT carry lightmap UV2 (character: skinned; foliage:
# unlit-shadow path, Art SAD §2.3). A declared `lightmap_uv2: true` here is wrong.
LIGHTMAP_UV2_FORBIDDEN = ("character_model", "creature_model", "armor_model", "foliage")

# Classes whose committed .glb files the I020-I023 rig rules structurally inspect
# (spec ④ §5). creature_model is deliberately absent: the existing NPC placeholder
# models predate the canonical rig and are not covered by I020-I023.
GLTF_RIG_CLASSES = ("character_model", "armor_model")

# Body geoset mesh naming (spec ④ §3): geo_<region>_lod<N>.
_GEOSET_MESH_RE = re.compile(r"^geo_([a-z0-9_]+)_lod(\d+)$")
# Any *_lod<N> suffix — the I023 LOD-chain probe (gear meshes are not
# geoset-named but authored chains still carry the _lod<N> suffix).
_LOD_SUFFIX_RE = re.compile(r"_lod(\d+)$")


@dataclass
class Result:
    """Mirrors validate_content.Result so the two validators compose."""

    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    stats: dict[str, int] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return not self.errors


def load_presets(presets_path: Path) -> dict:
    """Load and structurally validate presets.json; raise on a malformed preset set."""
    data = json.loads(presets_path.read_text(encoding="utf-8"))
    presets = data.get("presets")
    if not isinstance(presets, dict) or not presets:
        raise ValueError(f"{presets_path}: 'presets' must be a non-empty object")
    for name, preset in presets.items():
        for key in ("importer", "classes", "source_ext", "cite"):
            if key not in preset:
                raise ValueError(f"{presets_path}: preset '{name}' missing '{key}'")
        if not isinstance(preset["classes"], list) or not preset["classes"]:
            raise ValueError(f"{presets_path}: preset '{name}' has no classes listed")
    return data


def build_class_index(presets_data: dict) -> dict[str, tuple[str, dict]]:
    """class -> (preset_name, preset). Rejects a class claimed by two presets."""
    index: dict[str, tuple[str, dict]] = {}
    for name, preset in presets_data["presets"].items():
        for cls in preset["classes"]:
            if cls in index:
                other = index[cls][0]
                raise ValueError(
                    f"class '{cls}' claimed by both preset '{other}' and '{name}'"
                )
            index[cls] = (name, preset)
    return index


def _suffix_colorspace(source: str, preset: dict) -> str | None:
    """Return the expected colorspace for a texture source by its filename suffix."""
    table = preset.get("colorspace_by_suffix")
    if not table:
        return None
    stem = Path(source).stem.lower()
    for suffix, space in table.items():
        if stem.endswith(suffix):
            return space
    return None


def build_size_caps(presets_data: dict) -> dict[str, int]:
    """Aggregate every preset's size_caps_px into one class -> cap lookup.

    Texture import caps (Art PRD §2.3) are declared on the texture presets but keyed
    by the *model* class the textures belong to, so a model sidecar's declared
    texture_max_px is checked against the global cap for its class regardless of which
    preset (scene vs texture) the model itself imports under.
    """
    caps: dict[str, int] = {}
    for preset in presets_data["presets"].values():
        for cls, cap in preset.get("size_caps_px", {}).items():
            # First declaration wins; presets.json must not disagree on a class cap.
            caps.setdefault(cls, cap)
    return caps


def check_sidecar(
    doc: dict,
    rel_path: Path,
    class_index: dict[str, tuple[str, dict]],
    size_caps: dict[str, int],
) -> list[str]:
    """I001-I006 — the asset + its import_hints conform to the class preset."""
    errors: list[str] = []
    asset_class = doc.get("class")
    if asset_class not in GOVERNED_CLASSES:
        return errors  # not an import-governed class; nothing to check here

    entry = class_index.get(asset_class)
    if entry is None:
        errors.append(
            f"I001 {rel_path}: class '{asset_class}' has no import preset in "
            f"presets.json (Art SAD §2.3 preset table)"
        )
        return errors
    preset_name, preset = entry

    # I002 — source extension must be one the preset imports.
    source = str(doc.get("source", ""))
    ext = Path(source).suffix.lower()
    allowed_ext = [e.lower() for e in preset.get("source_ext", [])]
    if ext and ext not in allowed_ext:
        errors.append(
            f"I002 {rel_path}: source '{source}' has extension '{ext}', not one the "
            f"'{preset_name}' preset imports ({', '.join(allowed_ext)}) — Art SAD §2.3"
        )

    # I003 — declared texture dimension within the import size cap for the class.
    cap = size_caps.get(asset_class)
    declared_px = (doc.get("budget") or {}).get("texture_max_px")
    if cap is not None and isinstance(declared_px, int) and declared_px > cap:
        errors.append(
            f"I003 {rel_path}: texture_max_px {declared_px} exceeds the import cap "
            f"{cap} for class '{asset_class}' — imports must not upscale past the "
            f"class ceiling (Art SAD §2.3 / Art PRD §2.3)"
        )

    hints = doc.get("import_hints") or {}

    # I004 — lod_policy compatible with the preset's allowed policies (models only).
    allowed_lods = preset.get("lod_policy")
    lod_policy = hints.get("lod_policy")
    if allowed_lods and lod_policy and lod_policy not in allowed_lods:
        errors.append(
            f"I004 {rel_path}: import_hints.lod_policy '{lod_policy}' is not permitted "
            f"by the '{preset_name}' preset (allowed: {', '.join(allowed_lods)}) — "
            f"Art SAD §2.3 / Art PRD §2.1"
        )

    # I005 — lightmap UV2 policy per class (only meaningful when declared).
    lightmap = hints.get("lightmap_uv2")
    if lightmap is True and asset_class in LIGHTMAP_UV2_FORBIDDEN:
        errors.append(
            f"I005 {rel_path}: import_hints.lightmap_uv2 is true but class "
            f"'{asset_class}' uses no lightmaps (skinned / unlit-shadow path) — "
            f"Art SAD §2.3"
        )
    if lightmap is False and asset_class in LIGHTMAP_UV2_REQUIRED:
        errors.append(
            f"I005 {rel_path}: import_hints.lightmap_uv2 is false but class "
            f"'{asset_class}' is a static lightmapped class needing UV2 on the Low "
            f"baked-GI path (Art PRD §2.4/§4.2)"
        )

    # I006 — occluder conversion for classes whose preset generates occluders.
    if (
        preset.get("settings", {}).get("generate_occluder")
        and asset_class == "kit_piece"
    ):
        if hints.get("occluder") is False:
            errors.append(
                f"I006 {rel_path}: import_hints.occluder is false but kit pieces must "
                f"ship occluder geometry for raster occlusion culling "
                f"(Art PRD §2.2/§6.1, Art SAD §2.3 env-kit)"
            )

    # I007 — a texture_set source must carry a recognized channel suffix so the
    # import plugin can assign the correct sRGB (base color) / linear (data map)
    # colorspace (Art SAD §2.3 texture-std/vfx-texture). Icons are exempt (UI, sRGB).
    if preset.get("importer") == "texture" and asset_class == "texture_set":
        if _suffix_colorspace(source, preset) is None:
            known = ", ".join(preset.get("colorspace_by_suffix", {}))
            errors.append(
                f"I007 {rel_path}: texture source '{source}' has no recognized channel "
                f"suffix ({known}) — the import plugin cannot assign sRGB/linear "
                f"colorspace (Art SAD §2.3)"
            )

    return errors


def parse_import_params(text: str) -> dict[str, str]:
    """Parse the [params] block of a Godot .import (INI-ish) file into a flat dict."""
    params: dict[str, str] = {}
    in_params = False
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            in_params = line == "[params]"
            continue
        if in_params and "=" in line:
            key, _, value = line.partition("=")
            params[key.strip()] = value.strip()
    return params


# Key .import params whose value the template pins per preset; drift here breaks
# determinism (Art SAD §8.1). Only these load-bearing keys are compared — a real
# .import carries many editor-managed keys the template does not pin.
IMPORT_DRIFT_KEYS = {
    "scene": ("meshes/generate_lods",),
    "texture": ("compress/mode", "mipmaps/generate"),
    "wav": ("edit/compress/mode",),
}


def check_import_file(
    import_path: Path,
    rel_path: Path,
    preset: dict,
    template_params: dict[str, str],
) -> list[str]:
    """I010 — a committed .import file's key params match the class template."""
    errors: list[str] = []
    actual = parse_import_params(import_path.read_text(encoding="utf-8"))
    importer = preset.get("importer")
    for key in IMPORT_DRIFT_KEYS.get(importer, ()):
        want = template_params.get(key)
        if want is None:
            continue
        got = actual.get(key)
        if got is not None and got != want:
            errors.append(
                f"I010 {rel_path}: .import '{key}'={got} diverges from the class "
                f"template ({want}) — preset drift breaks import determinism "
                f"(Art SAD §8.1)"
            )
    return errors


def _load_template_params(presets_dir: Path, preset_name: str) -> dict[str, str]:
    tmpl = presets_dir / f"{preset_name}.import.tmpl"
    if not tmpl.exists():
        return {}
    return parse_import_params(tmpl.read_text(encoding="utf-8"))


def load_skeleton_defs(defs_path: Path) -> dict:
    """Read skeleton.defs.yaml → {'bones': [...], 'regions': [...]}.

    Absent/malformed file or empty enums yield empty lists — the I020-I023
    rules then skip (contract ①'s empty-means-skip; spec ④ §5).
    """
    try:
        doc = yaml.safe_load(defs_path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError):
        return {"bones": [], "regions": []}
    defs = (doc or {}).get("$defs") or {}
    bones = (defs.get("boneName") or {}).get("enum") or []
    regions = (defs.get("geosetRegion") or {}).get("enum") or []
    return {"bones": list(bones), "regions": list(regions)}


def _load_glb(glb_path: Path):
    """Parse a .glb structurally; None when absent or unparseable.

    Unparseable covers LFS pointer stubs and truncated blobs — source existence
    and integrity are L020/LFS scope, not the rig rules'.
    """
    if not glb_path.is_file():
        return None
    try:
        from pygltflib import GLTF2

        return GLTF2().load(str(glb_path))
    except Exception:  # noqa: BLE001 — any parse failure means "not checkable"
        return None


def _joint_names(gltf) -> set[str]:
    """Named joint nodes: skin joints when skins exist, else non-mesh nodes."""
    names: set[str] = set()
    if gltf.skins:
        for skin in gltf.skins:
            for j in skin.joints or []:
                if gltf.nodes[j].name:
                    names.add(gltf.nodes[j].name)
    else:
        for node in gltf.nodes:
            if node.mesh is None and node.name:
                names.add(node.name)
    return names


def check_gltf_rig(
    doc: dict,
    rel_path: Path,
    glb_path: Path,
    skeleton_defs: dict,
) -> list[str]:
    """I020-I023 — structural rig/geoset/LOD conformance of a committed .glb.

    Skeleton-only assets (no meshes) get I020 but not I022/I023; body assets
    (meshes present) get every applicable rule (spec ④ §5).
    """
    errors: list[str] = []
    canonical = skeleton_defs.get("bones") or []
    if not canonical:
        return errors  # empty enum → no bone-name check yet (contract ①)
    asset_class = doc.get("class")
    if asset_class not in GLTF_RIG_CLASSES:
        return errors
    gltf = _load_glb(glb_path)
    if gltf is None:
        return errors

    canonical_set = set(canonical)
    joints = _joint_names(gltf)
    mesh_names = [m.name or f"meshes[{i}]" for i, m in enumerate(gltf.meshes)]

    # I020 — character_model rigs carry exactly the canonical joint set.
    if asset_class == "character_model":
        missing = sorted(canonical_set - joints)
        extra = sorted(joints - canonical_set)
        if missing or extra:
            parts = []
            if missing:
                parts.append(f"missing: {', '.join(missing)}")
            if extra:
                parts.append(f"extra: {', '.join(extra)}")
            errors.append(
                f"I020 {rel_path}: joint-node name set diverges from the canonical "
                f"skeleton ({'; '.join(parts)}) — character_model rigs must carry "
                f"exactly the skeleton.defs.yaml bone set (spec ④ §5, Art SAD §2.3 "
                f"shared rig)"
            )

    # I021 — armor skins bind only canonical joints (gear binds a subset).
    if asset_class == "armor_model" and gltf.skins:
        bad = sorted(joints - canonical_set)
        if bad:
            errors.append(
                f"I021 {rel_path}: skin binds non-canonical joints "
                f"({', '.join(bad)}) — armor_model gear must bind only canonical "
                f"bones (spec ④ §5; conversion gate §7.3)"
            )

    # I022 — body geoset naming + lod0 region coverage (character bodies only).
    regions = skeleton_defs.get("regions") or []
    if asset_class == "character_model" and mesh_names and regions:
        region_set = set(regions)
        lod0_regions: set[str] = set()
        for name in mesh_names:
            m = _GEOSET_MESH_RE.match(name)
            if m is None:
                errors.append(
                    f"I022 {rel_path}: body mesh '{name}' is not geoset-named — "
                    f"character_model body meshes must be geo_<region>_lod<N> "
                    f"(spec ④ §3)"
                )
                continue
            region, lod = m.group(1), int(m.group(2))
            if region not in region_set:
                errors.append(
                    f"I022 {rel_path}: body mesh '{name}' names unknown geoset "
                    f"region '{region}' (regions: {', '.join(regions)}) — spec ④ §3"
                )
            elif lod == 0:
                lod0_regions.add(region)
        missing_regions = [r for r in regions if r not in lod0_regions]
        if missing_regions:
            errors.append(
                f"I022 {rel_path}: lod0 does not cover all geoset regions "
                f"(missing: {', '.join(missing_regions)}) — a body missing a "
                f"region is invalid (spec ④ §3/§5)"
            )

    # I023 — skeletal LOD chain unless the sidecar declares lod_policy 'single'.
    lod_policy = (doc.get("import_hints") or {}).get("lod_policy")
    if mesh_names and lod_policy != "single":
        lods = {
            int(m.group(1))
            for name in mesh_names
            if (m := _LOD_SUFFIX_RE.search(name))
        }
        if 0 not in lods or not any(lod >= 1 for lod in lods):
            found = ", ".join(f"lod{n}" for n in sorted(lods)) or "none"
            errors.append(
                f"I023 {rel_path}: no LOD chain in mesh names (need _lod0 plus "
                f"_lod1+; found: {found}) — skeletal assets ship an authored LOD "
                f"chain per Art PRD §2.1 unless import_hints.lod_policy is "
                f"'single' (spec ④ §5)"
            )

    return errors


def check_preset_coverage(
    class_index: dict[str, tuple[str, dict]],
) -> list[str]:
    """IPRESET — the preset set covers every import-governed asset class."""
    errors: list[str] = []
    for cls in GOVERNED_CLASSES:
        if cls not in class_index:
            errors.append(
                f"IPRESET presets.json: no import preset covers asset class '{cls}' "
                f"(Art SAD §2.3 preset table must cover every art class)"
            )
    return errors


def validate(
    content_dir: Path,
    presets_path: Path,
    imports_mode: str = "error",
    skeleton_defs_path: Path | None = None,
) -> Result:
    """Validate every art sidecar under content_dir against the import presets."""
    res = Result()
    root = content_dir.parent

    try:
        presets_data = load_presets(presets_path)
        class_index = build_class_index(presets_data)
    except (ValueError, json.JSONDecodeError, OSError) as exc:
        res.errors.append(f"IPRESET {presets_path}: malformed preset set — {exc}")
        return res

    # Canonical skeleton vocabulary for the I020-I023 rig rules (spec ④ §5);
    # empty when the defs file is absent or its boneName enum is unpopulated.
    skeleton_defs = load_skeleton_defs(
        skeleton_defs_path or (root / DEFAULT_SKELETON_DEFS)
    )

    # IPRESET coverage check is always a hard error (the policy must be complete).
    res.errors.extend(check_preset_coverage(class_index))

    size_caps = build_size_caps(presets_data)
    presets_dir = presets_path.parent
    template_cache: dict[str, dict[str, str]] = {}

    def pack_root(sidecar: Path) -> Path | None:
        """The nearest ancestor holding pack.yaml (sidecar `source` is relative to it)."""
        for parent in sidecar.parents:
            if (parent / "pack.yaml").exists():
                return parent
        return None

    def rel(path: Path) -> Path:
        try:
            return path.relative_to(root)
        except ValueError:
            return path

    sink = res.errors if imports_mode == "error" else res.warnings

    checked = 0
    for path in sorted(content_dir.rglob("*.asset.yaml")):
        try:
            doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        except yaml.YAMLError:
            continue  # validate_content.py owns PARSE reporting
        if not isinstance(doc, dict) or "class" not in doc:
            continue
        if doc.get("class") not in GOVERNED_CLASSES:
            continue
        checked += 1

        sink.extend(check_sidecar(doc, rel(path), class_index, size_caps))

        # I010 — validate a committed .import next to the source, if present.
        entry = class_index.get(doc.get("class"))
        source = doc.get("source")
        pack = pack_root(path)
        if entry and source and pack is not None:
            preset_name, preset = entry
            src_path = pack / source  # sidecar `source` is pack-root-relative
            import_path = src_path.with_name(src_path.name + ".import")
            if import_path.exists():
                if preset_name not in template_cache:
                    template_cache[preset_name] = _load_template_params(
                        presets_dir, preset_name
                    )
                sink.extend(
                    check_import_file(
                        import_path, rel(path), preset, template_cache[preset_name]
                    )
                )

            # I020-I023 — structural rig/geoset/LOD conformance of the committed
            # .glb for skeletal classes (spec ④ §5). Skips when the boneName enum
            # is empty or the .glb is absent/unparseable (LFS pointer stub).
            if doc.get("class") in GLTF_RIG_CLASSES:
                sink.extend(
                    check_gltf_rig(doc, rel(path), src_path, skeleton_defs)
                )

    res.stats = {
        "presets": len(presets_data["presets"]),
        "governed_sidecars": checked,
    }
    return res


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    root_default = Path(__file__).resolve().parent.parent
    parser.add_argument("--root", type=Path, default=root_default)
    parser.add_argument(
        "--presets",
        type=Path,
        default=None,
        help="path to presets.json (default: <root>/client/import-presets/presets.json)",
    )
    parser.add_argument(
        "--imports",
        choices=("warn", "error", "ignore"),
        default="error",
        help="severity for import-preset checks (default: error, per #142 hard-fail)",
    )
    args = parser.parse_args()

    if args.imports == "ignore":
        print("Import validation skipped (--imports ignore).")
        return 0

    presets_path = args.presets or (args.root / DEFAULT_PRESETS)
    res = validate(args.root / "content", presets_path, args.imports)

    s = res.stats
    print(
        f"Validated import presets: {s.get('presets', 0)} presets, "
        f"{s.get('governed_sidecars', 0)} governed art sidecars."
    )
    if res.warnings:
        print(f"\n{len(res.warnings)} warning(s):")
        for w in res.warnings:
            print(f"  {w}")
    if res.errors:
        print(f"\n{len(res.errors)} error(s):")
        for e in res.errors:
            print(f"  {e}")
        return 1
    print("OK — all assets conform to their class import preset.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
