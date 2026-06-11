#!/usr/bin/env python3
"""Interactive crafting-recipe editor for the voxel game.

A tiny localhost web app to build recipes with the mouse — pick an output block,
set how many it yields, add ingredient rows (block + count), and save. It writes
assets/recipes.yaml (the exact file vg::Crafting loads) preserving the file's
comments, and deploys a copy next to the built game. Block pickers show the real
block textures so you choose by icon, not by typing names.

    pip install flask ruamel.yaml
    python tools/recipe_tool.py
    # open http://127.0.0.1:5001

Nothing here re-implements crafting: it only edits the data file the game reads,
so what you save is exactly what the game uses.
"""
import io
import os
import sys

try:
    from flask import Flask, request, jsonify, send_file, Response
except ImportError:
    sys.exit("recipe_tool needs Flask:  pip install flask ruamel.yaml")
try:
    from ruamel.yaml import YAML
    from ruamel.yaml.comments import CommentedMap, CommentedSeq
except ImportError:
    sys.exit("recipe_tool needs ruamel.yaml (preserves comments):  pip install ruamel.yaml")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")
BLOCKS_FILE = os.path.join(ASSETS, "blocks.yaml")
RECIPES_FILE = os.path.join(ASSETS, "recipes.yaml")
TEXTURES = os.path.join(ASSETS, "textures")
DEPLOY_RECIPES = os.path.join(ROOT, "build", "bin", "assets", "recipes.yaml")

yaml = YAML()  # round-trip: preserves comments + ordering


# --- Data helpers (pure-ish; unit-tested via the Flask test client) ----------

def load_blocks():
    """Return [(name, texture_filename_or_None)] for every block except air."""
    with open(BLOCKS_FILE, encoding="utf-8") as f:
        doc = yaml.load(f)
    out = []
    for entry in doc:
        name = entry.get("name")
        if not name or name == "air":
            continue
        tex = None
        t = entry.get("textures")
        if t:
            # Pick a representative face for the icon (prefer all/top/side).
            for key in ("all", "top", "side", "posy", "negx", "posx", "negz", "posz", "bottom"):
                if key in t:
                    tex = t[key]
                    break
            if tex is None and len(t):
                tex = list(t.values())[0]
        out.append((str(name), tex))
    return out


def block_names():
    return {n for n, _ in load_blocks()}


def load_recipes_doc():
    """Load recipes.yaml as a round-trip doc; create a skeleton if missing."""
    if os.path.exists(RECIPES_FILE):
        with open(RECIPES_FILE, encoding="utf-8") as f:
            doc = yaml.load(f)
        if doc is None:
            doc = CommentedMap()
        if "recipes" not in doc or doc["recipes"] is None:
            doc["recipes"] = CommentedSeq()
        return doc
    doc = CommentedMap()
    doc["recipes"] = CommentedSeq()
    return doc


def recipes_list(doc):
    """A plain-python view of the recipes for the UI."""
    out = []
    for r in doc.get("recipes", []) or []:
        out.append({
            "output": str(r.get("output", "")),
            "count": int(r.get("count", 1)),
            "inputs": [{"item": str(i.get("item", "")), "count": int(i.get("count", 1))}
                       for i in (r.get("inputs", []) or [])],
        })
    return out


def validate(output, count, inputs, names):
    """Return an error string, or None if the recipe is valid."""
    if output not in names:
        return f"unknown output block '{output}'"
    if count < 1:
        return "output count must be >= 1"
    if not inputs:
        return "a recipe needs at least one ingredient"
    for it in inputs:
        if it["item"] not in names:
            return f"unknown ingredient '{it['item']}'"
        if it["count"] < 1:
            return "ingredient counts must be >= 1"
    return None


def make_recipe(output, count, inputs):
    """Build a CommentedMap recipe so the saved YAML matches the hand-authored style."""
    r = CommentedMap()
    r["output"] = output
    if count != 1:
        r["count"] = count
    seq = CommentedSeq()
    for it in inputs:
        m = CommentedMap()
        m["item"] = it["item"]
        m["count"] = it["count"]
        m.fa.set_flow_style()  # render as { item: x, count: n }
        seq.append(m)
    r["inputs"] = seq
    return r


