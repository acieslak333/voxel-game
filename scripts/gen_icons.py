#!/usr/bin/env python3
"""Bake the 16x16 inventory icons for every block and item.

Each hotbar entry (block or item) needs a small sprite the UI can draw flat in a
slot. Rather than assemble an isometric cube at runtime per frame, we PRERENDER
one 16x16 PNG per entry into assets/textures/icons/<name>.png. The block texture
array loads them as ordinary layers (all layers must be 16x16 — see TextureArray),
and vg::BlockRegistry interns icons/<name>.png as each entry's iconLayer.

Two icon styles, chosen from the entry's data:
  * ISOMETRIC CUBE  for solid-ish blocks (render cube / leafcube / model). Three
    shaded rhombus faces — the SAME hexagon geometry and per-face shading as the
    old runtime vg::Ui::isoCube (top 1.00, right 0.82, left 0.60), so blocks look
    just like before, only baked. Top face uses the block's +Y texture, the two
    side faces its +X (side) texture.
  * FLAT SPRITE     for items (anything with tool:/equip:/placeable:false) and for
    cross-rendered plants (flowers, grass, torch). Their texture is already an
    upright billboard, so the icon is just that sprite, drawn upright.

Sources: assets/blocks.yaml + assets/items.yaml (the entry list) and the face
PNGs in assets/textures/. Run from the repo root after adding a block/item or
changing a face texture:  python scripts/gen_icons.py
Requires PyYAML + Pillow. The PNGs are committed, so building the game needs no
Python. See docs/CONFIGURATION.md for the data-not-code convention.
"""
import os

try:
    import yaml
except ImportError:
    raise SystemExit("gen_icons needs PyYAML:  pip install pyyaml")
try:
    from PIL import Image, ImageDraw, ImageChops
except ImportError:
    raise SystemExit("gen_icons needs Pillow:  pip install pillow")

HERE = os.path.dirname(__file__)
ASSETS = os.path.join(HERE, "..", "assets")
TEX_DIR = os.path.join(ASSETS, "textures")
OUT_DIR = os.path.join(TEX_DIR, "icons")

SIZE = 16          # final icon edge (must match the rest of the texture array)
SS = 16            # supersample factor while rasterising the cube (256px canvas)
CANVAS = SIZE * SS
# Per-face shades — identical to vg::Ui::isoCube so baked icons match the look.
SHADE_TOP, SHADE_RIGHT, SHADE_LEFT = 1.00, 0.82, 0.60


# -----------------------------------------------------------------------------
#  Loading entries + resolving face textures
# -----------------------------------------------------------------------------
def load_entries():
    """All entries from blocks.yaml then items.yaml, in id order (air included)."""
    entries = []
    for fname in ("blocks.yaml", "items.yaml"):
        path = os.path.join(ASSETS, fname)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            seq = yaml.safe_load(f) or []
        entries.extend(seq)
    return entries


def first(value):
    """A face value may be a single filename or a list of variants; take one."""
    return value[0] if isinstance(value, list) else value


def face_texture(entry, which):
    """Filename for the `which` ('top'|'side') face, honouring all/top/side then
    the ${name}.block.png fallback used by the C++ loader."""
    tex = entry.get("textures") or {}
    if which in tex:
        return first(tex[which])
    if "all" in tex:
        return first(tex["all"])
    # No explicit map for this face: same convention as BlockRegistry.
    other = tex.get("side") or tex.get("top") or tex.get("bottom")
    return first(other) if other else entry["name"] + ".block.png"


def flat_texture(entry):
    """The single upright sprite for a flat item/plant icon."""
    tex = entry.get("textures") or {}
    for key in ("all", "side", "top", "bottom"):
        if key in tex:
            return first(tex[key])
    return entry["name"] + ".block.png"


def load_tex(filename):
    """Load a 16x16 RGBA face texture from assets/textures/."""
    path = os.path.join(TEX_DIR, filename)
    if not os.path.exists(path):
        raise SystemExit("gen_icons: missing texture '%s' (for an icon)" % filename)
    img = Image.open(path).convert("RGBA")
    if img.size != (SIZE, SIZE):
        img = img.resize((SIZE, SIZE), Image.NEAREST)
    return img


def is_flat(entry):
    """Flat sprite icon (vs isometric cube)? True for items and cross plants."""
    if entry.get("render") == "cross":
        return True
    return "tool" in entry or "equip" in entry or entry.get("placeable") is False


