# World generation — the simple concentric island

> **2026-06-14: the previous zone-based / metroidvania spec was SCRAPPED.** The world
> is now a single, simple island with concentric biome rings. The old design (region
> partition → zone graph → lock-and-key mission graph → connectors/dungeons, plus
> baked hydrology / droplet erosion / archipelago) is gone from the shipped world.
> The C++ that implemented it still exists but is **inert opt-in** (no shipped config
> authors it); see *Legacy code* at the bottom. The historical spec is preserved in
> git history if it is ever wanted again.

## The design

One island at the world origin. From the coast inward the land rises smoothly through
four bands, chosen by **elevation**, so they read as concentric rings:

```
        ocean  →  beach (sand)  →  forest  →  highlands  →  ridged peaks (stone)
                                   oak / birch            (gray, snow-capped tips)
```

The **forest ring** is split into oak / birch patches by temperature (cool → birch,
the rest → oak). That is the whole design — no zones, rivers,
hydrology, baked erosion, cave layers, archipelago or cliffs-as-structures.

Everything is a **pure function of world coordinates** (deterministic, streaming-safe).

## How the shape is built (`TerrainGenerator`, `assets/biomes.yaml`)

`island.enabled: 1` selects the single-island path in `TerrainGenerator::shapeHeight`.
For a column at warped distance `d` from `center`, with the smoothstep land mask `m`
(1 in the inner core, 0 past `radius`):

```
landRel = land_base  +  peak_height·m  +  continentalness·interior_var  +  mountains·m
rel     = ocean_floor + (landRel − ocean_floor)·m        # mixes to deep ocean outside
height  = sea_level + round(rel)                          # clamped to the world
```

- **`peak_height·m`** is the key term: a smooth radial rise toward the core. This is
  what turns the elevation bands into concentric rings (without it the island is flat
  forest, because the legacy `mountains` term only lifts land where the erosion noise
  happens to be low). Added 2026-06-14 as `island.peak_height` (default 0 = legacy).
- **`mountains = erosion_spline(erosion)·ridged_peaks`** adds the sharp ridged crests
  on the high core → the *ridged* peaks. `peaks:` uses ridged noise.
- **`coast_warp`** gives the irregular coastline; **`terrain3d.coast_flatten`** damps
  the 3D swell near sea level so beaches are smooth sandy slopes, not choppy cliffs.
- `terrain3d` adds a modest 3D swell (overhangs are mild); floating islands are OFF.

### Biome selection (`columnInfo`)
Biomes are matched by `(temperature, humidity, elevation)`, first match wins, tested
top-to-bottom. Ordering: the low band (beach) and high band (peaks) first, then the
forest-ring climate splits with **oak as the catch-all**. Elevation is what makes the
rings; temperature only splits the forest species.

## Tuning knobs (all in `assets/biomes.yaml`)

| knob | effect |
|------|--------|
| `island.radius` | island size (currently 512 ≈ 64 chunks) |
| `island.inner` | size of the full-height core; smaller → longer slope = wider rings |
| `island.peak_height` | how high the core rises (the ring gradient). **See the cap below.** |
| `island.land_base` / `interior_var` | coastal land height / gentle interior variation |
| `island.coast_warp` | coastline irregularity |
| `erosion_spline` | the ridged crest height on the core |
| `terrain3d.coast_flatten` | beach smoothness near sea level |
| biome `elevation` bands | where each ring starts/ends (must track `peak_height`) |
| `temperature` / `humidity` frequency | forest oak/birch patch size |

> ⚠️ **`peak_height` is capped (~26) by a latent lighting bug.** A large, uniformly-tall
> central dome trips an incremental-relight box-too-small bug in `World.cpp` (the same
> failure mode that disables floating islands — the `streaming recenter relight …` and
> `placing a sky-blocker …` logictests fail). Until that relight box is fixed, keep the
> peaks moderate. Floating islands stay disabled for the same reason
> (`terrain3d.float_threshold: 0.97`).

## Underground (`assets/world.yaml`)
Standard caves + ores, unchanged by the simplification: `caves`/`cavern_*` (noise
caves), `ores` (vein densities), `pools` (deep lava / shallow cave water). The **lava
ravines were removed** (`ravines.width: 0`).

## Build / test
```
& "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config Release
build/bin/Release/voxelgame.exe --logictest                 # must stay green
build/bin/Release/voxelgame.exe --selftest                  # in-process h1==h2 only
build/bin/Release/voxelgame.exe --genmap --mode top --mapsize 768 --mapstep 2 --out out.png
```
`--genmap --mode top` colours by surface block (sand=tan, grass=green, stone=gray,
snow=white) — the quickest way to check the concentric rings.

## Removed (2026-06-14 cleanup)
The scrapped zone-design modules were **deleted**: `LayoutMap`, `ZoneGraph`,
`MissionGraph`, `Connector`, `ErosionMap`, `CaveAutomata`, `HydroMap`, `DungeonLayout`
(+ their `layout:` / `erosion_bake:` / `hydro_bake:` / `island.archipelago` / `layout.caves`
/ `connectors` config paths and logictests). Also removed the unused **blocks** — cactus,
maple, willow, lily pad, vine, and every ore except iron — and the tree species `Maple`/
`Willow` and flora family `Desert`. Kept as general utilities: `Sdf.h`, `SurfaceNets`,
`Landforms.h`, `Noise`/`NoiseStack`.

## Per-biome shape (Option C — 2026-06-14)

Biomes can modulate the global 3D terrain instead of only painting the surface. The
design (see the tool discussion) is **Option C: per-biome override of the global
`terrain3d`, blended across borders** — the world structure stays global; biomes
locally tune it.

**Schema.** A biome may carry a `terrain3d:` block that *merges over* the global one
(omitted keys inherit). A global `terrain3d.blend` (blocks) sets the border-blend width.

```yaml
terrain3d: { amplitude: 62, blend: 16, density: { layers: [...] } }
biomes:
- name: plains
  terrain3d: { amplitude: 6 }     # flat
- name: badlands
  terrain3d: { amplitude: 48 }    # jagged
```

**The invariant that makes it safe.** Biome *selection* reads the BASE heightmap
(`shapeHeight`/`columnInfo`), which per-biome 3D modulation does NOT move (it perturbs
*solidity*, not the selection height). So there is no biome→height→biome feedback loop.

**Blending.** `densityAmpAt(x,z)` = the biome's amplitude (its override, else global)
averaged over a 3×3 box at ±`blend` blocks → borders ramp smoothly, no cliffs. It is a
pure function of `(seed,x,z)` (samples the same fields at offset points), so streaming
stays seam-safe. Cached per column (mainSolid hits it for every y).

**Byte-identical default.** If no biome sets a `terrain3d` override, `densityAmpAt`
returns the global `densityAmp_` and `maxAmp_ == densityAmp_`, so generation is
unchanged (the shipped world's `--selftest` hash does not move).

**Vertical budget, per biome.** `sea_level + base + biome.amplitude + crest` must stay
under `height_chunks×16` or that biome's peaks clamp. `maxAmp_` (max over global +
overrides) bounds the surface scans so a taller biome isn't clipped.

**Phase 1 (shipped):** per-biome `terrain3d.amplitude`. **Phase 2 (deferred):** per-biome
`density` stack (frequency/character — needs 2-stack crossfade in border bands) and
per-biome `float_*` (blocked on the latent incremental-relight box bug — floats are
globally off for the same reason).
