#!/usr/bin/env python3
"""Live particle-effect editor for the voxel game (.prtcl files).

    pip install flask
    python tools/particle_tool.py
    # open http://127.0.0.1:5001

Drag the sliders and watch the burst animate in the canvas (the JS preview mirrors
the C++ sim in src/entity/Particles.cpp: gravity, drag, an outward+up velocity
cone, a lifetime, and a linear size shrink). Pick an existing effect from the
dropdown to edit it, or type a new name; Save writes assets/particles/<name>.prtcl
in the exact key layout vg::ParticleEffect::load expects. The game loads these at
startup (e.g. break.prtcl for block-break chips), so re-launch to see changes.

This tool owns ONLY the data file; the runtime sim/render lives in the engine, so
the preview is an approximation for tuning, not a second source of truth.
"""
import os
import glob

from flask import Flask, request, jsonify, Response, send_from_directory

HERE = os.path.dirname(os.path.abspath(__file__))
PARTICLE_DIR = os.path.normpath(os.path.join(HERE, "..", "assets", "particles"))
TEXTURE_DIR = os.path.normpath(os.path.join(HERE, "..", "assets", "textures"))

DEFAULTS = {
    "name": "new_effect",
    "texture": "",
    "count": 14,
    "gravity": -22.0,
    "drag": 1.0,
    "spawn_radius": 0.30,
    "speed_min": 1.5, "speed_max": 3.5,
    "up_bias": 1.8,
    "life_min": 0.45, "life_max": 0.9,
    "size_min": 0.05, "size_max": 0.11,
    "size_end": 0.15,
    "spin_min": -10.0, "spin_max": 10.0,
}

app = Flask(__name__)


def parse_prtcl(text):
    """Minimal .prtcl reader (key: value / key: [a, b]); ignores comments."""
    p = dict(DEFAULTS)

    def num(s):
        try:
            return float(s)
        except ValueError:
            return None

    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or ":" not in line:
            continue
        key, val = line.split(":", 1)
        key, val = key.strip(), val.strip()
        if key == "name":
            p["name"] = val
        elif key == "texture":
            p["texture"] = val.strip('"').strip("'")
        elif key == "count":
            p["count"] = int(float(val))
        elif val.startswith("["):
            inner = val.strip("[]").split(",")
            if len(inner) >= 2:
                lo, hi = num(inner[0]), num(inner[1])
                base = {"speed": "speed", "lifetime": "life", "size": "size",
                        "spin": "spin"}.get(key)
                if base and lo is not None and hi is not None:
                    p[base + "_min"], p[base + "_max"] = lo, hi
        else:
            v = num(val)
            if v is None:
                continue
            mapping = {"gravity": "gravity", "drag": "drag",
                       "spawn_radius": "spawn_radius", "up_bias": "up_bias",
                       "size_end": "size_end"}
            if key in mapping:
                p[mapping[key]] = v
    return p


def write_prtcl(p):
    def f(x):
        return ("%g" % float(x))
    return f"""# Particle effect (ISSUES #13M). Authored in tools/particle_tool.py.
# Loaded by vg::ParticleEffect::load, played by vg::Particles (2D billboards).
name: {p['name']}
texture: "{p['texture']}"
count: {int(p['count'])}
gravity: {f(p['gravity'])}
drag: {f(p['drag'])}
spawn_radius: {f(p['spawn_radius'])}
speed: [{f(p['speed_min'])}, {f(p['speed_max'])}]
up_bias: {f(p['up_bias'])}
lifetime: [{f(p['life_min'])}, {f(p['life_max'])}]
size: [{f(p['size_min'])}, {f(p['size_max'])}]
size_end: {f(p['size_end'])}
spin: [{f(p['spin_min'])}, {f(p['spin_max'])}]
"""


@app.route("/")
def index():
    return Response(PAGE, mimetype="text/html")


@app.route("/list")
def list_effects():
    files = sorted(os.path.basename(f)[:-6]
                   for f in glob.glob(os.path.join(PARTICLE_DIR, "*.prtcl")))
    return jsonify(files)


