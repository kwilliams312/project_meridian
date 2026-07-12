#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic Warden's Kit armor — six skinned plates + RGB dye masks (story #569).

The Warden's Kit is M1's modular-gear proof: one armor piece per equip slot
(head, shoulders, chest, hands, legs, feet), each a low-poly plate SKINNED to the
canonical Meridian rig so it deforms with the body, plus a per-piece RGB dye-mask
texture (R/G/B == primary/secondary/accent, contract ① §6). It materialises the
promise the six ``core:item.warden_*`` item@2 files declare in ``visual.worn``.

Design (mirrors ``tools/art/generate_pickaxe_blockout.py`` and
``tools/blender/meridian_rig/generate_blockout.py``): a script-generated ORIGINAL,
no Blender and no third-party asset. Each plate is a subdivided box shell sized
directly from the rig's ``BoneSpec`` spans (``tools/blender/meridian_rig/bones.py``
— the single source of truth for bone rest positions), skinned with one influence
per vertex (weight 1.0) to the nearest canonical bone the piece covers. The GLB is
emitted directly as glTF 2.0 binary (stdlib only) and is byte-deterministic
(sorted JSON keys, fixed float rounding, no timestamps), so re-running reproduces
the committed bytes exactly. The dye masks are written by a minimal stdlib PNG
writer (zlib + CRC, no PIL), equally deterministic.

Budgets (Art PRD §2.1): each plate lands 3-8k tris; the full outfit's LOD0 total
stays ≤ 40k added. Skin binds ONLY canonical bone names (import validator I021).

Usage:
    python3 tools/art/generate_warden_kit.py [OUT_DIR]

OUT_DIR defaults to content/core/assets/art/item/armor. Writes
``warden_<slot>.glb`` + ``warden_<slot>_mask.png`` for all six slots.
"""

from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path

# Import the canonical bone table without requiring Blender (bones.py is pure-Python).
_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from tools.blender.meridian_rig.bones import ALL_BONES  # noqa: E402

Vec3 = tuple[float, float, float]

_BONE_HEAD: dict[str, Vec3] = {b.name: b.head_m for b in ALL_BONES}
_BONE_TAIL: dict[str, Vec3] = {b.name: b.tail_m for b in ALL_BONES}


def _bone_mid(name: str) -> Vec3:
    h, t = _BONE_HEAD[name], _BONE_TAIL[name]
    return ((h[0] + t[0]) / 2.0, (h[1] + t[1]) / 2.0, (h[2] + t[2]) / 2.0)


# --- Plate configuration ------------------------------------------------------
# One entry per equip slot. Each is a list of "shells" (a plate can be one central
# shell — chest/head — or a mirrored left/right pair — shoulders/hands/legs/feet).
# A shell: (center, size, subdivisions, [bind bone names]). Box sizes/centres are
# read off the BoneSpec spans in bones.py (see the module docstring); `n` is chosen
# so 12*n^2 tris per shell keeps every piece inside the 3-8k Art PRD §2.1 band and
# the six-piece LOD0 total under 40k. Paired shells share `n` (2*12*n^2 total).
#
# Shape = (center: Vec3, size: Vec3, n: int, bones: tuple[str, ...]).
Shell = tuple[Vec3, Vec3, int, tuple[str, ...]]

# Right-side shells for the paired slots; the left side is auto-mirrored (x -> -x,
# Right* -> Left*) so the two sides can never drift, exactly like bones.py.
_R = "Right"


def _mirror_bone_name(name: str) -> str:
    return "Left" + name[len(_R):] if name.startswith(_R) else name


def _mirror_shell(shell: Shell) -> Shell:
    (cx, cy, cz), size, n, bones = shell
    return ((-cx, cy, cz), size, n, tuple(_mirror_bone_name(b) for b in bones))


# Central (single-shell) plates.
_HEAD: list[Shell] = [((0.0, 1.62, 0.0), (0.22, 0.26, 0.24), 18, ("Head",))]
_CHEST: list[Shell] = [
    ((0.0, 1.245, 0.0), (0.42, 0.42, 0.30), 22, ("Spine", "Chest", "UpperChest")),
]

# Right shells for the paired plates (left mirrored below).
_SHOULDERS_R: Shell = ((0.13, 1.44, 0.0), (0.20, 0.16, 0.22), 13, ("RightShoulder",))
_HANDS_R: Shell = ((0.66, 1.42, 0.0), (0.24, 0.14, 0.14), 13, ("RightHand",))
_LEGS_R: Shell = ((0.09, 0.525, 0.0), (0.20, 0.90, 0.22), 15,
                  ("RightUpperLeg", "RightLowerLeg"))
_FEET_R: Shell = ((0.09, 0.12, 0.05), (0.18, 0.30, 0.34), 14,
                  ("RightFoot", "RightLowerLeg"))

SLOTS: dict[str, list[Shell]] = {
    "head": _HEAD,
    "shoulders": [_SHOULDERS_R, _mirror_shell(_SHOULDERS_R)],
    "chest": _CHEST,
    "hands": [_HANDS_R, _mirror_shell(_HANDS_R)],
    "legs": [_LEGS_R, _mirror_shell(_LEGS_R)],
    "feet": [_FEET_R, _mirror_shell(_FEET_R)],
}

# Authored plate colour (weathered steel). The dye path tints OVER this via the
# mask (S3); an unknown/absent dye leaves this authored albedo visible.
BASE_COLOR = (0.38, 0.40, 0.44, 1.0)

# One box face: (outward normal, 4 corners as +/-1 signs) — CCW seen from outside.
_FACES = (
    ((1, 0, 0), ((1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1))),
    ((-1, 0, 0), ((-1, -1, 1), (-1, 1, 1), (-1, 1, -1), (-1, -1, -1))),
    ((0, 1, 0), ((-1, 1, -1), (-1, 1, 1), (1, 1, 1), (1, 1, -1))),
    ((0, -1, 0), ((-1, -1, 1), (-1, -1, -1), (1, -1, -1), (1, -1, 1))),
    ((0, 0, 1), ((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1))),
    ((0, 0, -1), ((1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1))),
)


def _dist2(a: Vec3, b: Vec3) -> float:
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def _nearest_bone(point: Vec3, bones: tuple[str, ...]) -> str:
    """Bone (by name) whose rest midpoint is nearest `point` — the vertex's owner."""
    return min(bones, key=lambda name: _dist2(point, _bone_mid(name)))


