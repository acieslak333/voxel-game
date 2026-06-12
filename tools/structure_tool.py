#!/usr/bin/env python3
"""Structure editor — paint hand-authored voxel templates (assets/structures/*.yaml).

Structures are FIXED voxel templates stamped on land during generation (wells,
boulders, ruins). Unlike procedural features they're drawn by hand, cell by cell.
This tool is a layer-by-layer voxel painter: pick a block, click cells on the
current Y layer, and it exports the legend + layers format the engine loads
(src/world/Structure.cpp). Empty cells = `skip` (leave the terrain).

`anchor` is the cell that meets the surface: anchor.y is the layer sitting at
ground level (cells below it are buried foundation, cells above build up).

Run from the repo root (or via tools/hub.py):
    pip install flask pyyaml pillow
    python tools/structure_tool.py      # -> http://127.0.0.1:5007
"""
import io
import os
import sys

try:
    from flask import Flask, jsonify, request, send_file
except ImportError:
    sys.exit("structure tool needs Flask:  pip install flask")
try:
    import yaml
except ImportError:
    sys.exit("structure tool needs PyYAML:  pip install pyyaml")
try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")
ICONS = os.path.join(ASSETS, "textures", "icons")
TEXTURES = os.path.join(ASSETS, "textures")
BLOCKS_FILE = os.path.join(ASSETS, "blocks.yaml")
STRUCT_DIR = os.path.join(ASSETS, "structures")

PORT = 5007
app = Flask(__name__)


# -----------------------------------------------------------------------------
#  Assets
# -----------------------------------------------------------------------------
def _load_yaml(path, default):
    if not os.path.exists(path):
        return default
    with open(path, encoding="utf-8") as f:
        return yaml.safe_load(f) or default


def _avg_color(name):
    if Image is None:
        h = abs(hash(name))
        return "#%02x%02x%02x" % (120 + h % 90, 110 + (h >> 8) % 90, 100 + (h >> 16) % 90)
    for path in (os.path.join(ICONS, name + ".png"), os.path.join(TEXTURES, name + ".block.png")):
        if os.path.exists(path):
            try:
                im = Image.open(path).convert("RGBA"); im.thumbnail((16, 16))
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
        if not name or name == "air" or e.get("tool"):
            continue
        out.append({"name": name, "color": _avg_color(name)})
    out.append({"name": "water", "color": "#3a5fb0"})  # ensure liquids in the palette
    return out


def struct_list():
    if not os.path.isdir(STRUCT_DIR):
        return []
    return sorted(os.path.splitext(f)[0] for f in os.listdir(STRUCT_DIR)
                  if f.endswith((".yaml", ".yml")))


