#!/usr/bin/env python3
"""Unified live world-generation tool for the voxel game.

Merges the old genmap_tool (terrain SHAPE) and biome_tool (flora + caves/ores) into
ONE localhost web app with a shared live 3D voxel view. Drag a slider / pick a
dropdown and the game's own `voxelgame --genmap` re-runs, so the preview is the
EXACT generation the game uses (the C++ TerrainGenerator + the YAML are the single
source of truth — no noise logic is duplicated here).

It patches two data files in place (ruamel round-trip, comments preserved) and
deploys them to build/bin/assets so the game picks them up on next launch:
  * assets/biomes.yaml  — terrain shape, 3D relief, rivers/lakes, per-biome trees
  * assets/world.yaml   — caves, ravines, cave pools, ore density/depth

NEW vs the old tools:
  * SEED regenerate — type a seed or hit 🎲 to preview the SAME config under a
    different seed (passed to --genmap --seed; preview-only, never written to YAML —
    the game's own seed lives in world.yaml `seed`/`random_seed`).
  * WINDOW movement — pan the sampled region with the ←↑↓→ buttons / arrow keys
    (passed to --genmap --center CX CZ), so you can roam the world instead of always
    staring at the origin. The pan step is one screen-width of world.

    pip install flask ruamel.yaml
    python tools/worldgen_tool.py        # open http://127.0.0.1:5000
"""
import json
import os
import shutil
import subprocess
import sys
import time

try:
    from flask import Flask, jsonify, request, send_file, Response
except ImportError:
    sys.exit("worldgen_tool needs Flask:  pip install flask ruamel.yaml")
try:
    from ruamel.yaml import YAML
    from ruamel.yaml.comments import CommentedSeq, CommentedMap
except ImportError:
    sys.exit("worldgen_tool needs ruamel.yaml (preserves comments):  pip install ruamel.yaml")

import terrain3d  # shared live-3D terrain view (vendored Three.js, sea plane + sky)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = {
    "biomes": os.path.join(ROOT, "assets", "biomes.yaml"),
    "world":  os.path.join(ROOT, "assets", "world.yaml"),
}
DEPLOY = {
    "biomes": os.path.join(ROOT, "build", "bin", "assets", "biomes.yaml"),
    "world":  os.path.join(ROOT, "build", "bin", "assets", "world.yaml"),
}
MAP_OUT = os.path.join(HERE, "_genmap.png")
LOCATE_OUT = os.path.join(HERE, "_locate.png")  # scratch biome map for "show this biome"
EXE_CANDIDATES = [
    os.path.join(ROOT, "build", "bin", "Release", "voxelgame.exe"),
    os.path.join(ROOT, "build", "bin", "Debug", "voxelgame.exe"),
    os.path.join(ROOT, "build", "bin", "voxelgame"),  # single-config / linux
]

# The tree species the C++ loader accepts (TerrainGenerator parse).
TREE_SPECIES = ["oak", "birch", "pine"]
# Common surface/filler blocks offered in the per-biome top/filler dropdowns. The
# biome's current value is merged in (so a hand-authored block still shows/selects).
SURFACE_BLOCKS = ["grass", "dirt", "stone", "sand", "gravel", "snow", "cobblestone"]
# Steepness curves for a noise mask (must match vg::Falloff in src/world/NoiseMask.h).
FALLOFFS = ["step", "linear", "smoothstep", "smootherstep", "gain", "bezier"]
# Solo-biome view: side of the focused slice, in BLOCKS (128 = 8 chunks) at step 1.
SOLO_SLICE_BLOCKS = 128
# Single optional noise masks in world.yaml (modulate cave-carve / ore density). Each is
# a (label, dotted-base) — the engine reads <base>.{threshold,width,falloff,gain,invert,
# layers,bezier}. Empty (no layers) = off, default world unchanged.
WORLD_MASKS = [
    ("Cave mask (where caves cluster)", "caves.mask"),
    ("Ore mask (where iron concentrates)", "ores.iron.mask"),
]

yaml = YAML()  # round-trip: preserves comments + ordering

# Reserved form keys (not config paths).
SPECIAL = {"__pixels__", "__step__", "__view__", "__seed__", "__cx__", "__cz__"}


def exe_path():
    for p in EXE_CANDIDATES:
        if os.path.exists(p):
            return p
    sys.exit("voxelgame executable not found — build it first (Release recommended).")


# --- Static control tables. Each tuple: (label, biomes-dotted-path, min, max, step).
#     All of these live in biomes.yaml (terrain shape).
SHAPE_SLIDERS = [
    ("Sea level",            "sea_level",                 8,    240,  1),
    ("Snow line (rel)",      "snow_line",                 0,    120,  1),
    # Core shape noises authored as flat {frequency, octaves} (no `layers:` stack —
    # if you give one a `layers:` block it gets per-layer controls below instead and
    # these auto-hide). frequency = feature size, octaves = detail.
    ("Continental freq",     "continentalness.frequency", 0.0003, 0.01,  0.0001),
    ("Continental octaves",  "continentalness.octaves",   1,    8,    1),
    ("Erosion freq",         "erosion.frequency",         0.0003, 0.02,  0.0001),
    ("Erosion octaves",      "erosion.octaves",           1,    8,    1),
    ("Island enabled",       "island.enabled",            0,    1,    1),
    ("Island radius",        "island.radius",             200,  4000, 10),
    ("Island inner frac",    "island.inner",              0.1,  0.9,  0.01),
    ("Island coast warp",    "island.coast_warp",         0,    800,  10),
    ("Island land base",     "island.land_base",          0,    40,   1),
    ("Island peak height",   "island.peak_height",        0,    120,  1),
    ("Island interior var",  "island.interior_var",       0,    30,   1),
    ("Island ocean floor",   "island.ocean_floor",        -120, 0,    2),
]
SHAPE_3D_SLIDERS = [
    ("3D enabled",           "terrain3d.enabled",         0,    1,    1),
    ("3D amplitude",         "terrain3d.amplitude",       0,    128,  2),
    ("Float threshold",      "terrain3d.float_threshold", 0.0,  0.8,  0.01),
    ("Float freq",           "terrain3d.float_freq",      0.001, 0.02, 0.0005),
    ("Float gap",            "terrain3d.float_gap",       0,    60,   1),
    ("Float reach",          "terrain3d.float_reach",     0,    200,  2),
    ("River freq",           "rivers.frequency",          0.0005, 0.006, 0.0001),
    ("River width",          "rivers.width",              0.0,  0.3,  0.005),
    ("River depth",          "rivers.depth",              0,    16,   1),
    ("River max elev",       "rivers.max_elevation",      0,    60,   1),
    ("Lake spacing",         "lakes.spacing",             60,   600,  10),
    ("Lake chance",          "lakes.chance",              0.0,  1.0,  0.02),
    ("Lake radius min",      "lakes.radius_min",          4,    40,   1),
    ("Lake radius max",      "lakes.radius_max",          8,    80,   1),
    ("Lake depth",           "lakes.depth",               2,    20,   1),
    ("Coast flatten on",     "terrain3d.coast_flatten.enabled", 0, 1,  1),
    ("Coast flatten range",  "terrain3d.coast_flatten.range",   0, 80, 1),
    ("Coast flatten min",    "terrain3d.coast_flatten.min",     0.0, 1.0, 0.02),
]
# These live in world.yaml (caves / ores).
WORLD_SLIDERS = [
    ("Cave frequency",      "caves.frequency",            0.01, 0.08, 0.002),
    ("Cave threshold",      "caves.threshold",            0.0,  0.2,  0.005),
    ("Cave floor Y",        "caves.floor",                0,    20,   1),
    ("Cavern threshold",    "caves.cavern_threshold",     0.3,  0.8,  0.01),
    ("Cavern max Y",        "caves.cavern_max_y",         8,    120,  1),
    ("Ravine width",        "caves.ravines.width",        0.0,  0.06, 0.002),
    ("Ravine frequency",    "caves.ravines.frequency",    0.0003, 0.004, 0.0001),
    ("Ravine max Y",        "caves.ravines.max_y",        10,   120,  1),
    ("Ravine floor Y",      "caves.ravines.floor",        0,    40,   1),
    ("Lava pool max Y",     "caves.pools.lava_max_y",     0,    40,   1),
    ("Cave water max Y",    "caves.pools.water_max_y",    0,    80,   1),
    ("Cave water chance",   "caves.pools.water_chance",   0.0,  1.0,  0.02),
    ("Iron density",        "ores.iron.density",          0.0,  0.04, 0.001),
    ("Iron max Y",          "ores.iron.max_y",            10,   120,  1),
]
# Noise fields whose authored `layers:` stack the game actually runs (biomes.yaml).
STACK_FIELDS = ["continentalness", "erosion", "peaks", "temperature", "humidity",
                "rivers", "terrain3d.density"]

# View modes forwarded to `--genmap --mode ... [--layer ...]`.
VIEW_MODES = [
    ("3D terrain (live)", "3d"),
    ("Top-down surface", "top"),
    ("Biome regions", "biomes"),
    ("Noise: continentalness", "noise:cont"),
    ("Noise: erosion", "noise:ero"),
    ("Noise: peaks", "noise:peak"),
    ("Noise: rivers", "noise:river"),
    ("Noise: relief (height)", "noise:relief"),
    ("Cross-section", "cross"),
]


def get_path(doc, dotted):
    node = doc
    for key in dotted.split("."):
        node = node[int(key)] if key.lstrip("-").isdigit() else node[key]
    return node


def set_path(doc, dotted, value):
    keys = dotted.split(".")
    node = doc
    for key in keys[:-1]:
        if key.lstrip("-").isdigit():
            node = node[int(key)]
        else:
            if key not in node:
                node[key] = {}   # auto-create intermediate map (e.g. a biome's terrain3d)
            node = node[key]
    last = keys[-1]
    node[int(last) if last.lstrip("-").isdigit() else last] = value


def load_docs():
    docs = {}
    for key, path in SRC.items():
        with open(path, encoding="utf-8") as f:
            docs[key] = yaml.load(f)
    return docs


def deploy_only(docs):
    """Preview: write the edited configs to the DEPLOYED copies (what --genmap reads),
    NOT the committed source — so dragging a slider previews without saving."""
    for key, doc in docs.items():
        os.makedirs(os.path.dirname(DEPLOY[key]), exist_ok=True)
        with open(DEPLOY[key], "w", encoding="utf-8") as f:
            yaml.dump(doc, f)


def save_source(docs):
    """Persist the edits to the committed source files (and the deployed copies)."""
    for key, doc in docs.items():
        with open(SRC[key], "w", encoding="utf-8") as f:
            yaml.dump(doc, f)
    deploy_only(docs)


def _flow_point(x, y):
    """A [x, y] kept in YAML flow style (so a spline dumps as `- [x, y]`, not nested)."""
    s = CommentedSeq([x, y])
    s.fa.set_flow_style()
    return s


def _flow_block(name, w):
    """A {name, w} kept in flow style — a biome top/filler palette entry."""
    m = CommentedMap()
    m["name"] = name
    m["w"] = w
    m.fa.set_flow_style()
    return m


def _default_biome(n):
    """A sensible blank biome for the editor's '+ Add biome' button. Flow-style [min,max]
    sequences so it dumps like the hand-authored biomes (`temp: [-2, 2]`)."""
    m = CommentedMap()
    m["name"] = "new_biome_%d" % n
    m["temp"] = _flow_point(-2, 2)
    m["humidity"] = _flow_point(-2, 2)
    m["elevation"] = _flow_point(0, 20)
    m["top"] = "grass"
    m["filler"] = "dirt"
    m["trees"] = 0.04
    m["tree"] = "oak"
    return m


def biome_ref_hsv(idx):
    """The (hue, sat) the biome-regions map paints biome `idx` with — mirrors biomeColor()
    in src/main.cpp / the JS legend (golden-angle hue, S=0.5, V=0.92). The map's hillshade
    scales RGB uniformly, so hue+sat survive (only value changes); sat cleanly separates a
    blue biome (0.5) from blue water (~0.67)."""
    return (idx * 0.61803398875) % 1.0, 0.5


def run_locate(idx, seed, cx, cz):
    """Render a wide, coarse biome-regions map and return world coords of the nearest
    column of biome `idx` to the current center (or None). PIL+numpy, no engine change:
    we match the biome's known golden-angle hue/sat in HSV space."""
    from PIL import Image
    import numpy as np
    h, s = biome_ref_hsv(idx)
    th, ts = int(round(h * 255)) % 256, int(round(s * 255))
    for step, px in ((6, 512), (16, 640), (40, 640)):  # fine, wide, very wide passes
        args = [exe_path(), "--genmap", "--mapsize", str(px), "--mapstep", str(step),
                "--mode", "biomes", "--out", LOCATE_OUT, "--seed", str(int(seed)),
                "--center", str(int(cx)), str(int(cz))]
        subprocess.run(args, cwd=ROOT, check=True, capture_output=True)
        arr = np.asarray(Image.open(LOCATE_OUT).convert("HSV"), dtype=np.int16)
        Hc, Sc = arr[..., 0], arr[..., 1]
        dh = np.abs(Hc - th); dh = np.minimum(dh, 256 - dh)  # circular hue distance
        ys, xs = np.nonzero((dh <= 12) & (np.abs(Sc - ts) <= 26))
        if xs.size == 0:
            continue
        half = px // 2
        k = int(np.argmin((xs - half) ** 2 + (ys - half) ** 2))  # nearest to center
        return (cx + (int(xs[k]) - half) * step, cz + (int(ys[k]) - half) * step)
    return None