@app.route("/textures")
def list_textures():
    files = sorted(os.path.basename(f)
                   for f in glob.glob(os.path.join(TEXTURE_DIR, "*.block.png")))
    return jsonify(files)


@app.route("/tex/<path:name>")
def serve_texture(name):
    return send_from_directory(TEXTURE_DIR, name)


@app.route("/load")
def load_effect():
    name = request.args.get("name", "")
    path = os.path.join(PARTICLE_DIR, name + ".prtcl")
    if not os.path.isfile(path):
        return jsonify(DEFAULTS)
    with open(path, "r", encoding="utf-8") as fh:
        return jsonify(parse_prtcl(fh.read()))


@app.route("/save", methods=["POST"])
def save_effect():
    p = dict(DEFAULTS)
    data = request.get_json(force=True)
    for k in DEFAULTS:
        if k in data:
            p[k] = data[k]
    name = "".join(c for c in str(p["name"]) if c.isalnum() or c in "_-") or "new_effect"
    p["name"] = name
    os.makedirs(PARTICLE_DIR, exist_ok=True)
    path = os.path.join(PARTICLE_DIR, name + ".prtcl")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(write_prtcl(p))
    return jsonify({"ok": True, "path": path})


PAGE = """<!doctype html><html><head><meta charset="utf-8"><title>Particle Tool</title>
<style>
 body{margin:0;font-family:system-ui,sans-serif;background:#1b1d23;color:#e8e2d4;display:flex}
 #side{width:340px;padding:16px;overflow-y:auto;height:100vh;box-sizing:border-box;background:#23262e}
 #stage{flex:1;display:flex;align-items:center;justify-content:center}
 canvas{background:linear-gradient(#5a86c0,#bcd0e8);border-radius:8px}
 h1{font-size:18px;margin:0 0 12px}
 label{display:block;font-size:12px;margin:10px 0 2px;color:#b9c0cc}
 .row{display:flex;gap:8px}.row>div{flex:1}
 input[type=range]{width:100%}
 input[type=text],select{width:100%;box-sizing:border-box;background:#2e323c;color:#e8e2d4;border:1px solid #3a3f4b;border-radius:5px;padding:5px}
 .val{float:right;color:#8fd6a0}
 button{margin-top:14px;width:100%;padding:9px;border:0;border-radius:6px;background:#5a86c0;color:#fff;font-weight:600;cursor:pointer}
 #msg{margin-top:8px;font-size:12px;color:#8fd6a0;min-height:14px}
</style></head><body>
<div id="side">
 <h1>Particle Tool</h1>
 <label>Open effect</label>
 <select id="open"></select>
 <label>Name</label><input type="text" id="name">
 <label>Texture</label><select id="tex"></select>
 <div id="sliders"></div>
 <button id="save">Save .prtcl</button>
 <div id="msg"></div>
</div>
<div id="stage"><canvas id="c" width="560" height="560"></canvas></div>
<script>
const SPEC=[ // key, label, min, max, step
 ["count","count",1,80,1],
 ["gravity","gravity",-60,10,0.5],
 ["drag","drag",0,8,0.1],
 ["spawn_radius","spawn radius",0,1.5,0.01],
 ["speed_min","speed min",0,10,0.1],["speed_max","speed max",0,10,0.1],
 ["up_bias","up bias",0,10,0.1],
 ["life_min","life min",0.05,4,0.05],["life_max","life max",0.05,4,0.05],
 ["size_min","size min",0.01,0.5,0.01],["size_max","size max",0.01,0.5,0.01],
 ["size_end","size end x",0,2,0.05],
 ["spin_min","spin min",-20,20,0.5],["spin_max","spin max",-20,20,0.5],
];
let P={};
const sl={};
const box=document.getElementById('sliders');
for(const [k,lab,mn,mx,st] of SPEC){
 const l=document.createElement('label');l.innerHTML=lab+' <span class="val" id="v_'+k+'"></span>';
 const i=document.createElement('input');i.type='range';i.min=mn;i.max=mx;i.step=st;i.id='s_'+k;
 i.oninput=()=>{P[k]=parseFloat(i.value);document.getElementById('v_'+k).textContent=(+P[k]).toFixed(2);};
 box.appendChild(l);box.appendChild(i);sl[k]=i;
}
let texImg=null;
function loadTex(){ if(P.texture){texImg=new Image();texImg.src='/tex/'+encodeURIComponent(P.texture);}else{texImg=null;} }
function setUI(p){P=Object.assign({},p);document.getElementById('name').value=p.name;
 document.getElementById('tex').value=p.texture||'';loadTex();
 for(const [k] of SPEC){sl[k].value=P[k];document.getElementById('v_'+k).textContent=(+P[k]).toFixed(2);}}
document.getElementById('tex').onchange=e=>{P.texture=e.target.value;loadTex();};
function refreshTextures(){fetch('/textures').then(r=>r.json()).then(ts=>{
 const t=document.getElementById('tex');t.innerHTML='<option value="">(source block)</option>';
 for(const f of ts){const op=document.createElement('option');op.value=f;op.textContent=f;t.appendChild(op);}
 t.value=P.texture||'';});}
function refreshList(sel){fetch('/list').then(r=>r.json()).then(fs=>{
 const o=document.getElementById('open');o.innerHTML='<option value="">(new)</option>';
 for(const f of fs){const op=document.createElement('option');op.value=f;op.textContent=f;o.appendChild(op);}
 if(sel)o.value=sel;});}
document.getElementById('open').onchange=e=>{const n=e.target.value;
 if(!n){fetch('/load?name=__none__').then(r=>r.json()).then(setUI);return;}
 fetch('/load?name='+encodeURIComponent(n)).then(r=>r.json()).then(setUI);};
document.getElementById('save').onclick=()=>{P.name=document.getElementById('name').value;
 fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(P)})
 .then(r=>r.json()).then(j=>{document.getElementById('msg').textContent='Saved '+j.path;refreshList(P.name);});};

// --- live 3D preview: mirrors src/entity/Particles.cpp (full x/y/z sim) ---
const cv=document.getElementById('c'),cx=cv.getContext('2d');
const CXp=cv.width/2, CYp=cv.height*0.60, FOCAL=cv.height*0.95;
let parts=[],timer=0;
let yaw=0.7,pitch=0.30,dist=6.5,autorot=true;
function rnd(){return Math.random();}

// Orbit with drag, zoom with the wheel (stops auto-rotation once you grab it).
let drag=false,lx=0,ly=0;
cv.addEventListener('mousedown',e=>{drag=true;autorot=false;lx=e.clientX;ly=e.clientY;});
window.addEventListener('mouseup',()=>{drag=false;});
window.addEventListener('mousemove',e=>{if(!drag)return;
 yaw-=(e.clientX-lx)*0.01;pitch+=(e.clientY-ly)*0.01;
 pitch=Math.max(-1.4,Math.min(1.4,pitch));lx=e.clientX;ly=e.clientY;});
cv.addEventListener('wheel',e=>{e.preventDefault();dist=Math.max(2,Math.min(20,dist+e.deltaY*0.01));},{passive:false});

function emit(){parts=[];const p=P;
 for(let i=0;i<p.count;i++){
  const ang=rnd()*6.2831853, spd=p.speed_min+(p.speed_max-p.speed_min)*rnd();
  const q={x:(rnd()-0.5)*p.spawn_radius*2, y:(rnd()-0.5)*p.spawn_radius*2, z:(rnd()-0.5)*p.spawn_radius*2,
   vx:Math.cos(ang)*spd, vy:p.up_bias+rnd()*spd, vz:Math.sin(ang)*spd,
   size0:p.size_min+(p.size_max-p.size_min)*rnd(),
   life:p.life_min+(p.life_max-p.life_min)*rnd(), spin:rnd()*6.28,
   spinv:p.spin_min+(p.spin_max-p.spin_min)*rnd(), u:rnd()*0.75, v:rnd()*0.75};
  q.max=q.life;q.size=q.size0;parts.push(q);
 }
}
function step(dt){const p=P;
 for(const q of parts){q.life-=dt;q.spin+=q.spinv*dt;q.vy+=p.gravity*dt;
  if(p.drag>0){const d=Math.min(1,p.drag*dt);q.vx-=q.vx*d;q.vz-=q.vz*d;}
  const t=q.max>0?1-q.life/q.max:1;q.size=q.size0*(1+(p.size_end-1)*Math.max(0,Math.min(1,t)));
  q.x+=q.vx*dt;q.y+=q.vy*dt;q.z+=q.vz*dt; if(q.y<0){q.y=0;q.vy=0;}}
 parts=parts.filter(q=>q.life>0);
}
// Camera basis, recomputed per frame (lookAt origin at y=0.5; right-handed).
let RX,RY,RZ,UX,UY,UZ,FX,FY,FZ,EX,EY,EZ;const TY=0.5;
function setCam(){
 EX=dist*Math.cos(pitch)*Math.sin(yaw); EY=TY+dist*Math.sin(pitch); EZ=dist*Math.cos(pitch)*Math.cos(yaw);
 let fx=-EX,fy=TY-EY,fz=-EZ;const fl=Math.hypot(fx,fy,fz)||1;FX=fx/fl;FY=fy/fl;FZ=fz/fl;
 let rx=-FZ,rz=FX;const rl=Math.hypot(rx,0,rz)||1;RX=rx/rl;RY=0;RZ=rz/rl;
 UX=RY*FZ-RZ*FY;UY=RZ*FX-RX*FZ;UZ=RX*FY-RY*FX;
}
function proj(x,y,z){const dx=x-EX,dy=y-EY,dz=z-EZ;
 const cz=dx*FX+dy*FY+dz*FZ; if(cz<=0.05)return null;
 return {x:CXp+(dx*RX+dy*RY+dz*RZ)/cz*FOCAL, y:CYp-(dx*UX+dy*UY+dz*UZ)/cz*FOCAL, z:cz, s:FOCAL/cz};
}
function line3(x0,y0,z0,x1,y1,z1){const a=proj(x0,y0,z0),b=proj(x1,y1,z1);
 if(a&&b){cx.beginPath();cx.moveTo(a.x,a.y);cx.lineTo(b.x,b.y);cx.stroke();}}
let last=performance.now();
function frame(now){const dt=Math.min(0.05,(now-last)/1000);last=now;
 if(autorot)yaw+=dt*0.35;
 timer-=dt; if(timer<=0||parts.length===0){emit();timer=Math.max(P.life_max,0.6)+0.5;}
 step(dt); setCam();
 cx.clearRect(0,0,cv.width,cv.height);
 cx.strokeStyle='rgba(255,255,255,0.16)';cx.lineWidth=1;       // ground grid (y=0)
 for(let g=-3;g<=3;g++){line3(g,0,-3,g,0,3);line3(-3,0,g,3,0,g);}
 cx.strokeStyle='rgba(255,255,255,0.5)';line3(0,0,0,0,1.5,0);  // emit-point up-axis
 const vis=[];                                                  // depth-sort billboards
 for(const q of parts){const pr=proj(q.x,q.y,q.z);if(pr){pr.q=q;vis.push(pr);}}
 vis.sort((a,b)=>b.z-a.z);
 for(const pr of vis){const q=pr.q,s=q.size*pr.s*2;
  cx.save();cx.translate(pr.x,pr.y);cx.rotate(q.spin);
  if(texImg&&texImg.complete&&texImg.width){cx.imageSmoothingEnabled=false;const tw=texImg.width,th=texImg.height;
   cx.drawImage(texImg,q.u*tw,q.v*th,0.25*tw,0.25*th,-s/2,-s/2,s,s);}
  else{cx.fillStyle='#8a6b48';cx.fillRect(-s/2,-s/2,s,s);cx.strokeStyle='rgba(0,0,0,0.35)';cx.strokeRect(-s/2,-s/2,s,s);}
  cx.restore();}
 requestAnimationFrame(frame);
}
fetch('/load?name=__none__').then(r=>r.json()).then(p=>{setUI(p);refreshList();refreshTextures();});
requestAnimationFrame(frame);
</script></body></html>"""

if __name__ == "__main__":
    print("Particle tool: http://127.0.0.1:5001  (effects in %s)" % PARTICLE_DIR)
    app.run(host="127.0.0.1", port=5001, debug=False)
