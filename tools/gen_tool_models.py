#!/usr/bin/env python3
"""Author the Blockbench tool models (hammer/sword/pickaxe/torch) + their skins.

For each tool we define its cuboids (Blockbench units, 16 = 1 block) with a
material per box. The script UV-unwraps every face onto a shared 128x128 atlas,
paints it as *shaded* pixel art (per-face directional light + a 1px bevel so the
cuboid edges read + per-material grain/highlight detail), and emits BOTH the
`<tool>.png` skin and a matching `<tool>.bbmodel` whose per-face UVs point at the
painted rects. Keeping geometry + texture in one generator guarantees they stay
in sync. The held-item viewmodel in-game (App) renders these.
"""
import json
import math
import os
import random

from PIL import Image

random.seed(1337)

ASSETS = os.path.join(os.path.dirname(__file__), "..", "assets", "models")
ATLAS = 128          # shared skin dimensions (all tools must match for the array)
PXU = 4              # texture pixels per Blockbench unit

# Per-face directional brightness (a cheap form cue): top lit, bottom shadowed.
SHADE = {"up": 1.18, "down": 0.58, "south": 1.06, "north": 0.96, "east": 0.88, "west": 0.80}

BASE = {
    "wood":       (143, 98, 54),
    "leather":    (96, 62, 38),
    "iron":       (172, 177, 187),
    "steel":      (208, 218, 233),
    "gold":       (228, 180, 70),
    "flame_core": (255, 150, 40),
    "flame_tip":  (255, 228, 122),
    "skin":       (226, 170, 128),  # first-person hand
    "sleeve":     (108, 78, 52),    # leather forearm sleeve
}


def cc(v):
    return max(0, min(255, int(round(v))))


def shade(col, f):
    return (cc(col[0] * f), cc(col[1] * f), cc(col[2] * f))