def save_and_deploy(doc):
    with open(RECIPES_FILE, "w", encoding="utf-8") as f:
        yaml.dump(doc, f)
    # Deploy next to the built game if that tree exists, so a running build picks
    # it up on next launch (recipes load at startup).
    if os.path.isdir(os.path.dirname(DEPLOY_RECIPES)):
        with open(DEPLOY_RECIPES, "w", encoding="utf-8") as f:
            yaml.dump(doc, f)


# --- Web app -----------------------------------------------------------------

app = Flask(__name__)


@app.route("/")
def index():
    return Response(PAGE, mimetype="text/html")


@app.route("/blocks")
def blocks():
    return jsonify([{"name": n, "tex": (t is not None)} for n, t in load_blocks()])


@app.route("/icon/<name>.png")
def icon(name):
    for n, tex in load_blocks():
        if n == name and tex:
            path = os.path.join(TEXTURES, tex)
            if os.path.exists(path):
                return send_file(path, mimetype="image/png")
    # 1x1 transparent fallback so the <img> doesn't 404-flash.
    px = bytes.fromhex("89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4"
                       "890000000d49444154789c6360000002000100ffff03000006000557bfabd4"
                       "0000000049454e44ae426082")
    return send_file(io.BytesIO(px), mimetype="image/png")


@app.route("/recipes")
def recipes():
    return jsonify(recipes_list(load_recipes_doc()))


@app.route("/add", methods=["POST"])
def add():
    data = request.get_json(force=True, silent=True) or {}
    output = str(data.get("output", "")).strip()
    try:
        count = int(data.get("count", 1))
    except (TypeError, ValueError):
        count = 1
    inputs = []
    for it in data.get("inputs", []):
        try:
            inputs.append({"item": str(it.get("item", "")).strip(),
                           "count": int(it.get("count", 1))})
        except (TypeError, ValueError):
            return Response("bad ingredient count", status=400)
    err = validate(output, count, inputs, block_names())
    if err:
        return Response(err, status=400)
    doc = load_recipes_doc()
    doc["recipes"].append(make_recipe(output, count, inputs))
    save_and_deploy(doc)
    return jsonify(recipes_list(doc))


