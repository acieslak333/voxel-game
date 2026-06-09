#!/usr/bin/env python3
"""Generate the placeholder block textures for the voxel game.

These are deliberately simple "solid colour" PNGs, one per block face type, as
called for by the milestone spec. Each has a subtly darker 1px border so that
per-block texture tiling on greedy-meshed quads is visible (you can see the
grid), which is the whole point of the texture-array + REPEAT-sampler approach.

Run from the repo root:  python3 scripts/gen_textures.py
Outputs to assets/textures/. The PNGs are committed, so building does not
require Python.
"""
import os
import struct
import zlib

SIZE = 16
OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "textures")


def shade(rgb, factor):
    return tuple(max(0, min(255, int(c * factor))) for c in rgb)


def make_pixels(base, border=None, top_band=None):
    """Return SIZE*SIZE RGBA pixels: solid `base`, darker `border`, optional
    `top_band` = (color, height) drawn along the +V (top) edge."""
    border = border or shade(base, 0.82)
    px = []
    for y in range(SIZE):
        for x in range(SIZE):
            color = base
            if top_band and y < top_band[1]:
                color = top_band[0]
            if x == 0 or y == 0 or x == SIZE - 1 or y == SIZE - 1:
                color = border
            px.append((color[0], color[1], color[2], 255))
    return px


def write_png(path, pixels):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)  # filter type 0 (none)
        for x in range(SIZE):
            raw.extend(pixels[y * SIZE + x])

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0)  # 8-bit RGBA
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    grass = (108, 176, 74)
    dirt = (134, 96, 67)
    stone = (136, 136, 140)

    textures = {
        "grass_top.png": make_pixels(grass),
        # Grass side: dirt with a green band along the top edge.
        "grass_side.png": make_pixels(dirt, top_band=(grass, 4)),
        "dirt.png": make_pixels(dirt),
        "stone.png": make_pixels(stone),
    }
    for name, px in textures.items():
        path = os.path.join(OUT_DIR, name)
        write_png(path, px)
        print("wrote", os.path.normpath(path))


if __name__ == "__main__":
    main()
