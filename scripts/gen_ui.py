#!/usr/bin/env python3
"""Generate the 9-patch UI sprites -> assets/textures/ui_*.block.png.

Eight 16x16 RGBA sprites in a simple, single-colour, chunky-rounded style: a cream
border (2px) over a per-sprite fill, with the corners rounded off (transparent
outside). They're interned into the block texture array by BlockRegistry and drawn
sliced (9-patch) by the UI; a uniform 5px corner inset keeps the rounded corner
crisp at any panel size. Same manual zlib/struct PNG writer as gen_hammer.py / gen_
cracks.py (no PIL). Run: `python scripts/gen_ui.py`.
"""
import math
import os
import struct
import zlib

W = H = 16
RAD = 3.0   # corner radius (px)
BW = 2.0    # border thickness (px)

CREAM = (246, 219, 196, 255)  # one border colour for everything

# name -> fill colour (RGBA). `border` has no fill (ring only).
SPRITES = {
    "ui_bg":        (42, 39, 36, 255),    # main panel (dark)
    "ui_bg2":       (58, 53, 49, 255),    # sub-panel (mid)
    "ui_bg3":       (74, 63, 74, 255),    # accent panel (faint lilac)
    "ui_eq":        (30, 27, 26, 255),    # inventory/equip slot (recessed)
    "ui_button":    (74, 68, 64, 255),    # button (brighter than bg)
    "ui_slider_bg": (30, 27, 26, 255),    # slider track (recessed)
    "ui_slider":    (223, 158, 233, 255), # slider fill / handle (lilac accent)
    "ui_border":    None,                 # border ring only (transparent centre)
}


def make(fill):
    border_only = fill is None
    img = [[(0, 0, 0, 0) for _ in range(W)] for _ in range(H)]
    for y in range(H):
        for x in range(W):
            # Distance from the rounded-rect boundary (positive = inside). Corner
            # arcs are centred at (RAD, RAD) etc.; straight edges have qx or qy = 0.
            qx = max(RAD - x, x - (W - 1 - RAD), 0.0)
            qy = max(RAD - y, y - (H - 1 - RAD), 0.0)
            d = RAD - math.hypot(qx, qy)
            if d < -0.5:
                continue                      # outside the rounded corner -> clear
            if d < BW:
                img[y][x] = CREAM             # border ring
            elif not border_only:
                img[y][x] = fill              # interior fill
    return img


def write_png(path, img):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            raw += bytes(img[y][x])

    def chunk(typ, data):
        return (struct.pack('>I', len(data)) + typ + data +
                struct.pack('>I', zlib.crc32(typ + data) & 0xFFFFFFFF))

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


out_dir = os.path.join(os.path.dirname(__file__), '..', 'assets', 'textures')
for name, fill in SPRITES.items():
    p = os.path.join(out_dir, name + '.block.png')
    write_png(p, make(fill))
    print('wrote', os.path.normpath(p))
