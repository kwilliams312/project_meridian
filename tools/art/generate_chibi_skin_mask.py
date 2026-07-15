#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Derive the chibi body's SKIN dye-mask + a recolor-ready neutral base albedo
from the shared chibi_pill_body atlas (story #755, C4 rework — spec §6 + the
design ① §6 dye-mask model).

WHY (the rework): the first C4 pass recolored the whole body with a solid albedo,
which wiped the sculpt's painted eyes + cloth-wrap. The chosen fix is the
existing dye-mask path (client AssembledCharacter._apply_dyes / dye_tint.gdshader):
recolor ONLY the skin region and keep the eyes + cloth from the atlas. That
shader MULTIPLIES the masked albedo by the dye colour, so — exactly like the
Warden plates (generate_warden_kit.py: "base must be light for a dye to read") —
the skin base under the mask must be a light NEUTRAL, or blue/silver races can
never resolve (the atlas skin is saturated yellow, blue channel ~57/255).

This tool reads the body glb's embedded diffuse atlas and emits two textures:

  chibi_pill_body_mask.png         RGB dye mask. R (primary channel) = the
                                   recolorable SKIN region; G=B=0. Eyes, pupils,
                                   highlights and cloth-wrap are 0 → the dye
                                   never touches them (they keep the atlas).
  chibi_pill_body_recolor_base.png The albedo the dye multiplies: SKIN texels
                                   neutralised to a light luminance-preserving
                                   grey (so a race dye reads as its true colour);
                                   eyes + cloth kept BYTE-for-BYTE from the atlas.

Classification is a fixed HSV-ish threshold on the atlas texels (measured
clusters: skin = saturated yellow, sat≥.65 val≥210; cloth = warm grey sat≤.33;
eye = dark brown val<160 or white sat≈0). The thresholds + the neutralisation
map are PURE stdlib functions (unit-tested in tests/test_art_gen.py). Only the
atlas decode (Pillow) + glb read (pygltflib) are imported lazily in main(), so
the module imports without those deps and the logic is testable stdlib-only.

Run (Pillow + pygltflib are dev-only, not project deps):
  uv run --with pygltflib --with pillow python3 tools/art/generate_chibi_skin_mask.py \
    --glb content/chibi/assets/art/chibi_pill_body/sk_chibi_pill_body.glb \
    --out-dir content/chibi/assets/art/chibi_pill_body
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

# Output resolution (≤ the 2048 texture_set cap; the atlas is 2048²). 1024 keeps
# the eyes/cloth clearly readable while halving the committed bytes.
OUT_PX = 1024

# --- Skin classifier (fixed thresholds from the measured atlas clusters). ------
SKIN_MIN_SAT = 0.50  # cloth ≤ .33, brown eye ≈ .45 → excluded; yellow skin ≥ .65
SKIN_MIN_VAL = 160  # dark brown eye (val ~115) excluded; skin val ~210+

# Neutralisation: keep the skin texel's luminance structure but pull it to a
# light neutral grey so `base * dye` reads as the dye colour. LIFT compresses the
# distance to white (0 = unchanged, 1 = pure white); 0.45 lands typical skin
# (lum ~187) at ~224 — bright enough for a dye to read, dark enough to keep form.
NEUTRAL_LIFT = 0.45

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def is_skin(r: int, g: int, b: int) -> bool:
    """True if an atlas texel is the recolorable skin (saturated warm/yellow)."""
    mx = max(r, g, b)
    mn = min(r, g, b)
    if mx < SKIN_MIN_VAL:
        return False
    sat = 0.0 if mx == 0 else (mx - mn) / mx
    if sat < SKIN_MIN_SAT:
        return False
    # Warm/yellow: blue is the minimum channel (excludes any cool saturated hue).
    return b <= g and b <= r


def neutralize(r: int, g: int, b: int) -> tuple[int, int, int]:
    """Map a skin texel to a light, luminance-preserving neutral grey."""
    lum = round(0.299 * r + 0.587 * g + 0.114 * b)
    v = round(255 - (255 - lum) * (1.0 - NEUTRAL_LIFT))
    v = max(0, min(255, v))
    return (v, v, v)


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def encode_rgb_png(width: int, height: int, rgb_rows: bytes) -> bytes:
    """Deterministic 8-bit RGB PNG from raw scanlines (no per-row filter)."""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)  # filter type 0 (None)
        raw += rgb_rows[y * stride : (y + 1) * stride]
    compressed = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    out = bytearray()
    out += PNG_SIGNATURE
    out += _png_chunk(b"IHDR", ihdr)
    out += _png_chunk(b"IDAT", compressed)
    out += _png_chunk(b"IEND", b"")
    return bytes(out)


def build_mask_and_base(
    atlas_rgb: bytes, width: int, height: int
) -> tuple[bytes, bytes]:
    """From flat atlas RGB bytes, return (mask_rgb_bytes, base_rgb_bytes)."""
    mask = bytearray(len(atlas_rgb))
    base = bytearray(atlas_rgb)  # start as a copy; overwrite skin texels
    for i in range(0, len(atlas_rgb), 3):
        r, g, b = atlas_rgb[i], atlas_rgb[i + 1], atlas_rgb[i + 2]
        if is_skin(r, g, b):
            mask[i] = 255  # R = primary dye channel = recolorable skin
            nr, ng, nb = neutralize(r, g, b)
            base[i], base[i + 1], base[i + 2] = nr, ng, nb
        # else: mask stays (0,0,0); base keeps the original eye/cloth texel.
    return bytes(mask), bytes(base)


def _load_atlas(glb_path: str, out_px: int) -> tuple[bytes, int, int]:
    """Lazy (Pillow + pygltflib): decode the body atlas, resized to out_px²."""
    import io

    from pygltflib import GLTF2
    from PIL import Image

    g = GLTF2().load(glb_path)
    blob = g.binary_blob()
    bv = g.bufferViews[g.images[0].bufferView]
    start = bv.byteOffset or 0
    data = blob[start : start + bv.byteLength]
    img = Image.open(io.BytesIO(data)).convert("RGB")
    if img.size != (out_px, out_px):
        img = img.resize((out_px, out_px), Image.LANCZOS)
    return img.tobytes(), out_px, out_px


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--glb", required=True, help="Path to the body .glb (LFS).")
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args(argv)

    atlas, w, h = _load_atlas(args.glb, OUT_PX)
    mask_rgb, base_rgb = build_mask_and_base(atlas, w, h)
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    # Channel suffixes are required for the importer colorspace lint (I007):
    # _mask = the RGB dye mask, _bc = the base-colour (sRGB albedo).
    (out / "chibi_pill_body_mask.png").write_bytes(encode_rgb_png(w, h, mask_rgb))
    (out / "chibi_pill_body_recolor_bc.png").write_bytes(encode_rgb_png(w, h, base_rgb))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