def apply_form(docs, form):
    """Patch the docs in place from a request form (id = 'FILE::dotted.path').

    Spline points (`<key>_spline.<i>.<c>` in biomes) are collected and the whole list
    is REBUILT from whatever points the client submitted — so adding, deleting and
    reordering curve points all work (set_path can't append past a list's end), while
    the points stay sorted by x as Spline::at requires."""
    splines = {}    # spline key -> {index: [x, y]}
    palettes = {}   # (biome index, "top"|"filler") -> {entry index: {"name":, "w":}}
    dlayers = {}    # biome index -> {layer index: {"frequency":, "weight":, "octaves":, "type":}}
    smasks = {}     # biome index -> {mask index: {"_":scalars, "layers":{..}, "bezier":{..}}}
    wmasks = {}     # world mask base ("caves.mask"/"ores.iron.mask") -> {"_","layers","bezier"}
    for cid, raw in form.items():
        if cid in SPECIAL:
            continue
        try:
            file, path = cid.split("::", 1)
        except ValueError:
            continue
        parts = path.split(".")
        # Per-biome 3D density layer: biomes.<i>.terrain3d.density.layers.<j>.<field>.
        # Collected and the whole stack REBUILT below, so add/remove of layers works
        # (set_path can't append past a list's end).
        if (file == "biomes" and len(parts) == 7 and parts[0] == "biomes"
                and parts[1].isdigit() and parts[2] == "terrain3d" and parts[3] == "density"
                and parts[4] == "layers" and parts[5].isdigit()
                and parts[6] in ("frequency", "weight", "octaves", "type")):
            dlayers.setdefault(int(parts[1]), {}).setdefault(int(parts[5]), {})[parts[6]] = raw
            continue
        # Per-biome surface_masks (noise-mask block patches): scalar fields (len 5), per
        # mask's noise layers and bezier curve points (len 7). Collected, rebuilt below.
        if (file == "biomes" and parts[0] == "biomes" and len(parts) >= 5
                and parts[1].isdigit() and parts[2] == "surface_masks" and parts[3].isdigit()):
            bi, mi = int(parts[1]), int(parts[3])
            mrec = smasks.setdefault(bi, {}).setdefault(mi, {"_": {}, "layers": {}, "bezier": {}})
            if len(parts) == 5 and parts[4] in ("block", "threshold", "width", "gain",
                                                "falloff", "invert"):
                mrec["_"][parts[4]] = raw
                continue
            if (len(parts) == 7 and parts[4] == "layers" and parts[5].isdigit()
                    and parts[6] in ("frequency", "weight", "octaves", "type")):
                mrec["layers"].setdefault(int(parts[5]), {})[parts[6]] = raw
                continue
            if (len(parts) == 7 and parts[4] == "bezier" and parts[5].isdigit()
                    and parts[6] in ("0", "1")):
                pt = mrec["bezier"].setdefault(int(parts[5]), [None, None])
                try:
                    pt[int(parts[6])] = float(raw)
                except ValueError:
                    pass
                continue
        # Single masks: world caves.mask / ores.iron.mask, and biome selection masks
        # (biomes.<i>.mask). Collected by (file, base) and rebuilt below.
        sbase = None
        if file == "world":
            sbase = next((bs for bs in ("caves.mask", "ores.iron.mask")
                          if path == bs or path.startswith(bs + ".")), None)
        elif (file == "biomes" and len(parts) >= 3 and parts[0] == "biomes"
              and parts[1].isdigit() and parts[2] == "mask"):
            sbase = f"biomes.{parts[1]}.mask"
        if sbase is not None:
            rec = wmasks.setdefault((file, sbase), {"_": {}, "layers": {}, "bezier": {}})
            rp = path[len(sbase):].lstrip(".").split(".")
            if len(rp) == 1 and rp[0] in ("threshold", "width", "gain", "falloff", "invert"):
                rec["_"][rp[0]] = raw
            elif (len(rp) == 3 and rp[0] == "layers" and rp[1].isdigit()
                    and rp[2] in ("frequency", "weight", "octaves", "type")):
                rec["layers"].setdefault(int(rp[1]), {})[rp[2]] = raw
            elif len(rp) == 3 and rp[0] == "bezier" and rp[1].isdigit() and rp[2] in ("0", "1"):
                pt = rec["bezier"].setdefault(int(rp[1]), [None, None])
                try:
                    pt[int(rp[2])] = float(raw)
                except ValueError:
                    pass
            continue
        if (file == "biomes" and len(parts) == 3 and parts[0].endswith("_spline")
                and parts[1].isdigit() and parts[2] in ("0", "1")):
            try:
                num = float(raw)
            except ValueError:
                continue
            pt = splines.setdefault(parts[0], {}).setdefault(int(parts[1]), [None, None])
            pt[int(parts[2])] = int(num) if num == int(num) else num
            continue
        if (file == "biomes" and len(parts) == 5 and parts[0] == "biomes"
                and parts[1].isdigit() and parts[2] in ("top", "filler")
                and parts[3].isdigit() and parts[4] in ("name", "w")):
            ent = palettes.setdefault((int(parts[1]), parts[2]), {}).setdefault(int(parts[3]), {})
            ent[parts[4]] = raw
            continue
        if raw in TREE_SPECIES:        # a dropdown sends a string; a slider a number
            val = raw
        else:
            try:
                val = float(raw)
                if val == int(val):
                    val = int(val)
            except ValueError:
                val = raw
        try:
            set_path(docs[file], path, val)
        except Exception:
            pass
    for key, ptsmap in splines.items():
        pts = [p for _, p in sorted(ptsmap.items()) if None not in p]
        pts.sort(key=lambda p: p[0])   # x ascending (sorted control points)
        # Existing splines update in place; island_profile_spline may be CREATED (the
        # editor seeds default points, so editing it adds the curve to biomes.yaml).
        if pts and (key in docs["biomes"] or key == "island_profile_spline"):
            docs["biomes"][key] = [_flow_point(x, y) for x, y in pts]
    # Rebuild biome top/filler palettes from the submitted block rows (add/remove safe).
    # A single block with weight 1 stays a clean scalar; otherwise a weighted list.
    try:
        biome_list = docs["biomes"]["biomes"]
    except Exception:
        biome_list = None
    for (bi, field), entmap in palettes.items():
        if biome_list is None or bi >= len(biome_list):
            continue
        ents = [entmap[j] for j in sorted(entmap) if entmap[j].get("name")]
        if not ents:
            continue
        def _w(e):
            try:
                return float(e.get("w", 1))
            except ValueError:
                return 1.0
        if len(ents) == 1 and _w(ents[0]) == 1.0:
            biome_list[bi][field] = ents[0]["name"]
        else:
            biome_list[bi][field] = [_flow_block(e["name"],
                                     int(_w(e)) if _w(e) == int(_w(e)) else _w(e)) for e in ents]
    # Rebuild each biome's per-biome 3D density stack from the submitted layer rows.
    def _num(s, d):
        try:
            v = float(s)
            return int(v) if v == int(v) else v
        except (TypeError, ValueError):
            return d
    for bi, lmap in dlayers.items():
        if biome_list is None or bi >= len(biome_list):
            continue
        layers = []
        for j in sorted(lmap):
            e = lmap[j]
            m = CommentedMap()
            m["type"] = str(e.get("type", "perlin"))
            m["frequency"] = _num(e.get("frequency"), 0.01)
            m["octaves"] = int(_num(e.get("octaves"), 3))
            m["weight"] = _num(e.get("weight"), 1.0)
            m.fa.set_flow_style()
            layers.append(m)
        biome = biome_list[bi]
        if layers:
            t3 = biome.setdefault("terrain3d", CommentedMap())
            dens = t3.setdefault("density", CommentedMap())
            dens["layers"] = layers
        else:
            # all layers removed: drop the density override so the biome inherits global
            t3 = biome.get("terrain3d")
            if isinstance(t3, dict) and isinstance(t3.get("density"), dict):
                t3["density"].pop("layers", None)
                if not t3["density"]:
                    t3.pop("density", None)
    # Rebuild each biome's surface_masks (noise-mask block patches) from submitted fields.
    for bi, mmap in smasks.items():
        if biome_list is None or bi >= len(biome_list):
            continue
        masks = []
        for mi in sorted(mmap):
            rec = mmap[mi]
            sc = rec["_"]
            m = CommentedMap()
            m["block"] = str(sc.get("block", "stone"))
            m["threshold"] = _num(sc.get("threshold"), 0.0)
            m["width"] = _num(sc.get("width"), 0.5)
            m["falloff"] = str(sc.get("falloff", "smoothstep"))
            if _num(sc.get("gain"), 0.5) != 0.5:
                m["gain"] = _num(sc.get("gain"), 0.5)
            if str(sc.get("invert", "0")) in ("1", "true", "True"):
                m["invert"] = True
            lay = []
            for lj in sorted(rec["layers"]):
                e = rec["layers"][lj]
                lm = CommentedMap()
                lm["type"] = str(e.get("type", "perlin"))
                lm["frequency"] = _num(e.get("frequency"), 0.03)
                lm["octaves"] = int(_num(e.get("octaves"), 3))
                lm["weight"] = _num(e.get("weight"), 1.0)
                lm.fa.set_flow_style()
                lay.append(lm)
            if lay:
                m["layers"] = lay
            if m["falloff"] == "bezier":
                pts = [p for _, p in sorted(rec["bezier"].items()) if None not in p]
                pts.sort(key=lambda p: p[0])
                if len(pts) >= 2:
                    m["bezier"] = [_flow_point(x, y) for x, y in pts]
            # a mask with no noise layers does nothing (empty stack) — keep only if it has any
            if lay:
                masks.append(m)
        if masks:
            biome_list[bi]["surface_masks"] = masks
        elif "surface_masks" in biome_list[bi]:
            del biome_list[bi]["surface_masks"]
    # Rebuild single masks: world caves.mask / ores.iron.mask AND biome selection masks
    # (biomes.<i>.mask), each keyed by (file, base).
    for (wfile, wbase), rec in wmasks.items():
        wdoc = docs.get(wfile)
        if not isinstance(wdoc, dict):
            continue
        sc = rec["_"]
        lay = []
        for lj in sorted(rec["layers"]):
            e = rec["layers"][lj]
            lm = CommentedMap()
            lm["type"] = str(e.get("type", "perlin"))
            lm["frequency"] = _num(e.get("frequency"), 0.02)
            lm["octaves"] = int(_num(e.get("octaves"), 3))
            lm["weight"] = _num(e.get("weight"), 1.0)
            lm.fa.set_flow_style()
            lay.append(lm)
        keys = wbase.split(".")
        if lay:
            m = CommentedMap()
            m["threshold"] = _num(sc.get("threshold"), 0.0)
            m["width"] = _num(sc.get("width"), 0.5)
            m["falloff"] = str(sc.get("falloff", "smoothstep"))
            if _num(sc.get("gain"), 0.5) != 0.5:
                m["gain"] = _num(sc.get("gain"), 0.5)
            if str(sc.get("invert", "0")) in ("1", "true", "True"):
                m["invert"] = True
            m["layers"] = lay
            if m["falloff"] == "bezier":
                pts = [p for _, p in sorted(rec["bezier"].items()) if None not in p]
                pts.sort(key=lambda p: p[0])
                if len(pts) >= 2:
                    m["bezier"] = [_flow_point(x, y) for x, y in pts]
            set_path(wdoc, wbase, m)
        else:  # no layers: drop the mask key (off)
            node, ok = wdoc, True
            for k in keys[:-1]:
                if isinstance(node, dict) and k in node:
                    node = node[k]
                elif isinstance(node, list) and k.lstrip("-").isdigit() and int(k) < len(node):
                    node = node[int(k)]
                else:
                    ok = False
                    break
            if ok and isinstance(node, dict):
                node.pop(keys[-1], None)


