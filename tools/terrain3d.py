"""Shared live-3D terrain view for the worldgen + biome web tools.

Both tools render the engine's exposed-voxel export (voxelgame --genmap --mode
voxels, real 3D solidity: overhangs/caves/floating islands) as instanced cubes in
the browser (Three.js, vendored locally so it works offline). A tool splices HEAD
into its <head>, CANVAS into its #main, EX_ROW into its sidebar, calls
register(app) for the /vendor, /voxels.bin and /sealevel routes, and uses
vox_args() to run the export. The 3D JS (vendor/terrain3d.js) exposes
t3dShow(on) / t3dBuild(url); the tool owns the on/off state + regen wiring.
"""
import os

from flask import abort, jsonify, send_file

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VENDOR = os.path.join(HERE, "vendor")
VOX_OUT = os.path.join(HERE, "_genvox.bin")  # exposed-voxel slice (shows overhangs/caves)

# Local (offline) Three.js + the shared terrain renderer. Splice into a tool's <head>.
HEAD = ('<script src="/vendor/three.min.js"></script>'
        '<script src="/vendor/OrbitControls.js"></script>'
        '<script src="/vendor/terrain3d.js"></script>')
# The 3D canvas (lives in #main next to the 2D <img id="map">).
CANVAS = '<canvas id="gl"></canvas>'
# The voxel view is 1:1 (real blocks), so there's no height-exaggeration control.
EX_ROW = ""


def vox_args(exe, footprint, step=1):
    """CLI args for the engine's exposed-voxel export — real 3D solidity, so it shows
    overhangs/caves/floating islands. `footprint` is in BLOCKS and `step` is blocks
    per voxel cell on every axis: step 1 = true block resolution over a small patch,
    larger steps trade detail for breadth (footprint/step cells, engine-capped at
    255 per side) so a whole island fits in one view instead of a 96-block slice."""
    return [exe, "--genmap", "--mode", "voxels", "--mapsize", str(int(footprint)),
            "--mapstep", str(max(1, int(step))), "--out", VOX_OUT]


def sea_y():
    """Sea level in CELL units (the same grid the voxels use), read from the sidecar
    the engine writes next to the voxel export (runGenVoxels in src/main.cpp). The JS
    floats the water plane at this Y. Falls back to a sane default."""
    try:
        with open(VOX_OUT + ".sea", encoding="utf-8") as f:
            return float(f.read().strip())
    except Exception:
        return 64.0


def register(app):
    """Add the /vendor/<file>, /voxels.bin and /sealevel routes to a tool's app."""
    @app.route("/vendor/<path:fname>")
    def _vendor(fname):
        path = os.path.abspath(os.path.join(VENDOR, fname))
        if not path.startswith(VENDOR + os.sep) or not os.path.isfile(path):
            abort(404)
        return send_file(path)

    @app.route("/voxels.bin")
    def _voxels():
        return send_file(VOX_OUT, mimetype="application/octet-stream")

    @app.route("/sealevel")
    def _sealevel():
        return jsonify({"seaY": sea_y()})
