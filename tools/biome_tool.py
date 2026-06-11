#!/usr/bin/env python3
"""Live biome / flora / cave tuning tool for the voxel game.

A localhost web app (sibling to tools/genmap_tool.py, which tunes terrain SHAPE):
drag sliders / pick dropdowns for the things that drive WHAT GROWS and WHAT'S
UNDERGROUND, and watch the biome map update.

It patches two data files in place (ruamel round-trip, comments preserved) and
deploys them to build/bin/assets so the game picks them up on next launch:
  * assets/biomes.yaml  — per-biome tree/bush density + plant & tree themes
  * assets/world.yaml   — caves, ravines, cave pools, ore densities/depths

The preview is the game's own `voxelgame --genmap` top-down biome map (the same
generator the game runs). NOTE: trees/plants and caves/ores are placed per-voxel
by World, not by the generator-only genmap, so they don't appear in the preview —
the map shows the BIOME distribution that drives them. Launch the game to see the
flora/caves themselves; this tool makes dialing the values a no-recompile loop.

    pip install flask ruamel.yaml
    python tools/biome_tool.py
    # open http://127.0.0.1:5002
"""
import os
import subprocess
import sys

try:
    from flask import Flask, request, send_file, Response
except ImportError:
    sys.exit("biome_tool needs Flask:  pip install flask ruamel.yaml")
try:
    from ruamel.yaml import YAML
except ImportError:
    sys.exit("biome_tool needs ruamel.yaml (preserves comments):  pip install ruamel.yaml")

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
MAP_OUT = os.path.join(HERE, "_biomemap.png")
EXE_CANDIDATES = [
    os.path.join(ROOT, "build", "bin", "Release", "voxelgame.exe"),
    os.path.join(ROOT, "build", "bin", "Debug", "voxelgame.exe"),
    os.path.join(ROOT, "build", "bin", "voxelgame"),
]

# The plant / tree theme choices the C++ loaders accept (TerrainGenerator parse).
PLANT_THEMES = ["none", "bush", "grass", "forest", "desert"]
TREE_SPECIES = ["oak", "birch", "pine", "maple", "willow"]

yaml = YAML()


def exe_path():
    for p in EXE_CANDIDATES:
        if os.path.exists(p):
            return p
    sys.exit("voxelgame executable not found — build it first (Release recommended).")


def load_docs():
    docs = {}
    for key, path in SRC.items():
        with open(path, encoding="utf-8") as f:
            docs[key] = yaml.load(f)
    return docs


def save_and_deploy(docs):
    for key, doc in docs.items():
        with open(SRC[key], "w", encoding="utf-8") as f:
            yaml.dump(doc, f)
        os.makedirs(os.path.dirname(DEPLOY[key]), exist_ok=True)
        with open(DEPLOY[key], "w", encoding="utf-8") as f:
            yaml.dump(doc, f)


def get_path(doc, dotted):
    node = doc
    for key in dotted.split("."):
        node = node[int(key)] if key.lstrip("-").isdigit() else node[key]
    return node


def set_path(doc, dotted, value):
    keys = dotted.split(".")
    node = doc
    for key in keys[:-1]:
        node = node[int(key)] if key.lstrip("-").isdigit() else node[key]
    last = keys[-1]
    node[int(last) if last.lstrip("-").isdigit() else last] = value


# Fixed world.yaml controls (file, label, dotted-path, min, max, step).
WORLD_SLIDERS = [
    ("Cave frequency",      "caves.frequency",            0.01, 0.08, 0.002),
    ("Cave threshold",      "caves.threshold",            0.0,  0.2,  0.005),
    ("Cavern threshold",    "caves.cavern_threshold",     0.3,  0.8,  0.01),
    ("Cavern max Y",        "caves.cavern_max_y",         8,    120,  1),
    ("Ravine width",        "caves.ravines.width",        0.0,  0.06, 0.002),
    ("Ravine frequency",    "caves.ravines.frequency",    0.0003, 0.004, 0.0001),
    ("Ravine max Y",        "caves.ravines.max_y",        10,   120,  1),
    ("Lava pool max Y",     "caves.pools.lava_max_y",     0,    40,   1),
    ("Cave water max Y",    "caves.pools.water_max_y",    0,    80,   1),
    ("Cave water chance",   "caves.pools.water_chance",   0.0,  1.0,  0.02),
    ("Coal density",        "ores.coal.density",          0.0,  0.05, 0.001),
    ("Iron density",        "ores.iron.density",          0.0,  0.04, 0.001),
    ("Gold density",        "ores.gold.density",          0.0,  0.02, 0.0005),
    ("Ruby density",        "ores.ruby.density",          0.0,  0.01, 0.0002),
    ("Emerald density",     "ores.emerald.density",       0.0,  0.01, 0.0002),
    ("Mythril density",     "ores.mythril.density",       0.0,  0.01, 0.0002),
]


