#!/usr/bin/env python3
"""Feature editor — author procedural scatter objects (assets/features/*.yaml).

A feature is a list of SHAPE OPS (box / sphere / ellipsoid / cylinder / cone /
column / line) whose sizes, heights and block choices can be RANDOMIZED, plus
noise/shell fills. The engine (src/world/Feature.{h,cpp}) stamps them into the
world seam-safely; this tool builds the YAML and previews the result as an
isometric voxel render of THREE rerolled instances, so you can see the variation.

The preview JS mirrors Feature::at() closely enough to author against; the C++
engine stays authoritative (noise + per-cell block picks use world coords there).

Run from the repo root (or via tools/hub.py):
    pip install flask pyyaml pillow
    python tools/feature_tool.py        # -> http://127.0.0.1:5004
"""
import io
import math
import os
import sys

try:
    from flask import Flask, jsonify, request, send_file
except ImportError:
    sys.exit("feature tool needs Flask:  pip install flask")
try:
    import yaml
except ImportError:
    sys.exit("feature tool needs PyYAML:  pip install pyyaml")
try:
    from PIL import Image
except ImportError:
    Image = None  # colours fall back to a name hash

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")
ICONS = os.path.join(ASSETS, "textures", "icons")
TEXTURES = os.path.join(ASSETS, "textures")
BLOCKS_FILE = os.path.join(ASSETS, "blocks.yaml")
BIOMES_FILE = os.path.join(ASSETS, "biomes.yaml")
FEATURES_DIR = os.path.join(ASSETS, "features")

PORT = 5004
app = Flask(__name__)


# -----------------------------------------------------------------------------
#  Asset reads
# -----------------------------------------------------------------------------
def _load_yaml(path, default):
    if not os.path.exists(path):
        return default
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f) or default


def _avg_color(name):
    """Average non-transparent colour of a block's icon/texture -> '#rrggbb'."""
    if Image is None:
        h = abs(hash(name))
        return "#%02x%02x%02x" % (120 + h % 90, 110 + (h >> 8) % 90, 100 + (h >> 16) % 90)
    for path in (os.path.join(ICONS, name + ".png"),
                 os.path.join(TEXTURES, name + ".block.png")):
        if os.path.exists(path):
            try:
                im = Image.open(path).convert("RGBA")
                im.thumbnail((16, 16))
                r = g = b = n = 0
                for px in im.getdata():
                    if px[3] < 40:
                        continue
                    r += px[0]; g += px[1]; b += px[2]; n += 1
                if n:
                    return "#%02x%02x%02x" % (r // n, g // n, b // n)
            except Exception:
                pass
    return "#8a7d6b"


def blocks_data():
    out = []
    for e in _load_yaml(BLOCKS_FILE, []):
        name = e.get("name")
        if not name or name == "air":
            continue
        if e.get("tool"):           # tools aren't placeable world blocks
            continue
        out.append({"name": name, "color": _avg_color(name)})
    return out


def biome_names():
    doc = _load_yaml(BIOMES_FILE, {})
    return [b.get("name") for b in (doc.get("biomes") or []) if b.get("name")]


def feature_list():
    if not os.path.isdir(FEATURES_DIR):
        return []
    return sorted(os.path.splitext(f)[0] for f in os.listdir(FEATURES_DIR)
                  if f.endswith((".yaml", ".yml")))


# -----------------------------------------------------------------------------
#  Server-side isometric render — mirrors the in-browser preview exactly (same
#  hashes / shape math as Feature::at and the page JS), so you can inspect a
#  feature as a PNG without a browser. Backs /preview/<name>.png and `--render`.
# -----------------------------------------------------------------------------
def _u32(x): return x & 0xFFFFFFFF
def _imul(a, b): return _u32(a * b)
def _mix(a, b): return _u32(a ^ _u32(_u32(b + 0x9e3779b9) + _u32(a << 6) + (a >> 2)))


def _h01(x):
    x = _u32(x); x ^= x >> 16; x = _imul(x, 0x7feb352d); x ^= x >> 15
    x = _imul(x, 0x846ca68b); x ^= x >> 16
    return (x & 0xFFFFFF) / 16777216.0


def _opsalt(i): return _imul(i + 1, 2654435761)


def _rf(v, default):
    if isinstance(v, dict):
        return (False, float(v.get("min", default)), float(v.get("max", default)))
    if v is None:
        return (True, float(default), float(default))
    return (True, float(v), float(v))


def _rat(rf, h):
    fixed, a, b = rf
    return a if fixed else a + (b - a) * _h01(h)


def _ri(rf, h): return int(round(_rat(rf, h)))


def _vnoise(x, y, z):
    s = math.sin(x * 12.9898 + y * 78.233 + z * 37.719) * 43758.5453
    return (s - math.floor(s)) * 2 - 1


def _pick(blocks, roll):
    tot = sum(max(0.0, float(b.get("w", 1))) for b in blocks) or 1.0
    t = roll * tot
    for b in blocks:
        t -= max(0.0, float(b.get("w", 1)))
        if t < 0:
            return b.get("name")
    return blocks[-1].get("name")


def eval_feature(feat, seed):
    size = feat.get("size", [9, 9, 9])
    sx, sy, sz = int(size[0]), int(size[1]), int(size[2])
    ax, az = sx // 2, sz // 2
    # Pick this instance's variant (ops = #0, variants[i] = #i+1) from the seed.
    vlist = [feat.get("ops") or []] + [v.get("ops") or [] for v in (feat.get("variants") or [])]
    if len(vlist) > 1:
        vi = int(_h01(_mix(seed, 0x7a91)) * len(vlist))
        ops_src = vlist[min(vi, len(vlist) - 1)]
    else:
        ops_src = vlist[0]
    nops = []
    for o in ops_src:
        blk = o.get("block", [{"name": "stone", "w": 1}])
        if isinstance(blk, str):
            blk = [{"name": blk, "w": 1}]
        blk = [({"name": b, "w": 1} if isinstance(b, str) else b) for b in blk]
        s3 = o.get("size", [3, 3, 3])
        nops.append({
            "shape": o.get("shape", "box"), "at": o.get("at", [0, 0, 0]),
            "radius": _rf(o.get("radius"), 0 if o.get("shape") == "column" else 2),
            "height": _rf(o.get("height"), 4),
            "size": [_rf(s3[0], 3), _rf(s3[1], 3), _rf(s3[2], 3)],
            "thickness": _rf(o.get("thickness"), 1.5),
            "turns": float(o.get("turns", 2)), "scatter": float(o.get("scatter", 0.5)),
            "vox": {(int(c[0]), int(c[1]), int(c[2])): (c[3] if len(c) >= 4 else None)
                    for c in o.get("cells", []) if isinstance(c, (list, tuple)) and len(c) >= 3},
            "blocks": blk, "pick": o.get("block_pick", "per_cell"),
            "fill": o.get("fill", "solid"),
            "noise": o.get("noise", {"freq": 0.3, "threshold": 0.0}),
        })
    vox = {}
    for lx in range(sx):
        for ly in range(sy):
            for lz in range(sz):
                win = None
                for oi, op in enumerate(nops):
                    salt = _opsalt(oi)
                    fx = lx - ax - op["at"][0]; fy = ly - op["at"][1]; fz = lz - az - op["at"][2]
                    r = max(0, _ri(op["radius"], _mix(seed, salt ^ 0x101)))
                    h = max(1, _ri(op["height"], _mix(seed, salt ^ 0x202)))
                    Sx = max(1, _ri(op["size"][0], _mix(seed, salt ^ 0x303)))
                    Sy = max(1, _ri(op["size"][1], _mix(seed, salt ^ 0x404)))
                    Sz = max(1, _ri(op["size"][2], _mix(seed, salt ^ 0x505)))
                    tube = max(0.5, _rat(op["thickness"], _mix(seed, salt ^ 0x606)))
                    inside = False; margin = 9999.0; rad = math.hypot(fx, fz); sh = op["shape"]; vb = None
                    if sh == "box":
                        hx = Sx / 2; hz = Sz / 2
                        inside = abs(fx) <= hx and abs(fz) <= hz and 0 <= fy <= Sy - 1
                        if inside:
                            margin = min(hx - abs(fx), hz - abs(fz), fy, Sy - 1 - fy)
                    elif sh == "sphere":
                        d = math.hypot(fx, fy - r, fz); inside = d <= r + 0.001; margin = r - d
                    elif sh == "ellipsoid":
                        rx = Sx / 2; ry = Sy / 2; rz = Sz / 2
                        e = math.hypot(fx / max(0.5, rx), (fy - ry) / max(0.5, ry), fz / max(0.5, rz))
                        inside = e <= 1; margin = (1 - e) * min(rx, ry, rz)
                    elif sh in ("cylinder", "column"):
                        inside = rad <= r + 0.001 and 0 <= fy <= h - 1
                        if inside:
                            margin = min(r - rad, fy, h - 1 - fy)
                    elif sh == "cone":
                        rr = r * (1 - fy / h); inside = 0 <= fy <= h - 1 and rad <= rr + 0.001
                        if inside:
                            margin = min(rr - rad, fy, h - 1 - fy)
                    elif sh == "line":
                        inside = fz == 0 and fy == 0 and 0 <= fx <= h - 1
                    elif sh == "torus":
                        q = math.hypot(fx, fz) - r; dy = fy - tube
                        d = math.hypot(q, dy); inside = d <= tube + 0.001; margin = tube - d
                    elif sh == "arch":
                        ring = math.hypot(fx, fy) - r
                        inside = fy >= -0.001 and abs(fz) <= tube + 0.001 and abs(ring) <= tube + 0.001
                        if inside:
                            margin = min(tube - abs(ring), tube - abs(fz))
                    elif sh == "spiral":
                        if 0 <= fy <= h - 1:
                            th = (fy / max(1, h)) * op["turns"] * 6.2831853
                            hxp = r * math.cos(th); hzp = r * math.sin(th)
                            d = math.hypot(fx - hxp, fz - hzp); inside = d <= tube + 0.001; margin = tube - d
                    elif sh == "voxels":
                        vk = (round(fx), round(fy), round(fz))
                        if vk in op["vox"]:
                            inside = True; vb = op["vox"][vk]
                    if not inside:
                        continue
                    if op["fill"] == "shell" and margin > 1.5:
                        continue
                    if op["fill"] == "noise":
                        nz = op["noise"]; f = nz.get("freq", 0.3)
                        if _vnoise((lx + seed * 7) * f, ly * f, (lz + seed * 3) * f) <= nz.get("threshold", 0.0):
                            continue
                    if op["fill"] == "scatter":
                        sv = _h01(_mix(_u32(_imul(lx + 1, 374761393) ^ _imul(lz + 1, 668265263)
                                           ^ _imul(ly + 1, 2246822519)), salt ^ 0x5CA7))
                        if sv >= op["scatter"]:
                            continue
                    if op["pick"] == "per_instance":
                        roll = _h01(_mix(seed, salt ^ 0xB10C))
                    else:
                        roll = _h01(_mix(_u32(_imul(lx + 1, 73856093) ^ _imul(lz + 1, 19349663)
                                             ^ _imul(ly + 1, 83492791)), salt))
                    if sh == "voxels" and vb:
                        win = vb
                    elif op["blocks"]:
                        win = _pick(op["blocks"], roll)
                if win:
                    vox[(lx, ly, lz)] = win
    return vox, sx, sy, sz


def _shade(hexc, f):
    n = int(hexc[1:], 16)
    return (min(255, int(((n >> 16) & 255) * f)), min(255, int(((n >> 8) & 255) * f)),
            min(255, int((n & 255) * f)))


def render_iso(feat, seed, w=260, h=240):
    if Image is None:
        return None
    from PIL import ImageDraw
    vox, sx, sy, sz = eval_feature(feat, seed)
    colors = {b["name"]: b["color"] for b in blocks_data()}
    im = Image.new("RGBA", (w, h), (11, 13, 18, 255))
    dr = ImageDraw.Draw(im)
    T = max(6, min(15, int(150 / max(sx + sz, sy, 1))))
    cx = w / 2; cy = 46
    for (x, y, z) in sorted(vox.keys(), key=lambda k: k[0] + k[2] + k[1]):
        col = colors.get(vox[(x, y, z)], "#8a7d6b")
        px = cx + (x - z) * T; py = cy + (x + z) * T * 0.5 - y * T
        dr.polygon([(px, py), (px + T, py + T * 0.5), (px, py + T), (px - T, py + T * 0.5)], fill=_shade(col, 1.18))
        dr.polygon([(px - T, py + T * 0.5), (px, py + T), (px, py + 2 * T), (px - T, py + T * 1.5)], fill=_shade(col, 0.72))
        dr.polygon([(px + T, py + T * 0.5), (px, py + T), (px, py + 2 * T), (px + T, py + T * 1.5)], fill=_shade(col, 0.92))
    return im


# -----------------------------------------------------------------------------
#  Routes
# -----------------------------------------------------------------------------
@app.route("/")
def index():
    return PAGE


@app.route("/api/blocks")
def api_blocks():
    return jsonify(blocks_data())


@app.route("/api/biomes")
def api_biomes():
    return jsonify(biome_names())


@app.route("/api/features")
def api_features():
    return jsonify(feature_list())


@app.route("/api/feature/<name>")
def api_feature(name):
    path = os.path.join(FEATURES_DIR, name + ".yaml")
    return jsonify(_load_yaml(path, {}))


@app.route("/preview/<name>.png")
def api_preview(name):
    seed = int(request.args.get("seed", "1"))
    feat = _load_yaml(os.path.join(FEATURES_DIR, name + ".yaml"), {})
    im = render_iso(feat, seed)
    if im is None:
        return send_file(io.BytesIO(_TRANSPARENT_PNG), mimetype="image/png")
    buf = io.BytesIO(); im.save(buf, "PNG"); buf.seek(0)
    return send_file(buf, mimetype="image/png")


@app.route("/api/save", methods=["POST"])
def api_save():
    data = request.get_json(force=True)
    name = (data.get("name") or "feature").strip().replace(" ", "_")
    name = "".join(c for c in name if c.isalnum() or c in "_-").lower() or "feature"
    data["name"] = name
    os.makedirs(FEATURES_DIR, exist_ok=True)
    path = os.path.join(FEATURES_DIR, name + ".yaml")
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Authored with tools/feature_tool.py\n")
        yaml.safe_dump(data, f, sort_keys=False, default_flow_style=False)
    return jsonify({"ok": True, "name": name, "path": os.path.relpath(path, ROOT)})


_TRANSPARENT_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4"
    "890000000d49444154789c6360000002000100ffff03000006000557bfabd4"
    "0000000049454e44ae426082")


