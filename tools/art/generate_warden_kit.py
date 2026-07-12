#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic Warden's Kit armor — six skinned plates + RGB dye masks (story #569).

The Warden's Kit is M1's modular-gear proof: one armor piece per equip slot
(head, shoulders, chest, hands, legs, feet), each a low-poly plate SKINNED to the
canonical Meridian rig so it deforms with the body, plus a per-piece RGB dye-mask
texture (R/G/B == primary/secondary/accent, contract ① §6). It materialises the
promise the six ``core:item.warden_*`` item@2 files declare in ``visual.worn``.

CONFORMING PLATES (issue #589). Each plate is NOT a floating box: it is a shell
LOFTED to the real body surface it covers. The generator reads the committed
stylized body mesh (``content/core/assets/art/char/sk_ardent_male_base.glb``, cut
into ``geo_<region>_lod<N>`` geosets, ⑤/S4), samples the LOD0 surface of the
body region the piece hides (``worn.hides``), and builds the plate as a radial
loft that follows that surface pushed out by a thin ``margin``. Because the plate
occupies exactly where the body geoset was (plus a hair), hiding the underlying
region no longer leaves the blocky-plate voids/black gaps from the ⑤/S6 render —
the plates read as armor ON the body, not disconnected boxes.

Design (mirrors ``tools/art/generate_pickaxe_blockout.py``): a script-generated
ORIGINAL, no Blender and no third-party asset. The loft is skinned with one
influence per vertex (weight 1.0) to the nearest canonical bone the piece covers
(``tools/blender/meridian_rig/bones.py`` — the single source of truth for bone
rest positions). The GLB is emitted directly as glTF 2.0 binary (stdlib only) and
is byte-deterministic on a given machine (sorted JSON keys, fixed float rounding,
deterministic body-sample iteration, no timestamps). The dye masks are written by
a minimal stdlib PNG writer (zlib + CRC, no PIL), equally deterministic.

Budgets (Art PRD §2.1): each plate lands 3-8k tris; the full outfit's LOD0 total
stays ≤ 40k added. Skin binds ONLY canonical bone names (import validator I021).

Usage:
    python3 tools/art/generate_warden_kit.py [OUT_DIR]

OUT_DIR defaults to content/core/assets/art/item/armor. Writes
``warden_<slot>.glb`` + ``warden_<slot>_mask.png`` for all six slots.
"""

from __future__ import annotations

import json
import math
import struct
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path

# Import the canonical bone table without requiring Blender (bones.py is pure-Python).
_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from tools.blender.meridian_rig.bones import ALL_BONES  # noqa: E402

Vec3 = tuple[float, float, float]

_BONE_HEAD: dict[str, Vec3] = {b.name: b.head_m for b in ALL_BONES}
_BONE_TAIL: dict[str, Vec3] = {b.name: b.tail_m for b in ALL_BONES}

# The committed real body whose surface every plate conforms to (⑤/S4). Sampled
# read-only at generation time; its bytes are fixed content, so the sampling is
# deterministic. This module NEVER writes it (that is generate_blockout.py / the
# restyle pipeline) and NEVER touches the body geoset cut (issue #587).
_BODY_GLB = _REPO_ROOT / "content/core/assets/art/char/sk_ardent_male_base.glb"


def _bone_mid(name: str) -> Vec3:
    h, t = _BONE_HEAD[name], _BONE_TAIL[name]
    return ((h[0] + t[0]) / 2.0, (h[1] + t[1]) / 2.0, (h[2] + t[2]) / 2.0)


# --- Body-mesh sampling (stdlib GLB reader) -----------------------------------
_BODY_CACHE: dict[str, list[Vec3]] = {}


def _read_body_positions(mesh_name: str) -> list[Vec3]:
    """Return the LOD0 vertex positions of a body geoset mesh (``geo_<region>_lod0``).

    A minimal stdlib glTF-binary reader: parse the JSON + BIN chunks, find the
    named mesh's first primitive, and read its (tightly-packed float VEC3)
    POSITION accessor. Cached per mesh so a paired plate reads the file once.
    """
    if mesh_name in _BODY_CACHE:
        return _BODY_CACHE[mesh_name]
    glb = _BODY_GLB.read_bytes()
    magic, _ver, total = struct.unpack_from("<4sII", glb, 0)
    if magic != b"glTF":
        raise ValueError(f"{_BODY_GLB} is not a glTF binary")
    off, doc, bin_off = 12, None, None
    while off < total:
        clen, ctype = struct.unpack_from("<II", glb, off)
        off += 8
        if ctype == 0x4E4F534A:  # 'JSON'
            doc = json.loads(glb[off:off + clen])
        elif ctype == 0x004E4942:  # 'BIN\0'
            bin_off = off
        off += clen + ((4 - clen % 4) % 4)
    if doc is None or bin_off is None:
        raise ValueError(f"{_BODY_GLB} missing JSON or BIN chunk")
    mesh = next(m for m in doc["meshes"] if m["name"] == mesh_name)
    acc = doc["accessors"][mesh["primitives"][0]["attributes"]["POSITION"]]
    bv = doc["bufferViews"][acc["bufferView"]]
    base = bin_off + bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride") or 12
    out = [struct.unpack_from("<3f", glb, base + i * stride) for i in range(acc["count"])]
    _BODY_CACHE[mesh_name] = out
    return out


# --- Plate configuration ------------------------------------------------------
# One entry per equip slot: a list of conforming "shells". A plate is one central
# shell (chest/head) or a mirrored left/right pair (shoulders/hands/legs/feet).
# Each shell names the body geoset region(s) it covers and how to loft a shell to
# that surface; `n_slices`/`n_sectors` are chosen so 2*n_sectors*n_slices tris per
# shell keeps every piece inside the 3-8k Art PRD §2.1 band and the six-piece
# LOD0 total under 40k. Paired shells split the tri budget between the two sides.


@dataclass(frozen=True)
class ConformShell:
    """A plate shell lofted to the body surface of one region (issue #589).

    ``meshes`` are the body geoset LOD0 meshes to sample; ``axis`` is the loft
    (long) axis (0=X for the arm gloves, 1=Y for vertical torso/limb pieces);
    ``side`` filters body verts to one half (+1: x>0, -1: x<0, 0: whole) so a
    paired piece hugs its own limb; ``bones`` are the canonical bind bones (the
    nearest owns each vertex). ``margin`` pushes the shell just proud of the body
    so hiding the region leaves NO void; ``y_min``/``x_min_abs`` optionally crop
    the sampled region (the shoulder pauldron takes only the upper-outer torso).
    """

    meshes: tuple[str, ...]
    axis: int
    side: int
    n_slices: int
    n_sectors: int
    bones: tuple[str, ...]
    margin: float = 0.02
    min_radius: float = 0.03
    y_min: float | None = None
    x_min_abs: float | None = None


_R = "Right"


def _mirror_bone_name(name: str) -> str:
    return "Left" + name[len(_R):] if name.startswith(_R) else name


def _mirror_shell(shell: ConformShell) -> ConformShell:
    """Mirror a right-side shell to the left (side flip + Right*->Left* bones)."""
    return ConformShell(
        meshes=shell.meshes,
        axis=shell.axis,
        side=-shell.side,
        n_slices=shell.n_slices,
        n_sectors=shell.n_sectors,
        bones=tuple(_mirror_bone_name(b) for b in shell.bones),
        margin=shell.margin,
        min_radius=shell.min_radius,
        y_min=shell.y_min,
        x_min_abs=shell.x_min_abs,
    )


# Central (single-shell) plates — a helmet dome and a torso cuirass.
_HEAD = ConformShell(("geo_head_lod0",), axis=1, side=0,
                     n_slices=46, n_sectors=34, bones=("Head",), margin=0.018)
_CHEST = ConformShell(("geo_torso_lod0",), axis=1, side=0,
                      n_slices=56, n_sectors=40,
                      bones=("Spine", "Chest", "UpperChest"), margin=0.02)

# Right shells for the paired plates (left mirrored below).
# Shoulders: a pauldron over the upper-OUTER torso (deltoid cap), not the whole
# torso — its ``worn.hides`` was reduced to [] (issue #589) so a shoulders-alone
# equip can no longer erase the whole torso and void it; in the full kit the
# chest cuirass hides+covers the torso and the pauldron layers on top.
_SHOULDERS_R = ConformShell(("geo_torso_lod0",), axis=1, side=+1,
                            n_slices=32, n_sectors=26, bones=("RightShoulder",),
                            margin=0.022, y_min=1.33, x_min_abs=0.10)
# Hands: a glove lofted along the arm axis over the ``hands`` geoset — hand-scale
# and far out on the arm (~0.6-0.9 m), disjoint from the left glove with a wide
# empty gap across the torso (asserted in tests), never a bar across the T-pose.
_HANDS_R = ConformShell(("geo_hands_lod0",), axis=0, side=+1,
                        n_slices=32, n_sectors=26, bones=("RightHand",),
                        margin=0.014, min_radius=0.02)
# Legs: a thigh guard over the ``hips_legs`` geoset (right half).
_LEGS_R = ConformShell(("geo_hips_legs_lod0",), axis=1, side=+1,
                       n_slices=40, n_sectors=28,
                       bones=("RightUpperLeg", "RightLowerLeg"), margin=0.02)
# Feet: a greave+boot over the ``lower_legs`` + ``feet`` geosets (right half).
_FEET_R = ConformShell(("geo_lower_legs_lod0", "geo_feet_lod0"), axis=1, side=+1,
                       n_slices=36, n_sectors=28,
                       bones=("RightFoot", "RightLowerLeg"), margin=0.02)

SLOTS: dict[str, list[ConformShell]] = {
    "head": [_HEAD],
    "shoulders": [_SHOULDERS_R, _mirror_shell(_SHOULDERS_R)],
    "chest": [_CHEST],
    "hands": [_HANDS_R, _mirror_shell(_HANDS_R)],
    "legs": [_LEGS_R, _mirror_shell(_LEGS_R)],
    "feet": [_FEET_R, _mirror_shell(_FEET_R)],
}

# Authored plate colour — LIGHT brushed steel. The dye path MULTIPLIES this via
# the mask (S3: albedo = base * dye), so the base must be light for a dye to read
# as its true hue: grey (0.4) × russet ≈ mud, but light steel (0.75) × russet ≈
# russet (⑤/S6 lead GPU finding). An unknown/absent dye leaves this light steel
# visible. Kept slightly cool + just under 1.0 so undyed plates still read as
# metal, not white plastic.
BASE_COLOR = (0.74, 0.76, 0.80, 1.0)

def _dist2(a: Vec3, b: Vec3) -> float:
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def _nearest_bone(point: Vec3, bones: tuple[str, ...]) -> str:
    """Bone (by name) whose rest midpoint is nearest `point` — the vertex's owner."""
    return min(bones, key=lambda name: _dist2(point, _bone_mid(name)))


def _sample_region(shell: ConformShell) -> list[Vec3]:
    """Body verts of the region this shell covers, filtered to its side/crop."""
    verts: list[Vec3] = []
    for mesh in shell.meshes:
        verts += _read_body_positions(mesh)
    if shell.side != 0:
        verts = [v for v in verts if (v[0] > 0.0) == (shell.side > 0)]
    if shell.y_min is not None:
        verts = [v for v in verts if v[1] >= shell.y_min]
    if shell.x_min_abs is not None:
        verts = [v for v in verts if abs(v[0]) >= shell.x_min_abs]
    if not verts:
        raise ValueError(f"conform shell sampled no body verts: {shell}")
    return verts


def _loft_rings(shell: ConformShell):
    """Compute the loft's along-axis samples and per-(slice,sector) hull radii.

    Buckets the sampled body verts into ``n_slices`` bands along ``axis``; in each
    band, records the outward radius of the body surface per angular ``sector``
    about the region's cross-plane centroid (a star-shaped hull that captures
    asymmetric extents like the foot's toe or the head's crown). Empty sectors are
    filled from their nearest angular neighbour (floored at ``min_radius``) so the
    ring is always closed. Returns (a_lo, a_hi, cu, cw, cross_axes, rings).
    """
    verts = _sample_region(shell)
    axis = shell.axis
    cross = tuple(i for i in range(3) if i != axis)
    a_vals = [v[axis] for v in verts]
    a_lo, a_hi = min(a_vals), max(a_vals)
    span = a_hi - a_lo
    cu = sum(v[cross[0]] for v in verts) / len(verts)
    cw = sum(v[cross[1]] for v in verts) / len(verts)
    S, A = shell.n_slices, shell.n_sectors
    tau = 2.0 * math.pi
    half_band = (span / (S - 1)) if S > 1 else max(span, 1e-4)
    rings: list[list[float]] = []
    for i in range(S):
        a_i = a_lo + (span * i / (S - 1) if S > 1 else 0.0)
        sect = [0.0] * A
        for v in verts:
            if abs(v[axis] - a_i) > half_band:
                continue
            du = v[cross[0]] - cu
            dw = v[cross[1]] - cw
            r = math.hypot(du, dw)
            j = int((math.atan2(dw, du) % tau) / tau * A) % A
            if r > sect[j]:
                sect[j] = r
        for j in range(A):  # angular neighbour fill for empty sectors
            if sect[j] == 0.0:
                best = 0.0
                for d in range(1, A // 2 + 1):
                    best = max(sect[(j - d) % A], sect[(j + d) % A])
                    if best > 0.0:
                        break
                sect[j] = max(best, shell.min_radius)
        rings.append(sect)
    return a_lo, a_hi, cu, cw, cross, rings


def _build_conform_shell(shell: ConformShell):
    """Build one lofted shell's geometry, returning parallel per-vertex lists.

    Returns (positions, normals, uvs, quad_indices, cap_tris, vjoints). Positions
    lie on the body hull pushed out by ``margin`` (so hiding the region leaves no
    void); UVs are cylindrical (u around, v along) so the RGB dye mask (⑤/S3)
    wraps the whole piece; each vertex is single-influence-skinned to the nearest
    bind bone. The two along-axis ends are extended by ``margin`` and capped so
    the shell is a closed volume (no hollow interior showing at seams).
    """
    a_lo, a_hi, cu, cw, cross, rings = _loft_rings(shell)
    S, A, axis, m = shell.n_slices, shell.n_sectors, shell.axis, shell.margin
    tau = 2.0 * math.pi
    span = a_hi - a_lo if a_hi > a_lo else 1e-4

    def place(a: float, r: float, ang: float) -> Vec3:
        p = [0.0, 0.0, 0.0]
        p[axis] = a
        p[cross[0]] = cu + r * math.cos(ang)
        p[cross[1]] = cw + r * math.sin(ang)
        return (round(p[0], 6), round(p[1], 6), round(p[2], 6))

    def radial_normal(ang: float) -> Vec3:
        n = [0.0, 0.0, 0.0]
        n[cross[0]] = math.cos(ang)
        n[cross[1]] = math.sin(ang)
        return (round(n[0], 6), round(n[1], 6), round(n[2], 6))

    positions: list[Vec3] = []
    normals: list[Vec3] = []
    uvs: list[tuple[float, float]] = []
    vjoints: list[str] = []
    # Wall: S rings × (A+1) columns (the +1 column duplicates the seam for a clean UV).
    for i in range(S):
        # Extend the two ends outward by the margin so the plate overshoots the
        # region and the seam to the adjacent body/plate can never gap.
        a = a_lo + span * i / (S - 1) if S > 1 else a_lo
        if i == 0:
            a -= m
        elif i == S - 1:
            a += m
        v = i / (S - 1) if S > 1 else 0.0
        for j in range(A + 1):
            ang = tau * j / A
            r = rings[i][j % A] + m
            p = place(a, r, ang)
            positions.append(p)
            normals.append(radial_normal(ang))
            uvs.append((round(j / A, 6), round(v, 6)))
            vjoints.append(_nearest_bone(p, shell.bones))
    stride = A + 1
    quads: list[tuple[int, int, int, int]] = []
    for i in range(S - 1):
        for j in range(A):
            a0 = i * stride + j
            quads.append((a0, a0 + stride, a0 + stride + 1, a0 + 1))
    # Two end caps: a centre vertex fanned to the first A wall verts of the end ring.
    cap_tris: list[tuple[int, int, int]] = []
    for end, ring_base, a_end, nrm_sign in (
        (0, 0, a_lo - m, -1.0), (1, (S - 1) * stride, a_hi + m, +1.0),
    ):
        c_idx = len(positions)
        cp = [0.0, 0.0, 0.0]
        cp[axis] = a_end
        cp[cross[0]] = cu
        cp[cross[1]] = cw
        positions.append((round(cp[0], 6), round(cp[1], 6), round(cp[2], 6)))
        cn = [0.0, 0.0, 0.0]
        cn[axis] = nrm_sign
        normals.append((round(cn[0], 6), round(cn[1], 6), round(cn[2], 6)))
        uvs.append((round(0.5, 6), round(float(end), 6)))
        vjoints.append(_nearest_bone(positions[c_idx], shell.bones))
        for j in range(A):
            r0 = ring_base + j
            r1 = ring_base + (j + 1)
            if nrm_sign < 0:  # keep outward-consistent winding per cap
                cap_tris.append((c_idx, r1, r0))
            else:
                cap_tris.append((c_idx, r0, r1))
    return positions, normals, uvs, quads, cap_tris, vjoints


def build_shells(shells: list[ConformShell]):
    """Build merged geometry for a plate's conforming shells.

    Returns (positions, normals, indices, vjoints, used_bones, uvs) where
    `vjoints`[i] is the bone name owning vertex i (single influence, weight 1.0),
    `used_bones` is the sorted unique bone set (the skin's joint list), and
    `uvs`[i] is the per-vertex cylindrical UV that wraps the dye mask over the
    whole shell.
    """
    positions: list[Vec3] = []
    normals: list[Vec3] = []
    uvs: list[tuple[float, float]] = []
    indices: list[int] = []
    vjoints: list[str] = []
    used: set[str] = set()
    for shell in shells:
        used.update(shell.bones)
        spos, snorm, suv, quads, cap_tris, svj = _build_conform_shell(shell)
        base = len(positions)
        positions += spos
        normals += snorm
        uvs += suv
        vjoints += svj
        for a, b, c, d in quads:
            indices += [base + a, base + b, base + c, base + a, base + c, base + d]
        for a, b, c in cap_tris:
            indices += [base + a, base + b, base + c]
    return positions, normals, indices, vjoints, sorted(used), uvs


def shell_extents(slot: str) -> list[tuple[float, float, float]]:
    """Per-shell local AABB size (dx, dy, dz) for a plate — one entry per shell.

    Each shell is one contiguous armor volume (a glove, a boot, a pauldron). A
    paired plate returns two entries. Used to assert each piece is anatomy-scale
    (e.g. a glove is hand-scale) independent of how far apart the two sides sit in
    the T-pose — the six-piece plate can be wide, but no single volume should be.
    """
    out: list[tuple[float, float, float]] = []
    for shell in SLOTS[slot]:
        pos, *_ = build_shells([shell])
        out.append(tuple(  # type: ignore[arg-type]
            round(max(p[a] for p in pos) - min(p[a] for p in pos), 6)
            for a in range(3)
        ))
    return out


def _pad4(buf: bytes, fill: bytes = b"\x00") -> bytes:
    rem = len(buf) % 4
    return buf if rem == 0 else buf + fill * (4 - rem)


def build_plate_glb(slot: str) -> bytes:
    """Deterministic skinned glTF-binary bytes for one Warden's Kit plate."""
    shells = SLOTS[slot]
    positions, normals, indices, vjoints, used_bones, uvs = build_shells(shells)
    joint_index = {name: i for i, name in enumerate(used_bones)}

    # Vertex attribute buffers.
    pos_bytes = b"".join(struct.pack("<3f", *p) for p in positions)
    nrm_bytes = b"".join(struct.pack("<3f", *n) for n in normals)
    uv_bytes = b"".join(struct.pack("<2f", *uv) for uv in uvs)
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
    chunks = [pos_bytes, nrm_bytes, uv_bytes, joints_bytes, weights_bytes,
              ibm_bytes, _pad4(idx_bytes)]
    offsets: list[int] = []
    blob = b""
    for c in chunks:
        offsets.append(len(blob))
        blob += c
    (pos_off, nrm_off, uv_off, joi_off, wgt_off, ibm_off, idx_off) = offsets

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
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 6,
                               "JOINTS_0": 2, "WEIGHTS_0": 3},
                "indices": 5,
                "material": 0,
            }],
        }],
        "materials": [{
            "name": f"m_warden_{slot}",
            "pbrMetallicRoughness": {
                "baseColorFactor": list(BASE_COLOR),
                # Low metallic so the (dyed) ALBEDO drives the diffuse look — a
                # high metallic surface reflects the environment and swallows the
                # albedo, which is what made S2's dyes read dark/muddy (⑤/S6).
                "metallicFactor": 0.15,
                "roughnessFactor": 0.55,
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
            {"bufferView": 6, "componentType": 5126, "count": nverts,
             "type": "VEC2"},                                     # 6 TEXCOORD_0
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
            {"buffer": 0, "byteOffset": uv_off, "byteLength": len(uv_bytes),
             "target": 34962},
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
    """LOD0 triangle count for a plate: per shell, wall 2*A*(S-1) + 2 caps of A."""
    return sum(2 * s.n_sectors * s.n_slices for s in SLOTS[slot])


# --- Dye masks (RGB: R/G/B = primary/secondary/accent, contract ① §6) ---------
_MASK_SIZE = 64


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def build_mask_png(slot: str) -> bytes:
    """Deterministic RGB dye-mask PNG for one plate.

    R selects the primary-dye region, G the secondary, B the accent. Every texel
    belongs to exactly ONE channel — the mask covers the FULL dyeable surface with
    NO neutral gaps, so a primary dye tints the whole plate body rather than a
    stripe (⑤/S6 lead GPU finding). Layout: a dominant primary body, one secondary
    trim band (per-slot placement so the six masks stay distinct + deterministic),
    and a thin accent piping edge. With the plate's per-face UVs (build_shells),
    this banding maps across every face of the piece.
    """
    n = _MASK_SIZE
    # Per-slot phase so the six masks are not byte-identical (deterministic).
    phase = list(SLOTS).index(slot)
    # Secondary trim band: ~22% tall, its top edge walked per slot so no two masks
    # are identical (all six band tops are distinct: 0.30,0.40,0.50,0.60,0.70,0.00).
    band_top = round((0.30 + 0.10 * phase) % 0.80, 6)
    band_bot = band_top + 0.22
    rows = bytearray()
    for y in range(n):
        rows.append(0)  # PNG filter type 0 (None) per scanline
        v = y / n
        for _x in range(n):
            if band_top <= v < band_bot:
                px = (0, 255, 0)        # secondary trim band
            elif v >= 0.93:
                px = (0, 0, 255)        # accent piping (thin edge)
            else:
                px = (255, 0, 0)        # primary body (dominant, full coverage)
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