# -----------------------------------------------------------------------------
#  Isometric cube rasteriser
# -----------------------------------------------------------------------------
def shade(img, factor):
    """Multiply RGB by `factor` (keep alpha) — the per-face directional shading."""
    r, g, b, a = img.split()
    lut = [min(255, int(round(i * factor))) for i in range(256)]
    return Image.merge("RGBA", (r.point(lut), g.point(lut), b.point(lut), a))


def paste_face(canvas, tex, A, B, C):
    """Composite `tex` onto the parallelogram with corners A (uv 0,0), B (uv 1,0),
    C (uv 0,1) — its 4th corner B+C-A is implied. Affine-maps the unit texture
    square onto the rhombus, masked to the rhombus and the texture's own alpha
    (so leaf cutouts keep their holes)."""
    Ax, Ay = A
    ex0, ey0 = B[0] - Ax, B[1] - Ay   # edge A->B  (u axis)
    ex1, ey1 = C[0] - Ax, C[1] - Ay   # edge A->C  (v axis)
    det = ex0 * ey1 - ex1 * ey0
    if abs(det) < 1e-6:
        return
    # Inverse of [[ex0,ex1],[ey0,ey1]] maps canvas offset -> (u,v); scale by tex size.
    inv = (ey1 / det, -ex1 / det, -ey0 / det, ex0 / det)
    tw, th = tex.size
    a = inv[0] * tw
    b = inv[1] * tw
    c = -(inv[0] * Ax + inv[1] * Ay) * tw
    d = inv[2] * th
    e = inv[3] * th
    f = -(inv[2] * Ax + inv[3] * Ay) * th
    warped = tex.transform((CANVAS, CANVAS), Image.AFFINE, (a, b, c, d, e, f),
                           resample=Image.BILINEAR)
    # Restrict to the rhombus footprint, intersected with the warped alpha.
    poly = Image.new("L", (CANVAS, CANVAS), 0)
    ImageDraw.Draw(poly).polygon(
        [A, B, (B[0] + C[0] - Ax, B[1] + C[1] - Ay), C], fill=255)
    warped.putalpha(ImageChops.multiply(warped.getchannel("A"), poly))
    canvas.alpha_composite(warped)


def bake_cube(top_tex, side_tex):
    """Render the isometric cube into a SIZE x SIZE RGBA icon (supersampled)."""
    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    cx = cy = CANVAS / 2.0
    r = CANVAS * 0.5 * 0.94    # half-width; hexagon spans cx±r and cy±r
    q = r / 2.0
    A = (cx, cy - 2 * q)       # top apex
    B = (cx + r, cy - q)       # upper-right
    C = (cx - r, cy - q)       # upper-left
    O = (cx, cy)               # centre (faces meet here)
    D = (cx + r, cy + q)       # lower-right
    E = (cx - r, cy + q)       # lower-left
    F = (cx, cy + 2 * q)       # bottom apex
    # Same face quads as Ui::isoCube: right + left first, top last for a crisp ridge.
    paste_face(canvas, shade(side_tex, SHADE_RIGHT), O, B, F)   # right: O,B,D,F
    paste_face(canvas, shade(side_tex, SHADE_LEFT),  C, O, E)   # left:  C,O,F,E
    paste_face(canvas, shade(top_tex,  SHADE_TOP),   A, B, C)   # top:   A,B,O,C
    return canvas.resize((SIZE, SIZE), Image.LANCZOS)


# -----------------------------------------------------------------------------
#  Driver
# -----------------------------------------------------------------------------
def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    entries = load_entries()
    flat_n = cube_n = 0
    for entry in entries:
        name = entry.get("name")
        if not name or name == "air":
            continue
        if is_flat(entry):
            icon = load_tex(flat_texture(entry))
            flat_n += 1
        else:
            icon = bake_cube(load_tex(face_texture(entry, "top")),
                             load_tex(face_texture(entry, "side")))
            cube_n += 1
        icon.save(os.path.join(OUT_DIR, name + ".png"))
    print("gen_icons: wrote %d icons (%d cube, %d flat) -> %s"
          % (flat_n + cube_n, cube_n, flat_n,
             os.path.relpath(OUT_DIR, os.path.join(HERE, ".."))))


if __name__ == "__main__":
    main()