def _subdivided_face(normal, corners, center: Vec3, half: Vec3, n: int):
    """Yield (position, normal) for an n×n grid of quads on one box face.

    Returns (positions, normals, quads) where quads index into the returned
    positions list (local to this face).
    """
    cx, cy, cz = center
    hx, hy, hz = half
    # Two in-plane edge vectors of the face (corner0->corner1, corner0->corner3).
    c0, c1, _c2, c3 = corners
    e_u = (c1[0] - c0[0], c1[1] - c0[1], c1[2] - c0[2])
    e_v = (c3[0] - c0[0], c3[1] - c0[1], c3[2] - c0[2])
    positions: list[Vec3] = []
    for iu in range(n + 1):
        u = iu / n
        for iv in range(n + 1):
            v = iv / n
            sx = c0[0] + e_u[0] * u + e_v[0] * v
            sy = c0[1] + e_u[1] * u + e_v[1] * v
            sz = c0[2] + e_u[2] * u + e_v[2] * v
            positions.append((
                round(cx + sx * hx, 6),
                round(cy + sy * hy, 6),
                round(cz + sz * hz, 6),
            ))
    stride = n + 1
    quads: list[tuple[int, int, int, int]] = []
    for iu in range(n):
        for iv in range(n):
            a = iu * stride + iv
            b = a + 1
            c = a + stride
            d = c + 1
            # CCW winding consistent with the face's outward normal.
            quads.append((a, c, d, b))
    norm = tuple(float(x) for x in normal)
    return positions, norm, quads


