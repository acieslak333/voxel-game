#!/usr/bin/env python3
"""Voxel-game tools hub.

A localhost page that launches the project's dev editors and browses game content.

  * TOOLS  — one card per editor (worldgen, recipes, features, particles…). Each
             has a single "Open" button: it starts that editor as a subprocess on
             its fixed port (if not already up) and opens it in a new tab. If a tool
             fails to start, its output is captured to tools/_hub_logs/<id>.log and
             shown inline instead of failing silently.
  * BROWSE — Blocks (from blocks.yaml), Items (items.yaml) and Recipes (recipes.yaml),
             shown by their baked 16x16 icons. Read-only; edit via the tools.

Ports: particles 5001 · recipes 5003 · features 5004 · structures 5007  (hub 5005)

Run from the repo root:
    pip install flask pyyaml        # editors also want ruamel.yaml
    python tools/hub.py             # -> http://127.0.0.1:5005
"""
import atexit
import io
import json
import os
import socket
import subprocess
import sys
import time

try:
    from flask import Flask, jsonify, send_file
except ImportError:
    sys.exit("hub needs Flask:  pip install flask")
try:
    import yaml
except ImportError:
    sys.exit("hub needs PyYAML:  pip install pyyaml")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ASSETS = os.path.join(ROOT, "assets")
ICONS = os.path.join(ASSETS, "textures", "icons")
TEXTURES = os.path.join(ASSETS, "textures")
BLOCKS_FILE = os.path.join(ASSETS, "blocks.yaml")
ITEMS_FILE = os.path.join(ASSETS, "items.yaml")
RECIPES_FILE = os.path.join(ASSETS, "recipes.yaml")
LOG_DIR = os.path.join(HERE, "_hub_logs")

HUB_PORT = 5005

# The editors. `port` must match each tool's hard-coded app.run() port.
TOOLS = [
    {"id": "recipe",   "name": "Recipes",       "script": "recipe_tool.py",   "port": 5003,
     "desc": "Build crafting recipes by picking blocks.",    "accent": "#f0b35b"},
    {"id": "feature",  "name": "Features",      "script": "feature_tool.py",  "port": 5004,
     "desc": "Procedural + hand-voxel objects (3D editor).", "accent": "#f0a35b"},
    {"id": "particle", "name": "Particles",     "script": "particle_tool.py", "port": 5001,
     "desc": "Edit and preview particle effects.",           "accent": "#c79bf0"},
]
TOOL_BY_ID = {t["id"]: t for t in TOOLS}

app = Flask(__name__)
_procs = {}  # id -> Popen of a tool we launched


# -----------------------------------------------------------------------------
#  Process manager
# -----------------------------------------------------------------------------
def port_open(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.2)
        return s.connect_ex(("127.0.0.1", port)) == 0


def is_running(tool):
    return port_open(tool["port"])


def log_tail(tool, lines=12):
    path = os.path.join(LOG_DIR, tool["id"] + ".log")
    if not os.path.exists(path):
        return ""
    with open(path, encoding="utf-8", errors="replace") as f:
        return "".join(f.readlines()[-lines:]).strip()


def start_tool(tool):
    """Start the tool if needed and wait for its port. Returns (running, was_running).
    Child output -> tools/_hub_logs/<id>.log so a crash is visible, not swallowed."""
    if is_running(tool):
        return True, True
    os.makedirs(LOG_DIR, exist_ok=True)
    logf = open(os.path.join(LOG_DIR, tool["id"] + ".log"), "w", encoding="utf-8")
    env = {**os.environ, "PYTHONIOENCODING": "utf-8"}  # avoid Windows console encode crashes
    _procs[tool["id"]] = subprocess.Popen(
        [sys.executable, os.path.join(HERE, tool["script"])],
        cwd=ROOT, stdout=logf, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env)
    for _ in range(40):  # up to ~4s for Flask to bind
        if port_open(tool["port"]):
            return True, False
        if _procs[tool["id"]].poll() is not None:
            break  # exited -> crashed
        time.sleep(0.1)
    return is_running(tool), False