def struct_to_cells(doc):
    """Parse a structure's legend+layers into {'x,y,z': blockname} + size + anchor."""
    legend = {".": "skip"}
    for k, v in (doc.get("legend") or {}).items():
        legend[str(k)[:1]] = v
    layers = doc.get("layers") or []
    cells = {}
    sy = len(layers); sz = len(layers[0]) if sy else 0
    sx = len(layers[0][0]) if sz else 0
    for y, layer in enumerate(layers):
        for z, row in enumerate(layer):
            for x, ch in enumerate(str(row)):
                blk = legend.get(ch, "skip")
                if blk not in ("skip", "."):
                    cells["%d,%d,%d" % (x, y, z)] = blk
    anchor = doc.get("anchor") or [sx // 2, 0, sz // 2]
    return {"name": doc.get("name", "structure"), "size": [sx, sy, sz],
            "anchor": anchor, "weight": doc.get("weight", 1.0),
            "surface": doc.get("surface", True), "cells": cells}


def cells_to_struct(data):
    """Build legend+layers YAML from the painter's cells."""
    sx, sy, sz = (int(v) for v in data["size"])
    cells = data.get("cells", {})
    # Assign a legend char per unique block.
    blocks = sorted({b for b in cells.values()})
    chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz"
    legend = {}
    char_of = {}
    for i, b in enumerate(blocks):
        c = chars[i % len(chars)]
        legend[c] = b
        char_of[b] = c
    layers = []
    for y in range(sy):
        rows = []
        for z in range(sz):
            row = ""
            for x in range(sx):
                b = cells.get("%d,%d,%d" % (x, y, z))
                row += char_of[b] if b else "."
            rows.append(row)
        layers.append(rows)
    out = {"name": data["name"], "weight": float(data.get("weight", 1.0)),
           "surface": bool(data.get("surface", True)),
           "anchor": [int(v) for v in data["anchor"]],
           "legend": {".": "skip", **legend}, "layers": layers}
    return out


# -----------------------------------------------------------------------------
#  Routes
# -----------------------------------------------------------------------------
@app.route("/")
def index():
    return PAGE


@app.route("/api/blocks")
def api_blocks():
    return jsonify(blocks_data())


@app.route("/api/structures")
def api_structures():
    return jsonify(struct_list())


@app.route("/api/structure/<name>")
def api_structure(name):
    return jsonify(struct_to_cells(_load_yaml(os.path.join(STRUCT_DIR, name + ".yaml"), {})))


@app.route("/api/save", methods=["POST"])
def api_save():
    data = request.get_json(force=True)
    name = "".join(c for c in (data.get("name") or "structure").lower().replace(" ", "_")
                   if c.isalnum() or c in "_-") or "structure"
    data["name"] = name
    os.makedirs(STRUCT_DIR, exist_ok=True)
    path = os.path.join(STRUCT_DIR, name + ".yaml")
    with open(path, "w", encoding="utf-8") as f:
        f.write("# Authored with tools/structure_tool.py\n")
        yaml.safe_dump(cells_to_struct(data), f, sort_keys=False, default_flow_style=None)
    return jsonify({"ok": True, "name": name, "path": os.path.relpath(path, ROOT)})


_TRANSPARENT = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4"
    "890000000d49444154789c6360000002000100ffff03000006000557bfabd4"
    "0000000049454e44ae426082")


@app.route("/icon/<name>.png")
def icon(name):
    for path in (os.path.join(ICONS, name + ".png"), os.path.join(TEXTURES, name + ".block.png")):
        if os.path.exists(path):
            return send_file(path, mimetype="image/png")
    return send_file(io.BytesIO(_TRANSPARENT), mimetype="image/png")


