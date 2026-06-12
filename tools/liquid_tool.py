#!/usr/bin/env python3
"""Liquid-pool editor — tune where cave fluids appear (world.yaml caves.pools).

Caves flood with fluid as they're carved: a carved cell at/below `lava_max_y`
becomes lava (deep magma); higher up (<= `water_max_y`) a carved cell resting on a
solid floor becomes a shallow water film with probability `water_chance`. This is a
small form over those three knobs; it round-trips world.yaml with ruamel so your
comments and the rest of the file are preserved.

Run from the repo root (or via tools/hub.py):
    pip install flask ruamel.yaml
    python tools/liquid_tool.py         # -> http://127.0.0.1:5006
"""
import os
import sys

try:
    from flask import Flask, jsonify, request
except ImportError:
    sys.exit("liquid tool needs Flask:  pip install flask")
try:
    from ruamel.yaml import YAML
    _yaml = YAML()
    _yaml.preserve_quotes = True
except ImportError:
    YAML = None
    import yaml as _pyyaml

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORLD_FILE = os.path.join(ROOT, "assets", "world.yaml")
PORT = 5006
app = Flask(__name__)

DEFAULTS = {"lava_max_y": 8, "water_max_y": 32, "water_chance": 0.46}


def _load():
    if YAML:
        with open(WORLD_FILE, encoding="utf-8") as f:
            return _yaml.load(f)
    with open(WORLD_FILE, encoding="utf-8") as f:
        return _pyyaml.safe_load(f)


def pools():
    try:
        p = (_load().get("caves") or {}).get("pools") or {}
    except Exception:
        p = {}
    return {k: p.get(k, DEFAULTS[k]) for k in DEFAULTS}


@app.route("/")
def index():
    return PAGE


@app.route("/api/pools")
def api_pools():
    return jsonify(pools())


@app.route("/api/save", methods=["POST"])
def api_save():
    d = request.get_json(force=True)
    if not YAML:
        return jsonify({"ok": False, "error": "ruamel.yaml not installed (pip install ruamel.yaml)"})
    doc = _load()
    doc.setdefault("caves", {})
    doc["caves"].setdefault("pools", {})
    doc["caves"]["pools"]["lava_max_y"] = int(d["lava_max_y"])
    doc["caves"]["pools"]["water_max_y"] = int(d["water_max_y"])
    doc["caves"]["pools"]["water_chance"] = float(d["water_chance"])
    with open(WORLD_FILE, "w", encoding="utf-8") as f:
        _yaml.dump(doc, f)
    return jsonify({"ok": True})


PAGE = r"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Voxel Game — Liquid Pools</title>
<style>
  :root{--bg:#14161c;--panel:#1c1f28;--panel2:#232732;--line:#2c3240;--text:#e7ebf2;--dim:#9aa3b5;--acc:#5bc0f0}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--text);font:14px/1.6 -apple-system,Segoe UI,Roboto,sans-serif}
  header{padding:13px 20px;border-bottom:1px solid var(--line);font-size:16px;font-weight:600}
  .wrap{max-width:560px;margin:0 auto;padding:24px 22px}
  .card{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:18px 20px;margin-bottom:16px}
  label{display:block;font-size:12px;color:var(--dim);margin-bottom:4px}
  .val{float:right;color:var(--text);font-variant-numeric:tabular-nums}
  input[type=range]{width:100%}
  .desc{color:var(--dim);font-size:12.5px;margin:2px 0 14px}
  .btn{background:var(--acc);color:#08121a;border:none;border-radius:8px;padding:9px 22px;font:inherit;font-weight:600;cursor:pointer}
  .status{color:var(--dim);margin-left:10px;font-size:13px}
  .legend{display:flex;gap:18px;margin-top:6px}
  .legend span{display:flex;align-items:center;gap:6px;font-size:12.5px;color:var(--dim)}
  .sw{width:14px;height:14px;border-radius:3px;border:1px solid var(--line)}
</style></head>
<body>
<header>Liquid Pools <span style="color:var(--dim);font-size:12px;font-weight:400">· cave fluids (world.yaml caves.pools)</span></header>
<div class="wrap">
  <div class="card">
    <label>Lava up to Y <span class="val" id="v-lava">8</span></label>
    <input id="lava" type="range" min="0" max="64">
    <div class="desc">Carved cells at or below this world height fill with <b style="color:#e0682a">lava</b> — the deep magma at the bottom of cave systems. Raise it for more lava, lower for safer depths.</div>
    <label>Water up to Y <span class="val" id="v-wmax">32</span></label>
    <input id="wmax" type="range" min="0" max="120">
    <div class="desc">Above the lava band and up to this height, carved cells resting on solid floor can become a shallow <b style="color:#5bc0f0">water</b> film.</div>
    <label>Water chance <span class="val" id="v-wc">0.46</span></label>
    <input id="wc" type="range" min="0" max="1" step="0.01">
    <div class="desc">Probability a qualifying carved cell actually floods with water (0 = dry caves, 1 = very wet).</div>
    <div class="legend"><span><span class="sw" style="background:#e0682a"></span>lava</span><span><span class="sw" style="background:#5bc0f0"></span>water</span></div>
  </div>
  <button class="btn" id="save">Save to world.yaml</button>
  <span class="status" id="status"></span>
  <p class="desc" style="margin-top:18px">Changes apply on the next world generation (a fresh world / relaunch).</p>
</div>
<script>
const $=s=>document.querySelector(s);
function sync(){$('#v-lava').textContent=$('#lava').value;$('#v-wmax').textContent=$('#wmax').value;$('#v-wc').textContent=(+$('#wc').value).toFixed(2);}
(async()=>{
  const p=await(await fetch('/api/pools')).json();
  $('#lava').value=p.lava_max_y;$('#wmax').value=p.water_max_y;$('#wc').value=p.water_chance;sync();
  ['lava','wmax','wc'].forEach(id=>$('#'+id).oninput=sync);
  $('#save').onclick=async()=>{
    const r=await(await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({lava_max_y:+$('#lava').value,water_max_y:+$('#wmax').value,water_chance:+$('#wc').value})})).json();
    $('#status').textContent=r.ok?'saved ✓':('save failed: '+(r.error||''));
  };
})();
</script></body></html>"""

if __name__ == "__main__":
    url = "http://127.0.0.1:%d" % PORT
    print("liquid tool -> %s" % url)
    if "--no-browser" not in sys.argv:
        import threading, webbrowser
        threading.Timer(1.2, lambda: webbrowser.open(url)).start()
    app.run(host="127.0.0.1", port=PORT, debug=False)
