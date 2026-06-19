<!-- ===========================================================================
  CODE_INDEX.md — canonical, agent-first architecture map for voxel-game.
  Curated by humans/agents; the EXHAUSTIVE per-symbol companion is the
  machine-generated docs/index/SYMBOLS.md (regenerate with
  `python tools/codeindex/codeindex.py gen`). Keep this file accurate to the
  SHIPPED code — see "Documentation drift" before trusting any older doc.
  How to maintain: docs/CODE_INDEX_GUIDE.md  ·  /skill code-index
============================================================================ -->

# Voxel-Game — Code Index

First-person voxel **survival** game. **C++20 + Vulkan + GLFW + GLM**, CMake with
all third-party deps pulled by `FetchContent`. Endless streamed world,
data-driven worldgen, day/night + volumetric clouds, far-terrain LOD, survival
loop (mining / inventory / crafting / chests), liquids, Blockbench entity & tool
models, and an in-repo Python tools hub.

> **This is the canonical map.** It is the single place a new contributor or
> agent should read first. It supersedes the architecture overviews previously
> scattered across `README.md`, `CLAUDE.md`, and `docs/*`. The detailed design
> docs under `docs/` are retained as **deep-dive chapters / design history** and
> linked from each section — but where a design doc and the code disagree, the
> **code (and this index) win** (see [Documentation drift](#documentation-drift)).

## Contents

1. [Read-first order](#read-first-order)
2. [Repository map](#repository-map)
3. [Build, run, test, debug](#build-run-test-debug)
4. [Architecture at a glance](#architecture-at-a-glance)
5. [Threading model & invariants](#threading-model--invariants) ← most fragile
6. [Subsystem maps](#subsystem-maps) — `core` · `world` · `render` · `clouds` · `entity` · `player` · `utilities`
7. [Worldgen pipeline](#worldgen-pipeline)
8. [Streaming](#streaming)
9. [Rendering & GPU memory](#rendering--gpu-memory)
10. [Shaders](#shaders)
11. [Data & asset catalog](#data--asset-catalog)
12. [Python tools](#python-tools)
13. [CLI modes & debug env vars](#cli-modes--debug-env-vars)
14. [Documentation drift](#documentation-drift)
15. [Deep-dive docs](#deep-dive-docs)
16. [How this index is maintained](#how-this-index-is-maintained)

---

## Read-first order

A new agent should read these (small) files top-to-bottom to orient, then jump
into the subsystem maps below:

1. `CLAUDE.md` — operating rules, build/test commands, commit identity, the four threading invariants.
2. **This file** — architecture + where everything lives.
3. `src/utilities/hash/Hash.h` (57 L) — `floordiv`/`floormod`/`hash01`: the *definition* of worldgen randomness.
4. `src/world/Block.h` (59 L) + `src/world/Chunk.h` (59 L) — the voxel & the 16³ chunk.
5. `src/world/World.h` (325 L) — grid owner: generation, lighting, editing, streaming API.
6. `src/core/App.h` (381 L) — app ownership + the main loop's shape.
7. `src/render/Renderer.h` + `src/render/WorldRenderer.h` — the per-frame graph and chunk streaming/meshing.

For an exhaustive, always-current symbol dump, read
[`docs/index/SYMBOLS.md`](index/SYMBOLS.md) (generated) or grep the ctags file
`docs/index/symbols.tags`.

---

## Repository map

```
src/
  core/       app lifecycle, window, input, UI, settings, day/night, palettes
  world/      blocks, chunks, worldgen, greedy meshing, lighting, persistence,
              raycast, shapes, structures, features
  render/     Vulkan bring-up, frame graph, world/sky/entity/ui/composite
              renderers, GPU memory pool, mesh arena, textures, light atlas, LOD
  clouds/     volumetric cloud noise, weather map, cloud system evolution
  entity/     Blockbench model loader, box-rig armature, critters, items, particles
  player/     controller (physics/collision), camera, inventory, crafting, chests,
              equipment, save format
  utilities/  alloc/ (SpanAllocator) · hash/ (Hash) · noise/ (FastNoise + adapters)
  main.cpp    CLI entry: --frames/--screenshot/--flycam/--selftest/--logictest
shaders/      GLSL → SPIR-V at build time (chunk, sky, entity, ui, composite, cull)
assets/       all game data: YAML configs, textures, models, fonts, particles
tools/        Python data editors (hub.py launches them) + model generators
tests/        span_allocator_test.cpp (the rest of the test suite is in main.cpp)
scripts/      offline asset generators (textures, colors, icons)
third_party/  vendored single-header libs: stb_image, stb_image_write, stb_truetype
docs/         this index + deep-dive design docs + the index tooling
.claude/      vendored agents & skills (development aids; not needed to build/run)
```

Source size: **112 C++ files, ~26 k LOC**; 12 GLSL shaders; 7 Python tools.
Largest files (where the complexity concentrates): `world/World.cpp` (2598),
`utilities/noise/FastNoise.cpp` (2359, **vendored**), `core/App.cpp` (1945),
`render/WorldRenderer.cpp` (1292), `core/AppUi.cpp` (1224), `main.cpp` (1114),
`world/ChunkMesher.cpp` (840).

---

## Build, run, test, debug

**Toolchain.** C++20 compiler (MSVC 2022 / GCC 11+ / Clang 14+), CMake 3.20+, the
Vulkan runtime loader (ships with GPU drivers). GLFW, GLM, Vulkan-Headers,
yaml-cpp, and (if no system `glslc`/`glslangValidator`) glslang are fetched and
built automatically — nothing to install by hand. See `CMakeLists.txt`.

**Build (Windows / VS 2022 — the primary dev target).** `cmake` is not on PATH;
use the VS-bundled one:

```
& "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build build --config Release
```

Binary: `build/bin/Release/voxelgame.exe`. Assets are copied to
`build/bin/assets/` post-build (so the *player-facing* `settings.yaml` the game
reads/writes is `build/bin/assets/settings.yaml`, **not** `assets/`).

**Build (Linux / macOS).**
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
```
Linux needs X11 dev headers for GLFW (`xorg-dev libxkbcommon-dev`) and the
Vulkan loader (`libvulkan1`). Headless rendering works under `xvfb-run` + Mesa
lavapipe (`VK_ICD_FILENAMES=.../lvp_icd.json`) — see `README.md`.

**Tests (must stay green).**
- `voxelgame --logictest` — headless, no GPU. The growing assertion suite over
  the pure data/logic systems (mining times, tools, fall damage, swim physics,
  crafting, inventory & player-save round-trips, equipment, shaped blocks,
  lighting recompute, streaming relight). Lives in `src/main.cpp`.
- `voxelgame --selftest` — regenerates a world twice; only the **in-process
  `h1==h2`** regen check is meaningful. The golden hash is unreliable (worldgen
  is cross-process non-deterministic — REVIEW R10; do **not** "fix" a golden
  mismatch by rebaselining).

**Headless run / profiling.** `voxelgame --frames N [--screenshot out.png]`.
Debug instrumentation is env-gated (inert otherwise) — see
[CLI modes & debug env vars](#cli-modes--debug-env-vars).

**Generate API docs (Doxygen).** Once `doxygen` is installed:
```
cmake --build build --target docs        # or: doxygen Doxyfile  (from repo root)
```
Output → `docs/doxygen/html/index.html`. See [How this index is maintained](#how-this-index-is-maintained).

---

## Architecture at a glance

```
                 ┌──────────────────────────── App (core/) ───────────────────────────┐
   GLFW input →  │  Input → PlayerController → Camera                                   │
                 │  World (world/): generate · light · edit · stream                    │
   assets/*.yaml │  DayNight (core/) → SkyState                                         │
   ───────────►  │  Inventory/Crafting/Chests/Equipment (player/)                       │
                 │  ItemEntities/Particles/Critters (entity/)                           │
                 └───────────────┬──────────────────────────────────────────┬──────────┘
                                 │ CPU MeshData (greedy meshed)              │ draw data
                                 ▼                                           ▼
        ┌──────────────── WorldRenderer (render/) ───────────┐   ┌──── Renderer (render/) ────┐
        │ worker pool meshes chunks · streams the window ·   │   │ orchestrates the frame:    │
        │ uploads into the shared MeshArena · light atlas ·  │──►│ Pre → Scene → UI passes    │
        │ GPU-driven indirect draw (optional)                │   │ (offscreen → composite)    │
        └────────────────────────────────────────────────────┘   └────────────────────────────┘
                                                                     │ Sky→World→Entity→Composite→Ui
                                                                     ▼  via VulkanContext/Swapchain
```

**The frame** (`Renderer::drawFrame`, render/Renderer.cpp): wait fence → acquire
swapchain image → record [Pre-pass: stage mesh/light uploads · optional GPU cull
| Scene-pass (low-res offscreen): Sky → World opaque → World water → Entities |
UI-pass (swapchain): Composite upscale+dither+fog → UI overlay] → submit →
present. Two frames in flight. `Renderer::phaseTimes()` exposes the wait/record
split (high `wait` = GPU-bound, high `rec` = CPU-bound).

**The world** is a player-centred **window** of chunks (a `(2R+1) × heightChunks
× (2R+1)` ring buffer), not a fixed grid. As the player crosses a chunk boundary
the window recenters: new edge columns generate, the edge light slab re-floods
(off-thread), and affected chunks remesh on the worker pool. Only *edited* chunks
are persisted to disk.

---

## Threading model & invariants

> **This is the most fragile part of the codebase. Read before touching world
> mutation, streaming, or meshing.** Source of truth: `CLAUDE.md` + `REVIEW.md`
> audit + [`docs/STREAMING.md`](STREAMING.md) (Stage 4–5).

Four actors share one mutable `World`:
| Actor | Thread | May touch |
|---|---|---|
| Main thread | main | the **only** mutator (setBlock, recenter, relight apply) |
| Mesh worker pool | `WorldRenderer` workers | **READ** world only → produce CPU `MeshData` |
| Async relight | `relightFuture_` | writes only the **edge light slab** of a freshly-generated column |
| Strip pregen | `pregenFuture_` | reads only the **immutable generator + save files** |

The four invariants:

1. **Workers only READ.** The main thread is the sole mutator and must
   `streamBarrier()` (drain workers) before ANY world mutation.
2. **Background relight writes only the edge light slab**; the window must not be
   mutated while it runs — every edit/recenter path joins `relightFuture_` first.
3. **Strip pregen reads only the immutable generator + save files**, never window
   state (`generateColumnInto` is `const` for exactly this reason). It may overlap
   anything except a window move.
4. **Worker mesh results are version-stamped per slot** (`meshVersion_`); stale
   results are discarded, so re-requesting a mesh is always safe.

Corollaries enforced in code: GPU resource create/destroy is **main-thread only**
(`GpuAllocator`); retired buffers/arena spans are freed **deferred**
(`framesInFlight+1` frames) so the GPU never reads freed memory across the
2-frame overlap; **worldgen must stay a pure function of `(seed, world coords)`**
— never depend on visit order, thread timing, or mutable state (that purity is
what makes pregen/streaming safe).

---

## Subsystem maps

Each table lists the **key** public symbols per file. The complete, always-current
symbol list (every type/function with line numbers) is in
[`docs/index/SYMBOLS.md`](index/SYMBOLS.md); each subsystem also has its own
`src/<dir>/README.md`.

### core/ — app lifecycle, input, UI, settings  ·  [README](../src/core/README.md)

| File | Responsibility | Key symbols |
|---|---|---|
| `App.{h,cpp}` (381/1945) | Owns every subsystem; runs the main loop (poll → update → stream → UI → render). | `App::run(maxFrames, screenshotPath)`, `updateSurvival`, `editBlocks`, `streamWindow`, `flushPendingEdits`, `savePlayer`/`loadPlayer`, `saveChests`/`loadChests`, `buildModels` |
| `AppUi.cpp` (1224) | Builds the HUD + all menus (inventory, crafting, chest, equipment, palette, options). | `App::buildUi` |
| `Input.{h,cpp}` (77/112) | Samples GLFW keyboard/mouse once per frame into edge-detected `InputState`. | `Input::poll() → InputState`, `struct InputState` |
| `Settings.{h,cpp}` (69/85) | Player-facing options; load/save `build/bin/assets/settings.yaml`. | `Settings::load(path)`, `Settings::save(path)` |
| `DayNight.{h,cpp}` (208/392) | Time-of-day model + analytic atmosphere; snapshots `SkyState` for the sky & world shaders. | `DayNight::advance(dt)`, `DayNight::state() → SkyState`, live-tuning setters |
| `Ui.{h,cpp}` (94/257) | Immediate-mode UI primitive layer (rects/text/sprites) the renderer consumes. | `Ui` builder API |
| `Window.{h,cpp}` (80/119) | GLFW window + surface + resize/fullscreen. | `Window`, `pollEvents`, `framebufferSize` |
| `Palette.{h,cpp}`, `ColorPalette.{h,cpp}` (42/75, 34/73) | Named colour palette (`assets/colors.yaml`) + retro palette swatches. | `Palette`, `ColorPalette` |
| `ShapePicker.h` (48) | UI helper for choosing block shapes (slab/stairs/…). | `ShapePicker` |

### world/ — voxels, worldgen, meshing, lighting, persistence  ·  [README](../src/world/README.md)

| File | Responsibility | Key symbols |
|---|---|---|
| `Block.h` (59) | 3-byte voxel (`u16 id` + `u8 metadata`); render-type & face enums. | `struct Block`, `enum RenderType`, `enum Face` |
| `Chunk.h` (59) | 16³ block array, flat `index(x,y,z)=x+16*(y+16*z)`. | `Chunk`, `kChunkSize=16`, `getOrAir`, `inBounds` |
| `BlockRegistry.{h,cpp}` (248/311) | `id → BlockProperties`; loads `blocks.yaml`, interns/dedups textures, mining/tool/armor data. | `BlockRegistry::get/idByName/faceLayer/breakSeconds/canHarvest`, `struct BlockProperties` |
| `World.{h,cpp}` (325/2598) | Chunk grid owner: generation, sky+block lighting, editing, streaming window, save/load. | `World(cfg, blocksFile)`, `generateColumnInto` (**const**, pregen-safe), `recenter`, `pregenStrip`, `setBlock`/`setBlocksBatch`, `relightBoxes`, `blockAt`/`skyLightAt`/`blockLightAt`, `surfaceHeight` |
| `WorldConfig.{h,cpp}` (103/74) | Generation + streaming knobs from `world.yaml`. | `WorldConfig`, `WorldConfig::load(path)` |
| `ChunkMesher.{h,cpp}` (80/840) | CPU greedy meshing: merge coplanar same-block faces; AO; water + non-cube passes. | `ChunkMesher::greedyMesh(...) → MeshData`, `NeighborSampler`/`LightSampler`/`TintSampler` |
| `Feature.{h,cpp}` (117/358) | Data-driven procedural scatter (shape-ops); seam-safe per-origin hashing. | `Feature`, `Feature::at(...)`, `FeatureSet` |
| `Structure.{h,cpp}` (81/143) | Hand-authored voxel templates (well/boulder/pillar), weighted placement. | `Structure::at`, `Structure::pick`, `StructureSet` |
| `Shape.{h,cpp}` (71/104) | Shapeable blocks (slab/stairs/post/wall/vslab); metadata = single source of truth for mesh + collision. | `packShape`, `shapeKindOf`, `shapeBoxes` |
| `Raycast.{h,cpp}` (42/139) | DDA voxel ray-march (Amanatides & Woo); first solid hit + face normal. | `raycastVoxel(...) → RaycastHit` |

### render/ — Vulkan, frame graph, GPU memory, world rendering  ·  [README](../src/render/README.md)

| File | Responsibility | Key symbols |
|---|---|---|
| `VulkanContext.{h,cpp}` (120/401) | Instance/surface/device/queues + the shared `GpuAllocator`; transient command helper. | `VulkanContext`, `allocator()`, `beginSingleTimeCommands` |
| `Swapchain.{h,cpp}` (80/294) | Swapchain images, render pass, depth; recreated on resize. | `Swapchain::recreate`, `renderPass`, `framebuffer` |
| `Renderer.{h,cpp}` (164/382) | Per-frame orchestrator: 2 frames in flight, offscreen target, the Pre/Scene/UI passes. | `Renderer::drawFrame(recordPre, recordScene, recordUi)`, `phaseTimes()`, `setPixelScale` |
| `OffscreenTarget.{h,cpp}` (50/122) | Low-res colour+depth target the scene draws into (pixelation source). | `OffscreenTarget::colorView/depthView` |
| `GpuAllocator.{h,cpp}` (99/157) | **Block sub-allocator** — all device memory comes from a few large blocks (REVIEW O5). | `GpuAllocator::allocate/free → GpuAlloc` |
| `Buffer.{h,cpp}` (61/117) | RAII `VkBuffer` owning one `GpuAllocator` sub-allocation. | `Buffer`, `createDeviceLocal`, `upload` |
| `MeshArena.{h,cpp}` (96/66) | Shared device-local vertex+index arenas; all chunk geometry lives in 2 big buffers. | `MeshArena::allocate/free → Alloc` |
| `Pipeline.{h,cpp}` (57/240) | Graphics pipeline + descriptor/pipeline layout (opaque or translucent). | `Pipeline`, `descriptorSetLayout` |
| `Vertex.h` (114) | Packed 24-byte chunk vertex; **layout must match `attributeDescriptions()`**. | `struct Vertex`, `Vertex::attributeDescriptions` |
| `TextureArray.{h,cpp}` (67/257) | `2D_ARRAY` texture (one layer/texture) with REPEAT sampler + mip chain. | `TextureArray` |
| `LightAtlas.{h,cpp}` (104/184) | 3D light texture, `PAD³=18³` per chunk slot; deferred slot recycling (S7). | `LightAtlas::alloc/freeDeferred/recordWrite/tick` |
| `WorldRenderer.{h,cpp}` (376/1292) | Chunk meshing + streaming + worker pool + GPU-driven indirect draw + light writes. | `streamBarrier`, `streamRemesh`, `streamPump`, `remeshChunk(s)`, `record`, `recordPendingUploads`, `recordCull`, `meshVersion_` |
| `SkyRenderer.{h,cpp}` (82/280) | Procedural sky (atmosphere + sun/moon + stars + clouds) as a fullscreen triangle. | `SkyRenderer::record` |
| `EntityRenderer.{h,cpp}` (91/328) | Animated box-rig entities + first-person held items; per-frame baked vertex buffer. | `EntityRenderer::record`, `struct Draw` |
| `UiRenderer.{h,cpp}` (133/510) | 2D overlay: TTF font atlas + block-face sprites batched into one draw. | `UiRenderer::begin/rect/text/blockFace/record` |
| `CompositeRenderer.{h,cpp}` (87/333) | Post-pass: upscale low-res + ordered dither + fog + retro palette/interlace. | `CompositeRenderer::record`, `setSource`, `setPalette`, `struct Fog` |
| `VulkanUtils.{h,cpp}` (50/155) | Image/format helpers (create image/view, layout transitions, buffer→image). | `createImage`, `transitionImageLayout`, `findDepthFormat` |
| `Screenshot.{h,cpp}` (20/92) | Read back the frame and write a PNG. | `saveScreenshot` |

### clouds/ — volumetric clouds  ·  [README](../src/clouds/README.md)

| File | Responsibility | Key symbols |
|---|---|---|
| `CloudNoise.{h,cpp}` (60/299) | Base (Perlin-Worley 64³) + detail (Worley fBm 32³) 3D noise textures; cached to disk. | `CloudNoise` |
| `WeatherMap.{h,cpp}` (43/80) | Static 64² spatial weather field (coverage/type offsets), wind-scrolled in-shader. | `WeatherMap` |
| `CloudSystem.{h,cpp}` (183/288) | Weather evolution + the `GpuParams` (13 vec4s) appended to the sky UBO. | `CloudSystem::update(dt, dayNight)`, `gpuParams()`, `struct GpuParams` |

### entity/ — models, rigs, mobs, items, particles  ·  [README](../src/entity/README.md)

| File | Responsibility | Key symbols |
|---|---|---|
| `Armature.{h,cpp}` (129/183) | Pure-CPU box-part rig: skeleton + animation clips → baked world-space mesh (headless-testable). | `restPose`, `sampleClip`, `worldMatrices`, `bakeMesh`, `struct Skeleton`/`AnimationClip` |
| `BlockbenchModel.{h,cpp}` (40/255) | Loads `.bbmodel` JSON → `Skeleton` + skin (embedded or referenced PNG). | `loadBlockbenchModel(path)` |
| `Critters.{h,cpp}` (51/76) | Passive-mob AI (wander/turn/gravity); pure sim via a `solid()` predicate. | `Critters::update(dt, solid)`, `struct Critter` |
| `ItemEntity.{h,cpp}` (55/71) | Dropped-item physics + magnet pickup; pure sim, headless-testable. | `ItemEntities::spawn/update`, `struct ItemEntity` |
| `Particles.{h,cpp}` (88/114) | Data-driven particle bursts (`.prtcl`); gravity/drag/aging. | `Particles::spawnEffect/update`, `struct ParticleEffect` |

### player/ — controller, camera, items  ·  [README](../src/player/README.md)

| File | Responsibility | Key symbols |
|---|---|---|
| `PlayerController.{h,cpp}` (175/425) | Walking (gravity/collision/swim) + free-fly; health, equipment modifiers. | `PlayerController::update(dt, input)`, `fallDamage`, `SolidFn`/`BoxesFn`/`WaterFn` |
| `Camera.{h,cpp}` (39/39) | Yaw/pitch camera; look deltas and view matrix. | `Camera::addLook`, `front`, `viewMatrix` |
| `Inventory.{h,cpp}` (75/71) | 9-slot hotbar + backpack; stack merge/add/remove. | `Inventory::add/remove/takeFromSelected/count` |
| `Crafting.{h,cpp}` (54/71) | Recipe loading (`recipes.yaml`) + craftability; pure logic. | `Crafting::craftable/craft` |
| `ChestStore.{h,cpp}` (55/87) | Per-position chest storage, packed-key serialize. | `ChestStore::serialize/deserialize` |
| `Equipment.h` (63) | 1 armour + 4 trinket slots → aggregated `Stats`. | `Equipment::computeStats` |
| `PlayerSave.h` (109) | `PlayerSave` struct + versioned binary (de)serialize (round-trip tested). | `PlayerSave::serialize/deserialize` |

### utilities/ — allocation, hashing, noise  ·  [README](../src/utilities/README.md)

| File | Responsibility | Key symbols |
|---|---|---|
| `alloc/SpanAllocator.{h,cpp}` (85/140) | CPU free-list sub-allocator for the GPU mesh arenas; best-fit + coalescing. | `SpanAllocator::allocate/free`, `largestFreeBlock` |
| `hash/Hash.h` (57) | **Canonical worldgen randomness** — change a constant here and every world re-rolls. | `floordiv`, `floormod`, `hash01(2D/3D)` |
| `noise/Noise.{h,cpp}` (77/109) | Deterministic adapter over FastNoise: `perlin`/`fbm`/`fbmEroded`/`worley`. | `Noise` |
| `noise/NoiseStack.{h,cpp}` (165/185) | Weighted multi-layer noise blend + redistribution/terrace; auto-clamps invisible octaves. | `NoiseStack::addLayer/value`, `setRedistribution`, `setTerrace` |
| `noise/NoiseMask.{h,cpp}` (52/48) | `NoiseStack` → threshold/width band → falloff curve → `[0,1]` weight. | `NoiseMask::weight`, `enum Falloff` |
| `noise/NoiseLoad.h` (149) | YAML → `NoiseStack`/`NoiseMask`; Catmull-Rom falloff LUT. | `loadStack`, `loadMask`, `buildFalloffLut` |
| `noise/FastNoise.{h,cpp}` (317/2359) | ⚠️ **VENDORED** — Jordan Peck FastNoise (MIT). Not deep-indexed; do not document per-symbol. | (external API) |

---

## Worldgen pipeline

> Accurate to the shipped `src/world/World.cpp`. The richer *design intent* is in
> [`docs/WORLDGEN.md`](WORLDGEN.md) — but see [Documentation drift](#documentation-drift):
> there is **no `TerrainGenerator` class** and **no `biomes.yaml`**; generation
> lives in `World::generateColumnInto` and is configured by `assets/world.yaml`.

Pure function of `(seed, world coords)`. For each chunk column,
`World::generateColumnInto(cx, cz, …)` (const → safe on the pregen thread):

1. **Noise** — per-layer seeded `Noise` (over vendored FastNoise) blended by
   `NoiseStack`; gated/shaped by `NoiseMask`. `hash01` (`utilities/hash/Hash.h`)
   supplies all per-cell random decisions.
2. **Macro shape** — continentalness + relief selector (2D) choose ocean vs land
   and plain/hill/mountain character; domain-warp displaces the field for
   irregular coastlines.
3. **3D density** — a trilinearly-interpolated 3D density field plus a height
   gradient decides solidity per voxel (supports mild overhangs; floating
   islands are disabled by a latent relight bug — see drift note).
4. **Material / biome** — depth/relief splines pick the grass/dirt/stone stack;
   beaches, water.
5. **Scatter** — trees (`assets/features/*_tree.yaml`, seam-safe per-origin),
   structures (`assets/structures/*.yaml`, weighted), all derived from
   `hash(origin, seed, salt)` so streaming re-derives identical voxels at seams.
6. **Lighting** — `computeSkyLight` (BFS flood, falloff per step down) +
   `computeBlockLight` (emitter flood with hue). Edits relight incrementally
   (`setBlock`/`setBlocksBatch`); the streaming edge relights via `relightBoxes`
   (off-thread, edge slab only).
7. **Meshing** — `ChunkMesher::greedyMesh` merges coplanar same-block faces with
   per-corner AO; a separate translucent pass for source water; a non-cube pass
   for cross/model/leaf-cube/shaped/flowing geometry. Per-pixel sky+block light
   is sampled from the `LightAtlas` (S7), not baked into vertices.

**Persistence.** Only *edited* chunks are saved, one binary file per chunk under
`saves/<seed>/c.X.Y.Z.bin`, with a magic + `kChunkVersion` header
(`World.cpp:48`, currently **44**). Bump the version on any format change so old
files regenerate instead of loading garbage.

---

## Streaming

> Deep dive + staged history: [`docs/STREAMING.md`](STREAMING.md) (Stages 1–5,
> all implemented). The "Current model" table at the top of that doc describes
> the **pre-streaming** fixed grid for contrast — today's code is the windowed
> model.

The loaded world is a `(2R+1) × heightChunks × (2R+1)` **ring buffer** whose min
corner `originChunk_` follows the player; all `coord/kSize`, `coord%kSize` use
`floordiv`/`floormod` (negatives are wrong with truncating `/` `%`). A window
step (`World::recenter`, driven from `App::streamWindow`):

- drains workers (`streamBarrier`) and joins any in-flight relight first
  (invariants 1–2);
- generates / loads each newly-entered column (`std::execution::par` across the
  independent ring slots when `stream_workers > 0`);
- floods the edge light slab — synchronously, or off-thread via `relightBoxes`
  when `async_streaming` is on;
- enqueues the dirty chunks for the worker pool to remesh; finished meshes are
  applied within a per-frame budget (`streamPump`) and their staging→device
  copies recorded into the frame's own command buffer (`recordPendingUploads`),
  so uploads ride the frame's submit (no `vkQueueWaitIdle`).

The window may **trail** the player under fast travel and catch up at relight
throughput; the only blocking load is the genuine out-running case within
`kWindowEdgeSafetyChunks` of the leading edge (Stage 5 removed the old
force-drain hitch). Strip **pregen** (`pregenStrip`, on `pregenFuture_`) builds
the next column ahead of time from the immutable generator + save files only.

---

## Rendering & GPU memory

> Deep dive: [`docs/GPU_DRIVEN_RENDERING.md`](GPU_DRIVEN_RENDERING.md).

**Bring-up order.** `VulkanContext` (instance → surface → device → queues +
`GpuAllocator`) → `Swapchain` (format/present/extent, render pass, depth) →
`Renderer` (command buffers, 2 frames in flight, sync objects, offscreen target,
UI pass, composite).

**GPU memory model (REVIEW O5).** One `GpuAllocator` owns a few large
`VkDeviceMemory` **blocks**; every `Buffer` is a sub-allocation at an offset
(host-visible blocks stay persistently mapped) — not a per-buffer
`vkAllocateMemory`. All chunk geometry lives in **two** big device-local buffers
via `MeshArena` (one vertex arena, one index arena, addressed by element count),
so streaming a chunk in/out is a span alloc/free, not a buffer create/destroy.
`VG_MESH_TIME=1` prints the pool summary (blocks + allocated/live MiB).

**Frame graph.** `Renderer::drawFrame` records three passes (see
[Architecture](#architecture-at-a-glance)). Scene draws into a **low-res
offscreen** target; `CompositeRenderer` nearest-upscales it (chunky pixels) and
applies dither/fog/retro palette; UI draws on top in the swapchain pass.
**Reversed-Z** projection (near = depth 1, far = depth 0). Frustum culling is on
the CPU by default; `chunk_cull.comp` does it on the GPU (opt-in `VG_GPUCULL`),
emitting `VkDrawIndexedIndirectCommand`s consumed by `chunk_indirect.vert`.

**Lifetime safety.** Retired chunk buffers and arena spans are freed **deferred**
(`framesInFlight+1` frames; `retired_`/`retiredAllocs_`, reaped at the top of
`record()`); `LightAtlas` slots recycle the same way (`freeDeferred`→`tick`).
Combined with `meshVersion_` stamping, the GPU never reads freed/stale memory.

---

## Shaders

GLSL 4.50 → SPIR-V at build time (`add_shader` in `CMakeLists.txt`). Shared
descriptor set 0: `binding 0` CameraUBO, `1` block `sampler2DArray`, `2` entity
skin `sampler2DArray`, `3` light volume `sampler3D` (S7). Full binding lists are
in [`docs/index/SYMBOLS.md`](index/SYMBOLS.md).

| Shader | Stage | Purpose |
|---|---|---|
| `chunk.vert` / `chunk.frag` | vert / frag | Chunk geometry: world transform + foliage sway; sample block texture array, blend sky+block light (per-pixel from light volume), directional sun/moon, held point light, biome tint, retro dither/palette. |
| `chunk_indirect.vert` | vert | GPU-driven variant: reads per-chunk translation from an SSBO indexed by `gl_InstanceIndex`. |
| `chunk_cull.comp` | comp | Frustum-culls resident chunk slots → indirect draw commands (local size 64). |
| `sky.vert` / `sky.frag` | vert / frag | Fullscreen triangle; analytic Rayleigh+Mie single-scatter, sun/moon discs, stars/Milky-Way/planets, sunset bands, volumetric clouds. |
| `entity.vert` / `entity.frag` | vert / frag | Rig-baked entity geometry; block-atlas or skin-atlas with alpha cutout + directional light. |
| `composite.vert` / `composite.frag` | vert / frag | Upscale the low-res scene, ordered dither, retro colour-bits/interlace. |
| `ui.vert` / `ui.frag` | vert / frag | 2D overlay: pixel→clip transform; font atlas (R = coverage) or block-icon sampling. |

---

## Data & asset catalog

> Convention (project rule): tunables live in **documented YAML under `assets/`**,
> never as magic numbers in code — see [`docs/CONFIGURATION.md`](CONFIGURATION.md)
> for the what/where/effect comment style. Known violations are REVIEW R7.

| File / dir | Read by | Controls |
|---|---|---|
| `assets/blocks.yaml` | `BlockRegistry` (runtime) | Block types: name, solid/opaque, emission, per-face textures, mining/tool/armor, render type. |
| `assets/world.yaml` | `WorldConfig` (runtime) | World size (view radius / height), seed, terrain shaping, `stream_tuning:` knobs. **This is the worldgen config** (not `biomes.yaml`). |
| `assets/world1.yaml` | (alt profile) | A second world profile. |
| `assets/items.yaml` | registry/UI | Non-block item definitions. |
| `assets/recipes.yaml` | `Crafting` (runtime) | Crafting recipes (output + inputs). Edited via `recipe_tool.py`. |
| `assets/sky.yaml` | `DayNight` (runtime) | Day/night look: atmosphere (betaR/betaM/mieG/exposure), sun/moon discs, stars/Milky-Way, terrain light. |
| `assets/clouds.yaml` | `CloudSystem` (runtime) | Volumetric clouds: altitude/quality, noise tiling, density/erosion, light transport, wind, weather scheduler. |
| `assets/colors.yaml` (+ `colormap.png`) | `Palette` (runtime) + `gen_textures.py` | Named colour palette. **Generated** from `colormap.png` via `scripts/gen_colors.py` — do not hand-edit. |
| `assets/textures.yaml` | `scripts/gen_textures.py` (offline) | Placeholder texture generation knobs. |
| `assets/settings.yaml` | `Settings` (runtime, **read/written** at `build/bin/assets/`) | Player options: pixelate, light falloff, render distance, FOV, sensitivity, retro modes, day length, fullscreen. |
| `assets/features/*.yaml` | `Feature`/`FeatureSet` (worldgen) | Procedural scatter objects (trees). Edited via `feature_tool.py`. |
| `assets/structures/*.yaml` | `Structure`/`StructureSet` (worldgen) | Hand-authored voxel templates. Edited via `structure_tool.py`. |
| `assets/particles/*.prtcl` | `Particles` (runtime) | Particle effects. Edited via `particle_tool.py`. |
| `assets/models/<name>/<name>.bbmodel` | `BlockbenchModel`/`App::buildModels` | Entity & tool/held models + skins (hand, hammer, pickaxe, sword, torch, critter). |
| `assets/fonts/<name>/` | `UiRenderer` | TTF fonts for the UI. |
| `assets/colorpalettes/`, `assets/textures/` | runtime / icons | Retro palettes; generated block textures + UI icons. |

---

## Python tools

`python tools/hub.py` (port 5005) launches the editors; each writes the same YAML
the engine loads. (Note: older docs reference `worldgen_tool.py`/`biome_tool.py` —
those were **deleted**; see [drift](#documentation-drift).)

| Tool | Port | Authors |
|---|---|---|
| `hub.py` | 5005 | Launcher + read-only block/item/recipe browser. |
| `feature_tool.py` | 5004 | `assets/features/*.yaml` — procedural scatter (shape ops, randomization, noise fills). Preview mirrors `Feature::at`. |
| `recipe_tool.py` | 5003 | `assets/recipes.yaml` — crafting recipes (comment-preserving). |
| `particle_tool.py` | 5001 | `assets/particles/*.prtcl` — effect tuning; preview mirrors `Particles.cpp`. |
| `structure_tool.py` | 5007 | `assets/structures/*.yaml` — layer-by-layer voxel painter. |
| `gen_tool_models.py` | — | Generates hammer/sword/pickaxe/torch `.bbmodel` + shaded PNG skins into `assets/models/`. Run once per design. |
| `gen_critter_model.py` | — | Generates the quadruped critter rig + skin into `assets/models/critter/`. |

Offline asset generators live in `scripts/`: `gen_textures.py`, `gen_colors.py`,
`gen_icons.py`, `gen_cracks.py`, `gen_hammer.py`, `gen_ui.py` (their committed
outputs keep the build Python-free).

---

## CLI modes & debug env vars

**CLI** (`src/main.cpp`):

| Flag | Effect |
|---|---|
| `--frames N` | Render N frames then exit (headless smoke test). |
| `--screenshot PATH` | Render a few frames, write PNG, exit. |
| `--flycam` | Start in free-fly looking down over the world. |
| `--selftest` | Headless worldgen determinism (`h1==h2`, no golden — REVIEW R10). |
| `--logictest` | Headless logic test suite (mining/crafting/save/physics/lighting). |

**Debug env vars** (gated, inert otherwise — keep them; from `CLAUDE.md`):

| Var | Effect |
|---|---|
| `VG_MESH_TIME=1` | Startup phase stamps (generate/skyLight/blockLight/greedyMesh/upload) + GpuAllocator pool summary. |
| `VG_FRAME_TIME=1` | Per-frame profiler every 120 frames (avg/max, update/ui, draw `wait`/`rec` split). |
| `VG_UPDATE_TIME=1` | Prints any streamWindow/streamPump/farTerrain call over 4 ms. |
| `VG_AUTOWALK=<b/s>` | Flies the player along +X — headless chunk-boundary/streaming spike measurement. |
| `VG_STREAM_TIME=1` | Per window step: synchronous main-thread cost (`drain`/`apply`). Pair with `VG_AUTOWALK`. |
| `VG_SHAPES_DEMO`, `VG_WATER_DEMO`, `VG_MODEL_DEMO`, `VG_DROP_DEMO`, `VG_CRITTER_DEMO`, `VG_HOUR`/`VG_PITCH`/`VG_LOW`, `VG_MENU*`, `VG_PALETTE_DEMO`, `VG_HELD` | Screenshot/setup hooks in `App.cpp`. |

> Measurement gotcha: this dev machine runs heavy apps; wall-clock varies ~10× with
> contention. Trust phase stamps from quiet runs, not totals.

---

## Documentation drift

Several older docs describe designs that **diverge from the shipped code**. The
code (and this index, regenerated from it) is authoritative. Known divergences as
of this index:

- **`TerrainGenerator` class** — referenced by `CLAUDE.md` ("worldgen
  (TerrainGenerator + NoiseStack + Feature)") and `docs/WORLDGEN.md`. **No such
  class exists.** Generation is `World::generateColumnInto` in `world/World.cpp`.
- **`assets/biomes.yaml`** — referenced by `WORLDGEN.md`, `CONFIGURATION.md`,
  `CLAUDE.md`, `TOOLING.md` as the worldgen config. **It does not exist.** The
  worldgen config is `assets/world.yaml` (loaded in `App.cpp` ~L246).
- **`--genmap` CLI mode** and worldgen editor tools (`worldgen_tool.py`,
  `biome_tool.py`, `genmap_tool.py`, `worldgen_studio.py`) — referenced by
  `WORLDGEN.md`/`TOOLING.md`. **None exist** in `main.cpp` / `tools/` (they were
  deleted). The live tools are listed under [Python tools](#python-tools).
- **`docs/WORLDGEN.md` concentric-island design** — describes intent for a
  single-island generator; the shipped generator is the endless 3D-density
  pipeline above. Treat `WORLDGEN.md` as design notes, not a code description.
- **`docs/STREAMING.md` "Current model" table** — describes the *pre-streaming*
  fixed grid; the shipped model is the windowed ring buffer.

When you touch any of these areas, prefer correcting the doc (or pointing it
here) over propagating the stale claim.

---

## Deep-dive docs

Retained as design history / chapter detail (read after this index):

| Doc | Covers | Status |
|---|---|---|
| [`docs/STREAMING.md`](STREAMING.md) | Streaming staged plan + threading model (Stage 4–5 = shipped). | Mostly current; "Current model" table is pre-streaming. |
| [`docs/WORLDGEN.md`](WORLDGEN.md) | World-shape *design intent* (island/biome rings). | Design notes; diverges from shipped generator (see drift). |
| [`docs/GPU_DRIVEN_RENDERING.md`](GPU_DRIVEN_RENDERING.md) | Indirect-draw / GPU cull design. | Current. |
| [`docs/CONFIGURATION.md`](CONFIGURATION.md) | The "tunables live in YAML" rule + config-file table. | Current rule; `biomes.yaml` row is stale. |
| [`docs/TOOLING.md`](TOOLING.md) | Worldgen-editor merge history. | Historical; references deleted tools. |
| [`docs/FUTURE.md`](FUTURE.md) | Extension points & deferred features. | Roadmap. |
| [`docs/WORLDGEN_TODO.md`](WORLDGEN_TODO.md) | Worldgen follow-ups. | Roadmap. |
| `ISSUES.md` | Living backlog. | Living. |
| `REVIEW.md` | Current code-review fix list (R1–R12, O5/O6, S7). | Living. |

---

## How this index is maintained

This index has three layers; keep them in sync when code changes:

1. **`docs/index/`** — machine-generated, exhaustive, never editorialised.
   Regenerate after any source change:
   ```
   python tools/codeindex/codeindex.py gen     # symbols.json/.tags, SYMBOLS.md, manifest.json
   python tools/codeindex/codeindex.py check    # CI/hook guard: fails if stale
   ```
   A GitHub Action (`.github/workflows/code-index.yml`) and an optional
   pre-commit hook (`scripts/hooks/pre-commit`, install with
   `git config core.hooksPath scripts/hooks`) run `check` so the index can't
   silently rot.
2. **This file + `src/*/README.md`** — curated prose. Update the relevant
   subsystem table when you add/move a file or change a subsystem's shape.
3. **In-source Doxygen** — `@file` banners + per-symbol comments; run
   `cmake --build build --target docs` (or `doxygen Doxyfile`) to render
   `docs/doxygen/html/`.

The full procedure (conventions, comment template, what to update when) is in
[`docs/CODE_INDEX_GUIDE.md`](CODE_INDEX_GUIDE.md), and is runnable as a Claude
Code skill: **`/skill code-index`**.
