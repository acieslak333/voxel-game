#!/usr/bin/env python3
"""Live world-generation tuning tool for the voxel game.

A tiny localhost web app: drag sliders for the generation knobs and watch the
top-down map update. It edits assets/biomes.yaml and runs the game's own
`voxelgame --genmap`, so what you see is the EXACT generation the game uses — no
noise logic is duplicated here (the C++ TerrainGenerator + biomes.yaml are the
single source of truth).

    pip install flask ruamel.yaml
    python tools/genmap_tool.py
    # open http://127.0.0.1:5000

Comments in biomes.yaml are preserved (ruamel round-trip). The same file feeds the
game, so once you like a map, just launch the game on the same seed config.
"""
import os
import subprocess
import sys

try:
    from flask import Flask, request, send_file, Response
except ImportError:
    sys.exit("genmap_tool needs Flask:  pip install flask ruamel.yaml")
try:
    from ruamel.yaml import YAML
except ImportError:
    sys.exit("genmap_tool needs ruamel.yaml (preserves comments):  pip install ruamel.yaml")

import shutil

import terrain3d  # shared live-3D terrain view (vendored Three.js, sea plane + sky)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_BIOMES = os.path.join(ROOT, "assets", "biomes.yaml")
DEPLOY_BIOMES = os.path.join(ROOT, "build", "bin", "assets", "biomes.yaml")
MAP_OUT = os.path.join(HERE, "_genmap.png")
# Prefer the fast Release build; fall back to Debug.
EXE_CANDIDATES = [
    os.path.join(ROOT, "build", "bin", "Release", "voxelgame.exe"),
    os.path.join(ROOT, "build", "bin", "Debug", "voxelgame.exe"),
    os.path.join(ROOT, "build", "bin", "voxelgame"),  # single-config / linux
]

yaml = YAML()  # round-trip: preserves comments + ordering


def exe_path():
    for p in EXE_CANDIDATES:
        if os.path.exists(p):
            return p
    sys.exit("voxelgame executable not found — build it first (Release recommended).")


# Each control: (label, dotted yaml path, min, max, step). Dotted path indexes
# nested maps; "island.center.0" indexes a sequence element. NOTE: when a field
# has a `layers:` stack, its single `frequency` is a LEGACY fallback the game
# ignores — the per-layer controls (built dynamically below) are what matter.
CONTROLS = [
    ("Sea level",            "sea_level",                 8,   240, 1),
    ("Snow line (rel)",      "snow_line",                 0,   120, 1),
    ("Island enabled (1/0)", "island.enabled",            0,   1,   1),
    ("Island radius",        "island.radius",             200, 4000, 10),
    ("Island inner frac",    "island.inner",              0.1, 0.9, 0.01),
    ("Island coast warp",    "island.coast_warp",         0,   800, 10),
    ("Island land base",     "island.land_base",          0,   40,  1),
    ("Island interior var",  "island.interior_var",       0,   30,  1),
    ("Island ocean floor",   "island.ocean_floor",        -120, 0,  2),
    ("Temperature freq",     "temperature.frequency",     0.0003, 0.008, 0.0001),
    ("Humidity freq",        "humidity.frequency",        0.0003, 0.008, 0.0001),
]

# 3D volumetric terrain + water bodies (only visible in the 3D voxel view; the
# top-down map shows the heightmap surface only).
CONTROLS_3D = [
    ("3D enabled (1/0)",     "terrain3d.enabled",         0,   1,    1),
    ("3D amplitude",         "terrain3d.amplitude",       0,   128,  2),
    ("Float threshold",      "terrain3d.float_threshold", 0.0, 0.8,  0.01),
    ("Float freq",           "terrain3d.float_freq",      0.001, 0.02, 0.0005),
    ("Float gap",            "terrain3d.float_gap",       0,   60,   1),
    ("Float reach",          "terrain3d.float_reach",     0,   200,  2),
    ("River freq",           "rivers.frequency",          0.0005, 0.006, 0.0001),
    ("River width",          "rivers.width",              0.0, 0.3,  0.005),
    ("River depth",          "rivers.depth",              0,   16,   1),
    ("River max elev",       "rivers.max_elevation",      0,   60,   1),
    ("Lake spacing",         "lakes.spacing",             60,  600,  10),
    ("Lake chance",          "lakes.chance",              0.0, 1.0,  0.02),
    ("Lake radius min",      "lakes.radius_min",          4,   40,   1),
    ("Lake radius max",      "lakes.radius_max",          8,   80,   1),
    ("Lake depth",           "lakes.depth",               2,   20,   1),
]