def build_shells(shells: list[Shell]):
    """Build merged geometry for a plate's shells.

    Returns (positions, normals, indices, vjoints, used_bones) where `vjoints`[i]
    is the bone name owning vertex i (single influence, weight 1.0) and `used_bones`
    is the sorted unique bone set (the skin's joint list).
    """
    positions: list[Vec3] = []
    normals: list[Vec3] = []
    indices: list[int] = []
    vjoints: list[str] = []
    used: set[str] = set()
    for center, size, n, bones in shells:
        half = (size[0] / 2.0, size[1] / 2.0, size[2] / 2.0)
        used.update(bones)
        for normal, corners in _FACES:
            fpos, fnorm, quads = _subdivided_face(normal, corners, center, half, n)
            base = len(positions)
            for p in fpos:
                positions.append(p)
                normals.append(fnorm)
                vjoints.append(_nearest_bone(p, bones))
            for a, c, d, b in quads:
                indices += [base + a, base + c, base + d, base + a, base + d, base + b]
    return positions, normals, indices, vjoints, sorted(used)


def _pad4(buf: bytes, fill: bytes = b"\x00") -> bytes:
    rem = len(buf) % 4
    return buf if rem == 0 else buf + fill * (4 - rem)


def build_plate_glb(slot: str) -> bytes:
    """Deterministic skinned glTF-binary bytes for one Warden's Kit plate."""
    shells = SLOTS[slot]
    positions, normals, indices, vjoints, used_bones = build_shells(shells)
    joint_index = {name: i for i, name in enumerate(used_bones)}

    # Vertex attribute buffers.
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    # JOINTS_0 (VEC4 UBYTE) + WEIGHTS_0 (VEC4 float): one influence, weight 1.0.
    joints_bytes = b"".join(
        struct.pack("<4B", joint_index[vj], 0, 0, 0) for vj in vjoints
    )
    weights_bytes = b"".join(struct.pack("<4f", 1.0, 0.0, 0.0, 0.0) for _ in vjoints)
    idx_bytes = b"".join(struct.pack("<H", i) for i in indices)
    # inverseBindMatrices: translate(-head) per joint (column-major MAT4).
    ibm_bytes = b""
    for name in used_bones:
        hx, hy, hz = _BONE_HEAD[name]
        ibm_bytes += struct.pack(
            "<16f",
            1.0, 0.0, 0.0, 0.0,
            0.0, 1.0, 0.0, 0.0,
            0.0, 0.0, 1.0, 0.0,
            round(-hx, 6), round(-hy, 6), round(-hz, 6), 1.0,
        )

    # Lay bufferViews out 4-byte aligned in declaration order.
    chunks = [pos_bytes, nrm_bytes, joints_bytes, weights_bytes, ibm_bytes,
              _pad4(idx_bytes)]
    offsets: list[int] = []
    blob = b""
    for c in chunks:
        offsets.append(len(blob))
        blob += c
    (pos_off, nrm_off, joi_off, wgt_off, ibm_off, idx_off) = offsets

    mins = [min(p[a] for p in positions) for a in range(3)]
    maxs = [max(p[a] for p in positions) for a in range(3)]

    nverts = len(positions)
    njoints = len(used_bones)

    # Nodes: 0 = armature root; 1..njoints = joint nodes (translation = head);
    # last = the skinned mesh node (identity transform, ignored for skinning).
    nodes: list[dict] = [{
        "name": f"warden_{slot}_armature",
        "children": list(range(1, 1 + njoints)),
    }]
    for name in used_bones:
        hx, hy, hz = _BONE_HEAD[name]
        nodes.append({"name": name, "translation": [round(hx, 6), round(hy, 6),
                                                     round(hz, 6)]})
    mesh_node = 1 + njoints
    nodes.append({"name": f"warden_{slot}", "mesh": 0, "skin": 0})

    gltf = {
        "asset": {
            "version": "2.0",
            "generator": "meridian tools/art/generate_warden_kit.py",
        },
        "scene": 0,
        "scenes": [{"name": f"warden_{slot}", "nodes": [0, mesh_node]}],
        "nodes": nodes,
        "skins": [{
            "name": f"warden_{slot}_skin",
            "skeleton": 0,
            "joints": list(range(1, 1 + njoints)),
            "inverseBindMatrices": 4,
        }],
        "meshes": [{
            "name": f"warden_{slot}",
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "JOINTS_0": 2,
                               "WEIGHTS_0": 3},
                "indices": 5,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": f"m_warden_{slot}",
            "pbrMetallicRoughness": {
                "baseColorFactor": list(BASE_COLOR),
                "metallicFactor": 0.6,
                "roughnessFactor": 0.5,
            },
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": nverts,
             "type": "VEC3", "min": mins, "max": maxs},          # 0 POSITION
            {"bufferView": 1, "componentType": 5126, "count": nverts,
             "type": "VEC3"},                                     # 1 NORMAL
            {"bufferView": 2, "componentType": 5121, "count": nverts,
             "type": "VEC4"},                                     # 2 JOINTS_0 (UBYTE)
            {"bufferView": 3, "componentType": 5126, "count": nverts,
             "type": "VEC4"},                                     # 3 WEIGHTS_0
            {"bufferView": 4, "componentType": 5126, "count": njoints,
             "type": "MAT4"},                                     # 4 inverseBind
            {"bufferView": 5, "componentType": 5123, "count": len(indices),
             "type": "SCALAR"},                                   # 5 indices
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_off, "byteLength": len(pos_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": nrm_off, "byteLength": len(nrm_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": joi_off, "byteLength": len(joints_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": wgt_off, "byteLength": len(weights_bytes),
             "target": 34962},
            {"buffer": 0, "byteOffset": ibm_off, "byteLength": len(ibm_bytes)},
            {"buffer": 0, "byteOffset": idx_off, "byteLength": len(idx_bytes),
             "target": 34963},
        ],
        "buffers": [{"byteLength": len(blob)}],
    }

    json_bytes = json.dumps(gltf, sort_keys=True, separators=(",", ":")).encode()
    json_bytes = _pad4(json_bytes, b" ")

    total = 12 + 8 + len(json_bytes) + 8 + len(blob)
    out = struct.pack("<4sII", b"glTF", 2, total)
    out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes  # JSON
    out += struct.pack("<II", len(blob), 0x004E4942) + blob              # BIN
    return out


