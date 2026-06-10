#!/usr/bin/env python3
"""Generate the placeholder block textures for the voxel game.

These are simple procedurally-coloured PNGs, one per block face. The point is
not art -- it is to make adding/tuning a block a *data* change, never a code one.

ALL tunable values live in assets/textures.yaml (texture size, the pattern
constants, and the per-texture table), each described there. This script holds
no magic numbers: it just renders what the config says. To add a block face,
add an entry to assets/textures.yaml and re-run; to retune the look, edit the
config. See docs/CONFIGURATION.md for the project-wide convention.

Colours are names from the project palette (assets/colors.yaml, which
gen_colors.py samples from assets/colormap.png -- so the colormap image is the
single source of truth for colour; vg::Palette loads the same file). A literal
"#RRGGBB" also works for a one-off shade.

Patterns a texture can opt into (constants in textures.yaml -> patterns):
  speckle   per-pixel brightness grain (stone, sand, gravel)
  top_band  a coloured strip along the top edge, with a ragged lower edge (grass)
  planks    horizontal plank divisions + a vertical seam (wood planks)
  streaks   vertical bark streaks (log side)
  rings     concentric tree-ring banding (log top)

Run from the repo root:  python scripts/gen_textures.py
Requires PyYAML (pip install pyyaml). The PNGs are committed, so building the
game does not require Python.
"""
import os
import random
import re
import struct
import zlib

try:
    import yaml
except ImportError:
    raise SystemExit("gen_textures needs PyYAML:  pip install pyyaml")

HERE = os.path.dirname(__file__)
OUT_DIR = os.path.join(HERE, "..", "assets", "textures")
COLORS_YAML = os.path.join(HERE, "..", "assets", "colors.yaml")
CONFIG_YAML = os.path.join(HERE, "..", "assets", "textures.yaml")


# -----------------------------------------------------------------------------
#  Config + colour resolution
# -----------------------------------------------------------------------------
def load_config():
    """Load assets/textures.yaml (size, patterns, textures). All knobs live here."""
    with open(CONFIG_YAML, encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    for key in ("size", "patterns", "textures"):
        if key not in cfg:
            raise SystemExit("gen_textures: assets/textures.yaml is missing '%s'" % key)
    return cfg


def load_palette():
    """Parse assets/colors.yaml into {name: (r, g, b)}. Absent file -> empty."""
    palette = {}
    if not os.path.exists(COLORS_YAML):
        return palette
    line_re = re.compile(r'^\s*([A-Za-z0-9_]+)\s*:\s*"?#?([0-9A-Fa-f]{6})"?')
    with open(COLORS_YAML, encoding="utf-8") as f:
        for line in f:
            m = line_re.match(line)
            if m:
                h = m.group(2)
                palette[m.group(1)] = (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
    return palette


PALETTE = load_palette()


def resolve(color):
    """A colour spec -> (r, g, b). Accepts a palette name, '#RRGGBB', or a list."""
    if isinstance(color, str):
        if color.startswith("#"):
            h = color[1:]
            return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))
        if color in PALETTE:
            return PALETTE[color]
        raise SystemExit("gen_textures: unknown palette colour '%s' "
                         "(add it to assets/colors.yaml or use a literal)" % color)
    return tuple(color)


def shade(rgb, factor):
    return tuple(max(0, min(255, int(c * factor))) for c in rgb)