@atexit.register
def _cleanup():
    for p in _procs.values():
        if p.poll() is None:
            p.terminate()


# -----------------------------------------------------------------------------
#  Asset parsing (read-only)
# -----------------------------------------------------------------------------
def _load_seq(path, key=None):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as f:
        doc = yaml.safe_load(f) or []
    return (doc.get(key, []) if key else doc) or []


def blocks_data():
    out = []
    for e in _load_seq(BLOCKS_FILE):
        name = e.get("name")
        if not name or name == "air":
            continue
        out.append({
            "name": name, "render": e.get("render", "cube"),
            "solid": bool(e.get("solid", False)), "opaque": bool(e.get("opaque", False)),
            "hardness": e.get("hardness", 0), "light": e.get("light", 0),
            "light_opacity": e.get("light_opacity", 15 if e.get("opaque") else 0),
            "preferred_tool": e.get("preferred_tool"),
        })
    return out


def items_data():
    out = []
    for e in _load_seq(ITEMS_FILE):
        if not e.get("name"):
            continue
        out.append({k: e.get(k) for k in
                    ("name", "tool", "tool_speed", "tier", "attack_damage",
                     "equip", "armor", "speed_mul", "regen")})
    return out


def recipes_data():
    out = []
    for r in _load_seq(RECIPES_FILE, key="recipes"):
        out.append({"output": r.get("output"), "count": r.get("count", 1),
                    "inputs": [{"item": i.get("item"), "count": i.get("count", 1)}
                               for i in (r.get("inputs") or [])]})
    return out


# -----------------------------------------------------------------------------
#  Routes
# -----------------------------------------------------------------------------
@app.route("/")
def index():
    return PAGE


@app.route("/api/tools/<tid>/open", methods=["POST"])
def api_open(tid):
    tool = TOOL_BY_ID.get(tid)
    if not tool:
        return jsonify({"error": "unknown tool"}), 404
    running, was_running = start_tool(tool)
    return jsonify({"running": running, "was_running": was_running,
                    "url": "http://127.0.0.1:%d" % tool["port"],
                    "log": "" if running else log_tail(tool)})


@app.route("/api/blocks")
def api_blocks():
    return jsonify(blocks_data())


@app.route("/api/items")
def api_items():
    return jsonify(items_data())


@app.route("/api/recipes")
def api_recipes():
    return jsonify(recipes_data())


_TRANSPARENT_PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4"
    "890000000d49444154789c6360000002000100ffff03000006000557bfabd4"
    "0000000049454e44ae426082")


@app.route("/icon/<name>.png")
def icon(name):
    for path in (os.path.join(ICONS, name + ".png"),
                 os.path.join(TEXTURES, name + ".block.png")):
        if os.path.exists(path):
            return send_file(path, mimetype="image/png")
    return send_file(io.BytesIO(_TRANSPARENT_PNG), mimetype="image/png")


