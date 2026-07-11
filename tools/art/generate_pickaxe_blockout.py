#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic blockout .glb for core:art.item.weapon.pickaxe_rusty (story #540).

The item sidecar (content/core/assets/art/item_weapon_pickaxe_rusty.asset.yaml)
declared `source: assets/art/item/weapon/pickaxe_rusty.glb` as a placeholder
"until the first art drop". Spec (2) Task 3 (the AssembledCharacter node) needs
real weapon bytes to socket onto `socket_main_hand`, so this script materialises
that promise the same way the character blockout did (tools/blender/meridian_rig/
generate_blockout.py): a script-generated original greybox — two axis-aligned
boxes (haft + head), 24 tris, far under the weapon_model budget — written as a
minimal valid glTF 2.0 binary. No Blender required: the GLB is emitted directly
(stdlib only) and is byte-deterministic (sorted JSON keys, fixed float rounding,
no timestamps), so re-running the script reproduces the committed bytes exactly.

Grip convention: the origin is the grip point (matches the socket_main_hand
mount); the haft runs up +Y, the head crosses it along X near the top.

Usage:
    python3 tools/art/generate_pickaxe_blockout.py \
        content/core/assets/art/item/weapon/pickaxe_rusty.glb
"""

from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

# Blockout dimensions (metres). Haft: thin box up +Y from the grip origin.
# Head: wide flat box crossing the haft just below its top.
HAFT = ((0.0, 0.34, 0.0), (0.045, 0.80, 0.045))  # (center, size)
HEAD = ((0.0, 0.66, 0.0), (0.52, 0.07, 0.07))

# Authored blockout colour (rusty brown) — the M1 dye path tints OVER this via a
# material override, so "unknown dye -> authored colors" is observable.
BASE_COLOR = (0.42, 0.27, 0.17, 1.0)

# One box = 6 faces x 4 verts (per-face normals) + 12 tris.
_FACES = (
    ((1, 0, 0), ((1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1))),
    ((-1, 0, 0), ((-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1))),
    ((0, 1, 0), ((-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1))),
    ((0, -1, 0), ((-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1))),
    ((0, 0, 1), ((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1))),
    ((0, 0, -1), ((1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1))),
)


def _box(center, size, positions, normals, indices):
    cx, cy, cz = center
    hx, hy, hz = (size[0] / 2.0, size[1] / 2.0, size[2] / 2.0)
    for normal, corners in _FACES:
        base = len(positions)
        for sx, sy, sz in corners:
            positions.append((
                round(cx + sx * hx, 6),
                round(cy + sy * hy, 6),
                round(cz + sz * hz, 6),
            ))
            normals.append(normal)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]


def build_glb() -> bytes:
    positions: list[tuple] = []
    normals: list[tuple] = []
    indices: list[int] = []
    _box(*HAFT, positions, normals, indices)
    _box(*HEAD, positions, normals, indices)

    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
    if len(idx_bytes) % 4:
        idx_bytes += b"\x00" * (4 - len(idx_bytes) % 4)
    bin_chunk = pos_bytes + nrm_bytes + idx_bytes

    mins = [min(p[a] for p in positions) for a in range(3)]
    maxs = [max(p[a] for p in positions) for a in range(3)]

    gltf = {
        "asset": {
            "version": "2.0",
            "generator": "meridian tools/art/generate_pickaxe_blockout.py",
        },
        "scene": 0,
        "scenes": [{"name": "pickaxe_rusty", "nodes": [0]}],
        "nodes": [{"name": "pickaxe_rusty", "mesh": 0}],
        "meshes": [{
            "name": "pickaxe_rusty",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "indices": 2,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": "m_pickaxe_rusty",
            "pbrMetallicRoughness": {
                "baseColorFactor": list(BASE_COLOR),
                "metallicFactor": 0.1,
                "roughnessFactor": 0.9,
            },
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": len(positions),
             "type": "VEC3", "min": mins, "max": maxs},
            {"bufferView": 1, "componentType": 5126, "count": len(normals),
             "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": len(indices),
             "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes),
             "byteLength": len(nrm_bytes), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bytes) + len(nrm_bytes),
             "byteLength": len(indices) * 2, "target": 34963},
        ],
        "buffers": [{"byteLength": len(bin_chunk)}],
    }

    json_bytes = json.dumps(gltf, sort_keys=True, separators=(",", ":")).encode()
    if len(json_bytes) % 4:
        json_bytes += b" " * (4 - len(json_bytes) % 4)

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_chunk)
    out = struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes  # JSON
    out += struct.pack("<II", len(bin_chunk), 0x004E4942) + bin_chunk    # BIN
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    out_path = Path(sys.argv[1])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(build_glb())
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
