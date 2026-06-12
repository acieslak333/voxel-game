#!/usr/bin/env bash
# Launch the voxel-game Tools Hub -> http://127.0.0.1:5005
# A dashboard that starts/stops the worldgen, biome, recipe & particle editors and
# browses blocks/items/recipes. Needs:  pip install flask pyyaml
# Run from anywhere:  tools/hub.sh   (or double-click on a desktop that runs .sh)
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
py="${PYTHON:-}"
if [ -z "$py" ]; then
  if command -v python3 >/dev/null 2>&1; then py=python3; else py=python; fi
fi
exec "$py" tools/hub.py "$@"