def build_controls(docs):
    """Return HTML for all controls. Each control id is 'FILE::dotted.path'."""
    rows = []

    def slider(file, label, path, lo, hi, step):
        try:
            val = float(get_path(docs[file], path))
        except Exception:
            val = lo
        cid = f"{file}::{path}"
        return (f'<div class="r"><label title="{cid}">{label}</label>'
                f'<input type="range" min="{lo}" max="{hi}" step="{step}" value="{val}" '
                f'data-id="{cid}" oninput="onSlide(this)">'
                f'<span class="v" id="v_{cid}">{val:g}</span></div>')

    def choice(file, label, path, options):
        try:
            cur = str(get_path(docs[file], path))
        except Exception:
            cur = options[0]
        cid = f"{file}::{path}"
        opts = "".join(f'<option value="{o}"{" selected" if o == cur else ""}>{o}</option>'
                       for o in options)
        return (f'<div class="r"><label title="{cid}">{label}</label>'
                f'<select data-id="{cid}" onchange="onSlide(this)" class="sel">{opts}</select>'
                f'<span class="v"></span></div>')

    # --- Per-biome flora (dynamic from the biomes list) ---
    rows.append('<h3>Flora — per biome</h3>')
    biomes = docs["biomes"].get("biomes", [])
    for i, b in enumerate(biomes):
        name = b.get("name", f"biome {i}")
        rows.append(f'<div class="grp">{name}</div>')
        rows.append(slider("biomes", "trees", f"biomes.{i}.trees", 0.0, 0.1, 0.001))
        rows.append(slider("biomes", "bushes", f"biomes.{i}.bushes", 0.0, 0.2, 0.002))
        rows.append(choice("biomes", "plant", f"biomes.{i}.plant", PLANT_THEMES))
        rows.append(choice("biomes", "tree", f"biomes.{i}.tree", TREE_SPECIES))

    # --- Caves / ores (world.yaml) ---
    rows.append('<h3>Caves, ravines, pools &amp; ores</h3>')
    for label, path, lo, hi, step in WORLD_SLIDERS:
        rows.append(slider("world", label, path, lo, hi, step))

    return "\n".join(rows)


def run_genmap(pixels, step):
    subprocess.run([exe_path(), "--genmap", "--mapsize", str(pixels),
                    "--mapstep", str(step), "--out", MAP_OUT],
                   cwd=ROOT, check=True, capture_output=True)


app = Flask(__name__)


@app.route("/")
def index():
    docs = load_docs()
    page = PAGE.replace("__CONTROLS__", build_controls(docs))
    return Response(page, mimetype="text/html")


@app.route("/regen", methods=["POST"])
def regen():
    docs = load_docs()
    for cid, raw in request.form.items():
        if cid in ("__pixels__", "__step__"):
            continue
        try:
            file, path = cid.split("::", 1)
        except ValueError:
            continue
        # A theme dropdown sends a string; a slider sends a number.
        if raw in PLANT_THEMES or raw in TREE_SPECIES:
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
    save_and_deploy(docs)
    try:
        run_genmap(int(request.form.get("__pixels__", 640)),
                   int(request.form.get("__step__", 8)))
    except subprocess.CalledProcessError as e:
        return Response(e.stderr.decode(errors="ignore") or "genmap failed", status=500)
    return "ok"


@app.route("/map.png")
def map_png():
    return send_file(MAP_OUT, mimetype="image/png")


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>biome / flora tool</title>
<style>
 body{margin:0;background:#1b1a1a;color:#eaddc7;font:13px system-ui;display:flex}
 #side{width:360px;padding:14px;overflow:auto;height:100vh;box-sizing:border-box}
 #main{flex:1;display:flex;align-items:center;justify-content:center;background:#111}
 #map{max-width:96%;max-height:96vh;image-rendering:pixelated;border:1px solid #333}
 .r{display:grid;grid-template-columns:96px 1fr 52px;gap:6px;align-items:center;margin:4px 0}
 label{font-size:12px}.v{text-align:right;color:#c9a3e8}
 input[type=range]{width:100%}.sel{width:100%}
 h2{margin:4px 0 8px;font-size:15px}h3{margin:14px 0 6px;font-size:13px;color:#c9a3e8}
 .grp{margin:8px 0 2px;font-weight:bold;color:#9fd0a0}
 .hint{color:#9a8f7d;font-size:11px;margin-bottom:10px}
 .row{display:flex;gap:8px;margin:8px 0}.row label{width:60px}
</style></head><body>
<div id="side">
 <h2>biome / flora / cave tool</h2>
 <div class="hint">Edits assets/biomes.yaml + world.yaml (comments preserved) and
 re-runs --genmap. The map shows the BIOME surface (what drives flora); trees,
 plants, caves &amp; ores are placed per-voxel by the game, so launch it to see them.</div>
 <div class="row"><label>size</label><input id="px" type="number" value="640" min="128" max="2048" step="64"></div>
 <div class="row"><label>blk/px</label><input id="st" type="number" value="8" min="1" max="32"></div>
 __CONTROLS__
 <div id="status" style="margin-top:10px;color:#9a8f7d"></div>
</div>
<div id="main"><img id="map" src="/map.png?0"></div>
<script>
let timer=null;
function onSlide(el){const v=document.getElementById('v_'+el.dataset.id);
  if(v) v.textContent=(+el.value)||el.value;
  clearTimeout(timer); timer=setTimeout(regen,200);}
function regen(){
  const fd=new FormData();
  document.querySelectorAll('[data-id]').forEach(s=>fd.append(s.dataset.id,s.value));
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
    print("biome / flora tool → http://127.0.0.1:5002   (exe: %s)" % exe_path())
    app.run(host="127.0.0.1", port=5002, debug=False)
