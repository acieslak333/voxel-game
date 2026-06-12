# CLAUDE.md — working on this repo

First-person voxel survival game, C++20 + Vulkan + GLFW/GLM, CMake (deps via
FetchContent). Endless streamed world, data-driven worldgen, survival loop.
`ISSUES.md` is the living backlog; `REVIEW.md` is the current code-review fix
list (R1-R12); design docs live in `docs/`.

## Build, test, run (Windows / VS 2022)

cmake is NOT on PATH; use the VS bundled one:

```
& "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config Release
```

- Binary: `build/bin/Release/voxelgame.exe` (assets are copied to `build/bin/assets/`).
- **Tests:** `voxelgame --logictest` (headless, no GPU; must stay green).
  `voxelgame --selftest` regenerates a world twice — only the in-process
  `h1==h2` check is meaningful; the **golden hash is unreliable** (worldgen is
  cross-PROCESS non-deterministic, REVIEW R10 — do not "fix" a golden mismatch
  by rebaselining; verify worldgen/lighting changes with in-process diffs).
- Headless run: `voxelgame --frames N [--screenshot out.png]`. Screenshots at
  the repo root are gitignored on purpose.
- PowerShell 5.1 note: `exe 2>&1 | ...` can report bogus exit 1 (stderr
  wrapping). Redirect to a file and check `$LASTEXITCODE` instead.
- The player-facing `settings.yaml` the game reads/writes is
  `build/bin/assets/settings.yaml` (not `assets/`): fullscreen, pixelate,
  renderDistance, lod, falloffs.

## Debug instrumentation (env-gated, inert otherwise — keep these)

- `VG_MESH_TIME=1` — startup phase stamps: generate / skyLight / blockLight /
  greedyMesh / GPU upload.
- `VG_FRAME_TIME=1` — per-frame profiler, prints every 120 frames: avg/max,
  update/ui, and the draw split from `Renderer::phaseTimes()` (`wait` high =
  GPU-bound, `rec` high = CPU-bound recording).
- `VG_AUTOWALK=<blocks/s>` — flies the player along +X; the way to measure
  chunk-boundary/streaming frame spikes headlessly.
- `VG_SHAPES_DEMO`, `VG_WATER_DEMO`, `VG_MODEL_DEMO`, `VG_DROP_DEMO`,
  `VG_HOUR`/`VG_PITCH`/`VG_LOW` — screenshot/setup hooks in `App.cpp`.
- Measurement gotcha: this machine runs other heavy apps; wall-clock varies
  10x with contention. Trust the phase stamps from quiet runs, not totals.

## Threading invariants (the most fragile part of the codebase)

Four actors share one mutable world: main thread, mesh worker pool
(`WorldRenderer`), async relight (`relightFuture_`), strip pregen
(`pregenFuture_`). The rules (see `REVIEW.md` for the audit):

1. Workers only READ the world. The main thread is the only mutator and must
   `streamBarrier()` before ANY world mutation (setBlock, recenter, relight).
2. The background relight writes only the edge light slab; the window must not
   be mutated while it runs — every edit/recenter path joins `relightFuture_`
   first.
3. The strip pregen reads only the immutable generator + save files, never
   window state (`generateColumnInto` is `const` for this reason). It may
   overlap anything except a window move.
4. Worker mesh results are version-stamped per slot (`meshVersion_`); stale
   results are discarded, so re-requesting a mesh is always safe.

Worldgen must stay a **pure function of (seed, world coords)** — that property
is what makes pregen/streaming safe. Never make generation depend on chunk
visit order, thread timing, or mutable state.

## Project conventions

- **Tunables live in documented YAML under `assets/`, not as magic numbers in
  code** (`docs/CONFIGURATION.md`; comment style: what / where used / effect of
  changing). Known violations are catalogued as REVIEW R7.
- Commit and push as **acieslak333** (git config in this clone is already
  correct — never commit as a Claude identity).
- Heavy-cost gotcha: editor-authored noise stacks can explode generation cost.
  `NoiseStack::addLayer` clamps invisible octaves, but `terrain3d.amplitude`
  near world height makes every cell pay the density path (the 145s-startup
  incident). Watch generate-time stamps after biomes.yaml changes.
- Chunk saves (`saves/<seed>/c.X.Y.Z.bin`) have a magic+version header; bump
  `kChunkVersion` (World.cpp) on format changes — old files then regenerate
  instead of loading garbage. Only EDITED chunks are saved.

## Layout

`src/core` app/input/UI/settings · `src/world` chunks, worldgen
(TerrainGenerator + NoiseStack + Feature), lighting, persistence ·
`src/render` Vulkan, WorldRenderer (meshing/streaming/workers), far-terrain
LOD, composite/pixelate · `src/entity` Blockbench models, armature ·
`src/player` controller/camera · `tools/` Python editors (`tools/hub.py`
launches them) · `assets/` all game data (YAML + textures + models).