@app.route("/vendor/<path:fn>")
def vendor(fn):  # locally-served three.js etc. -> the 3D editor works offline
    p = os.path.join(HERE, "vendor", os.path.basename(fn))
    if os.path.exists(p):
        return send_file(p, mimetype="application/javascript")
    return ("", 404)


@app.route("/icon/<name>.png")
def icon(name):
    for path in (os.path.join(ICONS, name + ".png"),
                 os.path.join(TEXTURES, name + ".block.png")):
        if os.path.exists(path):
            return send_file(path, mimetype="image/png")
    return send_file(io.BytesIO(_TRANSPARENT_PNG), mimetype="image/png")


# -----------------------------------------------------------------------------
#  Page  (data comes via fetch; no string injection needed)
# -----------------------------------------------------------------------------
PAGE = r"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Voxel Game — Features</title>
<style>
  :root{--bg:#14161c;--panel:#1c1f28;--panel2:#232732;--line:#2c3240;--text:#e7ebf2;--dim:#9aa3b5;--acc:#f0a35b}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--text);font:13.5px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}
  header{padding:13px 20px;border-bottom:1px solid var(--line);font-size:16px;font-weight:600;
         display:flex;align-items:center;gap:14px}
  header .sub{color:var(--dim);font-size:12px;font-weight:400}
  .layout{display:grid;grid-template-columns:minmax(420px,1fr) minmax(420px,560px);gap:0;height:calc(100vh - 49px)}
  .editor{overflow:auto;padding:16px 18px;border-right:1px solid var(--line)}
  .preview{padding:16px 18px;overflow:auto;background:#0f1116}
  h2{font-size:11px;text-transform:uppercase;letter-spacing:1.1px;color:var(--dim);font-weight:600;margin:18px 0 8px}
  label{display:block;color:var(--dim);font-size:11.5px;margin:6px 0 2px}
  input,select{background:var(--panel2);border:1px solid var(--line);color:var(--text);border-radius:6px;
               padding:5px 7px;font:inherit;width:100%}
  input[type=checkbox]{width:auto}
  .row{display:flex;gap:8px}.row>*{flex:1}
  .pill{display:inline-flex;align-items:center;gap:4px;background:var(--panel2);border:1px solid var(--line);
        border-radius:999px;padding:2px 6px 2px 4px;margin:2px;font-size:11.5px;cursor:pointer}
  .pill img{width:18px;height:18px;image-rendering:pixelated}
  .pill.sel{border-color:var(--acc);box-shadow:0 0 0 1px var(--acc)}
  .op{background:var(--panel);border:1px solid var(--line);border-radius:9px;padding:11px 12px;margin:9px 0}
  .op .ohead{display:flex;align-items:center;gap:8px;margin-bottom:6px}
  .op .ohead select{width:auto;flex:0 0 auto}
  .op .ohead .sp{flex:1}
  .btn{background:var(--acc);color:#161616;border:none;border-radius:7px;padding:7px 16px;font:inherit;
       font-weight:600;cursor:pointer}
  .btn:hover{filter:brightness(1.08)}
  .btn.ghost{background:var(--panel2);color:var(--text);border:1px solid var(--line)}
  .x{cursor:pointer;color:var(--dim);font-size:16px;padding:0 4px}.x:hover{color:#ff8a8a}
  .rf{display:flex;gap:4px;align-items:center}.rf input{width:100%}
  .rf .t{cursor:pointer;color:var(--dim);border:1px solid var(--line);border-radius:5px;padding:3px 6px;font-size:11px;background:var(--panel2)}
  .rf .t.on{color:var(--acc);border-color:var(--acc)}
  canvas{background:#0b0d12;border:1px solid var(--line);border-radius:8px;image-rendering:pixelated}
  .cans{display:flex;gap:10px;flex-wrap:wrap}
  .vtab{display:inline-block;background:var(--panel2);border:1px solid var(--line);border-radius:7px;padding:5px 10px;margin:2px;cursor:pointer;font-size:12.5px}
  .vtab.sel{border-color:var(--acc);color:var(--acc)}
  .vtab.add{color:var(--dim)}
  .gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(118px,1fr));gap:10px}
  .gcard{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:6px;text-align:center;cursor:pointer}
  .gcard:hover{border-color:var(--acc)}
  .gcard img{width:100%;height:96px;object-fit:contain;background:#0b0d12;border-radius:6px}
  .gcard .gn{margin-top:4px;font-size:11.5px;word-break:break-word}
  /* 3D editor overlay */
  #ed3d{position:fixed;inset:0;background:#0a0c11;z-index:50;display:none;flex-direction:column}
  #ed3d .top{display:flex;align-items:center;gap:12px;padding:9px 14px;border-bottom:1px solid var(--line);font-size:13px}
  #ed3d .sw{width:22px;height:22px;border-radius:4px;border:1px solid var(--line);image-rendering:pixelated;background-size:cover}
  #ed3d .body{flex:1;display:flex;min-height:0}
  #ed3d canvas{flex:1;display:block;cursor:crosshair}
  #ed-palwrap{width:164px;display:flex;flex-direction:column;border-right:1px solid var(--line);background:#0c0e13}
  #ed-search{margin:6px 6px 0;padding:4px 6px}
  #ed-pal{flex:1;overflow:auto;padding:6px;display:flex;flex-wrap:wrap;gap:3px;align-content:flex-start}
  #ed-pal .ip{width:30px;height:30px;border:1px solid var(--line);border-radius:5px;cursor:pointer;padding:2px;background:#15171d}
  #ed-pal .ip img{width:100%;height:100%;image-rendering:pixelated}
  #ed-pal .ip.sel{border-color:var(--acc);box-shadow:0 0 0 1px var(--acc)}
  #ed3d .hint{color:var(--dim);font-size:12px}
  #ed3d .erase{padding:4px 10px;border:1px solid var(--line);border-radius:6px;background:var(--panel2);cursor:pointer}
  #ed3d .erase.on{border-color:#ff8a8a;color:#ff8a8a}
  .palette{max-height:148px;overflow:auto;background:var(--panel2);border:1px solid var(--line);border-radius:7px;padding:5px}
  .blkpick{display:flex;flex-wrap:wrap;gap:3px;margin-top:4px}
  .wblk{display:inline-flex;align-items:center;gap:3px;background:var(--panel2);border:1px solid var(--line);
        border-radius:6px;padding:2px 4px;font-size:11px}
  .wblk img{width:16px;height:16px;image-rendering:pixelated}
  .wblk input{width:34px;padding:1px 3px}
  .status{color:var(--dim);font-size:12px;margin-left:10px}
  .bar{display:flex;align-items:center;gap:8px;margin:10px 0}
</style>
<script src="/vendor/three.min.js"></script></head>
<body>
<div id="ed3d">
  <div class="top">
    <b>3D sculpt</b>
    <span class="sw" id="ed-sw"></span><span id="ed-blkname" class="hint">—</span>
    <label class="hint">tool <select id="ed-tool" style="width:auto"><option value="voxel">voxel</option><option value="box">box (fill)</option><option value="walls">walls</option><option value="line">line</option></select></label>
    <label class="hint">size <input id="ed-sx" type="number" style="width:40px"><input id="ed-sy" type="number" style="width:40px"><input id="ed-sz" type="number" style="width:40px"></label>
    <span class="erase" id="ed-erase">erase: off</span>
    <span class="hint" id="ed-msg"></span>
    <span style="flex:1"></span>
    <button class="btn" id="ed-done">Done</button>
  </div>
  <div class="body">
    <div id="ed-palwrap"><input id="ed-search" placeholder="filter blocks…"><div id="ed-pal"></div></div>
    <canvas id="ed-cv"></canvas>
  </div>
</div>
<header>Features
  <span class="sub">procedural scatter objects · saved to assets/features/</span>
  <span style="flex:1"></span>
  <select id="load" style="width:auto"><option value="">— load existing —</option></select>
  <button class="btn" id="save">Save</button>
  <span class="status" id="status"></span>
</header>
<div class="layout">
  <div class="editor">
    <h2>Feature</h2>
    <div class="row">
      <div><label>name</label><input id="f-name" value="my_feature"></div>
      <div><label>size x,y,z (max bbox)</label>
        <div class="row"><input id="f-sx" type="number" value="9"><input id="f-sy" type="number" value="9"><input id="f-sz" type="number" value="9"></div>
      </div>
    </div>
    <h2>Scatter</h2>
    <div class="row">
      <div><label>density</label><input id="s-den" type="number" step="0.01" value="0.15"></div>
      <div><label>spacing</label><input id="s-spc" type="number" value="44"></div>
      <div><label>surface only</label><input id="s-surf" type="checkbox" checked></div>
    </div>
    <div class="row">
      <div><label>min elevation</label><input id="s-mine" type="number" value="-9999"></div>
      <div><label>max elevation</label><input id="s-maxe" type="number" value="9999"></div>
    </div>
    <div class="row">
      <div><label>distribution</label><select id="s-dist"><option value="grid">grid (even)</option><option value="noise">noise (clumps)</option></select></div>
      <div><label>clump size</label><input id="s-nfreq" type="number" step="0.005" value="0.02"></div>
      <div><label>clump thresh</label><input id="s-nthr" type="number" step="0.05" value="0.3"></div>
    </div>
    <div class="row">
      <div><label>min slope</label><input id="s-mins" type="number" value="0"></div>
      <div><label>max slope</label><input id="s-maxs" type="number" value="100000"></div>
      <div><label>on water</label><input id="s-onw" type="checkbox"></div>
      <div><label>near water</label><input id="s-nearw" type="number" value="0"></div>
    </div>
    <h2>Noise mask <span class="status">optional — clump scatter by a multi-layer noise + steepness curve</span></h2>
    <div id="smask"></div>
    <label>biomes (none = any)</label>
    <div id="biomes" class="palette" style="max-height:90px"></div>
    <h2>Variants <span class="status">each instance randomly picks one form</span></h2>
    <div id="variants"></div>
    <h2>Shape ops <span class="status">later op overrides earlier</span></h2>
    <div id="ops"></div>
    <button class="btn ghost" id="addop">+ add op</button>
  </div>
  <div class="preview">
    <div class="bar"><button class="btn" id="reroll">⟳ Reroll seeds</button>
      <span class="status">3 instances — watch the randomization</span></div>
    <div class="cans">
      <canvas id="c0" width="260" height="240"></canvas>
      <canvas id="c1" width="260" height="240"></canvas>
      <canvas id="c2" width="260" height="240"></canvas>
    </div>
    <h2>Saved features <span class="status">click a thumbnail to load &amp; inspect</span></h2>
    <div id="gallery" class="gallery"></div>
    <h2>Palette</h2>
    <div id="palette" class="palette"></div>
    <p class="status">Click a block above, then a "+blk" slot in an op to add it. Weights set the mix.</p>
  </div>
</div>
<script>
const $ = s => document.querySelector(s);
const ic = n => `/icon/${n}.png`;
let BLOCKS = [], COLORS = {}, BIOMES = [], SELBLK = null, SEEDS = [1,2,3];
const SHAPES = ["box","sphere","ellipsoid","cylinder","cone","column","line","torus","arch","spiral","voxels"];

// ---- model -----------------------------------------------------------------
function newOp(){return {shape:"sphere", at:[0,0,0],
  radius:{fixed:false,a:2,b:4}, height:{fixed:true,a:5,b:5},
  size:[{fixed:true,a:3,b:3},{fixed:true,a:3,b:3},{fixed:true,a:3,b:3}],
  thickness:{fixed:true,a:1.5,b:1.5}, turns:2, scatter:0.5,
  blocks:[{name:"stone",w:1}], pick:"per_cell", fill:"solid",
  noise:{freq:0.3,threshold:0.0}, place:"force"};}
let VARIANTS=[[newOp()]], CURV=0, OPS=VARIANTS[0];
function setVariant(i){CURV=Math.max(0,Math.min(VARIANTS.length-1,i));OPS=VARIANTS[CURV];renderVariantBar();rebuild();}
function variantFor(seed){const n=VARIANTS.length;if(n<=1)return VARIANTS[0];let i=Math.floor(h01(mix(seed,0x7a91))*n);if(i>=n)i=n-1;return VARIANTS[i];}
function renderVariantBar(){const c=$('#variants');if(!c)return;c.innerHTML='';
  VARIANTS.forEach((v,i)=>{const t=document.createElement('span');t.className='vtab'+(i===CURV?' sel':'');t.textContent='form '+(i+1);
    t.onclick=()=>setVariant(i);
    if(VARIANTS.length>1){const x=document.createElement('b');x.textContent=' ✕';x.onclick=e=>{e.stopPropagation();VARIANTS.splice(i,1);if(CURV>=VARIANTS.length)CURV=VARIANTS.length-1;OPS=VARIANTS[CURV];renderVariantBar();rebuild();};t.appendChild(x);}
    c.appendChild(t);});
  const add=document.createElement('span');add.className='vtab add';add.textContent='+ form';
  add.onclick=()=>{VARIANTS.push([newOp()]);setVariant(VARIANTS.length-1);};c.appendChild(add);}

// ---- seam-safe hashes (mirror Feature.cpp) ---------------------------------
const u32 = x => x>>>0;
function mix(a,b){a=u32(a^(u32(b+0x9e3779b9)+u32(a<<6)+(a>>>2)));return a;}
function h01(x){x=u32(x);x^=x>>>16;x=u32(Math.imul(x,0x7feb352d));x^=x>>>15;x=u32(Math.imul(x,0x846ca68b));x^=x>>>16;return (x&0xFFFFFF)/16777216;}
const opSalt = i => u32(Math.imul(i+1,2654435761));
function rAt(rf,h){return rf.fixed? rf.a : rf.a+(rf.b-rf.a)*h01(h);}
function rI(rf,h){return Math.round(rAt(rf,h));}
// cheap value noise for the preview (engine uses Perlin; preview is approximate)
function vnoise(x,y,z){let s=Math.sin(x*12.9898+y*78.233+z*37.719)*43758.5453;return (s-Math.floor(s))*2-1;}

function pick(blocks,roll){let tot=blocks.reduce((a,b)=>a+(+b.w||0),0)||1;let t=roll*tot;
  for(const b of blocks){t-=(+b.w||0);if(t<0)return b.name;}return blocks[blocks.length-1].name;}

// ---- evaluate a feature instance into voxels (mirror of Feature::at) --------
function evalFeature(seed){
  const sx=+$('#f-sx').value, sy=+$('#f-sy').value, sz=+$('#f-sz').value;
  const ax=Math.floor(sx/2), ay=0, az=Math.floor(sz/2);
  const vox={};
  for(let lx=0;lx<sx;lx++)for(let ly=0;ly<sy;ly++)for(let lz=0;lz<sz;lz++){
    let win=null;
    variantFor(seed).forEach((op,oi)=>{
      const salt=opSalt(oi);
      const fx=lx-ax-op.at[0], fy=ly-ay-op.at[1], fz=lz-az-op.at[2];
      const r=Math.max(0,rI(op.radius,mix(seed,salt^0x101)));
      const h=Math.max(1,rI(op.height,mix(seed,salt^0x202)));
      const Sx=Math.max(1,rI(op.size[0],mix(seed,salt^0x303)));
      const Sy=Math.max(1,rI(op.size[1],mix(seed,salt^0x404)));
      const Sz=Math.max(1,rI(op.size[2],mix(seed,salt^0x505)));
      const tube=Math.max(0.5,rAt(op.thickness||{fixed:true,a:1.5},mix(seed,salt^0x606)));
      let inside=false,margin=9999,vb=null;
      const rad=Math.hypot(fx,fz);
      switch(op.shape){
        case "box":{const hx=Sx/2,hz=Sz/2;inside=Math.abs(fx)<=hx&&Math.abs(fz)<=hz&&fy>=0&&fy<=Sy-1;
          if(inside)margin=Math.min(hx-Math.abs(fx),hz-Math.abs(fz),fy,Sy-1-fy);break;}
        case "sphere":{const d=Math.hypot(fx,fy-r,fz);inside=d<=r+0.001;margin=r-d;break;}
        case "ellipsoid":{const rx=Sx/2,ry=Sy/2,rz=Sz/2;
          const e=Math.hypot(fx/Math.max(0.5,rx),(fy-ry)/Math.max(0.5,ry),fz/Math.max(0.5,rz));
          inside=e<=1;margin=(1-e)*Math.min(rx,ry,rz);break;}
        case "cylinder":case "column":{inside=rad<=r+0.001&&fy>=0&&fy<=h-1;
          if(inside)margin=Math.min(r-rad,fy,h-1-fy);break;}
        case "cone":{const rr=r*(1-fy/h);inside=fy>=0&&fy<=h-1&&rad<=rr+0.001;
          if(inside)margin=Math.min(rr-rad,fy,h-1-fy);break;}
        case "line":{inside=fz===0&&fy===0&&fx>=0&&fx<=h-1;break;}
        case "torus":{const q=Math.hypot(fx,fz)-r;const dy=fy-tube;const d=Math.hypot(q,dy);inside=d<=tube+0.001;margin=tube-d;break;}
        case "arch":{const ring=Math.hypot(fx,fy)-r;inside=fy>=-0.001&&Math.abs(fz)<=tube+0.001&&Math.abs(ring)<=tube+0.001;if(inside)margin=Math.min(tube-Math.abs(ring),tube-Math.abs(fz));break;}
        case "spiral":{if(fy<0||fy>h-1)break;const th=(fy/Math.max(1,h))*(op.turns||2)*6.2831853;const hxp=r*Math.cos(th),hzp=r*Math.sin(th);const d=Math.hypot(fx-hxp,fz-hzp);inside=d<=tube+0.001;margin=tube-d;break;}
        case "voxels":{const k=Math.round(fx)+','+Math.round(fy)+','+Math.round(fz);if(op.vox&&op.vox[k]){inside=true;vb=op.vox[k];}break;}
      }
      if(!inside)return;
      if(op.fill==="shell"&&margin>1.5)return;
      if(op.fill==="noise"&&vnoise((lx+seed*7)*op.noise.freq,ly*op.noise.freq,(lz+seed*3)*op.noise.freq)<=op.noise.threshold)return;
      if(op.fill==="scatter"&&h01(mix(u32(Math.imul(lx+1,374761393)^Math.imul(lz+1,668265263)^Math.imul(ly+1,2246822519)),salt^0x5CA7))>=(op.scatter==null?0.5:op.scatter))return;
      const roll = op.pick==="per_instance" ? h01(mix(seed,salt^0xB10C))
        : h01(mix(u32(Math.imul(lx+1,73856093)^Math.imul(lz+1,19349663)^Math.imul(ly+1,83492791)),salt));
      if(op.shape==="voxels"){ if(vb&&vb!=="_pal")win=vb; else if(op.blocks.length)win=pick(op.blocks,roll); }
      else if(op.blocks.length) win=pick(op.blocks,roll);
    });
    if(win) vox[lx+','+ly+','+lz]=win;
  }
  return {vox,sx,sy,sz};
}

// ---- isometric render ------------------------------------------------------
function shade(hex,f){const n=parseInt(hex.slice(1),16);let r=(n>>16)&255,g=(n>>8)&255,b=n&255;
  return `rgb(${Math.min(255,r*f|0)},${Math.min(255,g*f|0)},${Math.min(255,b*f|0)})`;}
function drawIso(cv,inst){
  const ctx=cv.getContext('2d');ctx.clearRect(0,0,cv.width,cv.height);
  const T=Math.max(6,Math.min(15, 150/Math.max(inst.sx+inst.sz,inst.sy)|0));
  const cx=cv.width/2, cy=46;
  const keys=Object.keys(inst.vox).map(k=>k.split(',').map(Number));
  keys.sort((a,b)=>(a[0]+a[2]+a[1])-(b[0]+b[2]+b[1]));
  for(const [x,y,z] of keys){
    const col=COLORS[inst.vox[x+','+y+','+z]]||"#8a7d6b";
    const sx=cx+(x-z)*T, sy=cy+(x+z)*T*0.5-y*T;
    // top
    ctx.fillStyle=shade(col,1.18);ctx.beginPath();
    ctx.moveTo(sx,sy);ctx.lineTo(sx+T,sy+T*0.5);ctx.lineTo(sx,sy+T);ctx.lineTo(sx-T,sy+T*0.5);ctx.closePath();ctx.fill();
    // left
    ctx.fillStyle=shade(col,0.72);ctx.beginPath();
    ctx.moveTo(sx-T,sy+T*0.5);ctx.lineTo(sx,sy+T);ctx.lineTo(sx,sy+T+T);ctx.lineTo(sx-T,sy+T*0.5+T);ctx.closePath();ctx.fill();
    // right
    ctx.fillStyle=shade(col,0.92);ctx.beginPath();
    ctx.moveTo(sx+T,sy+T*0.5);ctx.lineTo(sx,sy+T);ctx.lineTo(sx,sy+T+T);ctx.lineTo(sx+T,sy+T*0.5+T);ctx.closePath();ctx.fill();
  }
}
function render(){[0,1,2].forEach(i=>drawIso($('#c'+i),evalFeature(SEEDS[i])));}

// ---- op editor UI ----------------------------------------------------------
function rfField(label,rf,onch){
  const wrap=document.createElement('div');
  wrap.innerHTML=`<label>${label}</label>`;
  const row=document.createElement('div');row.className='rf';
  const a=document.createElement('input');a.type='number';a.step='0.5';a.value=rf.a;
  const b=document.createElement('input');b.type='number';b.step='0.5';b.value=rf.b;b.style.display=rf.fixed?'none':'';
  const t=document.createElement('div');t.className='t'+(rf.fixed?'':' on');t.textContent=rf.fixed?'fixed':'range';
  a.oninput=()=>{rf.a=+a.value;onch();};b.oninput=()=>{rf.b=+b.value;onch();};
  t.onclick=()=>{rf.fixed=!rf.fixed;b.style.display=rf.fixed?'none':'';t.textContent=rf.fixed?'fixed':'range';t.className='t'+(rf.fixed?'':' on');onch();};
  row.appendChild(a);row.appendChild(b);row.appendChild(t);wrap.appendChild(row);return wrap;
}
function opCard(op,oi){
  const el=document.createElement('div');el.className='op';
  const head=document.createElement('div');head.className='ohead';
  const sel=document.createElement('select');SHAPES.forEach(s=>{const o=document.createElement('option');o.value=o.textContent=s;if(s===op.shape)o.selected=true;sel.appendChild(o);});
  sel.onchange=()=>{op.shape=sel.value;rebuild();};
  const sp=document.createElement('span');sp.className='sp';sp.style.color='var(--dim)';sp.textContent='op '+(oi+1);
  const up=document.createElement('span');up.className='x';up.textContent='↑';up.onclick=()=>{if(oi>0){[OPS[oi-1],OPS[oi]]=[OPS[oi],OPS[oi-1]];rebuild();}};
  const rm=document.createElement('span');rm.className='x';rm.textContent='✕';rm.onclick=()=>{OPS.splice(oi,1);if(!OPS.length)OPS.push(newOp());rebuild();};
  head.appendChild(sel);head.appendChild(sp);head.appendChild(up);head.appendChild(rm);el.appendChild(head);

  const atrow=document.createElement('div');atrow.innerHTML='<label>at x,y,z (offset from anchor)</label>';
  const ar=document.createElement('div');ar.className='row';
  [0,1,2].forEach(k=>{const i=document.createElement('input');i.type='number';i.value=op.at[k];i.oninput=()=>{op.at[k]=+i.value;render();};ar.appendChild(i);});
  atrow.appendChild(ar);el.appendChild(atrow);

  const need=op.shape;
  if(!op.thickness)op.thickness={fixed:true,a:1.5,b:1.5};
  const grid=document.createElement('div');grid.className='row';
  if(["sphere","cylinder","cone","column","torus","arch","spiral"].includes(need)) grid.appendChild(rfField(["torus","arch","spiral"].includes(need)?"radius (major)":"radius",op.radius,render));
  if(["cylinder","cone","column","line","spiral"].includes(need)) grid.appendChild(rfField("height/len",op.height,render));
  if(["torus","arch","spiral"].includes(need)) grid.appendChild(rfField("thickness",op.thickness,render));
  el.appendChild(grid);
  if(need==="spiral"){const tr=document.createElement('div');tr.className='row';tr.appendChild(numField("turns",op.turns==null?2:op.turns,0.5,v=>{op.turns=v;render();}));el.appendChild(tr);}
  if(["box","ellipsoid"].includes(need)){
    const sg=document.createElement('div');sg.className='row';
    sg.appendChild(rfField("size x",op.size[0],render));
    sg.appendChild(rfField("size y",op.size[1],render));
    sg.appendChild(rfField("size z",op.size[2],render));
    el.appendChild(sg);
  }
  if(need==="voxels") el.appendChild(voxelPainter(op));

  const fr=document.createElement('div');fr.className='row';
  fr.appendChild(selField("fill",["solid","shell","noise","scatter"],op.fill,v=>{op.fill=v;rebuild();}));
  fr.appendChild(selField("block pick",["per_cell","per_instance"],op.pick,v=>{op.pick=v;render();}));
  fr.appendChild(selField("place",["force","air_only"],op.place,v=>{op.place=v;render();}));
  el.appendChild(fr);
  if(op.fill==="noise"){
    const nr=document.createElement('div');nr.className='row';
    nr.appendChild(numField("noise freq",op.noise.freq,0.05,v=>{op.noise.freq=v;render();}));
    nr.appendChild(numField("threshold",op.noise.threshold,0.05,v=>{op.noise.threshold=v;render();}));
    el.appendChild(nr);
  }
  if(op.fill==="scatter"){
    const sr=document.createElement('div');sr.className='row';
    sr.appendChild(numField("keep fraction (0-1)",op.scatter==null?0.5:op.scatter,0.05,v=>{op.scatter=v;render();}));
    el.appendChild(sr);
  }

  const bl=document.createElement('div');bl.innerHTML='<label>blocks (weighted)</label>';
  const bp=document.createElement('div');bp.className='blkpick';
  op.blocks.forEach((b,bi)=>{
    const w=document.createElement('span');w.className='wblk';
    w.innerHTML=`<img src="${ic(b.name)}"><span>${b.name}</span>`;
    const wi=document.createElement('input');wi.type='number';wi.value=b.w;wi.min=0;wi.step=1;wi.oninput=()=>{b.w=+wi.value;render();};
    const xx=document.createElement('span');xx.className='x';xx.textContent='✕';xx.onclick=()=>{op.blocks.splice(bi,1);if(!op.blocks.length)op.blocks.push({name:"stone",w:1});rebuild();};
    w.appendChild(wi);w.appendChild(xx);bp.appendChild(w);
  });
  const addb=document.createElement('span');addb.className='pill';addb.textContent='+ blk';
  addb.onclick=()=>{if(SELBLK){op.blocks.push({name:SELBLK,w:1});rebuild();}else $('#status').textContent='pick a block from the palette first';};
  bp.appendChild(addb);bl.appendChild(bp);el.appendChild(bl);
  return el;
}
function selField(label,opts,val,onch){const w=document.createElement('div');w.innerHTML=`<label>${label}</label>`;
  const s=document.createElement('select');opts.forEach(o=>{const op=document.createElement('option');op.value=op.textContent=o;if(o===val)op.selected=true;s.appendChild(op);});
  s.onchange=()=>onch(s.value);w.appendChild(s);return w;}
function numField(label,val,step,onch){const w=document.createElement('div');w.innerHTML=`<label>${label}</label>`;
  const i=document.createElement('input');i.type='number';i.step=step;i.value=val;i.oninput=()=>onch(+i.value);w.appendChild(i);return w;}

// Sculpt mode: a layer-by-layer voxel painter for a `voxels` op. Cells are keyed in
// OP-LOCAL coords (x - anchorX, y, z - anchorZ) so they match Feature::at exactly.
function voxelPainter(op){
  if(!op.vox)op.vox={};
  const sx=+$('#f-sx').value, sy=+$('#f-sy').value, sz=+$('#f-sz').value;
  const ax=Math.floor(sx/2), az=Math.floor(sz/2);
  if(op._y==null||op._y>sy-1)op._y=Math.min(Math.floor(sy/2),sy-1);
  const wrap=document.createElement('div');
  const bar=document.createElement('div');bar.className='row';
  const lab=document.createElement('label');lab.innerHTML='layer Y = <b>'+op._y+'</b> / '+(sy-1);
  const sl=document.createElement('input');sl.type='range';sl.min=0;sl.max=Math.max(0,sy-1);sl.value=op._y;
  sl.oninput=()=>{op._y=+sl.value;rebuild();};
  const m=document.createElement('label');m.style.flex='0 0 auto';
  m.innerHTML='<input type="checkbox" '+(op._mir?'checked':'')+'> mirror X';
  m.querySelector('input').onchange=e=>{op._mir=e.target.checked;};
  bar.appendChild(lab);bar.appendChild(sl);bar.appendChild(m);wrap.appendChild(bar);
  const b3d=document.createElement('button');b3d.className='btn';b3d.textContent='🧊 Open 3D editor';
  b3d.style.margin='4px 0';b3d.onclick=()=>open3D(op);wrap.appendChild(b3d);
  const grid=document.createElement('div');
  grid.style.cssText='display:grid;grid-template-columns:repeat('+sx+',15px);gap:1px;margin:6px 0';
  for(let z=0;z<sz;z++)for(let x=0;x<sx;x++){
    const key=(x-ax)+','+op._y+','+(z-az);
    const c=document.createElement('div');
    c.style.cssText='width:15px;height:15px;cursor:pointer;border:1px solid #2c3240;border-radius:2px';
    const bn=op.vox[key];
    c.style.background = bn ? (COLORS[bn]||'#9a8a6a') : '#0b0d12';
    c.title=bn||'';
    c.onclick=()=>{
      if(op.vox[key]) delete op.vox[key]; else op.vox[key]=SELBLK||'stone';
      if(op._mir){const mk=(ax-x)+','+op._y+','+(z-az); if(op.vox[key])op.vox[mk]=op.vox[key]; else delete op.vox[mk];}
      rebuild();
    };
    grid.appendChild(c);
  }
  wrap.appendChild(grid);
  const hint=document.createElement('div');hint.className='status';
  hint.textContent='pick a block in the Palette (right), then click cells; click again to erase. Slider = layer.';
  wrap.appendChild(hint);
  return wrap;
}

function rebuild(){const c=$('#ops');c.innerHTML='';OPS.forEach((op,i)=>c.appendChild(opCard(op,i)));render();}

// ---- saved-feature gallery (server-rendered thumbnails) --------------------
window.loadByName=async(n)=>{$('#load').value=n;loadFeature(await (await fetch('/api/feature/'+n)).json());
  document.querySelector('.preview').scrollTop=0;$('#status').textContent='loaded '+n;};
async function buildGallery(){
  const fl=await (await fetch('/api/features')).json();
  $('#gallery').innerHTML = fl.length ? fl.map(n=>
    `<div class="gcard" onclick="loadByName('${n}')"><img src="/preview/${n}.png?seed=1&amp;t=${Date.now()}"><div class="gn">${n}</div></div>`
    ).join('') : '<span class="status">no features yet — build one on the left and Save</span>';
  return fl;
}

// ---- load / save -----------------------------------------------------------
function toRF(v){return (v&&typeof v==='object')?{fixed:false,a:+v.min,b:+v.max}:{fixed:true,a:+v,b:+v};}
function fromRF(rf){return rf.fixed?rf.a:{min:rf.a,max:rf.b};}
function loadFeature(d){
  $('#f-name').value=d.name||'feature';
  const sz=d.size||[9,9,9];$('#f-sx').value=sz[0];$('#f-sy').value=sz[1];$('#f-sz').value=sz[2];
  const sc=d.scatter||{};$('#s-den').value=sc.density??0.15;$('#s-spc').value=sc.spacing??44;
  $('#s-surf').checked=sc.surface!==false;$('#s-mine').value=sc.min_elevation??-9999;$('#s-maxe').value=sc.max_elevation??9999;
  $('#s-dist').value=sc.distribution==='noise'?'noise':'grid';$('#s-nfreq').value=sc.noise_freq??0.02;$('#s-nthr').value=sc.noise_threshold??0.3;
  $('#s-mins').value=sc.min_slope??0;$('#s-maxs').value=sc.max_slope??100000;$('#s-onw').checked=!!sc.on_water;$('#s-nearw').value=sc.near_water??0;
  SMASK = sc.mask ? desMask(sc.mask) : null; renderMask();
  const bset=new Set(sc.biomes||[]);document.querySelectorAll('#biomes .pill').forEach(p=>p.classList.toggle('sel',bset.has(p.dataset.n)));
  VARIANTS=[desOps(d.ops),...(d.variants||[]).map(v=>desOps(v.ops))];
  if(!VARIANTS.length)VARIANTS=[[newOp()]];
  CURV=0;OPS=VARIANTS[0];renderVariantBar();rebuild();
}
// op (de)serialization, shared by load/save/variants.
function desOp(o){const sz=o.size||[3,3,3];return {
  shape:o.shape||"box",at:o.at||[0,0,0],radius:toRF(o.radius??2),height:toRF(o.height??5),
  size:[toRF(sz[0]),toRF(sz[1]),toRF(sz[2])],
  thickness:toRF(o.thickness??1.5),turns:o.turns??2,scatter:o.scatter??0.5,
  vox:(o.cells||[]).reduce((m,c)=>{m[c[0]+','+c[1]+','+c[2]]=c[3]||'stone';return m;},{}),
  blocks: Array.isArray(o.block)? o.block.map(b=>typeof b==='string'?{name:b,w:1}:{name:b.name,w:b.w??1}) : [{name:o.block||"stone",w:1}],
  pick:o.block_pick||"per_cell",fill:o.fill||"solid",noise:o.noise||{freq:0.3,threshold:0},place:o.place||"force"};}
function desOps(a){const r=(a||[]).map(desOp);return r.length?r:[newOp()];}
function serOp(op){const o={shape:op.shape,at:op.at};
  if(["sphere","cylinder","cone","column","torus","arch","spiral"].includes(op.shape))o.radius=fromRF(op.radius);
  if(["cylinder","cone","column","line","spiral"].includes(op.shape))o.height=fromRF(op.height);
  if(["torus","arch","spiral"].includes(op.shape))o.thickness=fromRF(op.thickness);
  if(op.shape==="spiral")o.turns=op.turns;
  if(["box","ellipsoid"].includes(op.shape))o.size=op.size.map(fromRF);
  if(op.shape==="voxels")o.cells=Object.entries(op.vox||{}).map(([k,b])=>{const p=k.split(',').map(Number);return [p[0],p[1],p[2],b];});
  o.block=op.blocks.map(b=>({name:b.name,w:b.w}));o.block_pick=op.pick;o.fill=op.fill;
  if(op.fill==="noise")o.noise=op.noise;
  if(op.fill==="scatter")o.scatter=op.scatter;
  o.place=op.place;return o;}
function serOps(a){return a.map(serOp);}
// ---- scatter noise mask (optional) ----------------------------------------
// Held as a JS object; serialised into scatter.mask. layers + threshold/width +
// steepness falloff (incl. a draggable bezier curve). Same primitive as the engine
// NoiseMask / the worldgen-tool biome masks. No layers -> omitted (no mask).
let SMASK=null;
const MFALLOFFS=["step","linear","smoothstep","smootherstep","gain","bezier"];
function newMask(){return {threshold:0,width:0.5,falloff:"smoothstep",gain:0.5,invert:false,
  layers:[{type:"perlin",frequency:0.02,octaves:3,weight:1}], bezier:[[0,0],[1,1]]};}
function serMask(m){const o={threshold:m.threshold,width:m.width,falloff:m.falloff};
  if(m.gain!==0.5)o.gain=m.gain; if(m.invert)o.invert=true;
  o.layers=m.layers.map(l=>({type:l.type,frequency:l.frequency,octaves:l.octaves,weight:l.weight}));
  if(m.falloff==='bezier')o.bezier=m.bezier.map(p=>[p[0],p[1]]); return o;}
function desMask(m){return {threshold:m.threshold??0,width:m.width??0.5,falloff:m.falloff||'smoothstep',
  gain:m.gain??0.5,invert:!!m.invert,
  layers:(m.layers||[]).map(l=>({type:l.type||'perlin',frequency:l.frequency??0.02,octaves:l.octaves??3,weight:l.weight??1})),
  bezier:(m.bezier&&m.bezier.length>=2)?m.bezier.map(p=>[+p[0],+p[1]]):[[0,0],[1,1]]};}
function renderMask(){
  const c=$('#smask'); if(!c)return;
  if(!SMASK){ c.innerHTML='<button class="btn ghost" onclick="SMASK=newMask();renderMask();">+ enable noise mask</button>'; return; }
  const m=SMASK;
  const fo=MFALLOFFS.map(o=>`<option value="${o}"${o===m.falloff?' selected':''}>${o}</option>`).join('');
  const lay=m.layers.map((l,j)=>`<div class="row">`
    +`<div><label>type</label><select onchange="SMASK.layers[${j}].type=this.value">`
      +['perlin','ridged','billow','worley'].map(t=>`<option${t===l.type?' selected':''}>${t}</option>`).join('')+`</select></div>`
    +`<div><label>freq</label><input type="number" step="any" value="${l.frequency}" oninput="SMASK.layers[${j}].frequency=+this.value"></div>`
    +`<div><label>oct</label><input type="number" step="1" value="${l.octaves}" oninput="SMASK.layers[${j}].octaves=+this.value"></div>`
    +`<div><label>weight</label><input type="number" step="0.05" value="${l.weight}" oninput="SMASK.layers[${j}].weight=+this.value"></div>`
    +`<button class="btn ghost" onclick="SMASK.layers.splice(${j},1);renderMask();">✖</button></div>`).join('');
  c.innerHTML=
    `<div class="row"><div><label>threshold</label><input type="number" step="0.02" value="${m.threshold}" oninput="SMASK.threshold=+this.value"></div>`
    +`<div><label>width (steepness)</label><input type="number" step="0.02" value="${m.width}" oninput="SMASK.width=+this.value"></div></div>`
    +`<div class="row"><div><label>falloff</label><select onchange="SMASK.falloff=this.value;renderMask()">${fo}</select></div>`
    +`<div><label>gain</label><input type="number" step="0.02" value="${m.gain}" oninput="SMASK.gain=+this.value"></div>`
    +`<div><label>invert</label><input type="checkbox" ${m.invert?'checked':''} onchange="SMASK.invert=this.checked"></div></div>`
    +`<label>noise layers (none = mask off)</label>`+lay
    +`<button class="btn ghost" onclick="SMASK.layers.push({type:'perlin',frequency:0.02,octaves:3,weight:1});renderMask();">+ layer</button>`
    +(m.falloff==='bezier'?`<div style="margin-top:6px"><svg id="mcurve" viewBox="0 0 240 130" preserveAspectRatio="none" style="width:100%;height:150px;background:#15131a;border-radius:6px;cursor:crosshair;touch-action:none"><rect x="26" y="8" width="206" height="106" fill="none" stroke="#3a3340"></rect><polyline fill="none" stroke="#d6b3f0" stroke-width="2" vector-effect="non-scaling-stroke"></polyline><g></g></svg><div><button class="btn ghost" onclick="maskCurveAdd()">+ point</button> <span class="status">drag a point · right-click to delete</span></div></div>`:'')
    +`<button class="btn ghost" style="margin-top:6px" onclick="SMASK=null;renderMask();">remove mask</button>`;
  if(m.falloff==='bezier')initMaskCurve();
}
function maskCurveAdd(){ const ps=SMASK.bezier.slice().sort((a,b)=>a[0]-b[0]);
  let x=0.5,y=0.5; if(ps.length>=2){let gi=0,gw=-1;for(let k=0;k<ps.length-1;k++){const w=ps[k+1][0]-ps[k][0];if(w>gw){gw=w;gi=k;}}x=(ps[gi][0]+ps[gi+1][0])/2;y=(ps[gi][1]+ps[gi+1][1])/2;}
  SMASK.bezier.push([Math.round(x*100)/100,Math.round(y*100)/100]); initMaskCurve(); }
function initMaskCurve(){
  const svg=$('#mcurve'); if(!svg)return;
  const NS='http://www.w3.org/2000/svg', W=240,H=130,L=26,Rt=8,T=8,B=16,pw=W-L-Rt,ph=H-T-B;
  const tx=x=>L+x*pw, ty=y=>T+(1-y)*ph, ix=p=>(p-L)/pw, iy=p=>1-(p-T)/ph;
  const poly=svg.querySelector('polyline'), pg=svg.querySelector('g');
  function cr(p0,p1,p2,p3,t){const t2=t*t,t3=t2*t;return 0.5*((2*p1)+(-p0+p2)*t+(2*p0-5*p1+4*p2-p3)*t2+(-p0+3*p1-3*p2+p3)*t3);}
  function pts(){return SMASK.bezier.map((p,i)=>({i,x:p[0],y:p[1]})).sort((a,b)=>a.x-b.x);}
  function draw(){const ps=pts();while(pg.firstChild)pg.removeChild(pg.firstChild);let s='';
    for(let k=0;k<ps.length-1;k++){const p0=ps[k>0?k-1:k],p1=ps[k],p2=ps[k+1],p3=ps[k+2<ps.length?k+2:k+1];
      for(let st=0;st<=8;st++){const t=st/8;const yy=Math.min(1,Math.max(0,cr(p0.y,p1.y,p2.y,p3.y,t)));const xx=p1.x+(p2.x-p1.x)*t;s+=tx(xx)+','+ty(yy)+' ';}}
    poly.setAttribute('points',s.trim());
    ps.forEach(p=>{const c=document.createElementNS(NS,'circle');c.setAttribute('cx',tx(p.x));c.setAttribute('cy',ty(p.y));c.setAttribute('r',5);c.setAttribute('fill','#ffce6b');c.setAttribute('stroke','#1b1a1a');c.dataset.i=p.i;c.style.cursor='grab';pg.appendChild(c);});}
  let active=null;
  const at=e=>{const r=svg.getBoundingClientRect();return [ix((e.clientX-r.left)/r.width*W),iy((e.clientY-r.top)/r.height*H)];};
  svg.onmousedown=e=>{if(e.target.tagName==='circle'){active=+e.target.dataset.i;e.preventDefault();}};
  svg.oncontextmenu=e=>{if(e.target.tagName==='circle'){e.preventDefault();if(SMASK.bezier.length>2){SMASK.bezier.splice(+e.target.dataset.i,1);initMaskCurve();}}};
  window.addEventListener('mousemove',e=>{if(active===null||!document.body.contains(svg))return;let p=at(e);
    const ps=pts(),pos=ps.findIndex(q=>q.i===active);
    const lo=pos>0?ps[pos-1].x+0.01:0,hi=pos<ps.length-1?ps[pos+1].x-0.01:1;
    const x=Math.min(hi,Math.max(lo,p[0])),y=Math.min(1,Math.max(0,p[1]));
    SMASK.bezier[active]=[Math.round(x*100)/100,Math.round(y*100)/100];draw();});
  window.addEventListener('mouseup',()=>{active=null;});
  draw();
}
function gather(){
  const biomes=[...document.querySelectorAll('#biomes .pill.sel')].map(p=>p.dataset.n);
  const sc={density:+$('#s-den').value,spacing:+$('#s-spc').value,surface:$('#s-surf').checked,
    min_elevation:+$('#s-mine').value,max_elevation:+$('#s-maxe').value};
  if(biomes.length)sc.biomes=biomes;
  if($('#s-dist').value==='noise'){sc.distribution='noise';sc.noise_freq=+$('#s-nfreq').value;sc.noise_threshold=+$('#s-nthr').value;}
  if(+$('#s-mins').value>0)sc.min_slope=+$('#s-mins').value;
  if(+$('#s-maxs').value<100000)sc.max_slope=+$('#s-maxs').value;
  if($('#s-onw').checked)sc.on_water=true;
  if(+$('#s-nearw').value>0)sc.near_water=+$('#s-nearw').value;
  if(SMASK && SMASK.layers.length)sc.mask=serMask(SMASK);
  return {name:$('#f-name').value,scatter:sc,
    size:[+$('#f-sx').value,+$('#f-sy').value,+$('#f-sz').value],
    anchor:[Math.floor(+$('#f-sx').value/2),0,Math.floor(+$('#f-sz').value/2)],
    ops:serOps(VARIANTS[0]),
    ...(VARIANTS.length>1?{variants:VARIANTS.slice(1).map(v=>({ops:serOps(v)}))}:{})};
}

// ---- 3D voxel editor (Three.js orbit + face picking) ----------------------
const T3={rend:null,scene:null,cam:null,raf:0,op:null,meshes:new Map(),ground:null,geo:null,
  erase:false,yaw:0.7,pitch:0.6,dist:14,target:null,bounds:null,tool:'voxel',pending:null};
function removeVox(c){delete T3.op.vox[c[0]+','+c[1]+','+c[2]];}
function setMsg(s){$('#ed-msg').textContent=s||(T3.tool==='voxel'
  ?'click a face to add · shift/right-click remove':'click two corners ('+T3.tool+')');}
function lineCells(a,b){const pts=[];let x=a[0],y=a[1],z=a[2];const X=b[0],Y=b[1],Z=b[2];
  const dx=Math.abs(X-x),dy=Math.abs(Y-y),dz=Math.abs(Z-z),sx=x<X?1:-1,sy=y<Y?1:-1,sz=z<Z?1:-1;
  let dm=Math.max(dx,dy,dz,1),xe=dm/2,ye=dm/2,ze=dm/2,i=dm;pts.push([x,y,z]);
  while(i-->0){xe-=dx;if(xe<0){xe+=dm;x+=sx;}ye-=dy;if(ye<0){ye+=dm;y+=sy;}ze-=dz;if(ze<0){ze+=dm;z+=sz;}pts.push([x,y,z]);}
  return pts;}
function regionFill(a,b,tool,erase){
  const apply=(x,y,z)=>{if(erase)removeVox([x,y,z]);else placeVox(x,y,z);};
  if(tool==='line'){lineCells(a,b).forEach(c=>apply(c[0],c[1],c[2]));return;}
  const x0=Math.min(a[0],b[0]),x1=Math.max(a[0],b[0]),y0=Math.min(a[1],b[1]),y1=Math.max(a[1],b[1]),
        z0=Math.min(a[2],b[2]),z1=Math.max(a[2],b[2]);
  for(let x=x0;x<=x1;x++)for(let y=y0;y<=y1;y++)for(let z=z0;z<=z1;z++){
    if(tool==='walls'&&!(x===x0||x===x1||z===z0||z===z1))continue; // 4 vertical walls (no floor/roof)
    apply(x,y,z);}
}
function ed3dResize(){const sx=Math.max(1,+$('#ed-sx').value),sy=Math.max(1,+$('#ed-sy').value),sz=Math.max(1,+$('#ed-sz').value);
  $('#f-sx').value=sx;$('#f-sy').value=sy;$('#f-sz').value=sz;
  T3.bounds={ax:Math.floor(sx/2),az:Math.floor(sz/2),sx,sy,sz};
  if(T3.ground)T3.scene.remove(T3.ground);
  T3.ground=new THREE.GridHelper(Math.max(sx,sz)+2,Math.max(sx,sz)+2,0x3a4456,0x222a36);T3.ground.position.y=-0.5;T3.scene.add(T3.ground);
  T3.target.set(0,sy/2-0.5,0);camUpdate();}
function camUpdate(){const t=T3.target;
  T3.cam.position.set(t.x+T3.dist*Math.cos(T3.pitch)*Math.sin(T3.yaw),t.y+T3.dist*Math.sin(T3.pitch),t.z+T3.dist*Math.cos(T3.pitch)*Math.cos(T3.yaw));
  T3.cam.lookAt(t);}
const MAT3={};  // cached textured material per block (real block texture on cubes)
function blockMat(name){
  if(MAT3[name])return MAT3[name];
  const tex=new THREE.TextureLoader().load('/icon/'+name+'.png');
  tex.magFilter=THREE.NearestFilter;tex.minFilter=THREE.NearestFilter; // crisp voxel look
  MAT3[name]=new THREE.MeshLambertMaterial({map:tex,color:0xffffff});
  return MAT3[name];
}
function build3D(){T3.meshes.forEach(m=>T3.scene.remove(m));T3.meshes.clear(); // shared cached mats -> don't dispose
  for(const k in T3.op.vox){const[x,y,z]=k.split(',').map(Number);
    const m=new THREE.Mesh(T3.geo,blockMat(T3.op.vox[k]));
    m.position.set(x,y,z);m.userData.k=k;T3.scene.add(m);T3.meshes.set(k,m);}}
function ed3dSwatch(){const n=SELBLK||'stone';
  $('#ed-sw').style.backgroundImage="url('/icon/"+n+".png')";$('#ed-blkname').textContent=n;
  document.querySelectorAll('#ed-pal .ip').forEach(p=>p.classList.toggle('sel',p.dataset.n===n));}
function selBlock(n){SELBLK=n;ed3dSwatch();}
function placeVox(x,y,z){const b=T3.bounds;
  if(x<-b.ax||x>b.sx-1-b.ax||y<0||y>b.sy-1||z<-b.az||z>b.sz-1-b.az)return;
  T3.op.vox[x+','+y+','+z]=SELBLK||'stone';}
function pick3D(e){const cv=$('#ed-cv'),r=cv.getBoundingClientRect();
  const ndc=new THREE.Vector2(((e.clientX-r.left)/r.width)*2-1,-((e.clientY-r.top)/r.height)*2+1);
  const rc=new THREE.Raycaster();rc.setFromCamera(ndc,T3.cam);
  const hits=rc.intersectObjects([...T3.meshes.values()],false);
  const erase=T3.erase||e.shiftKey||e.button===2;
  let cell=null;
  if(hits.length){const h=hits[0],p=h.object.position;
    if(erase)cell=[Math.round(p.x),Math.round(p.y),Math.round(p.z)];          // the hit voxel
    else{const n=h.face.normal;cell=[Math.round(p.x+n.x),Math.round(p.y+n.y),Math.round(p.z+n.z)];}} // adjacent
  else{const gp=new THREE.Plane(new THREE.Vector3(0,1,0),0.5),pt=new THREE.Vector3();
    if(rc.ray.intersectPlane(gp,pt))cell=[Math.round(pt.x),0,Math.round(pt.z)];}                     // ground
  if(!cell)return;
  if((T3.tool||'voxel')==='voxel'){ if(erase)removeVox(cell); else placeVox(cell[0],cell[1],cell[2]); }
  else if(!T3.pending){ T3.pending=cell; setMsg('pick the opposite corner'); } // first corner
  else { regionFill(T3.pending,cell,T3.tool,erase); T3.pending=null; setMsg(''); }
  build3D();render();}
function open3D(op){
  if(typeof THREE==='undefined'){alert('The 3D editor needs three.js (loaded from a CDN — check your connection). The layer grid below still works offline.');return;}
  if(!op.vox)op.vox={};T3.op=op;
  $('#ed3d').style.display='flex';
  const cv=$('#ed-cv'),W=window.innerWidth,H=window.innerHeight-46;
  if(!T3.rend){
    T3.rend=new THREE.WebGLRenderer({canvas:cv,antialias:true});
    T3.scene=new THREE.Scene();T3.scene.background=new THREE.Color(0x0a0c11);
    T3.cam=new THREE.PerspectiveCamera(50,W/H,0.1,500);
    T3.scene.add(new THREE.AmbientLight(0xffffff,0.65));
    const dl=new THREE.DirectionalLight(0xffffff,0.7);dl.position.set(0.6,1,0.4);T3.scene.add(dl);
    T3.geo=new THREE.BoxGeometry(1,1,1);T3.target=new THREE.Vector3();
    let dx0=0,dy0=0,moved=false;
    cv.addEventListener('pointerdown',e=>{dx0=e.clientX;dy0=e.clientY;moved=false;cv.setPointerCapture(e.pointerId);});
    cv.addEventListener('pointermove',e=>{if(e.buttons){const dx=e.clientX-dx0,dy=e.clientY-dy0;
      if(Math.abs(dx)+Math.abs(dy)>3){moved=true;T3.yaw-=dx*0.01;T3.pitch=Math.max(-1.45,Math.min(1.45,T3.pitch+dy*0.01));dx0=e.clientX;dy0=e.clientY;camUpdate();}}});
    cv.addEventListener('pointerup',e=>{if(!moved)pick3D(e);});
    cv.addEventListener('contextmenu',e=>e.preventDefault());
    cv.addEventListener('wheel',e=>{e.preventDefault();T3.dist=Math.max(3,Math.min(120,T3.dist*(e.deltaY>0?1.1:0.9)));camUpdate();},{passive:false});
    const loop=()=>{T3.raf=requestAnimationFrame(loop);if(T3.rend&&$('#ed3d').style.display!=='none')T3.rend.render(T3.scene,T3.cam);};loop();
    $('#ed-erase').onclick=()=>{T3.erase=!T3.erase;$('#ed-erase').textContent='erase: '+(T3.erase?'on':'off');$('#ed-erase').classList.toggle('on',T3.erase);};
    $('#ed-done').onclick=()=>{$('#ed3d').style.display='none';rebuild();};
    $('#ed-pal').innerHTML=BLOCKS.map(b=>`<div class="ip" data-n="${b.name}" title="${b.name}"><img src="${ic(b.name)}"></div>`).join('');
    document.querySelectorAll('#ed-pal .ip').forEach(p=>p.onclick=()=>selBlock(p.dataset.n));
    $('#ed-tool').onchange=()=>{T3.tool=$('#ed-tool').value;T3.pending=null;setMsg('');};
    ['ed-sx','ed-sy','ed-sz'].forEach(id=>$('#'+id).oninput=ed3dResize);
    $('#ed-search').oninput=()=>{const q=$('#ed-search').value.toLowerCase();
      document.querySelectorAll('#ed-pal .ip').forEach(p=>p.style.display=p.dataset.n.includes(q)?'':'none');};
  }
  T3.rend.setSize(W,H,false);T3.cam.aspect=W/H;T3.cam.updateProjectionMatrix();
  const sx=+$('#f-sx').value,sy=+$('#f-sy').value,sz=+$('#f-sz').value;
  $('#ed-sx').value=sx;$('#ed-sy').value=sy;$('#ed-sz').value=sz;$('#ed-tool').value=T3.tool;T3.pending=null;setMsg('');
  T3.bounds={ax:Math.floor(sx/2),az:Math.floor(sz/2),sx,sy,sz};
  T3.target.set(0,sy/2-0.5,0);T3.dist=Math.max(sx,sy,sz)*2.2+3;camUpdate();
  if(T3.ground)T3.scene.remove(T3.ground);
  T3.ground=new THREE.GridHelper(Math.max(sx,sz)+2,Math.max(sx,sz)+2,0x3a4456,0x222a36);
  T3.ground.position.y=-0.5;T3.scene.add(T3.ground);
  if(!SELBLK&&BLOCKS.length)SELBLK=BLOCKS[0].name;
  ed3dSwatch();
  build3D();
}

// ---- boot ------------------------------------------------------------------
(async()=>{
  BLOCKS=await (await fetch('/api/blocks')).json();BLOCKS.forEach(b=>COLORS[b.name]=b.color);
  BIOMES=await (await fetch('/api/biomes')).json();
  $('#palette').innerHTML=BLOCKS.map(b=>`<span class="pill" data-n="${b.name}"><img src="${ic(b.name)}">${b.name}</span>`).join('');
  document.querySelectorAll('#palette .pill').forEach(p=>p.onclick=()=>{SELBLK=p.dataset.n;
    document.querySelectorAll('#palette .pill').forEach(q=>q.classList.toggle('sel',q===p));$('#status').textContent='selected '+SELBLK;});
  $('#biomes').innerHTML=BIOMES.map(n=>`<span class="pill" data-n="${n}">${n}</span>`).join('')||'<span class="status">no biomes</span>';
  document.querySelectorAll('#biomes .pill').forEach(p=>p.onclick=()=>{p.classList.toggle('sel');render();});
  const fl=await buildGallery();
  $('#load').innerHTML='<option value="">— load existing —</option>'+fl.map(n=>`<option>${n}</option>`).join('');
  $('#load').onchange=async()=>{if($('#load').value){loadFeature(await (await fetch('/api/feature/'+$('#load').value)).json());}};
  ['f-sx','f-sy','f-sz','s-den'].forEach(id=>$('#'+id).oninput=render);
  $('#addop').onclick=()=>{OPS.push(newOp());rebuild();};
  $('#reroll').onclick=()=>{SEEDS=SEEDS.map(()=>Math.floor(Math.random()*1e9));render();};
  $('#save').onclick=async()=>{const d=gather();const r=await (await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d)})).json();
    $('#status').textContent=r.ok?('saved '+r.path):'save failed';
    const fl2=await buildGallery();$('#load').innerHTML='<option value="">— load existing —</option>'+fl2.map(n=>`<option>${n}</option>`).join('');};
  // Open showing a real feature (the first existing one) so it's immediately
  // inspectable, not a blank canvas. Falls back to the default new feature.
  if(fl.length){$('#load').value=fl[0];loadFeature(await (await fetch('/api/feature/'+fl[0])).json());}
  else {renderVariantBar();rebuild();renderMask();}
})();
</script>
</body></html>"""


if __name__ == "__main__":
    if "--render" in sys.argv:  # dump every feature x 3 seeds to PNGs for inspection
        outdir = os.path.join(HERE, "_feature_previews")
        os.makedirs(outdir, exist_ok=True)
        for nm in feature_list():
            feat = _load_yaml(os.path.join(FEATURES_DIR, nm + ".yaml"), {})
            for s in (1, 2, 3):
                im = render_iso(feat, s)
                if im:
                    im.save(os.path.join(outdir, "%s_%d.png" % (nm, s)))
            print("rendered", nm)
        print("-> %s" % outdir)
        sys.exit(0)
    url = "http://127.0.0.1:%d" % PORT
    print("feature tool -> %s" % url)
    if "--no-browser" not in sys.argv:
        import threading
        import webbrowser
        threading.Timer(1.2, lambda: webbrowser.open(url)).start()
    app.run(host="127.0.0.1", port=PORT, debug=False)