@app.route("/delete", methods=["POST"])
def delete():
    data = request.get_json(force=True, silent=True) or {}
    try:
        idx = int(data.get("index", -1))
    except (TypeError, ValueError):
        return Response("bad index", status=400)
    doc = load_recipes_doc()
    seq = doc.get("recipes", [])
    if 0 <= idx < len(seq):
        del seq[idx]
        save_and_deploy(doc)
        return jsonify(recipes_list(doc))
    return Response("index out of range", status=400)


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>recipe editor</title>
<style>
 body{margin:0;background:#1b1a1a;color:#eaddc7;font:14px system-ui;display:flex;height:100vh}
 #left{width:46%;padding:18px;overflow:auto;box-sizing:border-box;border-right:1px solid #333}
 #right{flex:1;padding:18px;overflow:auto;box-sizing:border-box}
 h2{margin:2px 0 14px;font-size:17px}.hint{color:#9a8f7d;font-size:12px;margin-bottom:14px}
 img.ic{width:28px;height:28px;image-rendering:pixelated;vertical-align:middle;
   background:#2a2826;border:1px solid #3a3734;border-radius:4px}
 select,input{background:#2a2826;color:#eaddc7;border:1px solid #444;border-radius:5px;
   padding:5px 6px;font:13px system-ui}
 input[type=number]{width:54px}
 button{background:#3a3431;color:#eaddc7;border:1px solid #555;border-radius:6px;
   padding:6px 11px;cursor:pointer;font:13px system-ui}
 button:hover{background:#4a423d}
 button.primary{background:#5b7a4a;border-color:#6c8c57}
 button.del{background:#7a3a3a;border-color:#8c4747;padding:3px 9px}
 .card{background:#211f1e;border:1px solid #383431;border-radius:9px;padding:14px;margin-bottom:14px}
 .row{display:flex;align-items:center;gap:8px;margin:7px 0;flex-wrap:wrap}
 .rec{display:flex;align-items:center;gap:8px;padding:9px;border-radius:8px;background:#211f1e;
   border:1px solid #34302d;margin-bottom:8px}
 .rec .arrow{color:#9a8f7d;margin:0 4px}.rec .sp{flex:1}
 .ing{display:inline-flex;align-items:center;gap:4px;margin-right:6px;color:#cdbfa6}
 #status{color:#9a8f7d;margin-left:8px}
 .name{min-width:120px}
</style></head><body>
<div id="left">
 <h2>recipes</h2>
 <div class="hint">Saved to assets/recipes.yaml (and deployed next to the build).
   The game loads it at launch.</div>
 <div id="list"></div>
</div>
<div id="right">
 <h2>new recipe</h2>
 <div class="card">
   <div class="row"><b>makes</b>
     <span id="outIcon"></span>
     <select id="output" class="name"></select>
     <span>x</span><input id="count" type="number" value="1" min="1">
   </div>
   <div style="margin:10px 0 4px;color:#9a8f7d">ingredients</div>
   <div id="inputs"></div>
   <div class="row"><button onclick="addRow()">+ add ingredient</button></div>
   <div class="row"><button class="primary" onclick="save()">Add recipe</button>
     <span id="status"></span></div>
 </div>
</div>
<script>
let BLOCKS=[];
const iconURL=n=>'/icon/'+encodeURIComponent(n)+'.png';
function opt(sel,val){ const s=document.createElement('select'); s.className='name';
  BLOCKS.forEach(b=>{const o=document.createElement('option');o.value=b.name;o.textContent=b.name;s.appendChild(o);});
  if(val)s.value=val; return s; }
function img(n){ const i=document.createElement('img'); i.className='ic'; i.src=iconURL(n); return i; }

function addRow(item, count){
  const wrap=document.getElementById('inputs');
  const row=document.createElement('div'); row.className='row';
  const ic=img(item||BLOCKS[0].name);
  const sel=opt(null,item); sel.onchange=()=>ic.src=iconURL(sel.value);
  const cnt=document.createElement('input'); cnt.type='number'; cnt.value=count||1; cnt.min=1;
  const del=document.createElement('button'); del.textContent='✕'; del.className='del';
  del.onclick=()=>row.remove();
  row.append(ic,sel,document.createTextNode('x'),cnt,del);
  wrap.appendChild(row);
}
function gather(){
  const inputs=[...document.querySelectorAll('#inputs .row')].map(r=>{
    const sel=r.querySelector('select'), cnt=r.querySelector('input');
    return {item:sel.value, count:parseInt(cnt.value)||1};
  });
  return {output:document.getElementById('output').value,
          count:parseInt(document.getElementById('count').value)||1, inputs};
}
function save(){
  fetch('/add',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(gather())}).then(async r=>{
    if(r.ok){ document.getElementById('status').textContent='saved'; render(await r.json()); }
    else document.getElementById('status').textContent='error: '+await r.text();
  });
}
function del(i){
  fetch('/delete',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({index:i})}).then(async r=>{ if(r.ok) render(await r.json()); });
}
function render(list){
  const el=document.getElementById('list'); el.innerHTML='';
  if(!list.length){ el.innerHTML='<div class="hint">no recipes yet</div>'; return; }
  list.forEach((r,i)=>{
    const d=document.createElement('div'); d.className='rec';
    d.appendChild(img(r.output));
    const lbl=document.createElement('b'); lbl.textContent=r.output+(r.count>1?' x'+r.count:'');
    d.appendChild(lbl);
    const ar=document.createElement('span'); ar.className='arrow'; ar.textContent='⟵'; d.appendChild(ar);
    r.inputs.forEach(it=>{ const s=document.createElement('span'); s.className='ing';
      s.appendChild(img(it.item)); s.appendChild(document.createTextNode((it.count>1?it.count+'x ':'')+it.item));
      d.appendChild(s); });
    const sp=document.createElement('span'); sp.className='sp'; d.appendChild(sp);
    const b=document.createElement('button'); b.className='del'; b.textContent='delete';
    b.onclick=()=>del(i); d.appendChild(b);
    el.appendChild(d);
  });
}
fetch('/blocks').then(r=>r.json()).then(b=>{
  BLOCKS=b;
  const out=document.getElementById('output');
  b.forEach(x=>{const o=document.createElement('option');o.value=x.name;o.textContent=x.name;out.appendChild(o);});
  const oi=document.getElementById('outIcon'); oi.appendChild(img(b[0].name));
  out.onchange=()=>oi.firstChild.src=iconURL(out.value);
  addRow();
  fetch('/recipes').then(r=>r.json()).then(render);
});
</script></body></html>"""


if __name__ == "__main__":
    print("recipe editor → http://127.0.0.1:5001   (edits %s)" % RECIPES_FILE)
    app.run(host="127.0.0.1", port=5001, debug=False)
