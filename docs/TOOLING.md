# Worldgen tooling — state, how to continue, and the editor-merge plan

*(2026-06-13. Continuation notes for the "tools are missing biomes.yaml fields /
only a slice is visible" task — partially done, wrap-up below.)*

## What was just fixed (done, committed)

- **3D view is no longer a 96-block slice.** `voxelgame --genmap --mode voxels`
  now takes `--mapstep` = blocks per voxel **cell** (1-16): `--mapsize 1600
  --mapstep 8` renders a 200×200-cell view spanning the whole 1600-block island;
  step 1 keeps the old block-true close-up. Binary header grew a 4th int32
  (`step`), coords are cell units, the `.sea` sidecar is in cell units;
  `tools/vendor/terrain3d.js` reads the 16-byte header. In both web tools the
  existing **size** field = blocks of world shown and **blk/px** = blocks per
  cell.
- **genmap_tool.py now edits what the game actually runs:** per-layer
  frequency / weight / octaves number inputs for every authored noise stack
  (continentalness, erosion, peaks, temperature, humidity, rivers,
  terrain3d.density) — the old single "freq" sliders edited legacy fallback
  fields the stacks override (that was the "sliders do nothing" mystery).
  Added control groups for `terrain3d` (enabled, amplitude, float_threshold/
  freq/gap/reach), `rivers`, `lakes`, `island.ocean_floor`. Controls for absent
  YAML fields are skipped instead of rendering dead sliders.

## Still missing (the "continue here" list)

1. **biome_tool.py per-biome gaps:** it edits trees/bushes/plant/tree but not
   `tint` ([r,g,b] — three number inputs; field may be ABSENT for some biomes:
   only render when present, like genmap_tool now does), `snow` (bool — needs
   apply_form to map "true"/"false" strings to real bools or write 1/0),
   `elevation`/`temp`/`humidity` ranges (`biomes.{i}.elevation.0/.1` paths work
   with the existing set_path), and `top`/`filler` block dropdowns (names from
   blocks.yaml).
2. **world.yaml structures knobs** (`structures.density`, `structures.spacing`)
   — two sliders in biome_tool's WORLD_SLIDERS table.
3. **Splines** (`continental_spline` / `erosion_spline`) have no UI anywhere.
   Best as a draggable polyline editor in the merged tool (below); until then
   they are edited in the YAML directly (each point is `[noise, blocks]`).
4. **Feature scatter is only editable per-feature in feature_tool.py.** A
   summary table (one row per assets/features/*.yaml: density / spacing /
   min/max_elevation / biome list, deep-linking to feature_tool for shapes)
   belongs in the merged tool — it answers "why are there no trees HERE".
5. **2D map can't show 3D terrain.** The top-down mode colors by heightmap
   `columnInfo`; floats/overhangs are invisible outside the 3D view. Optional:
   a `--mode top3d` that scans `surfaceY`+float occupancy per column (costlier;
   keep the size small).

## How to merge the biomes + terrain editors into ONE tool

The two Flask apps are ~80% identical (same regen/save/restore/deploy cycle,
same terrain3d module, same PAGE skeleton). Merge into `tools/worldgen_studio.py`:

1. **Adopt biome_tool's file-scoped control scheme everywhere.** Its control ids
   are `FILE::dotted.path` over a `docs` dict (`{"biomes": ..., "world": ...}`);
   genmap_tool's are bare paths over one doc. The merged tool loads
   `docs = {"biomes", "world"}` (+ optionally each `features/*.yaml` as
   `feat:<name>`), and every helper (get/set_path, apply_form, deploy_only,
   save_source, restore) comes straight from biome_tool unchanged.
2. **Sidebar = collapsible sections** (plain `<details>` is enough): Shape &
   island · 3D terrain & water · Noise layers · Biomes & flora (per-biome
   groups) · Caves & ores · Features (scatter summary). Sections 1-3 come from
   genmap_tool's CONTROLS/CONTROLS_3D/layer_controls; 4-5 from biome_tool's
   build_controls; 6 is new (item 4 above).
3. **One regen pipeline.** Keep genmap_tool's VIEW_MODES dropdown (3D / top /
   noise:<layer> / cross) — biome_tool's "3D checkbox" disappears. `regen`
   deploys ALL docs then runs the one selected view; `save`/`restore` operate on
   all files (the dirty flag already tracks any edit).
4. **Port + hub:** serve on 5000; replace the two entries in `tools/hub.py`
   with one "worldgen studio" entry (keep `feature_tool.py` separate — it's a
   shape editor, not a tuner). Leave thin `genmap_tool.py`/`biome_tool.py`
   shims that `print("merged -> worldgen_studio.py")` and exec it, or delete
   them and update hub + docs/CONFIGURATION.md references.
5. **Migration order that stays shippable:** (a) copy biome_tool.py →
   worldgen_studio.py; (b) port genmap_tool's CONTROLS/CONTROLS_3D/
   layer_controls into build_controls with the `biomes::` file prefix and add
   the view dropdown; (c) wire `<details>` sections; (d) add the per-biome gaps
   (item 1) and structures sliders (item 2); (e) feature summary (item 4);
   (f) retire the old tools in hub.py. Each step is independently testable:
   `python tools/worldgen_studio.py` + drag a slider + check the YAML diff.

## Where trees stop spawning (the user question that started this)

Built-in trees are OFF (`world.yaml features.tree_density: 0` is the master
switch). Real trees are **procedural features**: `assets/features/*_tree.yaml`,
each with a `scatter:` block — `density` (chance per grid cell), `spacing`,
`biomes: [forest, ...]` allowlist, and `min_elevation`/`max_elevation` in
blocks **relative to sea level** (absent = unlimited; oak currently has no
elevation cap, so oaks climb as high as the forest biome reaches). So trees
stop where (a) the feature's `max_elevation` says so, or (b) its allowed biomes
end — biome windows live in `assets/biomes.yaml` (`elevation: [min,max]` per
biome; e.g. `mountain` starts at +60 and only allows sparse pines via its own
`trees:`/`tree:` legacy fields, which only matter if the master switch is
re-enabled). Edit scatter in **tools/feature_tool.py** (Scatter panel: density,
spacing, elevation, biome pills) or the YAML directly.