def paint_face(mat, direction, w, h):
    """Return an RGBA image (w x h) of one cuboid face, shaded + detailed."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = img.load()
    base = BASE[mat]
    sh = SHADE[direction]
    end = direction in ("up", "down")  # end-grain / cap faces
    # Per-face stable grain columns for wood.
    grain = {x for x in range(w) if (x * 7 + 3) % 5 == 0}
    for y in range(h):
        for x in range(w):
            col = base
            f = sh
            if mat in ("wood", "leather"):
                if end:
                    f *= 0.92 + 0.08 * (1.0 - abs((x + 0.5) / w - 0.5) * 2)
                else:
                    if x in grain:
                        f *= 0.80
                    if mat == "leather" and y % 3 == 0:
                        f *= 0.85  # wrapped-grip banding
                    f *= 0.94 + 0.12 * random.random()
            elif mat in ("iron", "steel"):
                # metallic: bright top-left sheen falling to a darker lower-right.
                diag = 1.0 - (x / max(1, w - 1) + y / max(1, h - 1)) * 0.5
                f *= 0.86 + 0.30 * diag
                if mat == "steel" and not end:
                    # blade: bright central edge, darker flats.
                    c = abs((x + 0.5) / w - 0.5) * 2
                    f *= 1.16 - 0.34 * c
                if random.random() < 0.06:
                    f *= 0.88  # speckle
            elif mat == "gold":
                diag = 1.0 - (x / max(1, w - 1) + y / max(1, h - 1)) * 0.5
                f *= 0.84 + 0.34 * diag
            elif mat == "flame_core":
                t = 1.0 - y / max(1, h - 1)  # hotter (yellower) toward the top
                col = tuple(cc(base[i] + (BASE["flame_tip"][i] - base[i]) * t * 0.6) for i in range(3))
            elif mat == "flame_tip":
                t = 1.0 - y / max(1, h - 1)
                col = tuple(cc(base[i] + (255 - base[i]) * t * 0.7) for i in range(3))
            elif mat == "skin":
                f *= 0.96 + 0.08 * random.random()  # soft warm variation
                if not end and (y == 1 or y == h // 2):
                    f *= 0.9  # faint knuckle/crease lines
            elif mat == "sleeve":
                if not end and y % 3 == 1:
                    f *= 0.86  # leather stitch banding
            # 1px bevel: darken the outer ring so every cuboid edge reads crisply.
            if x == 0 or y == 0 or x == w - 1 or y == h - 1:
                f *= 0.78
            px[x, y] = (*shade(col, f), 255)
    return img


def face_size_units(box, d):
    fr, to = box["from"], box["to"]
    dx, dy, dz = (abs(to[i] - fr[i]) for i in range(3))
    if d in ("east", "west"):
        return dz, dy
    if d in ("up", "down"):
        return dx, dz
    return dx, dy  # north / south


FACES = ["north", "south", "east", "west", "up", "down"]


def build(tool, boxes):
    atlas = Image.new("RGBA", (ATLAS, ATLAS), (0, 0, 0, 0))
    # Shelf packer: place each face left->right, wrapping to a new shelf.
    sx, sy, shelf_h = 1, 1, 0
    elements = []
    for bi, box in enumerate(boxes):
        faces_json = {}
        for d in FACES:
            uw, uh = face_size_units(box, d)
            pw, ph = max(1, round(uw * PXU)), max(1, round(uh * PXU))
            if sx + pw + 1 > ATLAS:
                sx = 1
                sy += shelf_h + 1
                shelf_h = 0
            assert sy + ph + 1 <= ATLAS, f"{tool}: atlas overflow"
            mat = box.get("mat_" + d, box["mat"])
            atlas.paste(paint_face(mat, d, pw, ph), (sx, sy))
            faces_json[d] = {"uv": [sx, sy, sx + pw, sy + ph], "texture": 0}
            sx += pw + 1
            shelf_h = max(shelf_h, ph)
        el = {
            "name": box["name"],
            "from": box["from"],
            "to": box["to"],
            "origin": box["origin"],
            "uuid": f"{tool[0]}{bi:02d}00000-0000-0000-0000-000000000000",
            "faces": faces_json,
        }
        if "rotation" in box:
            el["rotation"] = box["rotation"]
        elements.append(el)

    model = {
        "meta": {"format_version": "4.5", "model_format": "free", "box_uv": False},
        "name": tool,
        "resolution": {"width": ATLAS, "height": ATLAS},
        "elements": elements,
        "outliner": [{
            "name": tool,
            "origin": [8, 0, 8],
            "uuid": f"{tool[0]}ff00000-0000-0000-0000-000000000000",
            "children": [e["uuid"] for e in elements],
        }],
        "textures": [{"name": tool, "id": "0", "path": tool + ".png"}],
    }
    atlas.save(os.path.join(ASSETS, tool + ".png"))
    with open(os.path.join(ASSETS, tool + ".bbmodel"), "w") as f:
        json.dump(model, f, indent=2)
    print("wrote", tool, "(.png + .bbmodel)")


def box(name, frm, to, origin, mat, rotation=None, **mats):
    b = {"name": name, "from": frm, "to": to, "origin": origin, "mat": mat}
    if rotation:
        b["rotation"] = rotation
    b.update(mats)
    return b


TOOLS = {
    "hammer": [
        box("handle", [7.5, 0, 7.5], [8.5, 9, 8.5], [8, 0, 8], "wood",
            mat_up="wood", mat_down="wood"),
        box("head", [5.5, 9, 6.5], [10.5, 13, 9.5], [8, 9, 8], "iron"),
        box("band", [7.3, 8.6, 7.3], [8.7, 9.4, 8.7], [8, 9, 8], "gold"),
    ],
    "sword": [
        box("blade", [7.5, 4, 7.7], [8.5, 14.5, 8.3], [8, 0, 8], "steel"),
        box("tip", [7.5, 14.5, 7.7], [8.5, 15.5, 8.3], [8, 0, 8], "steel"),
        box("guard", [6, 3, 7.2], [10, 4, 8.8], [8, 0, 8], "gold"),
        box("grip", [7.5, 0.5, 7.5], [8.5, 3, 8.5], [8, 0, 8], "leather"),
        box("pommel", [7.2, 0, 7.2], [8.8, 1, 8.8], [8, 0, 8], "gold"),
    ],
    "pickaxe": [
        box("handle", [7.5, 0, 7.5], [8.5, 10, 8.5], [8, 0, 8], "wood"),
        box("head", [7, 10, 7.4], [9, 12, 8.6], [8, 11, 8], "iron"),
        box("tip_l", [3, 10.5, 7.4], [7, 11.5, 8.6], [7, 11, 8], "iron", rotation=[0, 0, 30]),
        box("tip_r", [9, 10.5, 7.4], [13, 11.5, 8.6], [9, 11, 8], "iron", rotation=[0, 0, -30]),
    ],
    "torch": [
        box("stick", [7, 0, 7], [9, 9, 9], [8, 0, 8], "wood"),
        box("flame_core", [6.5, 9, 6.5], [9.5, 12, 9.5], [8, 9, 8], "flame_core"),
        box("flame_tip", [7.3, 12, 7.3], [8.7, 14, 8.7], [8, 12, 8], "flame_tip"),
    ],
    # First-person arm/hand, authored in the SAME space as the tools (handle at
    # x=8,z=8, base y~0) so the game applies the tool's view transform to it too and
    # the fist always grips the handle. The forearm extends down + toward the viewer
    # and reads as coming from the lower-right corner once the held-item tilt applies.
    "hand": [
        box("fist", [6.3, 0.6, 7.1], [9.7, 4.3, 10.7], [8, 0, 8], "skin"),
        box("thumb", [7.8, 3.6, 6.6], [9.6, 5.0, 8.4], [8, 0, 8], "skin"),
        box("forearm", [6.7, -7, 8.0], [9.3, 1.2, 11.6], [8, 0, 8], "sleeve"),
    ],
}

for name, boxes in TOOLS.items():
    build(name, boxes)