def build_controls(docs):
    """HTML for every control. Each id is 'FILE::dotted.path'."""
    rows = []

    def num_input(cid, val, step):
        # The authoritative form field (data-id). Unbounded, so you can type a value
        # beyond the slider's max (that's why 5120 in a 0..4000 slider used to vanish —
        # the range input clamped it; the number doesn't).
        return (f'<input type="number" step="{step}" value="{val:g}" '
                f'data-id="{cid}" class="nv" oninput="onNum(this)">')

    def slider(file, label, path, lo, hi, step):
        try:
            val = float(get_path(docs[file], path))
        except Exception:
            return ""  # field absent from this YAML: skip the control
        cid = f"{file}::{path}"
        # range = coarse drag (companion, no data-id so it's not double-submitted);
        # number = exact entry + the value actually sent. They mirror each other.
        return (f'<div class="r"><label title="{cid}">{label}</label>'
                f'<input type="range" min="{lo}" max="{hi}" step="{step}" value="{val}" '
                f'class="rng" data-for="{cid}" oninput="onRange(this)">'
                f'{num_input(cid, val, step)}</div>')

    def pair(file, label, path, step="any"):
        """Two number inputs on one row for a [min, max] / [a, b] sequence value."""
        try:
            seq = get_path(docs[file], path)
            v0, v1 = float(seq[0]), float(seq[1])
        except Exception:
            return ""
        c0, c1 = f"{file}::{path}.0", f"{file}::{path}.1"
        return (f'<div class="r2"><label title="{path}">{label}</label>'
                f'{num_input(c0, v0, step)}{num_input(c1, v1, step)}</div>')

    def choice(file, label, path, options):
        try:
            cur = str(get_path(docs[file], path))
        except Exception:
            return ""  # key absent: skip (don't author a key the biome didn't have)
        opts = sorted(dict.fromkeys(list(options) + [cur]))  # ensure current is listed
        cid = f"{file}::{path}"
        html = "".join(f'<option value="{o}"{" selected" if o == cur else ""}>{o}</option>'
                       for o in opts)
        return (f'<div class="r"><label title="{cid}">{label}</label>'
                f'<select data-id="{cid}" onchange="onSlide(this)" class="sel">{html}</select>'
                f'<span></span></div>')

    rows.append('<div class="grp">Shape &amp; island</div>')
    rows += [slider("biomes", *c) for c in SHAPE_SLIDERS]
    rows.append(pair("biomes", "island center", "island.center"))  # [x, z] world origin
    rows.append('<div class="grp">3D relief, floats &amp; water (see them in the 3D view)</div>')
    rows += [slider("biomes", *c) for c in SHAPE_3D_SLIDERS]

    # Per-layer controls for each authored noise stack (the stack IS what runs).
    for field in STACK_FIELDS:
        try:
            layers = get_path(docs["biomes"], field)["layers"]
        except Exception:
            continue
        rows.append(f'<div class="grp">{field} layers</div>')
        for i, l in enumerate(layers):
            base = f"{field}.layers.{i}"
            f = float(l.get("frequency", 0.01)); w = float(l.get("weight", 1.0))
            o = int(l.get("octaves", 3))
            rows.append(
                f'<div class="r4"><label>{i}</label>'
                f'<span>f<input type="number" step="any" value="{f:g}" data-id="biomes::{base}.frequency" oninput="onSlide(this)"></span>'
                f'<span>w<input type="number" step="0.02" value="{w:g}" data-id="biomes::{base}.weight" oninput="onSlide(this)"></span>'
                f'<span>o<input type="number" step="1" min="1" max="10" value="{o}" data-id="biomes::{base}.octaves" oninput="onSlide(this)"></span>'
                f'</div>')
            # type (perlin/ridged/billow/worley) + domain offset, only for the keys the
            # layer actually has — so Save doesn't inject type/offset into layers without.
            extra = []
            if "type" in l:
                cur = str(l["type"]); ts = ["perlin", "ridged", "billow", "worley"]
                if cur not in ts:
                    ts = [cur] + ts
                topts = "".join(f'<option value="{t}"{" selected" if t == cur else ""}>{t}</option>'
                                for t in ts)
                extra.append(f'<span>ty<select data-id="biomes::{base}.type" onchange="onSlide(this)">{topts}</select></span>')
            off = l.get("offset")
            if isinstance(off, list) and len(off) >= 2:
                extra.append(f'<span>ox<input type="number" step="any" value="{float(off[0]):g}" data-id="biomes::{base}.offset.0" oninput="onSlide(this)"></span>')
                extra.append(f'<span>oy<input type="number" step="any" value="{float(off[1]):g}" data-id="biomes::{base}.offset.1" oninput="onSlide(this)"></span>')
            if extra:
                rows.append('<div class="r4"><label></label>' + "".join(extra) + '</div>')

    def triple(file, label, path, step="0.01"):
        """Three number inputs for an [r, g, b] tint (skips if the biome has no tint)."""
        try:
            seq = get_path(docs[file], path)
            v = [float(seq[0]), float(seq[1]), float(seq[2])]
        except Exception:
            return ""
        ins = "".join(num_input(f"{file}::{path}.{k}", x, step) for k, x in enumerate(v))
        return f'<div class="r3"><label title="{path}">{label}</label>{ins}</div>'

    # Splines: an interactive curve — grab a point and drag it. The number rows below
    # mirror it (two-way), so you can also type exact values. The SVG's y-domain is
    # derived from the current points (with padding); x-domain is passed in.
    def spline_editor(key, pts, xmin, xmax, headline):
        try:
            ys = [float(p[1]) for p in pts]
        except Exception:
            return ""
        ylo, yhi = min(ys), max(ys)
        pad = max(2.0, (yhi - ylo) * 0.18)
        ylo, yhi = ylo - pad, yhi + pad
        out = [f'<div class="grp">{headline}</div>',
               f'<div class="splinewrap"><svg class="spline" data-key="{key}" data-xmin="{xmin}" '
               f'data-xmax="{xmax}" data-ymin="{ylo:g}" data-ymax="{yhi:g}" viewBox="0 0 240 130" '
               f'preserveAspectRatio="none">'
               f'<rect class="frame" x="26" y="8" width="206" height="106"></rect>'
               f'<line class="zero"></line><polyline class="curve"></polyline>'
               f'<g class="pts"></g></svg>'
               f'<div class="splinebar"><button type="button" onclick="splineAdd(\'{key}\')">＋ point</button>'
               f'<span class="dim">drag a point · right-click a point to delete</span></div></div>']
        out.append('<div class="splinepts" data-key="%s">%s</div>'
                   % (key, "".join(pair("biomes", f"pt {i}", f"{key}.{i}") for i in range(len(pts)))))
        return "".join(out)

    for key in list(docs["biomes"].keys()):
        if not str(key).endswith("_spline") or key == "island_profile_spline":
            continue  # island_profile_spline gets its own section (with the biome bands)
        pts = docs["biomes"][key]
        if isinstance(pts, list) and pts:
            rows.append(spline_editor(key, pts, -1, 1, f"{key} — drag the curve · [noise → height]"))

    # --- Island shape: the coast→core profile curve + the biome elevation bands -------
    prof = docs["biomes"].get("island_profile_spline")
    if not (isinstance(prof, list) and len(prof) >= 2):
        # Default ≈ the built-in smoothstep (x = d/radius: 0 core → 1 coast; y = rise 1→0).
        prof = [[0.0, 1.0], [0.2, 0.9], [0.4, 0.66], [0.5, 0.5],
                [0.6, 0.34], [0.8, 0.1], [1.0, 0.0]]
    rows.append(spline_editor("island_profile_spline", prof, 0, 1,
        "Island profile — drag the coast→core slope · [0 = core … 1 = coast, y = land rise]"))
    rows.append('<div class="hint" style="margin:2px 0 6px">Shapes how the land climbs from '
                'the coast to the core (replaces the built-in smoothstep). The biome rings below '
                'sit where this profile crosses each elevation band.</div>')
    rows.append('<div class="grp">Biome elevation bands (blocks vs sea level · floor → ceiling)</div>')
    for i, bb in enumerate(docs["biomes"].get("biomes", [])):
        nm = str(bb.get("name", f"biome {i}"))
        rows.append(slider("biomes", f"{nm} floor", f"biomes.{i}.elevation.0", -24, 140, 1))
        rows.append(slider("biomes", f"{nm} ceil",  f"biomes.{i}.elevation.1", -24, 140, 1))

    # Per-biome: climate window, elevation band, surface/filler blocks, tint, trees, and
    # per-biome OVERRIDES of the global terrain3d / rivers / lakes knobs. Each override
    # starts from the global value (read below) and only becomes authored once you change
    # it — so a biome you don't touch stays on the global value (world byte-identical).
    def gdef(path, d):
        try:
            return float(get_path(docs["biomes"], path))
        except Exception:
            return d
    # subpath (under the biome) -> (label, lo, hi, step, global default, tooltip).
    BIOME_OVERRIDES = [
        ("terrain3d.enabled",               "3D enabled",     0,    1,    1,
         gdef("terrain3d.enabled", 1),               "terrain3d.enabled — turn the 3D swell off for just this biome (0/1)"),
        ("terrain3d.amplitude",             "3D amplitude",   0,    128,  1,
         gdef("terrain3d.amplitude", 28),            "terrain3d.amplitude — overhang/cliff swell, in blocks, for this biome"),
        ("terrain3d.float_threshold",       "Float threshold",0.0,  1.0,  0.01,
         gdef("terrain3d.float_threshold", 0.4),     "terrain3d.float_threshold — lower = more/larger floating islands here"),
        ("terrain3d.float_reach",           "Float reach",    0,    200,  2,
         gdef("terrain3d.float_reach", 64),          "terrain3d.float_reach — how far above the surface floats form here"),
        ("rivers.width",                    "River width",    0.0,  0.3,  0.005,
         gdef("rivers.width", 0.06),                 "rivers.width — channel width where this biome's columns are (0 = no rivers here)"),
        ("lakes.chance",                    "Lake chance",    0.0,  1.0,  0.02,
         gdef("lakes.chance", 0.45),                 "lakes.chance — probability a candidate lake spawns when its centre is in this biome"),
        ("terrain3d.coast_flatten.enabled", "Coast flatten on",0,   1,    1,
         gdef("terrain3d.coast_flatten.enabled", 0), "terrain3d.coast_flatten.enabled — smooth the swell near sea level here (0/1)"),
        ("terrain3d.coast_flatten.range",   "Coast flatten range",0,80,  1,
         gdef("terrain3d.coast_flatten.range", 12),  "terrain3d.coast_flatten.range — blocks above/below sea the flatten ramps over"),
        ("terrain3d.coast_flatten.min",     "Coast flatten min",0.0,1.0,  0.02,
         gdef("terrain3d.coast_flatten.min", 0.2),   "terrain3d.coast_flatten.min — swell scale right at the waterline"),
    ]

    def bget(b, subpath):
        """The biome's authored value at a nested dotted subpath, or None if absent."""
        node = b
        for k in subpath.split("."):
            if not isinstance(node, dict) or k not in node:
                return None
            node = node[k]
        return node

    def ovr(i, b, subpath, label, lo, hi, step, gd, title):
        """A per-biome override row: range + number that mirror each other (ampOn). Shows
        the global value until edited; the first edit attaches the data-id so it submits
        and set_path auto-vivifies `biomes.i.<subpath>`. Type a value past the slider's
        max — the number box is unbounded."""
        cid = f"biomes::biomes.{i}.{subpath}"
        cur = bget(b, subpath)
        present = cur is not None
        val = float(cur) if present else float(gd)
        did = f' data-id="{cid}"' if present else ''
        return (f'<div class="r"><label title="{title}">{label}</label>'
                f'<input type="range" min="{lo}" max="{hi}" step="{step}" value="{val}" '
                f'class="rng" data-for="{cid}" oninput="ampOn(this)">'
                f'<input type="number" step="any" value="{val:g}"{did} data-ampcid="{cid}" '
                f'class="nv" oninput="ampOn(this)"></div>')

    def overrides_ctl(i, b):
        rows = [f'<div class="grp sub">terrain3d · rivers · lakes — per-biome overrides</div>']
        rows += [ovr(i, b, *o) for o in BIOME_OVERRIDES]
        return "".join(rows)

    def dlayers_ctl(i, b):
        """Per-biome 3D density noise stack (terrain3d.density.layers) — UNLIMITED layers,
        each authored just like the global peaks/density stacks. ＋ adds a layer, ✖ removes
        one; apply_form rebuilds the list from the submitted rows (add/remove safe). Empty
        = the biome inherits the global density blend."""
        t3   = b.get("terrain3d") if isinstance(b.get("terrain3d"), dict) else {}
        dens = t3.get("density")  if isinstance(t3.get("density"),  dict) else {}
        layers = dens.get("layers") if isinstance(dens.get("layers"), list) else []
        out = [f'<div class="palrow"><span class="pallabel" title="this biome\'s own 3D '
               f'density noise stack — overrides the global swell where the biome is">'
               f'3D density layers</span>'
               f'<button type="button" class="mini" onclick="dlayerAdd({i})">＋ layer</button></div>']
        for j, l in enumerate(layers):
            base = f"biomes.{i}.terrain3d.density.layers.{j}"
            fr = float(l.get("frequency", 0.01)); w = float(l.get("weight", 1.0))
            o  = int(l.get("octaves", 3));        ty = str(l.get("type", "perlin"))
            ts = ["perlin", "ridged", "billow", "worley"]
            if ty not in ts:
                ts = [ty] + ts
            topts = "".join(f'<option value="{t}"{" selected" if t == ty else ""}>{t}</option>'
                            for t in ts)
            out.append(
                f'<div class="r4 dlayer" data-bi="{i}">'
                f'<span>f<input type="number" step="any" value="{fr:g}" data-id="biomes::{base}.frequency" oninput="onSlide(this)"></span>'
                f'<span>w<input type="number" step="0.02" value="{w:g}" data-id="biomes::{base}.weight" oninput="onSlide(this)"></span>'
                f'<span>o<input type="number" step="1" value="{o}" data-id="biomes::{base}.octaves" oninput="onSlide(this)"></span>'
                f'<span>ty<select data-id="biomes::{base}.type" onchange="onSlide(this)">{topts}</select></span>'
                f'<button type="button" class="mini" onclick="dlayerDel(this)" title="remove">✖</button></div>')
        return f'<div class="dlayers" data-bi="{i}">{"".join(out)}</div>'

    def surfmasks_ctl(i, b):
        """Noise-driven surface block patches (`surface_masks:`). Each mask = a block + a
        NoiseMask (layers + threshold/width + a steepness falloff, incl. an authored Bezier
        curve). Where the mask passes, that block replaces the surface. ＋/✖ add-remove;
        apply_form rebuilds the list. The whole authoring UI for the noise-mask system."""
        masks = b.get("surface_masks")
        masks = masks if isinstance(masks, list) else []
        out = [f'<div class="palrow"><span class="pallabel" title="noise-driven surface block '
               f'patches — where a mask passes, its block replaces the surface (outcrops, '
               f'mossy bands, flower fields)">surface block masks</span>'
               f'<button type="button" class="mini" onclick="maskAdd({i})">＋ mask</button></div>']
        for mi, m in enumerate(masks):
            base = f"biomes.{i}.surface_masks.{mi}"
            blk = str(m.get("block", "stone"))
            thr = float(m.get("threshold", 0.0)); wid = float(m.get("width", 0.5))
            gn  = float(m.get("gain", 0.5));       fo = str(m.get("falloff", "smoothstep"))
            inv = 1 if m.get("invert") else 0
            bopts = "".join(f'<option value="{o}"{" selected" if o == blk else ""}>{o}</option>'
                            for o in sorted(dict.fromkeys(SURFACE_BLOCKS + [blk])))
            fopts = "".join(f'<option value="{o}"{" selected" if o == fo else ""}>{o}</option>'
                            for o in FALLOFFS)
            iopts = "".join(f'<option value="{v}"{" selected" if v == inv else ""}>{t}</option>'
                            for v, t in ((0, "normal"), (1, "invert")))
            # noise layers for this mask
            layers = m.get("layers") if isinstance(m.get("layers"), list) else []
            lrows = []
            for lj, l in enumerate(layers):
                lb = f"{base}.layers.{lj}"
                fr = float(l.get("frequency", 0.03)); w = float(l.get("weight", 1.0))
                o  = int(l.get("octaves", 3));        ty = str(l.get("type", "perlin"))
                ts = ["perlin", "ridged", "billow", "worley"]
                if ty not in ts:
                    ts = [ty] + ts
                topts = "".join(f'<option value="{t}"{" selected" if t == ty else ""}>{t}</option>'
                                for t in ts)
                lrows.append(
                    f'<div class="r4 mlayer" data-bi="{i}" data-mi="{mi}">'
                    f'<span>f<input type="number" step="any" value="{fr:g}" data-id="biomes::{lb}.frequency" oninput="onSlide(this)"></span>'
                    f'<span>w<input type="number" step="0.02" value="{w:g}" data-id="biomes::{lb}.weight" oninput="onSlide(this)"></span>'
                    f'<span>o<input type="number" step="1" value="{o}" data-id="biomes::{lb}.octaves" oninput="onSlide(this)"></span>'
                    f'<span>ty<select data-id="biomes::{lb}.type" onchange="onSlide(this)">{topts}</select></span>'
                    f'<button type="button" class="mini" onclick="mlayerDel(this)" title="remove">✖</button></div>')
            # Bezier curve editor (draggable) — authored points; shown only for falloff=bezier.
            bez = m.get("bezier") if isinstance(m.get("bezier"), list) else [[0, 0], [1, 1]]
            ckey = f"m{i}_{mi}"
            ptrows = "".join(
                f'<div class="r2 curvept" data-key="{ckey}">'
                f'<input type="number" step="any" value="{float(p[0]):g}" data-id="biomes::{base}.bezier.{pi}.0" oninput="onNum(this)">'
                f'<input type="number" step="any" value="{float(p[1]):g}" data-id="biomes::{base}.bezier.{pi}.1" oninput="onNum(this)"></div>'
                for pi, p in enumerate(bez) if isinstance(p, (list, tuple)) and len(p) == 2)
            curve = (f'<div class="curvewrap" data-key="{ckey}">'
                     f'<svg class="curve" data-key="{ckey}" data-xmin="0" data-xmax="1" '
                     f'data-ymin="0" data-ymax="1" data-falloff="{fo}" data-gain="{gn:g}" '
                     f'viewBox="0 0 240 130" preserveAspectRatio="none">'
                     f'<rect class="frame" x="26" y="8" width="206" height="106"></rect>'
                     f'<polyline class="ccurve"></polyline><g class="cpts"></g></svg>'
                     f'<div class="splinebar"><button type="button" class="cadd" onclick="curveAdd(\'{ckey}\')">＋ point</button>'
                     f'<span class="dim">steepness curve preview · bezier: drag · right-click to delete</span></div>'
                     f'<div class="curvepts" data-key="{ckey}">{ptrows}</div></div>')
            out.append(
                f'<div class="maskblk" data-bi="{i}" data-mi="{mi}">'
                f'<div class="r2"><label title="block painted where the mask passes">block</label>'
                f'<select data-id="biomes::{base}.block" onchange="onSlide(this)" class="sel">{bopts}</select>'
                f'<button type="button" class="mini" onclick="maskDel(this)" title="remove this mask">✖</button></div>'
                + slider_ovr("threshold", f"{base}.threshold", -1, 1, 0.02, thr,
                             "band centre in noise units (~-1..1) — where the patch turns on")
                + slider_ovr("width", f"{base}.width", 0.01, 1.0, 0.01, wid,
                             "transition half-width = steepness; smaller = sharper edge")
                + f'<div class="r2"><label title="steepness curve shape">falloff</label>'
                  f'<select data-id="biomes::{base}.falloff" onchange="onFalloff(this)" class="sel">{fopts}</select>'
                  f'<select data-id="biomes::{base}.invert" onchange="onSlide(this)" class="sel">{iopts}</select></div>'
                + slider_ovr("gain", f"{base}.gain", 0.02, 0.98, 0.02, gn,
                             "shape for the Gain falloff (0..1; 0.5 ~ linear)")
                + f'<div class="palrow"><span class="dim">noise layers (appendable)</span>'
                  f'<button type="button" class="mini" onclick="mlayerAdd({i},{mi})">＋ layer</button></div>'
                  f'<div class="mlayers" data-bi="{i}" data-mi="{mi}">{"".join(lrows)}</div>'
                + curve
                + '</div>')
        return f'<div class="surfmasks" data-bi="{i}">{"".join(out)}</div>'

    def slider_ovr(label, path, lo, hi, step, val, title):
        """A plain range+number row (both submit-mirrored) for a mask scalar at `path`."""
        cid = f"biomes::{path}"
        return (f'<div class="r"><label title="{title}">{label}</label>'
                f'<input type="range" min="{lo}" max="{hi}" step="{step}" value="{val}" '
                f'class="rng" data-for="{cid}" oninput="onRange(this)">'
                f'<input type="number" step="any" value="{val:g}" data-id="{cid}" '
                f'class="nv" oninput="onNum(this)"></div>')

    def cid_slider(label, cid, lo, hi, step, val, title):
        """range+number row for an arbitrary full cid (FILE::path)."""
        return (f'<div class="r"><label title="{title}">{label}</label>'
                f'<input type="range" min="{lo}" max="{hi}" step="{step}" value="{val}" '
                f'class="rng" data-for="{cid}" oninput="onRange(this)">'
                f'<input type="number" step="any" value="{val:g}" data-id="{cid}" '
                f'class="nv" oninput="onNum(this)"></div>')

    def world_mask_ctl(label, base, file="world"):
        """A SINGLE optional noise mask at <file>::<base> (caves.mask / ores.iron.mask, or a
        biome's selection `mask:`): no block (it gates, not paints), just threshold/width/
        falloff/gain/invert + appendable noise layers + a bezier curve. No layers = off."""
        def wg(key, d):
            try:
                return get_path(docs[file], base + "." + key)
            except Exception:
                return d
        thr = float(wg("threshold", 0.0)); wid = float(wg("width", 0.5))
        gn  = float(wg("gain", 0.5));       fo = str(wg("falloff", "smoothstep"))
        inv = 1 if wg("invert", False) else 0
        layers = wg("layers", []); layers = layers if isinstance(layers, list) else []
        fopts = "".join(f'<option value="{o}"{" selected" if o == fo else ""}>{o}</option>' for o in FALLOFFS)
        iopts = "".join(f'<option value="{v}"{" selected" if v == inv else ""}>{t}</option>'
                        for v, t in ((0, "normal"), (1, "invert")))
        lrows = []
        for lj, l in enumerate(layers):
            lb = f"{file}::{base}.layers.{lj}"
            fr = float(l.get("frequency", 0.02)); w = float(l.get("weight", 1.0))
            o  = int(l.get("octaves", 3));        ty = str(l.get("type", "perlin"))
            ts = ["perlin", "ridged", "billow", "worley"]
            if ty not in ts:
                ts = [ty] + ts
            topts = "".join(f'<option value="{t}"{" selected" if t == ty else ""}>{t}</option>' for t in ts)
            lrows.append(
                f'<div class="r4 mlayer">'
                f'<span>f<input type="number" step="any" value="{fr:g}" data-id="{lb}.frequency" oninput="onSlide(this)"></span>'
                f'<span>w<input type="number" step="0.02" value="{w:g}" data-id="{lb}.weight" oninput="onSlide(this)"></span>'
                f'<span>o<input type="number" step="1" value="{o}" data-id="{lb}.octaves" oninput="onSlide(this)"></span>'
                f'<span>ty<select data-id="{lb}.type" onchange="onSlide(this)">{topts}</select></span>'
                f'<button type="button" class="mini" onclick="mlayerDel(this)" title="remove">✖</button></div>')
        bez = wg("bezier", [[0, 0], [1, 1]]); bez = bez if isinstance(bez, list) else [[0, 0], [1, 1]]
        ckey = base.replace(".", "_")
        ptrows = "".join(
            f'<div class="r2 curvept" data-key="{ckey}">'
            f'<input type="number" step="any" value="{float(p[0]):g}" data-id="{file}::{base}.bezier.{pi}.0" oninput="onNum(this)">'
            f'<input type="number" step="any" value="{float(p[1]):g}" data-id="{file}::{base}.bezier.{pi}.1" oninput="onNum(this)"></div>'
            for pi, p in enumerate(bez) if isinstance(p, (list, tuple)) and len(p) == 2)
        curve = (f'<div class="curvewrap" data-key="{ckey}">'
                 f'<svg class="curve" data-key="{ckey}" data-xmin="0" data-xmax="1" data-ymin="0" data-ymax="1" '
                 f'data-falloff="{fo}" data-gain="{gn:g}" viewBox="0 0 240 130" preserveAspectRatio="none">'
                 f'<rect class="frame" x="26" y="8" width="206" height="106"></rect>'
                 f'<polyline class="ccurve"></polyline><g class="cpts"></g></svg>'
                 f'<div class="splinebar"><button type="button" class="cadd" onclick="curveAdd(\'{ckey}\')">＋ point</button>'
                 f'<span class="dim">steepness curve preview · bezier: drag · right-click to delete</span></div>'
                 f'<div class="curvepts" data-key="{ckey}">{ptrows}</div></div>')
        return (f'<div class="wmask"><div class="sub">{label}</div>'
                + cid_slider("threshold", f"{file}::{base}.threshold", -1, 1, 0.02, thr,
                             "band centre in noise units — where it turns on")
                + cid_slider("width", f"{file}::{base}.width", 0.01, 1.0, 0.01, wid,
                             "transition half-width = steepness; smaller = sharper")
                + f'<div class="r2"><label title="steepness curve">falloff</label>'
                  f'<select data-id="{file}::{base}.falloff" onchange="onFalloff(this)" class="sel">{fopts}</select>'
                  f'<select data-id="{file}::{base}.invert" onchange="onSlide(this)" class="sel">{iopts}</select></div>'
                + cid_slider("gain", f"{file}::{base}.gain", 0.02, 0.98, 0.02, gn,
                             "shape for the Gain falloff (0..1)")
                + f'<div class="palrow"><span class="dim">noise layers (none = mask off)</span>'
                  f'<button type="button" class="mini" onclick="wlayerAdd(\'{file}\',\'{base}\')">＋ layer</button></div>'
                  f'<div class="mlayers wlayers" data-wbase="{base}">{"".join(lrows)}</div>'
                + curve + '</div>')

    def palette_ctl(i, field, b):
        """A weighted block-list editor for a biome's top/filler (like a feature's block
        list). Each row = block dropdown + weight; ＋ adds, ✖ removes. apply_form rebuilds
        the list (single block w=1 → a clean scalar)."""
        raw = b.get(field)
        ents = []
        if isinstance(raw, str):
            ents = [(raw, 1)]
        elif isinstance(raw, list):
            for e in raw:
                if isinstance(e, str):
                    ents.append((e, 1))
                elif isinstance(e, dict) and e.get("name") is not None:
                    ents.append((str(e["name"]), e.get("w", 1)))
        if not ents:
            ents = [("grass" if field == "top" else "dirt", 1)]
        out = [f'<div class="palrow"><span class="pallabel">{field}</span>'
               f'<button type="button" class="mini" onclick="palAdd({i},\'{field}\')">＋ block</button></div>']
        for j, (nm, w) in enumerate(ents):
            opts = sorted(dict.fromkeys(SURFACE_BLOCKS + [nm]))
            sel = "".join(f'<option value="{o}"{" selected" if o == nm else ""}>{o}</option>' for o in opts)
            out.append(
                f'<div class="r2 palent" data-bi="{i}" data-field="{field}">'
                f'<select data-id="biomes::biomes.{i}.{field}.{j}.name" onchange="onSlide(this)" class="sel">{sel}</select>'
                f'<input type="number" step="any" value="{float(w):g}" title="weight" '
                f'data-id="biomes::biomes.{i}.{field}.{j}.w" class="nv" oninput="onNum(this)">'
                f'<button type="button" class="mini" onclick="palDel(this)" title="remove">✖</button></div>')
        return f'<div class="palette" data-bi="{i}" data-field="{field}">{"".join(out)}</div>'

    def name_ctl(i, b):
        """Editable biome name (also retitles the card header live via onName)."""
        nm = str(b.get("name", f"biome {i}"))
        return (f'<div class="r"><label title="biome name">name</label>'
                f'<input class="nv" value="{nm}" data-id="biomes::biomes.{i}.name" '
                f'oninput="onName(this)"><span></span></div>')

    def biome_card(i, b):
        # The card is large now, so group it into tabs (Surface / 3D / Masks). Climate is
        # gone; elevation lives in the "Biome elevation bands" section under the profile.
        surface = "".join([
            palette_ctl(i, "top", b),
            palette_ctl(i, "filler", b),
            triple("biomes", "tint",    f"biomes.{i}.tint"),
            slider("biomes", "trees",   f"biomes.{i}.trees", 0.0, 0.1, 0.001),
            choice("biomes", "tree",    f"biomes.{i}.tree", TREE_SPECIES),
        ])
        threed = overrides_ctl(i, b) + dlayers_ctl(i, b)
        masks = world_mask_ctl("Selection mask — appears only where this passes (empty = whole band)",
                               f"biomes.{i}.mask", "biomes") + surfmasks_ctl(i, b)
        secs = [("Surface", surface), ("3D & overrides", threed), ("Masks", masks)]
        tabs = "".join(f'<button type="button" class="btab{" on" if k == 0 else ""}" '
                       f'onclick="biomeTab(this,{i},{k})">{nm}</button>'
                       for k, (nm, _) in enumerate(secs))
        panes = "".join('<div class="bpane" data-bi="%d" data-k="%d"%s>%s</div>'
                        % (i, k, "" if k == 0 else ' style="display:none"', ct)
                        for k, (_, ct) in enumerate(secs))
        inner = name_ctl(i, b) + f'<div class="btabs">{tabs}</div>' + panes
        nm = b.get("name", f"biome {i}")
        return (f'<div class="biomecard" data-bi="{i}">'
                f'<div class="bhdr"><span class="ca" onclick="toggleBiome(this)">▾</span>'
                f'<span class="bname" onclick="toggleBiome(this)">{nm}</span>'
                f'<button type="button" class="bdel" title="delete this biome" '
                f'onclick="delBiome({i})">🗑</button></div>'
                f'<div class="bbody">{inner}</div></div>')

    rows.append('<div class="grp">Biomes — climate · elevation · blocks · trees · per-biome 3D/river/lake overrides · density layers</div>')
    rows.append('<div class="hint" style="margin:2px 0 6px">First match wins (top→down) — a '
                'new biome is inserted just above the catch-all so it can show. Add/delete saves.</div>')
    rows.append('<div class="row"><button type="button" class="addb" onclick="addBiome()" '
                'style="flex:1">＋ Add biome</button></div>')
    for i, b in enumerate(docs["biomes"].get("biomes", [])):
        rows.append(biome_card(i, b))

    rows.append('<div class="grp">Caves, ravines, pools &amp; ores</div>')
    rows += [slider("world", *c) for c in WORLD_SLIDERS]
    rows.append('<div class="hint" style="margin:6px 0 2px">Noise masks — add layers to '
                'concentrate caves/ore into regions (same threshold/steepness/bezier editor '
                'as the biome masks). No layers = off.</div>')
    rows += [world_mask_ctl(label, base) for label, base in WORLD_MASKS]

    return wrap_sections([r for r in rows if r])


