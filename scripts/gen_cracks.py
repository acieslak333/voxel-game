#!/usr/bin/env python3
"""Generate block-break crack overlays -> assets/textures/crack_0..3.block.png.

Four progressively-cracked 16x16 RGBA overlays (transparent except dark crack
lines). The game draws the matching stage over a block as it's mined; the entity
shader's alpha cutout shows only the crack pixels. Written with the same manual
zlib/struct PNG encoder as gen_hammer.py (no PIL). Run: `python scripts/gen_cracks.py`.
"""
import os
import struct
import zlib

W = H = 16
CRACK = (26, 24, 22, 235)   # near-black, mostly opaque
STAGES = 4


def lcg(seed):
    s = seed & 0xFFFFFFFF
    while True:
        s = (1103515245 * s + 12345) & 0xFFFFFFFF
        yield s


def make_stage(stage):
    img = [[(0, 0, 0, 0) for _ in range(W)] for _ in range(H)]
    rng = lcg(0xC0FFEE + stage * 7919)

    def rnd(n):
        return next(rng) % n

    def plot(x, y):
        if 0 <= x < W and 0 <= y < H:
            img[y][x] = CRACK

    # A few jagged cracks radiating from near the centre; more (and longer) as the
    # stage rises, so the block looks increasingly shattered.
    cracks = 2 + stage * 2
    for _ in range(cracks):
        x, y = 6 + rnd(5), 6 + rnd(5)        # start near centre
        steps = 5 + stage * 2 + rnd(4)
        dx = 1 if rnd(2) else -1
        dy = 1 if rnd(2) else -1
        for _ in range(steps):
            plot(x, y)
            if rnd(2):                        # step along the dominant axis...
                x += dx
            else:
                y += dy
            if rnd(5) == 0:                   # ...occasionally jog sideways (jagged)
                plot(x + (1 if rnd(2) else -1), y)
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
for s in range(STAGES):
    p = os.path.join(out_dir, 'crack_{}.block.png'.format(s))
    write_png(p, make_stage(s))
    print('wrote', os.path.normpath(p))
