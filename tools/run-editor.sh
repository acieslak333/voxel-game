#!/usr/bin/env bash
# Launch ONE voxel-game editor standalone (without the hub).
#   Usage:  tools/run-editor.sh [hub|worldgen|recipe|particle]   (default: hub)
# Ports:    hub 5005 · worldgen 5000 · particle 5001 · recipe 5003
# Needs:    pip install flask pyyaml ruamel.yaml
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
case "${1:-hub}" in
  hub)                                  script=tools/hub.py ;;
  worldgen|genmap|world|biome|flora)    script=tools/worldgen_tool.py ;;
  recipe|recipes)                       script=tools/recipe_tool.py ;;
  particle|particles)                   script=tools/particle_tool.py ;;
  *) echo "unknown editor '${1}'  (hub|worldgen|recipe|particle)" >&2; exit 2 ;;
esac
py="${PYTHON:-}"
if [ -z "$py" ]; then
  if command -v python3 >/dev/null 2>&1; then py=python3; else py=python; fi
fi
exec "$py" "$script"