def wrap_sections(rows):
    """Group the flat row list into collapsible <section>s, split at each `grp` marker
    (a `<div class="grp">Title</div>`). Each section gets a clickable header + body."""
    GRP_PRE, GRP_SUF = '<div class="grp">', '</div>'
    secs, body = [], []
    title = "General"
    for r in rows:
        if r.startswith(GRP_PRE) and r.endswith(GRP_SUF):
            secs.append((title, body)); title, body = r[len(GRP_PRE):-len(GRP_SUF)], []
        else:
            body.append(r)
    secs.append((title, body))
    out = []
    for sid, (t, b) in enumerate(secs):
        if not b:
            continue
        out.append(f'<div class="sec"><div class="hdr" onclick="toggleSec(this)">'
                   f'<span class="ca">▾</span>{t}</div><div class="body">{"".join(b)}</div></div>')
    return "".join(out)


def run_genmap(pixels, step, view, seed, cx, cz, solo=-1):
    """Run the engine's headless export for the chosen view, at the given seed/center.
    `solo` >= 0 renders the 3D voxel view as ONLY that biome (--only-biome)."""
    px = int(pixels)
    common = ["--seed", str(int(seed)), "--center", str(int(cx)), str(int(cz))]
    if view == "3d":
        if int(solo) >= 0:
            # Solo = a small, focused N-chunk SLICE of just this biome at full block
            # resolution (not the biome forced across the whole island-size view).
            px, step = SOLO_SLICE_BLOCKS, 1
        args = terrain3d.vox_args(exe_path(), px, step) + common
        if int(solo) >= 0:
            args += ["--only-biome", str(int(solo))]
    elif view.startswith("noise:"):
        args = [exe_path(), "--genmap", "--mapsize", str(px), "--mapstep", str(step),
                "--mode", "noise", "--layer", view.split(":", 1)[1], "--out", MAP_OUT] + common
    elif view == "cross":
        args = [exe_path(), "--genmap", "--mapsize", str(px), "--mapstep", str(step),
                "--mode", "cross", "--out", MAP_OUT] + common
    elif view == "biomes":
        args = [exe_path(), "--genmap", "--mapsize", str(px), "--mapstep", str(step),
                "--mode", "biomes", "--out", MAP_OUT] + common
    else:
        args = [exe_path(), "--genmap", "--mapsize", str(px), "--mapstep", str(step),
                "--out", MAP_OUT] + common
    cp = subprocess.run(args, cwd=ROOT, check=True, capture_output=True)
    # Surface the engine's generation warnings (R12 amplitude band, noise-octave clamp)
    # so a slow/degenerate config is visible in the tool instead of buried in stderr.
    err = cp.stderr.decode(errors="ignore")
    warn = " · ".join(dict.fromkeys(  # de-dup, keep order
        line.strip().lstrip("[worldgen] ").lstrip("[noise] ")
        for line in err.splitlines()
        if "WARNING" in line or "clamp" in line.lower()))
    return cp.stdout.decode(errors="ignore"), warn


