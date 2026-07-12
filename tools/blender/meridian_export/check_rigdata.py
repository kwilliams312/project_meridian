"""Run the meridian_export E100-E105 gate on a RigData snapshot JSON.

The E-rule checks (:mod:`rig_checks`) load the geoset/bone vocabulary via
PyYAML, which Blender's bundled Python does not ship. So a headless restyle
(``meridian_rig/restyle_body.py``) captures the E-rule inputs it observes on the
live Blender scene as a JSON snapshot, and this thin system-Python shell renders
the verdict — the same ``rig_checks.check_rig`` the interactive
``meridian_export`` addon runs before it lets an export proceed.

Usage::

    uv run python tools/blender/meridian_export/check_rigdata.py <snapshot.json>

Exit 0 when every E-rule passes; exit 1 (listing the failures) otherwise.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rig_checks  # noqa: E402


def rigdata_from_snapshot(doc: dict) -> rig_checks.RigData:
    """Rebuild a :class:`rig_checks.RigData` from a restyle snapshot dict."""
    transforms = [
        rig_checks.ObjectTransformState(
            name=o["name"], transforms_applied=bool(o["transforms_applied"])
        )
        for o in doc.get("object_transforms", [])
    ]
    return rig_checks.RigData(
        asset_class=doc["asset_class"],
        bone_names=list(doc.get("bone_names", [])),
        socket_names=list(doc.get("socket_names", [])),
        mesh_names=list(doc.get("mesh_names", [])),
        max_influences=int(doc.get("max_influences", 0)),
        weights_normalized=bool(doc.get("weights_normalized", True)),
        mesh_max_influences=dict(doc.get("mesh_max_influences", {})),
        unnormalized_meshes=list(doc.get("unnormalized_meshes", [])),
        object_transforms=transforms,
        unit_scale_ok=bool(doc.get("unit_scale_ok", True)),
    )


def main(argv: list[str] | None = None) -> int:
    args = argv if argv is not None else sys.argv[1:]
    if len(args) != 1:
        print("usage: check_rigdata.py <snapshot.json>", file=sys.stderr)
        return 2
    doc = json.loads(Path(args[0]).read_text(encoding="utf-8"))
    errors = rig_checks.check_rig(rigdata_from_snapshot(doc))
    if errors:
        print("meridian_export E-rule gate FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1
    print(
        f"meridian_export E-rule gate OK — {len(doc.get('mesh_names', []))} meshes, "
        f"{len(doc.get('bone_names', []))} bones, all E100-E105 pass"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
