#!/usr/bin/env python3
"""Author a quadruped critter Blockbench model + skin (a passive mob).

Sibling of gen_tool_models.py, but the critter needs an ANIMATABLE rig: its four
legs live in their own outliner GROUPS (with the hip as the group origin) so the
loader turns each into a joint the game can swing for a walk cycle. Body, head,
ears and tail are static loose elements under the root group.

Like the tool generator it UV-unwraps every face onto a shared 128x128 atlas,
paints shaded pixel art, and emits BOTH critter.png + critter.bbmodel into
assets/models/critter/ (the per-model dir the engine auto-discovers). The game
(App::buildModels) loads the rig and renders the wandering critters with it.
"""
import json
import os
import random

from PIL import Image

random.seed(7)

OUTDIR = os.path.join(os.path.dirname(__file__), "..", "assets", "models", "critter")
ATLAS = 256          # must be square; resized to the engine's atlas at load
PXU = 4              # texture pixels per Blockbench unit (16 units = 1 block)

SHADE = {"up": 1.18, "down": 0.55, "south": 1.08, "north": 0.94, "east": 0.88, "west": 0.80}

BASE = {
    "fur":      (150, 104, 66),   # warm brown body
    "fur_dark": (110, 74, 46),    # legs / ears / tail
    "snout":    (196, 150, 124),  # muzzle
    "hoof":     (60, 48, 40),     # foot caps
}


def cc(v):
    return max(0, min(255, int(round(v))))


def shade(col, f):
    return (cc(col[0] * f), cc(col[1] * f), cc(col[2] * f))


def paint_face(mat, direction, w, h):
    """One cuboid face as a shaded RGBA tile (w x h)."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = img.load()
    base = BASE[mat]
    sh = SHADE[direction]
    end = direction in ("up", "down")
    for y in range(h):
        for x in range(w):
            f = sh
            if mat in ("fur", "fur_dark"):
                # Soft fur: faint vertical strands + a little noise.
                if not end and (x * 5 + 1) % 4 == 0:
                    f *= 0.86
                f *= 0.93 + 0.13 * random.random()
            elif mat == "snout":
                f *= 0.97 + 0.06 * random.random()
            elif mat == "hoof":
                f *= 0.9
            # 1px bevel so each cuboid edge reads.
            if x == 0 or y == 0 or x == w - 1 or y == h - 1:
                f *= 0.8
            px[x, y] = (*shade(base, f), 255)
    return img


def face_size_units(box, d):
    fr, to = box["from"], box["to"]
    dx, dy, dz = (abs(to[i] - fr[i]) for i in range(3))
    if d in ("east", "west"):
        return dz, dy
    if d in ("up", "down"):
        return dx, dz
    return dx, dy


FACES = ["north", "south", "east", "west", "up", "down"]


# --- Geometry (Blockbench units; Y up, +Z = front/head). Centred near x=8,z=8. ---
# Each element: name, from, to, origin, mat, optional per-face mat_<dir>, and the
# leg group it belongs to ("group": (name, origin)) or None for the root body.
def el(name, frm, to, origin, mat, group=None, **mats):
    d = {"name": name, "from": frm, "to": to, "origin": origin, "mat": mat, "group": group}
    d.update(mats)
    return d


ELEMENTS = [
    # --- static body under the root group ---
    el("body", [5, 6, 4], [11, 11, 13], [8, 8, 8], "fur", mat_down="fur_dark"),
    el("head", [5.5, 7, 13], [10.5, 12.5, 17], [8, 9, 15], "fur"),
    el("snout", [6.5, 7, 17], [9.5, 9.5, 18.5], [8, 8, 17], "snout"),
    el("earL", [5.5, 12, 13.5], [7, 13.8, 15], [6, 12, 14], "fur_dark"),
    el("earR", [9, 12, 13.5], [10.5, 13.8, 15], [10, 12, 14], "fur_dark"),
    el("tail", [7.3, 8, 2], [8.7, 10.5, 4], [8, 9, 4], "fur_dark"),
    # --- legs: each in its own group (origin = hip pivot at y=6) so it can swing ---
    el("legFL", [5.5, 0, 10], [7.5, 6, 12], [6.5, 6, 11], "fur_dark",
       group=("legFL", [6.5, 6, 11]), mat_down="hoof"),
    el("legFR", [8.5, 0, 10], [10.5, 6, 12], [9.5, 6, 11], "fur_dark",
       group=("legFR", [9.5, 6, 11]), mat_down="hoof"),
    el("legBL", [5.5, 0, 5], [7.5, 6, 7], [6.5, 6, 6], "fur_dark",
       group=("legBL", [6.5, 6, 6]), mat_down="hoof"),
    el("legBR", [8.5, 0, 5], [10.5, 6, 7], [9.5, 6, 6], "fur_dark",
       group=("legBR", [9.5, 6, 6]), mat_down="hoof"),
]


def build():
    os.makedirs(OUTDIR, exist_ok=True)
    atlas = Image.new("RGBA", (ATLAS, ATLAS), (0, 0, 0, 0))
    sx, sy, shelf_h = 1, 1, 0
    elements_json = []
    for bi, box in enumerate(ELEMENTS):
        faces_json = {}
        for d in FACES:
            uw, uh = face_size_units(box, d)
            pw, ph = max(1, round(uw * PXU)), max(1, round(uh * PXU))
            if sx + pw + 1 > ATLAS:
                sx = 1
                sy += shelf_h + 1
                shelf_h = 0
            assert sy + ph + 1 <= ATLAS, "critter: atlas overflow"
            mat = box.get("mat_" + d, box["mat"])
            atlas.paste(paint_face(mat, d, pw, ph), (sx, sy))
            faces_json[d] = {"uv": [sx, sy, sx + pw, sy + ph], "texture": 0}
            sx += pw + 1
            shelf_h = max(shelf_h, ph)
        elements_json.append({
            "name": box["name"],
            "from": box["from"],
            "to": box["to"],
            "origin": box["origin"],
            "uuid": f"c{bi:02d}00000-0000-0000-0000-000000000000",
            "faces": faces_json,
        })

    # Outliner: root group "critter" holds the static body elements directly, plus a
    # sub-group per leg (origin = hip) so each leg becomes its own animatable joint.
    root_children = []
    for bi, box in enumerate(ELEMENTS):
        uuid = elements_json[bi]["uuid"]
        if box["group"] is None:
            root_children.append(uuid)
        else:
            gname, gorigin = box["group"]
            root_children.append({
                "name": gname,
                "origin": gorigin,
                "uuid": f"c{bi:02d}ffff0-0000-0000-0000-000000000000",
                "children": [uuid],
            })

    model = {
        "meta": {"format_version": "4.5", "model_format": "free", "box_uv": False},
        "name": "critter",
        "resolution": {"width": ATLAS, "height": ATLAS},
        "elements": elements_json,
        "outliner": [{
            "name": "critter",
            "origin": [8, 0, 8],
            "uuid": "cff00000-0000-0000-0000-000000000000",
            "children": root_children,
        }],
        "textures": [{"name": "critter", "id": "0", "path": "critter.png"}],
    }
    atlas.save(os.path.join(OUTDIR, "critter.png"))
    with open(os.path.join(OUTDIR, "critter.bbmodel"), "w") as f:
        json.dump(model, f, indent=2)
    print("wrote critter (.png + .bbmodel) ->", OUTDIR)


if __name__ == "__main__":
    build()
