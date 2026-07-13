"""Generate the iron_sword inventory icon (#605) — deterministic, stdlib-only.

A small 128×128 RGBA PNG showing an upright arming sword (blade + crossguard +
grip + pommel) on a transparent background — the UI inventory icon the item's
``visual.icon`` references. Pure standard library (``zlib`` + ``struct``): no
Pillow, so it runs in CI and on a bare checkout, and the output is byte-identical
every run (source_tier: original — hand-authored procedural art, no restyle step).

    uv run python tools/art/generate_sword_icon.py \
        --out content/core/assets/art/icon/item/iron_sword.png
"""

from __future__ import annotations

import argparse
import struct
import zlib
from pathlib import Path

SIZE = 128
DEFAULT_OUT = "content/core/assets/art/icon/item/iron_sword.png"

# Palette (RGBA). Steel blade, brass guard/pommel, dark leather grip.
TRANSPARENT = (0, 0, 0, 0)
BLADE = (198, 206, 216, 255)
BLADE_EDGE = (120, 128, 140, 255)
GUARD = (176, 140, 74, 255)
GRIP = (74, 54, 40, 255)
POMMEL = (196, 158, 88, 255)


def _blank() -> list[list[tuple[int, int, int, int]]]:
    return [[TRANSPARENT for _ in range(SIZE)] for _ in range(SIZE)]


def _fill_rect(px, x0: int, y0: int, x1: int, y1: int, color) -> None:
    for y in range(max(y0, 0), min(y1, SIZE)):
        for x in range(max(x0, 0), min(x1, SIZE)):
            px[y][x] = color


def render() -> list[list[tuple[int, int, int, int]]]:
    """Draw the upright sword. y grows downward: tip at top, pommel at bottom."""
    px = _blank()
    cx = SIZE // 2

    # Blade: a tapered vertical bar from the tip (y=10) down to the guard (y=84).
    for y in range(10, 84):
        # Half-width grows from 1px at the tip to 5px at the shoulder.
        hw = 1 + (y - 10) * 4 // 74
        _fill_rect(px, cx - hw, y, cx + hw, y + 1, BLADE)
        # Darker central fuller / edges for a little read.
        px[y][cx - hw] = BLADE_EDGE
        px[y][min(cx + hw, SIZE - 1)] = BLADE_EDGE

    # Crossguard: a horizontal brass bar just below the blade shoulder.
    _fill_rect(px, cx - 26, 84, cx + 26, 92, GUARD)

    # Grip: a short dark leather-wrapped column below the guard.
    _fill_rect(px, cx - 5, 92, cx + 5, 112, GRIP)

    # Pommel: a brass cap at the bottom.
    _fill_rect(px, cx - 8, 110, cx + 8, 118, POMMEL)
    return px


def _png_bytes(px) -> bytes:
    """Encode an RGBA pixel grid as a PNG (stdlib zlib, no third-party deps)."""
    raw = bytearray()
    for row in px:
        raw.append(0)  # filter type 0 (None) per scanline
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="generate_sword_icon.py")
    parser.add_argument("--out", default=DEFAULT_OUT, help="output .png path")
    args = parser.parse_args(argv)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(_png_bytes(render()))
    print(f"[generate_sword_icon] wrote {out} ({SIZE}x{SIZE} RGBA)")


if __name__ == "__main__":
    main()
