# `src/world/` — voxels, worldgen, meshing, lighting, persistence

The world model: the voxel and chunk types, the procedural generator, greedy
meshing, sky/block lighting, chunk persistence, and the geometry queries
(raycast, shapes, collision) the player and renderer rely on.

> Part of the [**Code Index**](../../docs/CODE_INDEX.md) (see *world/* and
> [Worldgen pipeline](../../docs/CODE_INDEX.md#worldgen-pipeline)). Exhaustive
> symbols: [`docs/index/SYMBOLS.md`](../../docs/index/SYMBOLS.md).

## Files

| File | Responsibility | Key symbols |
|---|---|---|
| `Block.h` | 3-byte voxel (`u16 id` + `u8 metadata`); render-type & face enums. | `struct Block`, `enum RenderType`, `enum Face` |
| `Chunk.h` | 16³ block array, flat `index(x,y,z) = x + 16*(y + 16*z)`. | `Chunk`, `kChunkSize`, `getOrAir`, `inBounds` |
| `BlockRegistry.{h,cpp}` | `id → BlockProperties`; loads `blocks.yaml`, interns textures, mining/tool/armor data. | `get`, `idByName`, `faceLayer`, `breakSeconds`, `canHarvest`, `struct BlockProperties` |
| `World.{h,cpp}` | Chunk-grid owner: generation, lighting, editing, the streaming window, save/load. | `generateColumnInto` (**const**), `recenter`, `pregenStrip`, `setBlock`/`setBlocksBatch`, `relightBoxes`, `blockAt`/`skyLightAt`/`blockLightAt`, `surfaceHeight` |
| `WorldConfig.{h,cpp}` | Generation + `stream_tuning` knobs from `assets/world.yaml`. | `WorldConfig::load` |
| `ChunkMesher.{h,cpp}` | CPU greedy meshing: merge coplanar same-block faces; AO; water + non-cube passes. | `greedyMesh() → MeshData`, `NeighborSampler`/`LightSampler`/`TintSampler` |
| `Feature.{h,cpp}` | Data-driven procedural scatter (shape-ops); seam-safe per-origin hashing. | `Feature::at`, `FeatureSet` |
| `Structure.{h,cpp}` | Hand-authored voxel templates; weighted placement. | `Structure::at`, `Structure::pick`, `StructureSet` |
| `Shape.{h,cpp}` | Shapeable blocks (slab/stairs/post/wall); metadata drives both mesh and collision. | `packShape`, `shapeKindOf`, `shapeBoxes` |
| `Raycast.{h,cpp}` | DDA voxel ray-march (Amanatides & Woo); first solid hit + face normal. | `raycastVoxel() → RaycastHit` |

## Invariants (read before editing)

- **Worldgen is a pure function of `(seed, world coords)`.** `generateColumnInto`
  is `const` so the streaming pregen thread can call it safely. Never make
  generation depend on visit order, thread timing, or mutable state — that purity
  is what makes streaming seam-safe. (There is **no `TerrainGenerator` class** and
  **no `biomes.yaml`** despite some older docs — generation lives here in
  `World.cpp`; config is `assets/world.yaml`.)
- **The main thread is the only world mutator.** Mesh workers only read. Any
  mutation (`setBlock`, `recenter`, relight apply) must drain workers first. See
  [Threading model](../../docs/CODE_INDEX.md#threading-model--invariants).
- **Chunk save format** has a magic + `kChunkVersion` header (`World.cpp`,
  currently `44`). Only *edited* chunks are written. **Bump `kChunkVersion` on any
  format change** so old files regenerate instead of loading garbage.
- All window index math uses `floordiv`/`floormod` (`utilities/hash/Hash.h`) —
  truncating `/`/`%` are wrong for negative coordinates.

## Read first

`Block.h` → `Chunk.h` → `World.h` → `ChunkMesher.h`, then `World::generateColumnInto`
and the lighting floods in `World.cpp`.
