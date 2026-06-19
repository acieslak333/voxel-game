# CLAUDE.md — working on this repo

First-person voxel survival game, C++20 + Vulkan + GLFW/GLM, CMake (deps via
FetchContent). Endless streamed world, data-driven worldgen, survival loop.
`ISSUES.md` is the living backlog; `REVIEW.md` is the current code-review fix
list (R1-R12); design docs live in `docs/`.

> **Architecture & code map → [`docs/CODE_INDEX.md`](docs/CODE_INDEX.md)** (canonical:
> per-subsystem file/symbol maps, frame graph, worldgen, threading, shaders, asset
> catalog). Each `src/<dir>/` has a `README.md`; the exhaustive symbol list is the
> generated [`docs/index/SYMBOLS.md`](docs/index/SYMBOLS.md). This file holds the
> **operating rules** only. To refresh the index or document new code, run
> **`/skill code-index`** (method: [`docs/CODE_INDEX_GUIDE.md`](docs/CODE_INDEX_GUIDE.md)).

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
  greedyMesh / GPU upload, plus the GpuAllocator pool summary (blocks + allocated/
  live MiB — confirms chunk buffers share a few device allocations, REVIEW O5).
- `VG_FRAME_TIME=1` — per-frame profiler, prints every 120 frames: avg/max,
  update/ui, and the draw split from `Renderer::phaseTimes()` (`wait` high =
  GPU-bound, `rec` high = CPU-bound recording).
- `VG_AUTOWALK=<blocks/s>` — flies the player along +X; the way to measure
  chunk-boundary/streaming frame spikes headlessly.
- `VG_STREAM_TIME=1` — per window step, prints the synchronous main-thread cost
  (`drain` = worker-pool drain, `apply` = strip move/recenter); the heavy relight
  + remesh are off-thread/budget-spread, so this is the boundary's true frame cost.
  Pair with `VG_AUTOWALK`.
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
  incident). Watch generate-time stamps after `world.yaml` changes. For a big
  view_radius, `terrain3d.interpolate` (REVIEW O6) approximates the density on a
  coarse lattice — ~4x faster generate — at the cost of smoothed sub-cell detail;
  off by default (default terrain stays byte-identical).
- Chunk saves (`saves/<seed>/c.X.Y.Z.bin`) have a magic+version header; bump
  `kChunkVersion` (World.cpp) on format changes — old files then regenerate
  instead of loading garbage. Only EDITED chunks are saved.

## Layout

Full per-subsystem maps (files → responsibilities → key symbols), the frame
graph, the worldgen pipeline, and the asset catalog live in
**[`docs/CODE_INDEX.md`](docs/CODE_INDEX.md)** (with a `README.md` in each
`src/<dir>/`). Orientation only:

`src/core` app/input/UI/settings · `src/world` chunks, worldgen
(`World::generateColumnInto` + `NoiseStack` + `Feature`), lighting, persistence ·
`src/render` Vulkan, `WorldRenderer` (meshing/streaming/workers), far-terrain
LOD, composite/pixelate; every `Buffer`'s memory is sub-allocated from
`GpuAllocator`'s shared blocks (not a per-buffer `vkAllocateMemory` — REVIEW O5) ·
`src/clouds` volumetric clouds · `src/entity` Blockbench models, armature ·
`src/player` controller/camera · `src/utilities` alloc/hash/noise · `tools/`
Python editors (`tools/hub.py` launches them) + `tools/codeindex/` (the index
generator) · `assets/` all game data (YAML + textures + models).

> Note: worldgen is in `World.cpp` (there is **no** `TerrainGenerator` class) and
> its config is `assets/world.yaml` (**no** `biomes.yaml`); some older `docs/`
> still reference those — see CODE_INDEX.md → *Documentation drift*.

## Claude Code tooling (`.claude/`, checked in)

Domain helpers are vendored in the repo so any clone gets them. Reach for them
when the task matches — don't reinvent what they cover.

- **Agents** (`.claude/agents/`, spawn via the Agent tool): `cpp-pro` (modern
  C++20/23, templates, zero-overhead — sonnet), `game-developer` (engine /
  graphics / gameplay systems — sonnet), `performance-engineer` (profiling,
  bottlenecks, scaling — sonnet), `refactoring-specialist` (behavior-preserving
  cleanups — sonnet), `code-reviewer` (quality + security review — opus).
- **Skills** (`.claude/skills/`, invoke via the Skill tool):
  - `code-index` — build/refresh the code index (machine symbol index, the
    `CODE_INDEX.md` map, in-source Doxygen). Use after changing source or to
    document code (method: `docs/CODE_INDEX_GUIDE.md`).
  - `grill-me` / `grilling` — relentless, one-question-at-a-time interview to
    stress-test a plan or design before building (vendored from
    `mattpocock/skills`, MIT).
  - `ponytail` (+ `-review`/`-audit`/`-debt`/`-gain`/`-help`) — "lazy senior dev"
    mode: push for the simplest, shortest solution that works (YAGNI, stdlib
    first) and hunt over-engineering in diffs/repos (vendored from
    `DietrichGebert/ponytail`, MIT).
  - `renderdoc-gpu-debug` — GPU frame capture/inspection via `rdc-cli`
    (Vulkan/D3D/GL). Use for rendering artifacts, shadow/z-fighting/blend bugs,
    pixel history, shader debugging. Needs RenderDoc + `pip install rdc-cli`
    (see its `README.md`/`CLAUDE.md`).
  - `vulkan-compute` — compute-shader authoring: GLSL/HLSL → SPIR-V, pipelines,
    descriptor sets, barriers.
  - `blockbench-mcp-overview` / `-animation` / `-texturing` — drive Blockbench
    over its MCP server (modeling, rigging, keyframes, painting/UV). Tracked in
    `skills-lock.json` (source: `jasonjgardner/blockbench-mcp-project`).
