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
# nested maps; "island.center.0" indexes a sequence element.
CONTROLS = [
    ("Sea level",            "sea_level",                 8,   240, 1),
    ("Snow line (rel)",      "snow_line",                 0,   120, 1),
    ("Island enabled (1/0)", "island.enabled",            0,   1,   1),
    ("Island radius",        "island.radius",             200, 4000, 10),
    ("Island inner frac",    "island.inner",              0.1, 0.9, 0.01),
    ("Island coast warp",    "island.coast_warp",         0,   800, 10),
    ("Island land base",     "island.land_base",          0,   40,  1),
    ("Island interior var",  "island.interior_var",       0,   30,  1),
    ("Continental freq",     "continentalness.frequency", 0.0008, 0.012, 0.0002),
    ("Erosion freq",         "erosion.frequency",         0.001, 0.012, 0.0002),
    ("Peaks freq",           "peaks.frequency",           0.002, 0.02, 0.0005),
    ("Temperature freq",     "temperature.frequency",     0.0003, 0.004, 0.0001),
    ("Humidity freq",        "humidity.frequency",        0.0003, 0.004, 0.0001),
]


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


def save_and_deploy(doc):
    with open(SRC_BIOMES, "w", encoding="utf-8") as f:
        yaml.dump(doc, f)
    os.makedirs(os.path.dirname(DEPLOY_BIOMES), exist_ok=True)
    with open(DEPLOY_BIOMES, "w", encoding="utf-8") as f:
        yaml.dump(doc, f)


def run_genmap(pixels, step):
    subprocess.run([exe_path(), "--genmap", "--mapsize", str(pixels),
                    "--mapstep", str(step), "--out", MAP_OUT],
                   cwd=ROOT, check=True, capture_output=True)


app = Flask(__name__)


@app.route("/")
def index():
    doc = load_doc()
    rows = []
    for label, path, lo, hi, step in CONTROLS:
        try:
            val = float(get_path(doc, path))
        except Exception:
            val = lo
        rows.append(
            f'<div class="r"><label>{label}</label>'
            f'<input type="range" min="{lo}" max="{hi}" step="{step}" value="{val}" '
            f'data-path="{path}" oninput="onSlide(this)">'
            f'<span class="v" id="v_{path}">{val:g}</span></div>')
    controls = "\n".join(rows)
    return Response(PAGE.replace("__CONTROLS__", controls), mimetype="text/html")


@app.route("/regen", methods=["POST"])
def regen():
    doc = load_doc()
    for path, raw in request.form.items():
        if path in ("__pixels__", "__step__"):
            continue
        try:
            val = float(raw)
            if val == int(val):
                val = int(val)
            set_path(doc, path, val)
        except Exception:
            pass
    save_and_deploy(doc)
    try:
        run_genmap(int(request.form.get("__pixels__", 640)),
                   int(request.form.get("__step__", 8)))
    except subprocess.CalledProcessError as e:
        return Response(e.stderr.decode(errors="ignore") or "genmap failed", status=500)
    return "ok"


@app.route("/map.png")
def map_png():
    return send_file(MAP_OUT, mimetype="image/png")


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>worldgen tool</title>
<style>
 body{margin:0;background:#1b1a1a;color:#eaddc7;font:13px system-ui;display:flex}
 #side{width:340px;padding:14px;overflow:auto;height:100vh;box-sizing:border-box}
 #main{flex:1;display:flex;align-items:center;justify-content:center;background:#111}
 #map{max-width:96%;max-height:96vh;image-rendering:pixelated;border:1px solid #333}
 .r{display:grid;grid-template-columns:120px 1fr 52px;gap:6px;align-items:center;margin:5px 0}
 label{font-size:12px}.v{text-align:right;color:#c9a3e8}
 input[type=range]{width:100%}
 h2{margin:4px 0 12px;font-size:15px}.hint{color:#9a8f7d;font-size:11px;margin-bottom:10px}
 .row{display:flex;gap:8px;margin:8px 0}.row label{width:60px}
</style></head><body>
<div id="side">
 <h2>worldgen tool</h2>
 <div class="hint">Drag a slider → biomes.yaml is patched and the game's --genmap re-runs.
 Blue = ocean (darker = deeper), green/tan/white = land/beach/snow, hillshade = relief.</div>
 <div class="row"><label>size</label><input id="px" type="number" value="640" min="128" max="2048" step="64"></div>
 <div class="row"><label>blk/px</label><input id="st" type="number" value="8" min="1" max="32"></div>
 __CONTROLS__
 <div id="status" style="margin-top:10px;color:#9a8f7d"></div>
</div>
<div id="main"><img id="map" src="/map.png?0"></div>
<script>
let timer=null;
function onSlide(el){document.getElementById('v_'+el.dataset.path).textContent=(+el.value);
  clearTimeout(timer); timer=setTimeout(regen,160);}
function regen(){
  const fd=new FormData();
  document.querySelectorAll('input[type=range]').forEach(s=>fd.append(s.dataset.path,s.value));
  fd.append('__pixels__',document.getElementById('px').value);
  fd.append('__step__',document.getElementById('st').value);
  document.getElementById('status').textContent='generating…';
  fetch('/regen',{method:'POST',body:fd}).then(r=>r.text().then(t=>{
    if(r.ok){document.getElementById('map').src='/map.png?'+Date.now();
             document.getElementById('status').textContent='ok';}
    else document.getElementById('status').textContent='error: '+t;}));}
document.getElementById('px').onchange=regen; document.getElementById('st').onchange=regen;
regen();
</script></body></html>"""


if __name__ == "__main__":
    print("worldgen tool → http://127.0.0.1:5000   (exe: %s)" % exe_path())
    app.run(host="127.0.0.1", port=5000, debug=False)