# -----------------------------------------------------------------------------
#  Texture builder -- one base fill plus optional patterns, applied in order.
#  Every constant comes from `patterns` (assets/textures.yaml); see that file for
#  what each one means and changes.
# -----------------------------------------------------------------------------
def make_texture(spec, size, patterns, flat=False):
    """Render a size*size list of RGBA pixels from a texture spec dict.

    When `flat` is true every texture is a solid fill of its base colour: the
    per-pixel patterns (speckle/grain, bands, rings, streaks, planks, ore flecks)
    — the "noise" — are skipped, but the alpha-cutout shape (leaves/bush) is kept
    so foliage still reads. The pattern code is left intact below, just gated, so
    flipping `flat` back off in textures.yaml restores the detailed look."""
    base = resolve(spec["base"])
    # Deterministic grain so the committed PNGs are stable across runs.
    rng = random.Random(spec["_seed"])

    speckle = spec.get("speckle", 0.0)
    top_band = spec.get("top_band")       # [color, height] or None
    planks = spec.get("planks", False)
    streaks = spec.get("streaks", False)
    rings = spec.get("rings", False)
    leaves = spec.get("leaves", False)    # alpha-cutout foliage (transparent gaps)
    dither = spec.get("dither", 0.0)      # screen-door alpha: punch a fixed checkerboard of pixels fully transparent so liquids read as see-through through the chunk.frag cutout
    ore_color = spec.get("ore_color")     # if set, scatter coloured mineral flecks over the base

    band_jitter = patterns["band_jitter"]
    streak_cols = set(patterns["streak"]["columns"]) if streaks else set()
    streak_shade = patterns["streak"]["shade"]
    plank_period = patterns["planks"]["period"]
    plank_groove = patterns["planks"]["groove_shade"]
    plank_seam = patterns["planks"]["seam_shade"]
    ring_step = patterns["rings"]["shade_step"]
    leaf_cfg = patterns.get("leaves", {})
    leaf_round = leaf_cfg.get("round", 0.0)    # rounds the tile's corners to a leafy blob
    leaf_gap = leaf_cfg.get("gap", 0.0)        # extra random transparent pixels (leaf gaps)
    leaf_vein = leaf_cfg.get("vein_shade", 1.0) # darken some opaque pixels into veins
    ore_cfg = patterns.get("ore", {})
    ore_chance = ore_cfg.get("fleck_chance", 0.0) # fraction of pixels turned into ore flecks
    ore_jitter = ore_cfg.get("fleck_jitter", 0.0) # per-fleck brightness variation
    center = (size - 1) / 2.0

    # An uneven top band (grass over dirt): jitter the band's lower edge per column
    # so the grass-to-dirt boundary is ragged instead of a straight line. Drawn
    # only when a band exists, so other textures' grain sequence is unchanged.
    band_h = None
    if top_band:
        band_h = [max(0, top_band[1] + rng.choice(band_jitter)) for _ in range(size)]

    px = []
    for y in range(size):
        for x in range(size):
            color = base

            # The per-pixel "noise"/pattern layer. Kept intact but skipped in flat
            # mode (every block becomes a solid base colour). To bring the detail
            # back, set `flat: false` in assets/textures.yaml.
            if not flat:
                if top_band and y < band_h[x]:
                    color = resolve(top_band[0])
                if rings:
                    d = max(abs(x - center), abs(y - center))
                    color = shade(base, 1.0 - ring_step * (int(d) % 2))
                if streaks and x in streak_cols:
                    color = shade(color, streak_shade)
                if planks:
                    if y % plank_period == plank_period - 1:  # dark groove between planks
                        color = shade(color, plank_groove)
                    elif x == size // 2:                       # staggered vertical seam
                        color = shade(color, plank_seam)

                if speckle:
                    color = shade(color, 1.0 + rng.uniform(-speckle, speckle))

                # Ore flecks: recolour a fraction of pixels to the mineral colour,
                # each jittered so a vein reads as glinting crystals, not flat dots.
                if ore_color and rng.random() < ore_chance:
                    color = shade(resolve(ore_color),
                                  1.0 + rng.uniform(-ore_jitter, ore_jitter))

            alpha = 255
            if leaves:
                # Round the corners into a roughly circular leaf mass, then punch
                # a few random gaps so the canopy reads as broken foliage rather
                # than a solid disc. The cutout SHAPE is kept even when flat (so
                # foliage still reads); only the vein shading is part of the noise.
                d = ((x - center) ** 2 + (y - center) ** 2) ** 0.5
                if leaf_round and d > leaf_round:
                    alpha = 0
                elif leaf_gap and rng.random() < leaf_gap:
                    alpha = 0
                elif not flat and leaf_vein != 1.0 and rng.random() < 0.18:
                    color = shade(color, leaf_vein)

            # Screen-door transparency: knock out a fixed checkerboard so the
            # block reads as see-through via chunk.frag's alpha cutout (no blend
            # pass needed). 0.5 = classic 50% stipple; the offset rows keep the
            # holes evenly spread instead of striping.
            if dither and alpha == 255:
                if ((x + y) & 1) == 0 if dither >= 0.5 else ((x & 1) == 0 and (y & 1) == 0):
                    alpha = 0

            px.append((color[0], color[1], color[2], alpha))
    return px


def write_png(path, pixels, size):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 (none)
        for x in range(size):
            raw.extend(pixels[y * size + x])

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def main():
    cfg = load_config()
    size = cfg["size"]
    patterns = cfg["patterns"]
    flat = bool(cfg.get("flat", False))  # solid-colour blocks; keep noise for later

    os.makedirs(OUT_DIR, exist_ok=True)
    for name, spec in cfg["textures"].items():
        spec.setdefault("_seed", name)  # stable per-texture grain, keyed by filename
        path = os.path.join(OUT_DIR, name)
        write_png(path, make_texture(spec, size, patterns, flat), size)
        print("wrote", os.path.normpath(path))
    if flat:
        print("(flat mode: solid base colours; set flat:false in textures.yaml to restore detail)")


if __name__ == "__main__":
    main()