def gen_params(form):
    return (int(form.get("__pixels__", 640)), int(form.get("__step__", 8)),
            form.get("__view__", "top"), int(form.get("__seed__", 1337)),
            int(form.get("__cx__", 0)), int(form.get("__cz__", 0)),
            int(form.get("__solo__", -1)))


app = Flask(__name__)
terrain3d.register(app)  # /vendor/<file>, /voxels.bin, /sealevel


@app.route("/")
def index():
    docs = load_docs()
    opts = "\n".join(f'<option value="{v}">{lbl}</option>' for lbl, v in VIEW_MODES)
    names = [str(b.get("name", f"biome {i}")) for i, b in enumerate(docs["biomes"].get("biomes", []))]
    page = (PAGE.replace("__CONTROLS__", build_controls(docs))
            .replace("__VIEWS__", opts).replace("__BIOMES__", json.dumps(names))
            .replace("__SURFACE__", json.dumps(SURFACE_BLOCKS))
            .replace("__FALLOFFS__", json.dumps(FALLOFFS))
            .replace("__HEAD__", terrain3d.HEAD).replace("__EXROW__", terrain3d.EX_ROW))
    return Response(page, mimetype="text/html")


@app.route("/regen", methods=["POST"])
def regen():
    docs = load_docs()
    apply_form(docs, request.form)
    deploy_only(docs)  # preview only — does NOT touch the committed source
    t0 = time.perf_counter()
    try:
        out, warn = run_genmap(*gen_params(request.form))
    except subprocess.CalledProcessError as e:
        return jsonify({"ok": False, "err": e.stderr.decode(errors="ignore") or "genmap failed"}), 500
    ms = int((time.perf_counter() - t0) * 1000)
    # The engine prints a "(... details ...)" summary — surface it in the HUD.
    info = out[out.find("(") + 1:out.rfind(")")] if "(" in out and ")" in out else ""
    return jsonify({"ok": True, "ms": ms, "info": info, "warn": warn})


@app.route("/locate", methods=["POST"])
def locate():
    """Find where a chosen biome occurs and report its world coords, so the client can
    jump the live 3D view there ('Show this biome'). Applies the current (unsaved) edits
    to the preview first, so you locate the biome under the config you're tuning."""
    docs = load_docs()
    apply_form(docs, request.form)
    deploy_only(docs)
    try:
        idx = int(request.form.get("__biome_idx__", 0))
        seed = int(request.form.get("__seed__", 1337))
        cx = int(request.form.get("__cx__", 0)); cz = int(request.form.get("__cz__", 0))
    except ValueError:
        return jsonify({"ok": False, "err": "bad params"}), 400
    try:
        res = run_locate(idx, seed, cx, cz)
    except subprocess.CalledProcessError as e:
        return jsonify({"ok": False, "err": e.stderr.decode(errors="ignore") or "genmap failed"}), 500
    if not res:
        return jsonify({"ok": False, "err": "biome not found nearby — pan toward the island or try another seed"})
    return jsonify({"ok": True, "x": res[0], "z": res[1]})


def _biome_names(lst):
    return [str(b.get("name", f"biome {i}")) for i, b in enumerate(lst)]


@app.route("/biome/add", methods=["POST"])
def biome_add():
    """Append a blank biome (just above the last/catch-all entry so it can match) and
    SAVE — a structural change must persist to source, then re-render the controls."""
    docs = load_docs()
    apply_form(docs, request.form)
    lst = docs["biomes"].setdefault("biomes", CommentedSeq())
    lst.insert(max(0, len(lst) - 1), _default_biome(len(lst)))
    save_source(docs)
    return jsonify({"ok": True, "controls": build_controls(docs),
                    "names": _biome_names(lst), "msg": "added biome → saved"})


@app.route("/biome/delete", methods=["POST"])
def biome_delete():
    docs = load_docs()
    apply_form(docs, request.form)
    try:
        idx = int(request.form.get("__del__", -1))
    except ValueError:
        idx = -1
    lst = docs["biomes"].get("biomes", [])
    if not (0 <= idx < len(lst)):
        return jsonify({"ok": False, "err": "bad biome index"}), 400
    if len(lst) <= 1:
        return jsonify({"ok": False, "err": "can't delete the last biome"}), 400
    del lst[idx]
    save_source(docs)
    return jsonify({"ok": True, "controls": build_controls(docs),
                    "names": _biome_names(lst), "msg": "deleted biome → saved"})


@app.route("/save", methods=["POST"])
def save():
    docs = load_docs()
    apply_form(docs, request.form)
    save_source(docs)  # persist edits to assets/biomes.yaml + world.yaml
    return "saved"


@app.route("/restore", methods=["POST"])
def restore():
    for key in SRC:  # discard unsaved edits (revert the deployed copies to source)
        os.makedirs(os.path.dirname(DEPLOY[key]), exist_ok=True)
        shutil.copyfile(SRC[key], DEPLOY[key])
    return "restored"