def plate_tri_count(slot: str) -> int:
    """LOD0 triangle count for a plate (12 tris per box face × subdivisions)."""
    return sum(6 * n * n * 2 for _c, _s, n, _b in SLOTS[slot])


# --- Dye masks (RGB: R/G/B = primary/secondary/accent, contract ① §6) ---------
_MASK_SIZE = 64


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def build_mask_png(slot: str) -> bytes:
    """Deterministic RGB dye-mask PNG for one plate.

    R selects the primary-dye region, G the secondary, B the accent. Regions are a
    simple, per-slot-varied banding so each piece has a distinct primary/secondary
    split; the shader (S3) multiplies each channel's region by the chosen dye.
    """
    n = _MASK_SIZE
    # Per-slot phase so the six masks are not byte-identical (deterministic).
    phase = list(SLOTS).index(slot)
    rows = bytearray()
    for y in range(n):
        rows.append(0)  # PNG filter type 0 (None) per scanline
        for x in range(n):
            t = (y + phase * 4) / n
            if t < 0.55:
                px = (255, 0, 0)        # primary
            elif t < 0.9:
                px = (0, 255, 0)        # secondary
            else:
                px = (0, 0, 255)        # accent
            # A narrow neutral seam every 16 px keeps some albedo undyed.
            if (x + phase) % 16 == 0:
                px = (0, 0, 0)
            rows.extend(px)
    ihdr = struct.pack(">IIBBBBB", n, n, 8, 2, 0, 0, 0)  # 8-bit RGB
    png = b"\x89PNG\r\n\x1a\n"
    png += _png_chunk(b"IHDR", ihdr)
    png += _png_chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += _png_chunk(b"IEND", b"")
    return png


def main(argv: list[str]) -> int:
    out_dir = Path(argv[1]) if len(argv) > 1 else (
        _REPO_ROOT / "content/core/assets/art/item/armor"
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    total_tris = 0
    for slot in SLOTS:
        glb = build_plate_glb(slot)
        (out_dir / f"warden_{slot}.glb").write_bytes(glb)
        (out_dir / f"warden_{slot}_mask.png").write_bytes(build_mask_png(slot))
        tris = plate_tri_count(slot)
        total_tris += tris
        print(f"  warden_{slot}: {tris} tris, {len(glb)} glb bytes")
    print(f"wrote 6 plates + 6 masks to {out_dir} (outfit LOD0 {total_tris} tris)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