# -----------------------------------------------------------------------------
#  Page
# -----------------------------------------------------------------------------
PAGE = r"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Voxel Game — Tools</title>
<style>
  :root{--bg:#14161c;--panel:#1c1f28;--panel2:#232732;--line:#2c3240;--text:#e7ebf2;--dim:#9aa3b5}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--text);
       font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}
  header{padding:16px 24px;border-bottom:1px solid var(--line);font-size:17px;font-weight:600}
  .wrap{max-width:1100px;margin:0 auto;padding:22px 24px 60px}
  h2{font-size:12px;text-transform:uppercase;letter-spacing:1.1px;color:var(--dim);
     font-weight:600;margin:26px 0 12px}
  /* Tool cards */
  .tools{display:grid;grid-template-columns:repeat(auto-fill,minmax(250px,1fr));gap:14px}
  .card{background:var(--panel);border:1px solid var(--line);border-left:3px solid var(--c);
        border-radius:10px;padding:15px 16px;display:flex;flex-direction:column;gap:6px;
        transition:transform .1s,border-color .1s}
  .card:hover{transform:translateY(-2px)}
  .card .top{display:flex;align-items:baseline;gap:8px}
  .card h3{margin:0;font-size:15px}
  .card .port{color:var(--dim);font-size:11.5px;font-variant-numeric:tabular-nums}
  .card p{margin:2px 0 10px;color:var(--dim);font-size:12.5px;flex:1}
  .open{align-self:flex-start;font:inherit;font-weight:600;border:none;color:#0c1018;
        background:var(--c);padding:8px 22px;border-radius:7px;cursor:pointer}
  .open:hover{filter:brightness(1.08)}
  .open:disabled{opacity:.6;cursor:default}
  .err{color:#ff8a8a;font-size:12px;white-space:pre-wrap;margin:12px 0 0;
       font-family:Consolas,monospace;display:none;background:#251a1c;border:1px solid #50313a;
       border-radius:8px;padding:10px 12px}
  /* Tabs + grids */
  .tabs{display:flex;gap:4px;border-bottom:1px solid var(--line)}
  .tab{padding:9px 16px;cursor:pointer;color:var(--dim);border-bottom:2px solid transparent;margin-bottom:-1px}
  .tab.sel{color:var(--text);border-bottom-color:#6db4ff}
  .tab .n{color:var(--dim);font-size:11px}
  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(116px,1fr));gap:11px;margin-top:16px}
  .tile{background:var(--panel);border:1px solid var(--line);border-radius:9px;padding:11px 8px;text-align:center}
  .tile img{width:46px;height:46px;image-rendering:pixelated;filter:drop-shadow(0 2px 3px rgba(0,0,0,.4))}
  .tile .nm{margin-top:6px;font-size:12px;word-break:break-word}
  .tile .meta{margin-top:3px;font-size:10.5px;color:var(--dim);line-height:1.4}
  .badge{display:inline-block;padding:1px 6px;border-radius:6px;background:var(--panel2);
         border:1px solid var(--line);font-size:10px;margin:1px}
  .hidden{display:none}
  .recipe{display:flex;align-items:center;gap:8px;background:var(--panel);border:1px solid var(--line);
          border-radius:9px;padding:9px 13px;margin-bottom:9px;flex-wrap:wrap}
  .recipe img{width:34px;height:34px;image-rendering:pixelated}
  .ing{display:flex;align-items:center;gap:5px;background:var(--panel2);border:1px solid var(--line);
       border-radius:7px;padding:3px 8px 3px 4px;font-size:12px}
  .arrow{color:var(--dim);font-size:18px;margin:0 3px}
  .out{display:flex;align-items:center;gap:6px;font-weight:600}
  /* Embedded workspace: open tools as tabs in iframes inside the hub */
  .row2{display:flex;gap:8px}
  .ghost{background:var(--panel2)!important;color:var(--text)!important;padding:8px 12px!important}
  /* Break the workspace OUT of the centered max-width .wrap so the embedded tool
     gets the full window width (the editors need ~1000px of side panel + 3D view). */
  #workspace{width:100vw;margin-left:calc(50% - 50vw);margin-bottom:14px;
             border-top:1px solid var(--line);border-bottom:1px solid var(--line);
             overflow:hidden;background:var(--panel)}
  .wstabs{display:flex;gap:3px;background:var(--panel2);border-bottom:1px solid var(--line);padding:5px 14px 0;flex-wrap:wrap;align-items:center}
  .wstab{background:var(--panel);border:1px solid var(--line);border-bottom:0;border-radius:8px 8px 0 0;
         color:var(--dim);padding:7px 14px;cursor:pointer;font:inherit;font-weight:600}
  .wstab.sel{color:var(--text);background:var(--bg)}
  .wstab .x{margin-left:9px;opacity:.55;font-size:11px}.wstab .x:hover{opacity:1;color:#ff8a8a}
  .wsfull{order:99;margin-left:auto;background:none;border:0;color:var(--dim);font-size:12px;cursor:pointer;padding:7px 12px}.wsfull:hover{color:var(--text)}
  .wsframes{height:90vh}
  .wsframe{width:100%;height:100%;min-height:600px;border:0;display:block;background:#0c1014}
  /* Maximised: the workspace fills the whole viewport (Esc / the ⤢ button toggles). */
  #workspace.max{position:fixed;inset:0;width:100vw;margin:0;z-index:50}
  #workspace.max .wsframes{height:calc(100vh - 42px)}
</style></head>
<body>
<header>Voxel Game — Tools</header>
<div class="wrap">
  <div id="workspace" class="hidden">
    <div class="wstabs" id="wstabs"><button class="wsfull" onclick="toggleMax()" title="maximise / restore (Esc)">⤢ maximise</button></div>
    <div class="wsframes" id="wsframes"></div>
  </div>
  <h2>Editors <span class="n" style="text-transform:none;letter-spacing:0">— "Open here" docks the tool as a tab above; "↗" opens a separate browser tab</span></h2>
  <div class="tools" id="tools"></div>
  <div class="err" id="err"></div>

  <h2>Content</h2>
  <div class="tabs" id="tabs">
    <div class="tab sel" data-tab="blocks">Blocks <span class="n" id="c-blocks"></span></div>
    <div class="tab" data-tab="items">Items <span class="n" id="c-items"></span></div>
    <div class="tab" data-tab="recipes">Recipes <span class="n" id="c-recipes"></span></div>
  </div>
  <div id="panel-blocks"><div class="grid" id="blocks"></div></div>
  <div id="panel-items" class="hidden"><div class="grid" id="items"></div></div>
  <div id="panel-recipes" class="hidden"><div id="recipes" style="margin-top:16px"></div></div>
</div>

<script>
const TOOLS = __TOOLS__;
const $ = s => document.querySelector(s);
const ic = n => `/icon/${n}.png`;
const esc = s => (s==null?'':String(s));

// ---- Tools (Open-only) -----------------------------------------------------
$('#tools').innerHTML = TOOLS.map(t => `
  <div class="card" style="--c:${t.accent}">
    <div class="top"><h3>${esc(t.name)}</h3><span class="port">:${t.port}</span></div>
    <p>${esc(t.desc)}</p>
    <div class="row2">
      <button class="open" id="b-${t.id}" onclick="openTool('${t.id}',true)">Open here</button>
      <button class="open ghost" onclick="openTool('${t.id}',false)" title="open in a separate browser tab">↗</button>
    </div>
  </div>`).join('');

const OPEN = {};  // id -> {frame, tab}  (tools docked in the workspace)
async function openTool(id, embed){
  const btn = $('#b-'+id), err = $('#err'); err.style.display='none';
  const tab = embed ? null : window.open('about:blank','_blank');  // new tab opened within the click
  if(btn){ btn.disabled = true; var label = btn.textContent; btn.textContent = 'Starting…'; }
  try{
    const d = await (await fetch('/api/tools/'+id+'/open',{method:'POST'})).json();
    if (d.running){ if(embed) dock(id, d.url); else { if(tab) tab.location=d.url; else location=d.url; } }
    else { if(tab) tab.close(); err.textContent = id+' failed to start:\n'+(d.log||'(no output)'); err.style.display='block'; }
  }catch(e){ if(tab) tab.close(); err.textContent = id+' error: '+e; err.style.display='block'; }
  if(btn){ btn.disabled=false; btn.textContent=label; }
}
// Dock a tool as an iframe + tab in the workspace (one window for everything).
function dock(id, url){
  $('#workspace').classList.remove('hidden');
  if(!OPEN[id]){
    const t = TOOLS.find(x=>x.id===id) || {name:id};
    const fr = document.createElement('iframe'); fr.src = url; fr.className='wsframe'; fr.dataset.id=id;
    $('#wsframes').appendChild(fr);
    const tb = document.createElement('button'); tb.className='wstab'; tb.dataset.id=id;
    tb.innerHTML = esc(t.name)+' <span class="x" title="close">✕</span>';
    tb.onclick = ev => { if(ev.target.classList.contains('x')) closeDock(id); else showDock(id); };
    $('#wstabs').appendChild(tb);
    OPEN[id] = {frame:fr, tab:tb};
  }
  showDock(id); window.scrollTo(0,0);
}
function showDock(id){ for(const k in OPEN){ OPEN[k].frame.style.display=(k===id)?'':'none';
  OPEN[k].tab.classList.toggle('sel', k===id); } }
function closeDock(id){ if(!OPEN[id])return; OPEN[id].frame.remove(); OPEN[id].tab.remove(); delete OPEN[id];
  const ids=Object.keys(OPEN); if(ids.length) showDock(ids[0]); else { $('#workspace').classList.remove('max'); $('#workspace').classList.add('hidden'); } }
function toggleMax(){ $('#workspace').classList.toggle('max'); window.scrollTo(0,0); }
document.addEventListener('keydown',e=>{ if(e.key==='Escape') $('#workspace').classList.remove('max'); });

// ---- Content browser -------------------------------------------------------
function tile(name, meta){
  return `<div class="tile"><img src="${ic(name)}" alt="${esc(name)}" loading="lazy">
          <div class="nm">${esc(name)}</div><div class="meta">${meta||''}</div></div>`;
}
(async () => {
  const b = await (await fetch('/api/blocks')).json();
  $('#c-blocks').textContent = b.length;
  $('#blocks').innerHTML = b.map(x=>{
    const tags=[];
    if(x.render!=='cube') tags.push(`<span class="badge">${x.render}</span>`);
    if(x.light>0) tags.push(`<span class="badge">light ${x.light}</span>`);
    if(x.light_opacity>0 && x.light_opacity<15) tags.push(`<span class="badge">shade ${x.light_opacity}</span>`);
    const hard = x.hardness<0?'∞':x.hardness;
    return tile(x.name, `hardness ${hard}${x.preferred_tool?' · '+x.preferred_tool:''}<br>${tags.join(' ')}`);
  }).join('');

  const it = await (await fetch('/api/items')).json();
  $('#c-items').textContent = it.length;
  $('#items').innerHTML = it.map(x=>{
    const m=[];
    if(x.tool) m.push(`<span class="badge">${x.tool} T${x.tier??'?'}</span>`);
    if(x.tool_speed) m.push(`${x.tool_speed}× speed`);
    if(x.attack_damage) m.push(`atk ${x.attack_damage}`);
    if(x.equip) m.push(`<span class="badge">${x.equip}</span>`);
    if(x.armor) m.push(`armor ${x.armor}`);
    if(x.speed_mul) m.push(`speed ${x.speed_mul}×`);
    if(x.regen) m.push(`regen +${x.regen}`);
    return tile(x.name, m.join('<br>'));
  }).join('');

  const rs = await (await fetch('/api/recipes')).json();
  $('#c-recipes').textContent = rs.length;
  $('#recipes').innerHTML = rs.map(r=>`
    <div class="recipe">
      ${r.inputs.map(i=>`<span class="ing"><img src="${ic(i.item)}">${esc(i.item)} ×${i.count}</span>`)
        .join('<span class="arrow">+</span>')}
      <span class="arrow">→</span>
      <span class="out"><img src="${ic(r.output)}">${esc(r.output)}${r.count>1?' ×'+r.count:''}</span>
    </div>`).join('') || '<div style="color:var(--dim)">no recipes</div>';
})();

document.querySelectorAll('.tab').forEach(t=>t.onclick=()=>{
  document.querySelectorAll('.tab').forEach(x=>x.classList.toggle('sel',x===t));
  for(const n of ['blocks','items','recipes']) $('#panel-'+n).classList.toggle('hidden', n!==t.dataset.tab);
});
</script>
</body></html>""".replace("__TOOLS__", json.dumps(
    [{"id": t["id"], "name": t["name"], "desc": t["desc"], "port": t["port"], "accent": t["accent"]}
     for t in TOOLS]))


if __name__ == "__main__":
    url = "http://127.0.0.1:%d" % HUB_PORT
    print("tools hub -> %s" % url)
    if "--no-browser" not in sys.argv:
        import threading
        import webbrowser
        threading.Timer(1.2, lambda: webbrowser.open(url)).start()
    app.run(host="127.0.0.1", port=HUB_PORT, debug=False)