PAGE = r"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Voxel Game — Structures</title>
<style>
  :root{--bg:#14161c;--panel:#1c1f28;--panel2:#232732;--line:#2c3240;--text:#e7ebf2;--dim:#9aa3b5;--acc:#6db4ff}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--text);font:13.5px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}
  header{padding:13px 20px;border-bottom:1px solid var(--line);font-size:16px;font-weight:600;display:flex;align-items:center;gap:14px}
  header .sub{color:var(--dim);font-size:12px;font-weight:400}
  .layout{display:grid;grid-template-columns:1fr 360px;height:calc(100vh - 49px)}
  .main{overflow:auto;padding:16px 18px}
  .side{overflow:auto;padding:16px 18px;border-left:1px solid var(--line);background:#0f1116}
  h2{font-size:11px;text-transform:uppercase;letter-spacing:1.1px;color:var(--dim);font-weight:600;margin:16px 0 8px}
  label{display:block;color:var(--dim);font-size:11.5px;margin:6px 0 2px}
  input,select{background:var(--panel2);border:1px solid var(--line);color:var(--text);border-radius:6px;padding:5px 7px;font:inherit;width:100%}
  input[type=checkbox]{width:auto}.row{display:flex;gap:8px}.row>*{flex:1}
  .btn{background:var(--acc);color:#0c1018;border:none;border-radius:7px;padding:7px 16px;font:inherit;font-weight:600;cursor:pointer}
  .btn.ghost{background:var(--panel2);color:var(--text);border:1px solid var(--line)}
  .pill{display:inline-flex;align-items:center;gap:4px;background:var(--panel2);border:1px solid var(--line);border-radius:999px;padding:2px 6px 2px 4px;margin:2px;cursor:pointer;font-size:11.5px}
  .pill img{width:18px;height:18px;image-rendering:pixelated}.pill.sel{border-color:var(--acc);box-shadow:0 0 0 1px var(--acc)}
  .palette{max-height:150px;overflow:auto;background:var(--panel2);border:1px solid var(--line);border-radius:7px;padding:5px}
  canvas{background:#0b0d12;border:1px solid var(--line);border-radius:8px}
  .status{color:var(--dim);font-size:12px}
</style></head>
<body>
<header>Structures
  <span class="sub">hand-painted voxel templates · assets/structures/</span>
  <span style="flex:1"></span>
  <select id="load" style="width:auto"><option value="">— load existing —</option></select>
  <button class="btn" id="save">Save</button><span class="status" id="status"></span>
</header>
<div class="layout">
  <div class="main">
    <div class="row">
      <div><label>name</label><input id="s-name" value="my_structure"></div>
      <div><label>size x,y,z</label><div class="row"><input id="s-sx" type="number" value="5"><input id="s-sy" type="number" value="5"><input id="s-sz" type="number" value="5"></div></div>
    </div>
    <div class="row">
      <div><label>weight</label><input id="s-w" type="number" step="0.1" value="1.0"></div>
      <div><label>anchor y (ground layer)</label><input id="s-ay" type="number" value="0"></div>
      <div><label>surface only</label><input id="s-surf" type="checkbox" checked></div>
    </div>
    <h2>Paint — layer <span id="ylab"></span></h2>
    <input id="ysl" type="range" min="0" value="0">
    <label><input type="checkbox" id="mir"> mirror X</label>
    <div id="grid" style="margin:10px 0"></div>
    <p class="status">Pick a block at right, click cells to place; click again to erase. Empty = skip (terrain shows). The slider changes layer; faint cells are the layer below.</p>
  </div>
  <div class="side">
    <h2>Preview</h2>
    <canvas id="cv" width="320" height="280"></canvas>
    <h2>Palette</h2>
    <div id="palette" class="palette"></div>
  </div>
</div>
<script>
const $=s=>document.querySelector(s), ic=n=>`/icon/${n}.png`;
let BLOCKS=[],COLORS={},SEL=null,CELLS={},CURY=0;
const key=(x,y,z)=>x+','+y+','+z;
const sz3=()=>[+$('#s-sx').value,+$('#s-sy').value,+$('#s-sz').value];

function drawGrid(){
  const [sx,sy,sz]=sz3();
  $('#ysl').max=Math.max(0,sy-1); if(CURY>sy-1)CURY=sy-1;
  $('#ysl').value=CURY; $('#ylab').textContent='Y='+CURY+' / '+(sy-1);
  const g=$('#grid'); g.style.cssText='display:grid;grid-template-columns:repeat('+sx+',20px);gap:2px';
  g.innerHTML='';
  for(let z=0;z<sz;z++)for(let x=0;x<sx;x++){
    const c=document.createElement('div');
    c.style.cssText='width:20px;height:20px;border:1px solid #2c3240;border-radius:3px;cursor:pointer';
    const here=CELLS[key(x,CURY,z)], below=CURY>0?CELLS[key(x,CURY-1,z)]:null;
    if(here)c.style.background=COLORS[here]||'#9a8';
    else if(below){c.style.background=COLORS[below]||'#444';c.style.opacity=0.35;}
    else c.style.background='#0b0d12';
    c.title=here||'';
    c.onclick=()=>{
      if(CELLS[key(x,CURY,z)])delete CELLS[key(x,CURY,z)]; else CELLS[key(x,CURY,z)]=SEL||'cobblestone';
      if($('#mir').checked){const mx=sx-1-x;const mk=key(mx,CURY,z);if(CELLS[key(x,CURY,z)])CELLS[mk]=CELLS[key(x,CURY,z)];else delete CELLS[mk];}
      drawGrid();drawIso();
    };
    g.appendChild(c);
  }
}
function shade(hex,f){const n=parseInt((hex||'#888').slice(1),16);return `rgb(${Math.min(255,(n>>16&255)*f|0)},${Math.min(255,(n>>8&255)*f|0)},${Math.min(255,(n&255)*f|0)})`;}
function drawIso(){
  const cv=$('#cv'),ctx=cv.getContext('2d');ctx.clearRect(0,0,cv.width,cv.height);
  const [sx,sy,sz]=sz3();const T=Math.max(6,Math.min(16,(150/Math.max(sx+sz,sy))|0));
  const cx=cv.width/2,cy=40;
  const ks=Object.keys(CELLS).map(k=>k.split(',').map(Number)).sort((a,b)=>(a[0]+a[2]+a[1])-(b[0]+b[2]+b[1]));
  for(const[x,y,z]of ks){const col=COLORS[CELLS[key(x,y,z)]]||'#8a7d6b';
    const px=cx+(x-z)*T,py=cy+(x+z)*T*0.5-y*T;
    ctx.fillStyle=shade(col,1.18);ctx.beginPath();ctx.moveTo(px,py);ctx.lineTo(px+T,py+T*0.5);ctx.lineTo(px,py+T);ctx.lineTo(px-T,py+T*0.5);ctx.closePath();ctx.fill();
    ctx.fillStyle=shade(col,0.72);ctx.beginPath();ctx.moveTo(px-T,py+T*0.5);ctx.lineTo(px,py+T);ctx.lineTo(px,py+2*T);ctx.lineTo(px-T,py+T*1.5);ctx.closePath();ctx.fill();
    ctx.fillStyle=shade(col,0.92);ctx.beginPath();ctx.moveTo(px+T,py+T*0.5);ctx.lineTo(px,py+T);ctx.lineTo(px,py+2*T);ctx.lineTo(px+T,py+T*1.5);ctx.closePath();ctx.fill();}
}
function loadStruct(d){
  $('#s-name').value=d.name||'structure';const s=d.size||[5,5,5];
  $('#s-sx').value=s[0];$('#s-sy').value=s[1];$('#s-sz').value=s[2];
  $('#s-w').value=d.weight??1;$('#s-ay').value=(d.anchor||[0,0,0])[1];$('#s-surf').checked=d.surface!==false;
  CELLS={...(d.cells||{})};CURY=0;drawGrid();drawIso();
}
function gather(){const[sx,sy,sz]=sz3();return{name:$('#s-name').value,size:[sx,sy,sz],
  weight:+$('#s-w').value,surface:$('#s-surf').checked,
  anchor:[Math.floor(sx/2),+$('#s-ay').value,Math.floor(sz/2)],cells:CELLS};}
(async()=>{
  BLOCKS=await(await fetch('/api/blocks')).json();BLOCKS.forEach(b=>COLORS[b.name]=b.color);
  $('#palette').innerHTML=BLOCKS.map(b=>`<span class="pill" data-n="${b.name}"><img src="${ic(b.name)}">${b.name}</span>`).join('');
  document.querySelectorAll('#palette .pill').forEach(p=>p.onclick=()=>{SEL=p.dataset.n;document.querySelectorAll('#palette .pill').forEach(q=>q.classList.toggle('sel',q===p));$('#status').textContent='block: '+SEL;});
  const sl=await(await fetch('/api/structures')).json();
  $('#load').innerHTML='<option value="">— load existing —</option>'+sl.map(n=>`<option>${n}</option>`).join('');
  $('#load').onchange=async()=>{if($('#load').value)loadStruct(await(await fetch('/api/structure/'+$('#load').value)).json());};
  ['s-sx','s-sy','s-sz'].forEach(id=>$('#'+id).oninput=()=>{drawGrid();drawIso();});
  $('#ysl').oninput=()=>{CURY=+$('#ysl').value;drawGrid();};
  $('#save').onclick=async()=>{const r=await(await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(gather())})).json();
    $('#status').textContent=r.ok?'saved '+r.path:'save failed';
    const sl=await(await fetch('/api/structures')).json();$('#load').innerHTML='<option value="">— load existing —</option>'+sl.map(n=>`<option>${n}</option>`).join('');};
  drawGrid();drawIso();
})();
</script></body></html>"""

if __name__ == "__main__":
    url = "http://127.0.0.1:%d" % PORT
    print("structure tool -> %s" % url)
    if "--no-browser" not in sys.argv:
        import threading, webbrowser
        threading.Timer(1.2, lambda: webbrowser.open(url)).start()
    app.run(host="127.0.0.1", port=PORT, debug=False)