@app.route("/map.png")
def map_png():
    return send_file(MAP_OUT, mimetype="image/png")


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>worldgen tool</title>
<style>
 :root{--bg:#171519;--pan:#211e26;--line:#383143;--in:#2a2433;--inb:#52426a;--txt:#efe6d4;--dim:#a99e8c;--acc:#d6b3f0;--ok:#a7dba8}
 *{box-sizing:border-box}
 body{margin:0;background:var(--bg);color:var(--txt);font:16px/1.55 system-ui,-apple-system,Segoe UI,sans-serif;display:flex;height:100vh;overflow:hidden}
 ::-webkit-scrollbar{width:13px}::-webkit-scrollbar-thumb{background:#3a3343;border-radius:6px}
 #side{width:640px;padding:22px;overflow:auto;height:100vh;border-right:1px solid var(--line)}
 #right{width:330px;padding:22px;overflow:auto;height:100vh;border-left:1px solid var(--line);background:var(--pan)}
 #main{flex:1;position:relative;background:#0c1014;overflow:hidden;cursor:grab}
 a,button,select,input,.sec,.hdr,.pt,.seg button{transition:background .12s,border-color .12s,transform .08s,color .12s,filter .12s}
 #map{position:absolute;inset:0;margin:auto;max-width:96%;max-height:96vh;image-rendering:pixelated;border:1px solid #333}
 #gl{position:absolute;inset:0;width:100%;height:100%;display:none}
 #cross{position:absolute;inset:0;pointer-events:none}
 #cross::before,#cross::after{content:'';position:absolute;left:50%;top:50%;background:rgba(255,90,90,.8)}
 #cross::before{width:1px;height:26px;transform:translate(-50%,-50%)}
 #cross::after{height:1px;width:26px;transform:translate(-50%,-50%)}
 .r{display:grid;grid-template-columns:168px 1fr 96px;gap:8px;align-items:center;margin:6px 0}
 .r2{display:grid;grid-template-columns:168px 1fr 1fr;gap:8px;align-items:center;margin:6px 0}
 .r3{display:grid;grid-template-columns:168px 1fr 1fr 1fr;gap:7px;align-items:center;margin:6px 0}
 /* interactive spline curve editor */
 .splinewrap{margin:6px 0}
 svg.spline{width:100%;height:170px;background:#15131a;border-radius:6px;cursor:crosshair;touch-action:none}
 svg.spline .frame{fill:none;stroke:#3a3340}
 svg.spline .zero{stroke:#4a5a4a;stroke-dasharray:3 3}
 svg.spline .curve{fill:none;stroke:var(--acc);stroke-width:2;vector-effect:non-scaling-stroke}
 svg.spline .pt{fill:#ffce6b;stroke:#1b1a1a;stroke-width:1.5;cursor:grab}
 svg.spline .pt:hover{fill:#fff}
 .splinebar{display:flex;align-items:center;gap:8px;margin-top:4px}
 .splinebar .dim{color:var(--dim);font-size:11px}
 .lg{display:flex;align-items:center;gap:6px;margin:2px 0;font-size:11px}
 .sw{width:12px;height:12px;border-radius:2px;display:inline-block;flex:0 0 auto;border:1px solid #0006}
 .nv{width:100%;background:var(--in);color:var(--txt);border:1px solid var(--inb);border-radius:4px;padding:5px 6px;font-size:13px}
 .r4{display:grid;grid-template-columns:64px 1fr 1fr 0.95fr;gap:6px;align-items:center;margin:5px 0}
 .r4 span{display:flex;align-items:center;gap:3px;color:var(--dim);font-size:13px}
 .r4 input[type=number],.r4 select{width:100%;background:var(--in);color:var(--txt);border:1px solid var(--inb);border-radius:4px;padding:4px 5px;font-size:13px}
 .sub{margin:10px 0 3px;font-weight:bold;color:#cdbfa6;font-size:14px}
 /* noise-mask (surface_masks) editor + bezier steepness curve */
 .maskblk,.wmask{border:1px solid var(--line);border-radius:7px;padding:7px 9px;margin:7px 0;background:#191620}
 .curvewrap{margin:5px 0 2px}
 svg.curve{width:100%;height:150px;background:#15131a;border-radius:6px;cursor:crosshair;touch-action:none}
 svg.curve .frame{fill:none;stroke:#3a3340}
 svg.curve .ccurve{fill:none;stroke:var(--acc);stroke-width:2;vector-effect:non-scaling-stroke}
 svg.curve .cpt{fill:#ffce6b;stroke:#1b1a1a;stroke-width:1.5;cursor:grab}
 svg.curve .cpt:hover{fill:#fff}
 .curvepts{display:none}   /* numeric mirror of the curve points (still submitted) */
 label{font-size:15px}.v{text-align:right;color:var(--acc)}
 input[type=range]{width:100%;height:22px}.sel{width:100%}
 .nv,.sel{background:var(--in);color:var(--txt);border:1px solid var(--inb);border-radius:6px;padding:7px 8px;font-size:15px}
 h2{margin:2px 0 14px;font-size:24px;font-weight:700;letter-spacing:.3px}
 h3{margin:18px 0 8px;font-size:13px;text-transform:uppercase;letter-spacing:1px;color:var(--dim)}
 .hint{color:var(--dim);font-size:13.5px;margin:7px 0 11px;line-height:1.55}
 .row{display:flex;gap:10px;margin:11px 0;align-items:center}.row label{width:58px;color:var(--dim)}
 .num{width:100%;background:var(--in);color:var(--txt);border:1px solid var(--inb);border-radius:7px;padding:9px 10px;font-size:16px}
 button{background:linear-gradient(#3f3350,#352b45);color:var(--txt);border:1px solid #5a4a6a;border-radius:8px;
   padding:11px 17px;cursor:pointer;font:16px system-ui;font-weight:600}
 button:hover{filter:brightness(1.16)}button:active{transform:translateY(1px)}
 .pad{display:grid;grid-template-columns:repeat(3,1fr);grid-template-rows:repeat(3,42px);gap:6px}
 .pad button{padding:0;font-size:19px;border-radius:7px}
 /* segmented World / Biome toggle */
 .seg{display:flex;border:1px solid var(--inb);border-radius:8px;overflow:hidden;margin-bottom:9px}
 .seg button{flex:1;border:0;border-radius:0;background:var(--in);padding:11px 0}
 .seg button.on{background:linear-gradient(#6b4b8f,#553a73);color:#fff}
 button.on{background:linear-gradient(#6b4b8f,#553a73);color:#fff;border-color:#7a5a9a}
 /* block palette editor */
 .palrow{display:flex;align-items:center;gap:10px;margin:11px 0 4px}
 .pallabel{font-weight:700;color:var(--acc);text-transform:capitalize;font-size:14px}
 .palent{grid-template-columns:1fr 72px 36px !important;gap:6px}
 .mini{padding:7px 12px;font-size:14px;border-radius:7px}
 /* collapsible sections (left panel) */
 .sec{border:1px solid var(--line);border-radius:7px;margin:8px 0;overflow:hidden}
 .hdr{background:#262228;padding:10px 12px;font-weight:bold;color:var(--ok);font-size:14px;cursor:pointer;user-select:none}
 .ca{display:inline-block;width:10px;transition:transform .12s}
 .sec.collapsed .ca{transform:rotate(-90deg)}
 .sec.collapsed .body{display:none}
 .body{padding:6px 12px 10px}
 /* per-biome cards (collapsible, add/delete) */
 .biomecard{border:1px solid var(--line);border-radius:7px;margin:9px 0;overflow:hidden;background:#1d1a22}
 .bhdr{display:flex;align-items:center;gap:8px;background:#2b2530;padding:9px 11px;user-select:none}
 .bhdr .ca{cursor:pointer}.bhdr .bname{flex:1;font-weight:bold;color:#cdbfa6;cursor:pointer;font-size:15px}
 .biomecard.collapsed .ca{transform:rotate(-90deg)}
 .biomecard.collapsed .bbody{display:none}
 .bbody{padding:6px 12px 10px}
 .btabs{display:flex;gap:4px;margin:4px 0 8px;border-bottom:1px solid var(--line)}
 .btab{background:var(--in);border:1px solid var(--inb);border-bottom:0;border-radius:7px 7px 0 0;padding:7px 12px;font-size:13px;font-weight:600}
 .btab.on{background:linear-gradient(#6b4b8f,#553a73);color:#fff;border-color:#7a5a9a}
 .bdel{padding:4px 10px;font-size:15px;background:linear-gradient(#5a2f3a,#46252f);border-color:#7a3a4a}
 .addb{background:linear-gradient(#2f5a3a,#25462f);border-color:#3a7a4a}
 #filter{width:100%;background:var(--in);color:var(--txt);border:1px solid var(--inb);border-radius:6px;padding:9px 11px;margin-bottom:6px;font-size:15px}
 .kbd{font:13px Consolas,monospace;background:var(--in);border:1px solid var(--inb);border-radius:4px;padding:1px 5px}
 .info{font-size:13px;color:var(--dim);line-height:1.6}
 #dirty{color:#e0a060;font-size:12px}
 #ex_row{display:none}
</style>
__HEAD__
</head><body>
<div id="side">
 <h2>worldgen</h2>
 <input id="filter" placeholder="filter controls… (e.g. peak, river, tint)" oninput="applyFilter()">
 <div id="controls">__CONTROLS__</div>
</div>
<div id="main" tabindex="0"><img id="map" src="/map.png?0" draggable="false"><canvas id="gl"></canvas><div id="cross"></div></div>
<div id="right">
 <h3>View</h3>
 <div class="seg"><button id="vwWorld" class="on" onclick="setView('top')">🗺 World</button>
   <button id="vwBiome" onclick="setView('biomes')">🌿 Biomes</button></div>
 <div class="row"><label>more</label><select id="view" style="flex:1">__VIEWS__</select></div>
 <div class="row"><label>size</label><input id="px" class="num" type="number" value="640" min="128" max="2048" step="64"></div>
 <div class="row"><label>blk/px</label><input id="st" class="num" type="number" value="8" min="1" max="32"></div>
 __EXROW__
 <div id="legend" style="display:none;margin-top:6px"></div>
 <h3>Per-biome</h3>
 <div class="row"><select id="bsel" style="flex:1"></select></div>
 <div class="row"><button id="bjump" style="flex:1" title="jump the live 3D view to where this biome occurs">🔍 Show this biome</button></div>
 <div class="row"><button id="bsolo" style="flex:1" title="3D view: a focused 8-chunk slice of ONLY this biome (its surface + features + per-biome 3D/river/lake params), at full block resolution">🔬 Solo this biome (3D)</button></div>
 <h3>Seed</h3>
 <div class="row"><input id="seed" class="num" type="number" value="1337" min="0"></div>
 <div class="row"><button id="dice" style="flex:1" title="random seed (preview only)">🎲 regenerate</button></div>
 <h3>Navigate</h3>
 <div class="pad">
   <span></span><button data-pan="0,-1" title="north (−Z)">↑</button><span></span>
   <button data-pan="-1,0" title="west (−X)">←</button>
   <button id="home" title="back to origin">⌂</button>
   <button data-pan="1,0" title="east (+X)">→</button>
   <span></span><button data-pan="0,1" title="south (+Z)">↓</button><span></span>
 </div>
 <div class="info" style="margin-top:6px">center <span id="ctr" style="color:var(--acc)">0, 0</span><br>
   <span style="color:var(--dim)">drag map to pan · wheel to zoom</span></div>
 <h3>Actions</h3>
 <div class="row"><button id="undoBtn" style="flex:1" disabled title="Ctrl+Z">↶ Undo</button><button id="redoBtn" style="flex:1" disabled title="Ctrl+Shift+Z">↷ Redo</button></div>
 <div class="row"><button id="save" style="flex:1">Save</button><button id="restore" style="flex:1">Restore</button></div>
 <div class="row"><span id="dirty"></span></div>
 <h3>Status</h3>
 <div class="info"><span id="status">…</span><br><span id="gi" style="color:var(--dim)"></span></div>
 <div id="warn" style="display:none;margin-top:6px;font-size:12.5px;color:#e8b24a;background:#2a2113;border:1px solid #5a4420;border-radius:6px;padding:6px 8px;line-height:1.5"></div>
 <h3>Shortcuts</h3>
 <div class="info"><span class="kbd">↑↓←→</span> pan · <span class="kbd">+/−</span> zoom<br>
   <span class="kbd">R</span> regen · <span class="kbd">S</span> save<br>
   <span class="kbd">Ctrl+Z</span> undo · <span class="kbd">Ctrl+Shift+Z</span> redo</div>
</div>
<script>
let timer=null, lastGen=0, dirty=false, cx=0, cz=0, drag=null, soloBiome=-1;
const SPLINES=[];   // redraw fns for each spline curve (kept in sync with the numbers)
const BIOMES=__BIOMES__;   // biome names (file order) — for the biome-view legend
const SURFACE=__SURFACE__;  // block names offered in the biome top/filler palettes
const $=id=>document.getElementById(id);
function is3D(){ return $('view').value==='3d'; }
// Golden-angle hue per biome index — MUST match biomeColor() in src/main.cpp so the
// legend swatches equal the on-map colours.
function hsv(h,s,v){ const i=Math.floor(h*6),f=h*6-i,p=v*(1-s),q=v*(1-f*s),t=v*(1-(1-f)*s);
  const m=[[v,t,p],[q,v,p],[p,v,t],[p,q,v],[t,p,v],[v,p,q]][i%6];
  return 'rgb('+m.map(x=>Math.round(x*255)).join(',')+')'; }
function biomeColor(i){ return hsv((i*0.61803398875)%1,0.5,0.92); }
function renderLegend(){
  const el=$('legend'), show=$('view').value==='biomes'; el.style.display=show?'':'none';
  if(show) el.innerHTML='<div class="dim" style="margin-bottom:3px">biome regions</div>'+
    BIOMES.map((n,i)=>'<div class="lg"><span class="sw" style="background:'+biomeColor(i)+'"></span>'+n+'</div>').join('');
}
function setDirty(b){ dirty=b; $('dirty').textContent=b?'● unsaved changes':''; }
// ---- Per-biome: pick a biome and jump the 3D view to where it occurs --------------
function fillBiomes(){
  const sel=$('bsel'), keep=sel.value;
  sel.innerHTML = BIOMES.map((n,i)=>'<option value="'+i+'">'+n+'</option>').join('');
  if(keep!==''&&+keep<BIOMES.length) sel.value=keep;   // keep the picked biome across renames
}
function jumpBiome(){
  const i=$('bsel').value, fd=formData(); fd.append('__biome_idx__', i);
  $('status').textContent='locating '+(BIOMES[i]||i)+'…';
  fetch('/locate',{method:'POST',body:fd}).then(async r=>{
    let d; try{ d=await r.json(); }catch(e){ d={ok:false,err:'bad response'}; }
    if(r.ok&&d.ok){ cx=d.x; cz=d.z; setCtr(); $('view').value='3d'; syncSeg(); regen();
      $('status').textContent='✓ '+(BIOMES[i]||i)+' @ '+d.x+', '+d.z; }
    else $('status').textContent='⚠ '+((d&&d.err)||r.status);
  });
}
// ---- Per-biome hide/show + add/delete --------------------------------------------
function toggleBiome(el){ el.closest('.biomecard').classList.toggle('collapsed'); }
function biomeTab(btn,i,k){ const card=btn.closest('.biomecard'); if(!card)return;
  card.querySelectorAll('.btabs .btab').forEach((b,j)=>b.classList.toggle('on',j===k));
  card.querySelectorAll('.bpane[data-bi="'+i+'"]').forEach(p=>p.style.display=(+p.dataset.k===k)?'':'none'); }
function onName(el){ const c=el.closest('.biomecard'), nm=c.querySelector('.bname'), i=+c.dataset.bi;
  if(nm) nm.textContent=el.value; if(i>=0&&i<BIOMES.length) BIOMES[i]=el.value; fillBiomes(); onSlide(el); }
function swapControls(html){           // replace the left-panel controls after a structural edit
  $('controls').innerHTML=html; SPLINES.length=0;
  document.querySelectorAll('svg.spline').forEach(initSpline);
  document.querySelectorAll('svg.curve').forEach(initCurve); applyFilter();
}
function handleStruct(r){ return r.json().then(d=>{
  if(d.ok){ swapControls(d.controls); BIOMES.length=0; d.names.forEach(n=>BIOMES.push(n));
    fillBiomes(); renderLegend(); setDirty(false); $('status').textContent=d.msg||'updated';
    // A biome add/delete renumbers indices and is already saved to disk: undo across it
    // would be inconsistent, so it's a fresh history baseline.
    undoStack.length=0; redoStack.length=0; pendingChange=false; lastSnapshot=snapshot(); updateHistoryUI();
    regen(); }
  else $('status').textContent='⚠ '+(d.err||'failed');
}).catch(()=>{ $('status').textContent='⚠ bad response'; }); }
function addBiome(){ $('status').textContent='adding biome…';
  fetch('/biome/add',{method:'POST',body:formData()}).then(handleStruct); }
function delBiome(i){ if(!confirm('Delete biome "'+(BIOMES[i]||i)+'"?  (saves immediately)')) return;
  const fd=formData(); fd.append('__del__', i); $('status').textContent='deleting biome…';
  fetch('/biome/delete',{method:'POST',body:fd}).then(handleStruct); }
function redrawSplines(){ SPLINES.forEach(f=>f()); }
// ---- Undo / redo (Ctrl+Z · Ctrl+Shift+Z) -----------------------------------------
// A history of whole-editor snapshots. A snapshot = the #controls HTML (with every live
// input value baked into its attribute so it survives innerHTML) + the BIOMES name list +
// the view/seed/zoom/centre. Edits are debounced into ONE entry (a burst of keystrokes =
// one undo step). Adding/deleting a biome is a server save that re-baselines and CLEARS
// history (its snapshots reference biome indices the server may renumber).
let undoStack=[], redoStack=[], lastSnapshot=null, restoring=false, pendingChange=false;
const HISTORY_MAX=100;
function bakeValues(root){            // write live input/select state into attributes
  root.querySelectorAll('input').forEach(el=>{
    if(el.type==='checkbox'||el.type==='radio'){ if(el.checked) el.setAttribute('checked',''); else el.removeAttribute('checked'); }
    else el.setAttribute('value', el.value);
  });
  root.querySelectorAll('select').forEach(sel=>{
    [].forEach.call(sel.options,o=>{ if(o.value===sel.value) o.setAttribute('selected',''); else o.removeAttribute('selected'); });
  });
}
function snapshot(){ const c=$('controls'); bakeValues(c);
  return {html:c.innerHTML, biomes:BIOMES.slice(), cx:cx, cz:cz,
          seed:$('seed').value, view:$('view').value, px:$('px').value, st:$('st').value}; }
function commitHistory(){            // fold the pending edit burst into one undo entry
  if(restoring||!pendingChange) return; pendingChange=false;
  if(lastSnapshot){ undoStack.push(lastSnapshot); if(undoStack.length>HISTORY_MAX) undoStack.shift(); }
  redoStack.length=0; lastSnapshot=snapshot(); updateHistoryUI();
}
function restoreState(s){
  restoring=true; clearTimeout(timer);
  swapControls(s.html);              // re-inits splines + applyFilter
  BIOMES.length=0; s.biomes.forEach(n=>BIOMES.push(n)); fillBiomes();
  $('seed').value=s.seed; $('view').value=s.view; $('px').value=s.px; $('st').value=s.st;
  cx=s.cx; cz=s.cz; setCtr(); syncSeg(); renderLegend();
  restoring=false; setDirty(true); updateHistoryUI(); regen();
}
function undo(){ if(pendingChange) commitHistory(); if(!undoStack.length) return;
  redoStack.push(lastSnapshot); lastSnapshot=undoStack.pop(); restoreState(lastSnapshot);
  $('status').textContent='↶ undo · '+undoStack.length+' more'; }
function redo(){ if(!redoStack.length) return;
  undoStack.push(lastSnapshot); lastSnapshot=redoStack.pop(); restoreState(lastSnapshot);
  $('status').textContent='↷ redo · '+redoStack.length+' more'; }
function updateHistoryUI(){ const u=$('undoBtn'), r=$('redoBtn');
  if(u) u.disabled=!undoStack.length; if(r) r.disabled=!redoStack.length; }
function schedule(){ setDirty(true); redrawSplines(); if(!restoring) pendingChange=true;
  clearTimeout(timer); timer=setTimeout(()=>{ commitHistory(); regen(); },180); }
function onSlide(el){ schedule(); }
function onRange(el){ const n=document.querySelector('[data-id="'+el.dataset.for+'"]'); if(n) n.value=el.value; maskGainSync(el); schedule(); }
function onNum(el){ const r=document.querySelector('.rng[data-for="'+el.dataset.id+'"]'); if(r) r.value=el.value; maskGainSync(el); schedule(); }
// Per-biome 3D-amp control: first touch attaches the data-id so it starts submitting
// (creating the override); mirrors the slider and number either way.
function ampOn(el){
  const cid=el.dataset.ampcid||el.dataset.for;
  const num=document.querySelector('[data-ampcid="'+cid+'"]');
  const rng=document.querySelector('.rng[data-for="'+cid+'"]');
  if(num && !num.dataset.id) num.dataset.id=cid;     // becomes a submitted override
  if(el===rng && num) num.value=el.value; else if(rng && num) rng.value=num.value;
  schedule();
}

function toggleSec(h){ h.parentElement.classList.toggle('collapsed'); }
function applyFilter(){
  const q=$('filter').value.trim().toLowerCase();
  document.querySelectorAll('#side .sec').forEach(sec=>{
    let any=false;
    sec.querySelectorAll('.r,.r2,.r3,.r4').forEach(row=>{
      const lab=(row.querySelector('label')||{}).textContent||'';
      const show=!q||lab.toLowerCase().includes(q); row.style.display=show?'':'none'; any=any||show; });
    // Biome cards: also match on the biome NAME (reveals its whole card); hide & collapse the rest.
    sec.querySelectorAll('.biomecard').forEach(card=>{
      const bn=((card.querySelector('.bname')||{}).textContent||'').toLowerCase();
      let cany=false;
      if(q && bn.includes(q)){ cany=true; card.querySelectorAll('.r,.r2,.r3,.r4').forEach(r=>r.style.display=''); }
      else card.querySelectorAll('.r,.r2,.r3,.r4').forEach(r=>{ if(r.style.display!=='none') cany=true; });
      card.style.display=(q&&!cany)?'none':''; if(q&&cany){ card.classList.remove('collapsed'); any=true; }
    });
    sec.style.display=(q&&!any)?'none':''; if(q&&any) sec.classList.remove('collapsed');
  });
}

function formData(){
  const fd=new FormData();
  document.querySelectorAll('#side [data-id]').forEach(s=>fd.append(s.dataset.id,s.value));
  fd.append('__pixels__',$('px').value); fd.append('__step__',$('st').value);
  fd.append('__view__',$('view').value); fd.append('__seed__',$('seed').value);
  fd.append('__cx__',cx); fd.append('__cz__',cz); fd.append('__solo__',soloBiome);
  return fd;
}
// One screen of world: 2D map spans size*blk/px blocks; the 3D view spans `size` blocks.
function footprint(){ return is3D()? +$('px').value : +$('px').value*+$('st').value; }
function setCtr(){ $('ctr').textContent=cx+', '+cz; }
function pan(dx,dz){ const f=footprint(); cx+=dx*f; cz+=dz*f; setCtr(); regen(); }
function zoom(d){ let v=Math.max(1,Math.min(32,+$('st').value+d)); if(v!=+$('st').value){ $('st').value=v; regen(); } }

function regen(){
  t3dShow(is3D());
  $('cross').style.display=is3D()?'none':'block';
  renderLegend();
  $('status').textContent='generating…';
  fetch('/regen',{method:'POST',body:formData()}).then(async r=>{
    let d; try{ d=await r.json(); }catch(e){ d={ok:false,err:'bad response'}; }
    if(r.ok&&d.ok){ lastGen=Date.now();
      if(is3D()) t3dBuild('/voxels.bin?'+lastGen); else $('map').src='/map.png?'+lastGen;
      $('status').textContent='✓ '+d.ms+' ms'; $('gi').textContent=d.info||'';
      const wel=$('warn'); if(wel){ wel.textContent=d.warn?('⚠ '+d.warn):''; wel.style.display=d.warn?'':'none'; } }
    else { $('status').textContent='⚠ '+((d&&d.err)||r.status); }
  });
}
function doSave(){ $('status').textContent='saving…';
  fetch('/save',{method:'POST',body:formData()}).then(async r=>{ const d=await r.json().catch(()=>({}));
    if(r.ok){ setDirty(false); $('status').textContent='saved → biomes.yaml + world.yaml'; }
    else $('status').textContent='⚠ save failed'; }); }

// World/Biome quick toggle mirrors the view dropdown.
function syncSeg(){ const v=$('view').value;
  $('vwWorld').classList.toggle('on', v==='top'); $('vwBiome').classList.toggle('on', v==='biomes'); }
function setView(v){ $('view').value=v; syncSeg(); regen(); }
// Block-palette add/remove (per-biome top/filler), mirrors the spline add/del pattern.
function palIdx(bi,field){ const a=[];
  document.querySelectorAll('.palette[data-bi="'+bi+'"][data-field="'+field+'"] [data-id]').forEach(el=>{
    const m=el.dataset.id.match(/\\.(\\d+)\\.name$/); if(m) a.push(+m[1]); }); return a; }
function palAdd(bi,field){
  const c=document.querySelector('.palette[data-bi="'+bi+'"][data-field="'+field+'"]'); if(!c)return;
  const idx=palIdx(bi,field), nj=idx.length?Math.max.apply(null,idx)+1:0, def=field==='top'?'grass':'dirt';
  const opts=SURFACE.concat(SURFACE.indexOf(def)<0?[def]:[]).map(o=>'<option value="'+o+'"'+(o===def?' selected':'')+'>'+o+'</option>').join('');
  const row=document.createElement('div'); row.className='r2 palent'; row.dataset.bi=bi; row.dataset.field=field;
  row.innerHTML='<select data-id="biomes::biomes.'+bi+'.'+field+'.'+nj+'.name" onchange="onSlide(this)" class="sel">'+opts+'</select>'
    +'<input type="number" step="any" value="1" title="weight" data-id="biomes::biomes.'+bi+'.'+field+'.'+nj+'.w" class="nv" oninput="onNum(this)">'
    +'<button type="button" class="mini" onclick="palDel(this)" title="remove">✖</button>';
  c.appendChild(row); schedule();
}
function palDel(btn){ const row=btn.closest('.palent'), c=row.parentElement;
  if(c.querySelectorAll('.palent').length<=1) return; row.remove(); schedule(); }
// Per-biome 3D density layer add/remove (mirrors palAdd/palDel). Layers can go to zero
// (the biome then inherits the global density blend), so del has no minimum.
function dlayerAdd(bi){
  const c=document.querySelector('.dlayers[data-bi="'+bi+'"]'); if(!c)return;
  let nj=0; c.querySelectorAll('.dlayer input[data-id]').forEach(m=>{
    const k=+m.dataset.id.split('.layers.')[1].split('.')[0]; if(k>=nj) nj=k+1; });
  const base='biomes::biomes.'+bi+'.terrain3d.density.layers.'+nj;
  const tys=['perlin','ridged','billow','worley'].map(t=>'<option>'+t+'</option>').join('');
  const row=document.createElement('div'); row.className='r4 dlayer'; row.dataset.bi=bi;
  row.innerHTML='<span>f<input type="number" step="any" value="0.01" data-id="'+base+'.frequency" oninput="onSlide(this)"></span>'
    +'<span>w<input type="number" step="0.02" value="1" data-id="'+base+'.weight" oninput="onSlide(this)"></span>'
    +'<span>o<input type="number" step="1" value="3" data-id="'+base+'.octaves" oninput="onSlide(this)"></span>'
    +'<span>ty<select data-id="'+base+'.type" onchange="onSlide(this)">'+tys+'</select></span>'
    +'<button type="button" class="mini" onclick="dlayerDel(this)" title="remove">✖</button>';
  c.appendChild(row); schedule();
}
function dlayerDel(btn){ const row=btn.closest('.dlayer'); if(row){ row.remove(); schedule(); } }
// ---- Surface-mask editor (noise patches) + bezier steepness curve ----------------
const FALLOFFS=__FALLOFFS__;
function mrow(label,cid,lo,hi,step,val){
  return '<div class="r"><label>'+label+'</label>'
    +'<input type="range" min="'+lo+'" max="'+hi+'" step="'+step+'" value="'+val+'" class="rng" data-for="'+cid+'" oninput="onRange(this)">'
    +'<input type="number" step="any" value="'+val+'" data-id="'+cid+'" class="nv" oninput="onNum(this)"></div>';
}
function mlayerHTML(base){ const tys=['perlin','ridged','billow','worley'].map(t=>'<option>'+t+'</option>').join('');
  return '<span>f<input type="number" step="any" value="0.03" data-id="'+base+'.frequency" oninput="onSlide(this)"></span>'
    +'<span>w<input type="number" step="0.02" value="1" data-id="'+base+'.weight" oninput="onSlide(this)"></span>'
    +'<span>o<input type="number" step="1" value="3" data-id="'+base+'.octaves" oninput="onSlide(this)"></span>'
    +'<span>ty<select data-id="'+base+'.type" onchange="onSlide(this)">'+tys+'</select></span>'
    +'<button type="button" class="mini" onclick="mlayerDel(this)" title="remove">✖</button>'; }
function maskAdd(bi){
  const c=document.querySelector('.surfmasks[data-bi="'+bi+'"]'); if(!c)return;
  let mi=0; c.querySelectorAll('.maskblk').forEach(m=>{ const k=+m.dataset.mi; if(k>=mi)mi=k+1; });
  const base='biomes::biomes.'+bi+'.surface_masks.'+mi, ckey='m'+bi+'_'+mi;
  const bopts=SURFACE.concat(SURFACE.indexOf('stone')<0?['stone']:[]).map(o=>'<option value="'+o+'"'+(o==='stone'?' selected':'')+'>'+o+'</option>').join('');
  const fopts=FALLOFFS.map(o=>'<option value="'+o+'"'+(o==='smoothstep'?' selected':'')+'>'+o+'</option>').join('');
  const div=document.createElement('div'); div.className='maskblk'; div.dataset.bi=bi; div.dataset.mi=mi;
  div.innerHTML=
    '<div class="r2"><label>block</label><select data-id="'+base+'.block" onchange="onSlide(this)" class="sel">'+bopts+'</select>'
    +'<button type="button" class="mini" onclick="maskDel(this)" title="remove this mask">✖</button></div>'
    +mrow('threshold',base+'.threshold',-1,1,0.02,0)+mrow('width',base+'.width',0.01,1,0.01,0.5)
    +'<div class="r2"><label>falloff</label><select data-id="'+base+'.falloff" onchange="onFalloff(this)" class="sel">'+fopts+'</select>'
    +'<select data-id="'+base+'.invert" onchange="onSlide(this)" class="sel"><option value="0">normal</option><option value="1">invert</option></select></div>'
    +mrow('gain',base+'.gain',0.02,0.98,0.02,0.5)
    +'<div class="palrow"><span class="dim">noise layers (appendable)</span><button type="button" class="mini" onclick="mlayerAdd('+bi+','+mi+')">＋ layer</button></div>'
    +'<div class="mlayers" data-bi="'+bi+'" data-mi="'+mi+'"><div class="r4 mlayer" data-bi="'+bi+'" data-mi="'+mi+'">'+mlayerHTML(base+'.layers.0')+'</div></div>'
    +'<div class="curvewrap" data-key="'+ckey+'">'
      +'<svg class="curve" data-key="'+ckey+'" data-xmin="0" data-xmax="1" data-ymin="0" data-ymax="1" data-falloff="smoothstep" data-gain="0.5" viewBox="0 0 240 130" preserveAspectRatio="none">'
      +'<rect class="frame" x="26" y="8" width="206" height="106"></rect><polyline class="ccurve"></polyline><g class="cpts"></g></svg>'
      +'<div class="splinebar"><button type="button" class="cadd" onclick="curveAdd(\\''+ckey+'\\')">＋ point</button><span class="dim">steepness curve preview · bezier: drag · right-click to delete</span></div>'
      +'<div class="curvepts" data-key="'+ckey+'">'
        +'<div class="r2 curvept" data-key="'+ckey+'"><input type="number" step="any" value="0" data-id="'+base+'.bezier.0.0" oninput="onNum(this)"><input type="number" step="any" value="0" data-id="'+base+'.bezier.0.1" oninput="onNum(this)"></div>'
        +'<div class="r2 curvept" data-key="'+ckey+'"><input type="number" step="any" value="1" data-id="'+base+'.bezier.1.0" oninput="onNum(this)"><input type="number" step="any" value="1" data-id="'+base+'.bezier.1.1" oninput="onNum(this)"></div>'
      +'</div></div>';
  c.appendChild(div); initCurve(div.querySelector('svg.curve')); schedule();
}
function maskDel(btn){ const m=btn.closest('.maskblk'); if(m){ m.remove(); schedule(); } }
function mlayerAdd(bi,mi){
  const c=document.querySelector('.mlayers[data-bi="'+bi+'"][data-mi="'+mi+'"]'); if(!c)return;
  let nj=0; c.querySelectorAll('.mlayer input[data-id]').forEach(m=>{ const k=+m.dataset.id.split('.layers.')[1].split('.')[0]; if(k>=nj)nj=k+1; });
  const row=document.createElement('div'); row.className='r4 mlayer'; row.dataset.bi=bi; row.dataset.mi=mi;
  row.innerHTML=mlayerHTML('biomes::biomes.'+bi+'.surface_masks.'+mi+'.layers.'+nj);
  c.appendChild(row); schedule();
}
function mlayerDel(btn){ const r=btn.closest('.mlayer'); if(r){ r.remove(); schedule(); } }
// World-mask (caves.mask / ores.iron.mask) noise layer add — single mask, no mi.
function wlayerAdd(file,wbase){
  const c=document.querySelector('.wlayers[data-wbase="'+wbase+'"]'); if(!c)return;
  let nj=0; c.querySelectorAll('.mlayer input[data-id]').forEach(m=>{ const k=+m.dataset.id.split('.layers.')[1].split('.')[0]; if(k>=nj)nj=k+1; });
  const row=document.createElement('div'); row.className='r4 mlayer';
  row.innerHTML=mlayerHTML(file+'::'+wbase+'.layers.'+nj);
  c.appendChild(row); schedule();
}
function onFalloff(sel){ const m=sel.closest('.maskblk, .wmask');
  if(m){ const svg=m.querySelector('svg.curve'); if(svg){ svg.dataset.falloff=sel.value; if(svg._draw) svg._draw(); } } schedule(); }
// Redraw the steepness preview live when the Gain slider moves (gain shapes that curve).
function maskGainSync(el){ const cid=el.dataset.id||el.dataset.for||''; if(!cid.endsWith('.gain')) return;
  const m=el.closest('.maskblk, .wmask'); if(!m) return; const svg=m.querySelector('svg.curve');
  if(svg){ svg.dataset.gain=el.value; if(svg._draw) svg._draw(); } }
// Bezier steepness-curve editor (its own draggable widget; mirrors the spline editor but
// over the [0,1]×[0,1] falloff domain and a Catmull-Rom smooth render to match the engine).
function cRows(key){ return Array.prototype.slice.call(document.querySelectorAll('.curvepts[data-key="'+key+'"] .curvept')); }
function cPoints(key){ return cRows(key).map(r=>{ const ins=r.querySelectorAll('input');
    const m=ins[0].dataset.id.match(/\\.bezier\\.(\\d+)\\.0$/); return {i:m?+m[1]:0, x:+ins[0].value, y:+ins[1].value}; })
    .sort((a,b)=>a.x-b.x); }
function cInp(key,i,c){ return document.querySelector('.curvepts[data-key="'+key+'"] [data-id$=".bezier.'+i+'.'+c+'"]'); }
function curveAdd(key){
  const c=document.querySelector('.curvepts[data-key="'+key+'"]'); if(!c)return;
  const ps=cPoints(key), idx=cRows(key).map(r=>{ const m=r.querySelector('input').dataset.id.match(/\\.bezier\\.(\\d+)\\.0$/); return m?+m[1]:0; });
  const ni=idx.length?Math.max.apply(null,idx)+1:0;
  let x=0.5,y=0.5; if(ps.length>=2){ let gi=0,gw=-1; for(let k=0;k<ps.length-1;k++){ const w=ps[k+1].x-ps[k].x; if(w>gw){gw=w;gi=k;} }
    x=(ps[gi].x+ps[gi+1].x)/2; y=(ps[gi].y+ps[gi+1].y)/2; }
  x=Math.round(x*100)/100; y=Math.round(y*100)/100;
  const base=c.querySelector('input').dataset.id.replace(/\\.bezier\\.\\d+\\.0$/,'.bezier.'+ni);
  const row=document.createElement('div'); row.className='r2 curvept'; row.dataset.key=key;
  row.innerHTML='<input type="number" step="any" value="'+x+'" data-id="'+base+'.0" oninput="onNum(this)"><input type="number" step="any" value="'+y+'" data-id="'+base+'.1" oninput="onNum(this)">';
  c.appendChild(row); const svg=document.querySelector('svg.curve[data-key="'+key+'"]'); if(svg&&svg._draw)svg._draw(); schedule();
}
// Analytic steepness falloff — MUST match applyFalloff() in src/world/NoiseMask.cpp.
function falloffY(fo,t,g){ t=Math.min(1,Math.max(0,t));
  if(fo==='step') return t<0.5?0:1;
  if(fo==='linear') return t;
  if(fo==='smoothstep') return t*t*(3-2*t);
  if(fo==='smootherstep') return t*t*t*(t*(t*6-15)+10);
  if(fo==='gain'){ g=Math.min(0.999,Math.max(0.001,g)); const e=Math.log(1-g)/Math.log(0.5);
    return t<0.5? 0.5*Math.pow(Math.max(0,2*t),e) : 1-0.5*Math.pow(Math.max(0,2-2*t),e); }
  return t; }
function initCurve(svg){
  if(!svg||svg._init)return; svg._init=true;
  const NS='http://www.w3.org/2000/svg', key=svg.dataset.key;
  const W=240,H=130,L=26,Rt=8,T=8,B=16, pw=W-L-Rt, ph=H-T-B;
  const tx=x=>L+x*pw, ty=y=>T+(1-y)*ph, ix=p=>(p-L)/pw, iy=p=>1-(p-T)/ph;
  const curve=svg.querySelector('.ccurve'), pg=svg.querySelector('.cpts');
  function cr(p0,p1,p2,p3,t){ const t2=t*t,t3=t2*t;
    return 0.5*((2*p1)+(-p0+p2)*t+(2*p0-5*p1+4*p2-p3)*t2+(-p0+3*p1-3*p2+p3)*t3); }
  function draw(){ while(pg.firstChild)pg.removeChild(pg.firstChild);
    const fo=svg.dataset.falloff||'bezier';
    const cadd=svg.parentElement.querySelector('.cadd'); if(cadd) cadd.style.display=(fo==='bezier')?'':'none';
    if(fo!=='bezier'){            // read-only analytic preview of the chosen steepness
      const g=+svg.dataset.gain||0.5; let s='';
      for(let st=0;st<=48;st++){ const t=st/48; s+=tx(t)+','+ty(falloffY(fo,t,g))+' '; }
      curve.setAttribute('points',s.trim()); return; }
    const ps=cPoints(key); if(!ps.length){ curve.setAttribute('points',''); return; }
    let s='';
    for(let k=0;k<ps.length-1;k++){ const p0=ps[k>0?k-1:k],p1=ps[k],p2=ps[k+1],p3=ps[k+2<ps.length?k+2:k+1];
      for(let st=0;st<=8;st++){ const t=st/8; const yx=Math.min(1,Math.max(0,cr(p0.y,p1.y,p2.y,p3.y,t)));
        const xx=p1.x+(p2.x-p1.x)*t; s+=tx(xx)+','+ty(yx)+' '; } }
    curve.setAttribute('points',s.trim());
    ps.forEach(p=>{ const c=document.createElementNS(NS,'circle'); c.setAttribute('class','cpt');
      c.setAttribute('cx',tx(p.x)); c.setAttribute('cy',ty(p.y)); c.setAttribute('r',5); c.dataset.i=p.i; pg.appendChild(c); }); }
  svg._draw=draw;
  let active=null;
  const at=e=>{ const r=svg.getBoundingClientRect(); return [ix((e.clientX-r.left)/r.width*W), iy((e.clientY-r.top)/r.height*H)]; };
  svg.addEventListener('mousedown',e=>{ if(e.target.classList.contains('cpt')){ active=+e.target.dataset.i; e.preventDefault(); } });
  svg.addEventListener('contextmenu',e=>{ if(e.target.classList.contains('cpt')){ e.preventDefault();
    if(cRows(key).length<=2)return; const el=cInp(key,+e.target.dataset.i,0); if(el){ el.closest('.curvept').remove(); draw(); schedule(); } } });
  window.addEventListener('mousemove',e=>{ if(active===null)return; let [x,y]=at(e);
    const ps=cPoints(key), pos=ps.findIndex(p=>p.i===active);
    const lo=pos>0?ps[pos-1].x+0.01:0, hi=pos<ps.length-1?ps[pos+1].x-0.01:1;
    x=Math.min(hi,Math.max(lo,x)); y=Math.min(1,Math.max(0,y));
    cInp(key,active,0).value=Math.round(x*100)/100; cInp(key,active,1).value=Math.round(y*100)/100; draw(); schedule(); });
  window.addEventListener('mouseup',()=>{ active=null; });
  draw();
}
$('px').onchange=regen; $('st').onchange=regen; $('view').onchange=()=>{ syncSeg(); regen(); }; $('seed').onchange=regen;
$('dice').onclick=()=>{ $('seed').value=Math.floor(Math.random()*2147483647); regen(); };
document.querySelectorAll('[data-pan]').forEach(b=>b.onclick=()=>{ const[dx,dz]=b.dataset.pan.split(',').map(Number); pan(dx,dz); });
$('home').onclick=()=>{ cx=0; cz=0; setCtr(); regen(); };
$('save').onclick=doSave;
$('undoBtn').onclick=undo; $('redoBtn').onclick=redo;
$('bjump').onclick=jumpBiome;
// Solo: regenerate the 3D view as ONLY the selected biome (engine --only-biome). Click
// again (or pick another biome and click) to change; toggles off if the same one is on.
$('bsolo').onclick=()=>{ const i=+$('bsel').value;
  soloBiome=(soloBiome===i)?-1:i;
  $('bsolo').classList.toggle('on', soloBiome>=0);
  $('bsolo').textContent=(soloBiome>=0)?('🔬 Solo: '+(BIOMES[soloBiome]||soloBiome)+' (click to exit)'):'🔬 Solo this biome (3D)';
  if(soloBiome>=0 && !is3D()){ $('view').value='3d'; syncSeg(); }
  regen();
};
$('restore').onclick=()=>{ if(dirty&&!confirm('Discard unsaved changes and reload from source?'))return;
  fetch('/restore',{method:'POST'}).then(()=>location.reload()); };

// Drag the 2D map to pan (mouse-grab), wheel to zoom blk/px.
const main=$('main');
main.addEventListener('mousedown',e=>{ if(is3D())return; drag={x:e.clientX,y:e.clientY}; main.style.cursor='grabbing'; });
window.addEventListener('mouseup',e=>{ if(!drag)return; const m=$('map');
  const f=footprint(), w=m.clientWidth||1, h=m.clientHeight||1;
  const ddx=Math.round(-(e.clientX-drag.x)/w*f), ddz=Math.round(-(e.clientY-drag.y)/h*f);
  drag=null; main.style.cursor='grab'; if(ddx||ddz){ cx+=ddx; cz+=ddz; setCtr(); regen(); } });
main.addEventListener('wheel',e=>{ if(is3D())return; e.preventDefault(); zoom(e.deltaY>0?1:-1); },{passive:false});
// Keyboard shortcuts. Undo/redo are handled FIRST so they fire even while a control is
// focused (Ctrl+Z / Ctrl+Shift+Z, plus Ctrl+Y for redo); the rest are ignored mid-typing.
window.addEventListener('keydown',e=>{
  const ctrl=e.ctrlKey||e.metaKey;
  if(ctrl&&(e.key==='z'||e.key==='Z')){ e.preventDefault(); if(e.shiftKey) redo(); else undo(); return; }
  if(ctrl&&(e.key==='y'||e.key==='Y')){ e.preventDefault(); redo(); return; }
  const t=e.target.tagName; if(t==='INPUT'||t==='SELECT'||t==='TEXTAREA')return;
  const m={ArrowUp:[0,-1],ArrowDown:[0,1],ArrowLeft:[-1,0],ArrowRight:[1,0]};
  if(m[e.key]){ e.preventDefault(); pan(...m[e.key]); }
  else if(e.key==='+'||e.key==='='){ zoom(-1); } else if(e.key==='-'){ zoom(1); }
  else if(e.key==='r'||e.key==='R'){ regen(); } else if(e.key==='s'||e.key==='S'){ e.preventDefault(); doSave(); }
});
// --- Interactive spline editor ---------------------------------------------------
// Drag a point to reshape; ＋ adds a point at the widest gap; right-click a point
// deletes it. The number rows below mirror the curve (two-way). Points are addressed
// by their data-id index (which may have gaps after deletes), and always drawn /
// submitted in x-sorted order — the only ordering the engine's Spline::at needs.
function sInp(key,i,c){ return document.querySelector('[data-id="biomes::'+key+'.'+i+'.'+c+'"]'); }
function sIdx(key){ const a=[];
  document.querySelectorAll('.splinepts[data-key="'+key+'"] [data-id]').forEach(el=>{
    const m=el.dataset.id.match(/\\.(\\d+)\\.0$/); if(m) a.push(+m[1]); }); return a; }
function sPoints(key){ return sIdx(key).map(i=>({i,x:+sInp(key,i,0).value,y:+sInp(key,i,1).value}))
  .sort((a,b)=>a.x-b.x); }
function mkNum(cid,val){ return '<input type="number" step="any" value="'+val+'" data-id="'+cid+'" class="nv" oninput="onNum(this)">'; }
function splineAdd(key){
  const c=document.querySelector('.splinepts[data-key="'+key+'"]'); if(!c)return;
  const ps=sPoints(key), idx=sIdx(key), ni=idx.length?Math.max.apply(null,idx)+1:0;
  let x=0,y=0;
  if(ps.length>=2){ let gi=0,gw=-1; for(let k=0;k<ps.length-1;k++){ const w=ps[k+1].x-ps[k].x; if(w>gw){gw=w;gi=k;} }
    x=(ps[gi].x+ps[gi+1].x)/2; y=Math.round((ps[gi].y+ps[gi+1].y)/2); }
  else if(ps.length===1){ x=Math.min(1,ps[0].x+0.2); y=ps[0].y; }
  x=Math.round(x*100)/100;
  const row=document.createElement('div'); row.className='r2';
  row.innerHTML='<label title="pt">pt</label>'+mkNum('biomes::'+key+'.'+ni+'.0',x)+mkNum('biomes::'+key+'.'+ni+'.1',y);
  c.appendChild(row); redrawSplines(); schedule();
}
function splineDel(key,i){
  if(sIdx(key).length<=2)return;           // keep at least two control points
  const el=sInp(key,i,0); if(el){ el.closest('.r2').remove(); redrawSplines(); schedule(); }
}
function initSpline(svg){
  const NS='http://www.w3.org/2000/svg', key=svg.dataset.key;
  const W=240,H=130,L=26,Rt=8,T=8,B=16, pw=W-L-Rt, ph=H-T-B;
  const xmin=+svg.dataset.xmin,xmax=+svg.dataset.xmax,ymin=+svg.dataset.ymin,ymax=+svg.dataset.ymax;
  const tx=x=>L+(x-xmin)/(xmax-xmin)*pw, ty=y=>T+(1-(y-ymin)/(ymax-ymin))*ph;
  const ix=p=>xmin+(p-L)/pw*(xmax-xmin), iy=p=>ymin+(1-(p-T)/ph)*(ymax-ymin);
  const curve=svg.querySelector('.curve'), pg=svg.querySelector('.pts'), zero=svg.querySelector('.zero');
  function draw(){
    const ps=sPoints(key); let s='';
    while(pg.firstChild) pg.removeChild(pg.firstChild);
    ps.forEach(p=>{ const X=tx(p.x), Y=ty(p.y); s+=X+','+Y+' ';
      const c=document.createElementNS(NS,'circle'); c.setAttribute('class','pt');
      c.setAttribute('cx',X); c.setAttribute('cy',Y); c.setAttribute('r',5); c.dataset.i=p.i; pg.appendChild(c); });
    curve.setAttribute('points',s.trim());
    if(ymin<=0&&ymax>=0){ const zy=ty(0); zero.setAttribute('x1',L); zero.setAttribute('x2',W-Rt);
      zero.setAttribute('y1',zy); zero.setAttribute('y2',zy); }
  }
  let active=null;
  const at=e=>{ const r=svg.getBoundingClientRect();
    return [ix((e.clientX-r.left)/r.width*W), iy((e.clientY-r.top)/r.height*H)]; };
  svg.addEventListener('mousedown',e=>{ if(e.target.classList.contains('pt')){ active=+e.target.dataset.i; e.preventDefault(); } });
  svg.addEventListener('contextmenu',e=>{ if(e.target.classList.contains('pt')){ e.preventDefault(); splineDel(key,+e.target.dataset.i); } });
  window.addEventListener('mousemove',e=>{ if(active===null)return;
    let [x,y]=at(e); const ps=sPoints(key), pos=ps.findIndex(p=>p.i===active);
    const lo=pos>0?ps[pos-1].x+0.01:xmin, hi=pos<ps.length-1?ps[pos+1].x-0.01:xmax;
    x=Math.min(hi,Math.max(lo,x)); y=Math.min(ymax,Math.max(ymin,y));
    sInp(key,active,0).value=Math.round(x*100)/100; sInp(key,active,1).value=Math.round(y);
    draw(); schedule(); });
  window.addEventListener('mouseup',()=>{ active=null; });
  SPLINES.push(draw); draw();
}
document.querySelectorAll('svg.spline').forEach(initSpline);
document.querySelectorAll('svg.curve').forEach(initCurve);
fillBiomes(); syncSeg(); setCtr();
lastSnapshot=snapshot(); updateHistoryUI();   // history baseline = the freshly-loaded state
regen();
</script></body></html>"""


if __name__ == "__main__":
    print("worldgen tool -> http://127.0.0.1:5000   (exe: %s)" % exe_path())
    app.run(host="127.0.0.1", port=5000, debug=False)
