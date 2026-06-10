# World Generation

The terrain/biome generator is **data-driven** and split into a *shape* stage and
a *climate* stage, in the spirit of Minecraft 1.18. All of it is a pure function of
world coordinates, so it is deterministic and chunk-streaming-safe (the
`--selftest` golden hash changes whenever output changes — rebaseline on purpose).

This file is the contract a future **generation-tuning tool** edits: everything
lives in `assets/biomes.yaml`, read by `vg::TerrainGenerator`
(`src/world/TerrainGenerator.{h,cpp}`). `World` just calls into it.

## Pipeline

```
                          assets/biomes.yaml (all knobs)
                                     │
   continentalness ┐                 │ splines
   erosion         ├─ TerrainShaper ─┴─► surface HEIGHT  ── oceans, plains, mountains
   peaks (ridged)  ┘                          │
                                              ▼
   temperature ┐                         ColumnInfo ──► World::generateColumn
   humidity    ┴─ selectBiome ──► biome ─► surface block + filler + snow + tree/bush density
```

### Shape (height) — biome-independent, so borders never cliff
Three noises map through **splines** to a height in blocks relative to sea level:
- **continentalness** (largest scale) → base elevation via `continental_spline`
  (deep ocean → shelf → coast → inland highlands).
- **erosion** → a *mountain amplitude* via `erosion_spline` (low erosion = tall
  ranges, high erosion = flat).
- **peaks** → a ridged value in `[0,1]` that scales the amplitude, so mountains
  read as ridgelines, not blobs.

`height = sea_level + continental_spline(c) + erosion_spline(e) * ridged(peaks)`,
clamped to the world. Any air at/below the column's **water level** floods with
water, and submerged surfaces become an ocean/lake floor.

**Rivers** carve a winding channel toward sea level where a river noise is near
zero, but only in lowlands (`rivers.max_elevation`), so mountains keep their relief
instead of growing canals. **Perched lakes** come from a coarse candidate grid
(`lakes:`): each carves a bowl and raises that column's water level to its own
(above-sea) surface, so lakes sit inland on high ground. Both feed a per-column
`waterLevel` (= sea level, or the lake's level) that `World` fills to.

### Climate (surface) — picks a biome for the surface only
**temperature** + **humidity** noises (temperature also cools with altitude) select
a biome from the `biomes:` table — the *first* biome whose `temp`, `humidity` and
`elevation` (blocks vs sea) ranges all contain the column wins, so author specific
biomes (beach, mountain, desert…) before the temperate catch-all (`plains`, last).
A biome sets the surface block, the filler under it, whether it's snowy, and the
per-column tree/bush probabilities. Peaks above `snow_line` get a snow cap
regardless of biome.

## Tuning (`assets/biomes.yaml`)
- `sea_level`, `snow_line` (blocks above sea).
- Per-noise `frequency` / `octaves` for continentalness/erosion/peaks/temperature/humidity.
- `continental_spline` / `erosion_spline`: lists of `[x, y]` control points.
- `biomes:` each with `temp`, `humidity`, `elevation` ranges, `top`/`filler` block
  names, `snow`, and `trees`/`bushes` densities.

Missing keys fall back to sensible baked-in defaults (the game runs without the file).

## World height
`assets/world.yaml` `height_chunks` sets the vertical extent (× 16 blocks). It's
currently **8 (128 tall)** with sea level 64 in the middle. For taller, more
dramatic mountains set `height_chunks: 16` (256) and scale `sea_level` (~128) and
the spline y-values / `snow_line` up with it — note it costs ~2× the vertical
memory and light/generation work, so consider lowering `view_radius`.

Caves (in `World::caveAt`, tuned in `world.yaml` `caves:`) are now two kinds:
winding **spaghetti tunnels** plus deep **large caverns**, and they carve the whole
solid column with a surface taper so the strongest tunnels **breach as cave mouths**
on hillsides (a submerged surface is never breached, to avoid air pockets under water).

## Still TODO (next passes)
- **Ravines** — long narrow vertical canyons (optional cave-system flavour).
- **The tuning tool** — a headless `--genmap` top-down PNG export is the cheap
  backend; an in-game live panel (like the sky tuner) on top later.
- **256-tall bump** — `world.yaml height_chunks: 16` + scale `biomes.yaml`.

## Done
- Shape + climate biomes (oceans, beaches, plains, mountains, snow).
- Rivers (lowland channels) and perched lakes (inland, above sea level).
- Cave variety: spaghetti tunnels + deep caverns + surface cave-mouths.