# Fields whose noise can be an authored `layers:` stack — each layer gets live
# frequency / weight / octaves number inputs (the stack IS what the game runs).
STACK_FIELDS = ["continentalness", "erosion", "peaks", "temperature", "humidity",
                "rivers", "terrain3d.density"]


def layer_controls(doc):
    """Number-input rows for every layer of every authored noise stack."""
    rows = []
    for field in STACK_FIELDS:
        try:
            layers = get_path(doc, field)["layers"]
        except Exception:
            continue
        rows.append(f'<div class="grp">{field} layers</div>')
        for i, l in enumerate(layers):
            base = f"{field}.layers.{i}"
            typ = str(l.get("type", "perlin"))
            f = float(l.get("frequency", 0.01))
            w = float(l.get("weight", 1.0))
            o = int(l.get("octaves", 3))
            rows.append(
                f'<div class="r4"><label>{i} {typ}</label>'
                f'<span>f<input type="number" step="any" value="{f:g}" data-path="{base}.frequency" oninput="onSlide(this)"></span>'
                f'<span>w<input type="number" step="0.02" value="{w:g}" data-path="{base}.weight" oninput="onSlide(this)"></span>'
                f'<span>o<input type="number" step="1" min="1" max="10" value="{o}" data-path="{base}.octaves" oninput="onSlide(this)"></span>'
                f'</div>')
    return rows


def get_path(doc, dotted):
    node = doc
    for key in dotted.split("."):
        node = node[int(key)] if key.isdigit() else node[key]
    return node


def set_path(doc, dotted, value):
    keys = dotted.split(".")
    node = doc
    for key in keys[:-1]:
        node = node[int(key)] if key.isdigit() else node[key]
    last = keys[-1]
    if last.isdigit():
        node[int(last)] = value
    else:
        node[last] = value


def load_doc():
    with open(SRC_BIOMES, encoding="utf-8") as f:
        return yaml.load(f)


def deploy_only(doc):
    """Preview: write the edited config to the DEPLOYED copy (what --genmap reads),
    NOT the committed source. So dragging a slider previews without saving."""
    os.makedirs(os.path.dirname(DEPLOY_BIOMES), exist_ok=True)
    with open(DEPLOY_BIOMES, "w", encoding="utf-8") as f:
        yaml.dump(doc, f)


def save_source(doc):
    """Persist the edits to the committed source biomes.yaml (and the deployed copy)."""
    with open(SRC_BIOMES, "w", encoding="utf-8") as f:
        yaml.dump(doc, f)
    deploy_only(doc)


def apply_form(doc, form):
    """Patch `doc` with the slider/dropdown values from a request form (in place)."""
    for path, raw in form.items():
        if path in ("__pixels__", "__step__", "__view__"):
            continue
        try:
            val = float(raw)
            if val == int(val):
                val = int(val)
            set_path(doc, path, val)
        except Exception:
            pass


# View modes the genmap backend supports. "top" is the surface map; "noise:<layer>"
# renders one raw noise layer; "cross" is a vertical cross-section. The tool just
# forwards these to `voxelgame --genmap --mode ... [--layer ...]`.
VIEW_MODES = [
    ("3D terrain (live)", "3d"),
    ("Top-down surface", "top"),
    ("Noise: continentalness", "noise:cont"),
    ("Noise: erosion", "noise:ero"),
    ("Noise: peaks", "noise:peak"),
    ("Noise: temperature", "noise:temp"),
    ("Noise: humidity", "noise:hum"),
    ("Noise: rivers", "noise:river"),
    ("Noise: relief (height)", "noise:relief"),
    ("Cross-section (Z=0)", "cross"),
]


