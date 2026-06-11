#!/usr/bin/env python3
"""Generate a simple 16x16 hammer item icon -> assets/textures/hammer.block.png.

The block textures are procedural (gen_textures.py), but item icons like the
pickaxe/sword are small hand-authored sprites. This draws a plain mallet (grey
head + wooden handle) the same way gen_textures writes PNGs (manual zlib/struct,
no PIL dependency), so the hammer has its own icon instead of borrowing the
pickaxe. Run by hand: `python scripts/gen_hammer.py`.
"""
import os
import struct
import zlib

W = H = 16
img = [[(0, 0, 0, 0) for _ in range(W)] for _ in range(H)]


def rect(x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if 0 <= x < W and 0 <= y < H:
                img[y][x] = c


HEAD   = (158, 160, 168, 255)  # steel head
HEAD_D = (96, 98, 108, 255)    # head outline / shading
HANDLE = (132, 84, 46, 255)    # wood handle
HANDLE_D = (88, 54, 28, 255)   # handle shading

# Mallet head across the top, with a darker outline so it reads on any background.
rect(3, 2, 12, 6, HEAD)
rect(3, 2, 12, 2, HEAD_D)
rect(3, 6, 12, 6, HEAD_D)
rect(3, 2, 3, 6, HEAD_D)
rect(12, 2, 12, 6, HEAD_D)
# Wooden handle down the middle.
rect(7, 6, 8, 14, HANDLE)
rect(8, 6, 8, 14, HANDLE_D)  # right column shaded for a bit of round
rect(7, 14, 8, 14, HANDLE_D)

raw = bytearray()
for y in range(H):
    raw.append(0)  # filter byte 0 (none) per scanline
    for x in range(W):
        raw += bytes(img[y][x])


def chunk(typ, data):
    return (struct.pack('>I', len(data)) + typ + data +
            struct.pack('>I', zlib.crc32(typ + data) & 0xFFFFFFFF))


png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))  # 8-bit RGBA
png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
png += chunk(b'IEND', b'')

out = os.path.join(os.path.dirname(__file__), '..', 'assets', 'textures',
                   'hammer.block.png')
with open(out, 'wb') as f:
    f.write(png)
print('wrote', os.path.normpath(out))
