# Worldgen — status & TODO

## 2026-06-14: the zone/metroidvania generation was SCRAPPED

The world is now the **simple concentric island** described in `docs/WORLDGEN.md`:
beach → forest (oak/birch/maple) → highlands → ridged peaks. The big zone-based /
metroidvania build-out (LayoutMap → ZoneGraph → MissionGraph → connectors/dungeons,
plus baked hydrology, droplet erosion, archipelago, cave floor-layer, layout cliffs)
is **no longer in the shipped world**. The shipped `assets/biomes.yaml` is a clean,
simple file with none of those blocks.

`kChunkVersion` is **9**; the `--selftest` in-process hash is `0x173e789f86a1f4a9`
(it moves whenever the island is tuned — there is no golden, REVIEW R10).

## The shipped world (what to tune)

All in `assets/biomes.yaml` (shape + biomes) and `assets/world.yaml` (size, caves,
ores). The shape is the single-island radial rise + ridged crests; biomes are matched
by elevation (rings) with the forest split by temperature. See `docs/WORLDGEN.md` for
the formula and the knob table.

- Island: `radius 512`, `inner 0.12`, `peak_height 26`, `land_base 6`, `interior_var 8`,
  `coast_warp 90`, `ocean_floor -40`. Sea level **48** (a 256-tall world, `world.yaml
  height_chunks 16`). Beaches smoothed via `terrain3d.coast_flatten`.
- Biome bands (elev vs sea): beach `[-6,4]`, forest `[4,12]`, highlands `[12,25]`,
  peaks `[25,…]`. Forest splits: birch (cool) / maple (warm) / oak (catch-all).

## Open items

- [ ] **Fix the incremental-relight box-too-small bug** (the real blocker). It caps
      `peak_height` (~26) and forces floating islands off (`float_threshold 0.97`).
      It lives in `World.cpp` relight (the bounded sky box on `recenter`/`setBlock`) —
      currently co-edited lighting WIP. Symptoms: the `streaming recenter relight …`
      and `placing a sky-blocker …` logictests fail when terrain has a tall feature
      (a big dome or a floating mass). Fix it and the peaks can be tall again + floats
      re-enabled.
- [ ] **World height vs sea level.** The world is 256 tall but sea is 48, so the island
      sits low with a lot of empty sky. Either raise `sea_level` (+ `ocean_floor`,
      splines, snow_line, `world.yaml` cave/ore max_y) to centre the terrain, or drop
      `world.yaml height_chunks` to 8 (128 tall, also halves the streaming cost). Note
      the high flycam screenshot looks far away because of this; the genmap is the
      reliable check.
- [ ] **Spawn placement.** The player has been seen spawning low/in shadow; confirm
      spawn lands on the surface near the island core.
- [ ] **Optional:** tune the look (peak prominence, forest density/variety, highlands
      distinctiveness — it shares grass with forest so it only reads via the pine trees
      + tint in-game), then re-enable taller peaks once the relight box is fixed.

## Legacy modules (inert, opt-in — keep or delete)

Built for the scrapped design, compiled + unit-tested, but **not authored by any
shipped config** so they never run: `LayoutMap`, `ZoneGraph`, `MissionGraph`,
`Connector`, `ErosionMap`, `CaveAutomata`, `HydroMap`, `DungeonLayout` (all in
`src/world/`), plus the `layout:` / `erosion_bake:` / `hydro_bake:` / `island.archipelago`
parse+bake paths and `layout.caves` / `layout.connectors` carves in `TerrainGenerator`
+ `World`. Their logictests still run (via temp configs) and pass. Decide whether to
delete them (simpler tree) or keep them as a reusable toolbox. `Sdf.h`, `SurfaceNets`,
`Noise`/`NoiseStack` are general and worth keeping regardless.