def run_genmap(pixels, step, view="top"):
    px = int(pixels)
    args = [exe_path(), "--genmap", "--mapstep", str(step)]
    if view == "3d":
        # blk/px doubles as blocks-per-voxel-cell: size 1600 / step 8 = a 200-cell
        # view spanning 1600 blocks — the whole island, not a 96-block slice.
        subprocess.run(terrain3d.vox_args(exe_path(), px, step),
                       cwd=ROOT, check=True, capture_output=True)
        return
    if view.startswith("noise:"):
        args += ["--mapsize", str(px), "--mode", "noise", "--layer", view.split(":", 1)[1], "--out", MAP_OUT]
    elif view == "cross":
        args += ["--mapsize", str(px), "--mode", "cross", "--out", MAP_OUT]
    else:
        args += ["--mapsize", str(px), "--out", MAP_OUT]
    subprocess.run(args, cwd=ROOT, check=True, capture_output=True)


app = Flask(__name__)
terrain3d.register(app)  # /vendor/<file>, /heightmap.png, /sealevel


@app.route("/")
def index():
    doc = load_doc()
    rows = []

    def slider_rows(controls):
        for label, path, lo, hi, step in controls:
            try:
                val = float(get_path(doc, path))
            except Exception:
                continue  # field absent from this biomes.yaml: skip the control
            rows.append(
                f'<div class="r"><label>{label}</label>'
                f'<input type="range" min="{lo}" max="{hi}" step="{step}" value="{val}" '
                f'data-path="{path}" oninput="onSlide(this)">'
                f'<span class="v" id="v_{path}">{val:g}</span></div>')

    rows.append('<div class="grp">Shape &amp; island</div>')
    slider_rows(CONTROLS)
    rows.append('<div class="grp">3D terrain, floats &amp; water (see them in the 3D view)</div>')
    slider_rows(CONTROLS_3D)
    rows.extend(layer_controls(doc))
    controls = "\n".join(rows)
    opts = "\n".join(f'<option value="{val}">{label}</option>' for label, val in VIEW_MODES)
    page = (PAGE.replace("__CONTROLS__", controls).replace("__VIEWS__", opts)
            .replace("__HEAD__", terrain3d.HEAD).replace("__EXROW__", terrain3d.EX_ROW))
    return Response(page, mimetype="text/html")


@app.route("/regen", methods=["POST"])
def regen():
    doc = load_doc()
    apply_form(doc, request.form)
    deploy_only(doc)  # preview only — does NOT touch the committed source
    try:
        run_genmap(int(request.form.get("__pixels__", 640)),
                   int(request.form.get("__step__", 8)),
                   request.form.get("__view__", "top"))
    except subprocess.CalledProcessError as e:
        return Response(e.stderr.decode(errors="ignore") or "genmap failed", status=500)
    return "ok"


@app.route("/save", methods=["POST"])
def save():
    doc = load_doc()
    apply_form(doc, request.form)
    save_source(doc)  # persist the current edits to assets/biomes.yaml
    return "saved"


@app.route("/restore", methods=["POST"])
def restore():
    shutil.copyfile(SRC_BIOMES, DEPLOY_BIOMES)  # discard unsaved edits (revert preview)
    return "restored"


@app.route("/map.png")
def map_png():
    return send_file(MAP_OUT, mimetype="image/png")




PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>worldgen tool</title>
<style>
 body{margin:0;background:#1b1a1a;color:#eaddc7;font:13px system-ui;display:flex}
 #side{width:340px;padding:14px;overflow:auto;height:100vh;box-sizing:border-box}
 #main{flex:1;position:relative;background:#0c1014;overflow:hidden}
 #map{position:absolute;inset:0;margin:auto;max-width:96%;max-height:96vh;image-rendering:pixelated;border:1px solid #333}
 #gl{position:absolute;inset:0;width:100%;height:100%;display:none}
 .r{display:grid;grid-template-columns:120px 1fr 52px;gap:6px;align-items:center;margin:5px 0}
 .r4{display:grid;grid-template-columns:74px 1fr 1fr 0.8fr;gap:4px;align-items:center;margin:3px 0}
 .r4 span{display:flex;align-items:center;gap:2px;color:#9a8f7d;font-size:11px}
 .r4 input[type=number]{width:100%;background:#262030;color:#eaddc7;border:1px solid #4a3a5e;border-radius:3px;padding:2px 3px;font-size:11px}
 .grp{margin:12px 0 4px;font-weight:bold;color:#9fd0a0;font-size:12px}
 label{font-size:12px}.v{text-align:right;color:#c9a3e8}
 input[type=range]{width:100%}
 h2{margin:4px 0 12px;font-size:15px}.hint{color:#9a8f7d;font-size:11px;margin-bottom:10px}
 .row{display:flex;gap:8px;margin:8px 0}.row label{width:60px}
 button{background:#3a2f4a;color:#eaddc7;border:1px solid #5a4a6a;border-radius:5px;
   padding:6px 14px;cursor:pointer;font:13px system-ui}button:hover{background:#4a3a5e}
 #ex_row{display:none}
</style>
__HEAD__
</head><body>
<div id="side">
 <h2>worldgen tool</h2>
 <div class="hint">Drag a slider → biomes.yaml is patched and the game's --genmap re-runs, live.
 <b>3D terrain</b> renders the generator's real 3D solidity (overhangs, floating islands,
 sea) as voxels: <b>size</b> = blocks of world shown, <b>blk/px</b> = blocks per voxel
 cell — size 1600 at blk/px 8 shows the whole island coarsely; drop blk/px to 1 for a
 block-true close-up. Layer rows (f/w/o = frequency, weight, octaves) edit the noise
 STACKS the game actually runs; splines + biome list are edited in the YAML directly.</div>
 <div class="row"><label>view</label><select id="view" style="flex:1">__VIEWS__</select></div>
 <div class="row"><label>size</label><input id="px" type="number" value="640" min="128" max="2048" step="64"></div>
 <div class="row"><label>blk/px</label><input id="st" type="number" value="8" min="1" max="32"></div>
 <div class="row"><button id="save">Save</button><button id="restore">Restore</button>
   <span id="dirty" style="align-self:center;color:#e0a060;font-size:12px"></span></div>
 __EXROW__
 __CONTROLS__
 <div id="status" style="margin-top:10px;color:#9a8f7d"></div>
</div>
<div id="main"><img id="map" src="/map.png?0"><canvas id="gl"></canvas></div>
<script>
let timer=null, lastGen=0, dirty=false;
function is3D(){ return document.getElementById('view').value==='3d'; }
// Slider edits are PREVIEW-only (written to the build copy, never the committed source)
// until you hit Save. The dirty flag tracks unsaved edits; Restore reverts to source.
function setDirty(b){ dirty=b; document.getElementById('dirty').textContent=b?'● unsaved':''; }
function onSlide(el){const v=document.getElementById('v_'+el.dataset.path);
  if(v) v.textContent=(+el.value);
  setDirty(true); clearTimeout(timer); timer=setTimeout(regen,160);}

function formData(){
  const fd=new FormData();
  document.querySelectorAll('#side [data-path]').forEach(s=>fd.append(s.dataset.path,s.value));
  fd.append('__pixels__',document.getElementById('px').value);
  fd.append('__step__',document.getElementById('st').value);
  fd.append('__view__',document.getElementById('view').value);
  return fd;
}

// 3D voxel terrain (t3dShow/t3dBuild) lives in /vendor/terrain3d.js.
function regen(){
  t3dShow(is3D());
  document.getElementById('status').textContent='generating…';
  fetch('/regen',{method:'POST',body:formData()}).then(r=>r.text().then(t=>{
    if(r.ok){ lastGen=Date.now();
      if(is3D()) t3dBuild('/voxels.bin?'+lastGen);
      else document.getElementById('map').src='/map.png?'+lastGen;
      document.getElementById('status').textContent='ok'; }
    else document.getElementById('status').textContent='error: '+t; }));}
document.getElementById('px').onchange=regen; document.getElementById('st').onchange=regen;
document.getElementById('view').onchange=regen;
document.getElementById('save').onclick=function(){
  document.getElementById('status').textContent='saving…';
  fetch('/save',{method:'POST',body:formData()}).then(r=>r.text().then(t=>{
    if(r.ok){ setDirty(false); document.getElementById('status').textContent='saved to biomes.yaml'; }
    else document.getElementById('status').textContent='error: '+t; }));};
document.getElementById('restore').onclick=function(){
  if(dirty && !confirm('Discard unsaved changes and reload from biomes.yaml?')) return;
  fetch('/restore',{method:'POST'}).then(()=>location.reload());};
regen();
</script></body></html>"""


if __name__ == "__main__":
    print("worldgen tool -> http://127.0.0.1:5000   (exe: %s)" % exe_path())
    app.run(host="127.0.0.1", port=5000, debug=False)
