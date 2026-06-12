#!/usr/bin/env bash
# Copy assets/models/* into the built game's asset dir so a Blockbench export shows
# up WITHOUT a full rebuild. The game still loads skins at startup, so restart it
# after running this.  Loop:  Blockbench export -> tools/deploy-models.sh -> rerun
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/assets/models"
# The build copies assets to build/bin/assets (VG_ASSET_DIR); mirror models there.
dst="$root/build/bin/assets/models"
if [ ! -d "$dst" ]; then
  echo "deploy-models: '$dst' not found — build the game once first." >&2
  exit 1
fi
mkdir -p "$dst"
cp -rf "$src"/* "$dst"/   # recursive: each model lives in its own <name>/ subdir
echo "deploy-models: copied $(ls -1 "$src" | wc -l) model dirs -> build/bin/assets/models  (restart the game to see them)"
