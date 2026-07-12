#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic stdlib PNG generator for ardent/male face + skin customization
textures (story #568, ⑤/S1 — Character Content Foundation).

Spec §2 (docs/superpowers/specs/2026-07-11-character-content-foundation-design.md):
face (4) + skin (3) are "procedural stylized textures/palettes, `source_tier:
original`, scripted generator under `tools/art/`. Deterministic; no Meshy."

Follows the deterministic-binary-writer posture of
`tools/art/generate_pickaxe_blockout.py`: no third-party deps (no PIL, no
numpy), pure Python stdlib (`zlib` for IDAT compression + CRC32), fixed
integer/arithmetic pixel generation (no randomness, no timestamps, no
filesystem-order dependence) so re-running the script reproduces the exact
same bytes — a hash of each output is stable across runs (asserted by
tests/test_art_gen.py).

Output is a minimal valid PNG: signature + IHDR (8-bit RGB, no interlace) +
one IDAT chunk (zlib-compressed scanlines, filter type 0/None per row) + IEND.
Dimensions (256x256) are far under the Art PRD §2.3 / presets.json
`texture_set` 2048px cap and the `character_model` 2048px "player character
body" texture-set budget the ardent's face/skin customization textures share.

Faces carry subtle painted feature variation (eyebrow arch, eye size, lip
tint, freckles) over a neutral base tone — the skin TONE itself is the skin
palette's job. Skin textures are painterly tone ramps: three distinct hue
families (fair/tan/deep), each a smooth vertical gradient with faint painted
banding.

Usage:
    python3 tools/art/generate_face_skin.py --out content/core/assets/art/char/ardent/male
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

WIDTH = 256
HEIGHT = 256

FACE_VARIANTS = 4
SKIN_VARIANTS = 3

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"

# --- Face paint palette (feature paint over a neutral base; skin tone is the
# skin-palette's job, so faces stay tone-neutral to compose with any palette). -
FACE_BASE_TONE = (210, 175, 145)
BROW_COLOR = (90, 60, 40)
EYE_COLOR = (40, 30, 25)
LIP_BASE = (170, 90, 90)
FRECKLE_COLOR = (150, 105, 80)

# --- Skin palette tone ramps: (top, bottom) RGB per variant 1..3. -------------
SKIN_TONE_RAMPS: dict[int, tuple[tuple[int, int, int], tuple[int, int, int]]] = {
    1: ((247, 214, 186), (196, 145, 110)),  # fair warm ramp
    2: ((214, 170, 122), (140, 92, 55)),  # tan/olive ramp
    3: ((150, 105, 70), (70, 45, 30)),  # deep umber ramp
}


def _face_variant_params(variant: int) -> dict:
    """Deterministic per-variant feature parameters (subtle, not tone changes)."""
    arch = (variant - 1) * 0.015
    lip_shift = (variant - 1) * 8
    freckles = variant in (2, 4)
    eye_size = 0.028 + (variant % 2) * 0.004
    return {
        "arch": arch,
        "lip_shift": lip_shift,
        "freckles": freckles,
        "eye_size": eye_size,
    }


def _face_pixel(x: int, y: int, w: int, h: int, params: dict) -> tuple[int, int, int]:
    u = x / (w - 1)
    v = y / (h - 1)
    color = FACE_BASE_TONE

    # Eyebrows: two bars near v~0.33-0.365, with a per-variant arch that lifts
    # the bar toward the brow's center.
    for cx in (0.32, 0.68):
        bx0, bx1 = cx - 0.09, cx + 0.09
        if bx0 <= u <= bx1:
            arch_local = params["arch"] * (1 - abs(u - cx) / 0.09)
            by0, by1 = 0.33 - arch_local, 0.365 - arch_local
            if by0 <= v <= by1:
                color = BROW_COLOR

    # Eyes: ellipses near v~0.44, sized per variant.
    for cx in (0.34, 0.66):
        rx = params["eye_size"] + 0.01
        ry = params["eye_size"]
        dx = (u - cx) / rx
        dy = (v - 0.44) / ry
        if dx * dx + dy * dy <= 1.0:
            color = EYE_COLOR

    # Nose shadow: a thin vertical band, subtly darker than the base tone.
    if color == FACE_BASE_TONE and 0.485 <= u <= 0.515 and 0.46 <= v <= 0.62:
        color = tuple(max(0, c - 12) for c in color)

    # Lips: a band near v~0.72-0.765, hue-shifted per variant.
    if 0.40 <= u <= 0.60 and 0.72 <= v <= 0.765:
        r, g, b = LIP_BASE
        color = (min(255, r + params["lip_shift"]), g, b)

    # Freckles: a sparse deterministic dot grid on the cheeks (variants 2 & 4).
    if params["freckles"]:
        for cx in (0.24, 0.76):
            if abs(u - cx) < 0.10 and 0.52 <= v <= 0.64 and x % 6 == 0 and y % 6 == 0:
                color = FRECKLE_COLOR

    return color  # type: ignore[return-value]


def _skin_pixel(x: int, y: int, h: int, variant: int) -> tuple[int, int, int]:
    top, bottom = SKIN_TONE_RAMPS[variant]
    v = y / (h - 1)
    r = round(top[0] + (bottom[0] - top[0]) * v)
    g = round(top[1] + (bottom[1] - top[1]) * v)
    b = round(top[2] + (bottom[2] - top[2]) * v)
    # Painterly banding: every 8th scanline pair reads a touch darker, giving a
    # hand-painted streak instead of a perfectly smooth machine gradient.
    if y % 8 in (0, 1):
        r, g, b = max(0, r - 6), max(0, g - 6), max(0, b - 6)
    return (r, g, b)


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    )


def _encode_png(width: int, height: int, pixel_fn) -> bytes:
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None) per scanline
        for x in range(width):
            r, g, b = pixel_fn(x, y)
            raw += bytes((r & 0xFF, g & 0xFF, b & 0xFF))
    compressed = zlib.compress(bytes(raw), 9)
    ihdr = struct.pack(
        ">IIBBBBB", width, height, 8, 2, 0, 0, 0
    )  # 8-bit, color type 2 (RGB)

    out = bytearray()
    out += PNG_SIGNATURE
    out += _png_chunk(b"IHDR", ihdr)
    out += _png_chunk(b"IDAT", compressed)
    out += _png_chunk(b"IEND", b"")
    return bytes(out)


def build_face_png(variant: int) -> bytes:
    if not 1 <= variant <= FACE_VARIANTS:
        raise ValueError(f"face variant must be 1..{FACE_VARIANTS}, got {variant}")
    params = _face_variant_params(variant)
    return _encode_png(
        WIDTH, HEIGHT, lambda x, y: _face_pixel(x, y, WIDTH, HEIGHT, params)
    )


def build_skin_png(variant: int) -> bytes:
    if not 1 <= variant <= SKIN_VARIANTS:
        raise ValueError(f"skin variant must be 1..{SKIN_VARIANTS}, got {variant}")
    return _encode_png(WIDTH, HEIGHT, lambda x, y: _skin_pixel(x, y, HEIGHT, variant))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        required=True,
        help="output directory to write face_{1..4}_bc.png + skin_{1..3}_bc.png into",
    )
    args = parser.parse_args(argv)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    written: list[Path] = []
    for variant in range(1, FACE_VARIANTS + 1):
        path = out_dir / f"face_{variant}_bc.png"
        path.write_bytes(build_face_png(variant))
        written.append(path)
    for variant in range(1, SKIN_VARIANTS + 1):
        path = out_dir / f"skin_{variant}_bc.png"
        path.write_bytes(build_skin_png(variant))
        written.append(path)

    for path in written:
        print(f"wrote {path} ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
