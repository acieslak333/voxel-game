# Future Work & Extension Points

This file tracks features that are **intentionally deferred** and documents the
seams in the code where they should hook in. The guiding principle is *build
minimal, design for extensibility*: we don't implement these yet, but nothing in
the current code should make them hard to add. Search the codebase for
`TODO(future)` to find the exact spots referenced below.

---

## Deferred features

| Feature                | Status     | Where it hooks in (see also `TODO(future)` comments) |
|------------------------|------------|------------------------------------------------------|
| Inventory              | Not built  | `player/` — a component owned by the player.         |
| Crafting               | Not built  | New `crafting/` module driven by the block registry. |
| Mobs / entities        | Not built  | New `entity/` module; world tick loop.               |
| Custom block models    | Not built  | Block registry render-type + mesher dispatch.        |
| Island shaping         | Not built  | World generation noise pipeline.                     |
| Chunk streaming        | Not built  | `World` load/unload around the player.               |
| Texture mipmapping     | Not built  | Texture array upload.                                |

---

## Architectural seams (current code)

### Rendering
- **`Renderer::drawFrame(RecordFn)`** takes a callback that records draw commands
  *inside* the render pass. New render passes (shadow maps, UI, post-processing)
  can be added without touching the frame-synchronisation logic.
- **`Swapchain`** keeps the render pass alive across resizes so pipelines built
  against it stay valid. A depth attachment will be added here in Milestone 1.
- **`VulkanContext::findMemoryType`** is already in place for the buffer/image
  allocations that vertex data and textures will need.

### Window / input
- **`Input`** collapses GLFW polling into a per-frame `InputState` struct, so the
  player controller has no dependency on the windowing library. Rebindable keys
  or gamepad support would slot in here.

### Player
- **`PlayerController`** holds `health_` and is where additional survival stats
  (hunger, thirst, stamina) and a damage API will live (see its `TODO(future)`).
- Collision goes through a `SolidFn` predicate, so the controller is independent
  of how the world is stored — it works unchanged when the single chunk becomes
  a streamed multi-chunk world.
- Collision response is a per-axis **swept-AABB** solve: each axis advances
  exactly to the first block face it would hit (no one-frame gap), while leaving
  the other axes free so the player still slides along walls.
- **`Camera`** is intentionally minimal (position + yaw/pitch); other drivers
  (spectator, cutscene) could reuse it.

### World / meshing
- **`BlockRegistry`** is the single source of truth for block properties and the
  per-face texture layers. Add a block by adding an id in `Block.h` and one
  `registerBlock(...)` call — `internTexture()` deduplicates texture layers.
- **`World`** generates a *fixed* grid of chunks up front. Two extension points:
  *chunk streaming* (load/unload chunks around the player instead of one fixed
  grid) and *island shaping* (multiply the height field by a radial falloff so
  land is surrounded by water). The generator (`columnHeight` + material noise)
  is the place to add biomes, caves, ores, trees.
- **Cross-chunk meshing** *(done)*: `ChunkMesher::greedyMesh` takes a
  `NeighborSampler` that resolves chunk-local coords just past an edge to the
  neighbouring chunk's block (`World::blockAt`), so faces between two solid
  chunks are culled. A chunk only emits faces for blocks it owns, so a shared
  solid/air boundary is meshed exactly once by the chunk holding the solid side.
- **`WorldRenderer`** meshes the whole world up front, but per-chunk remeshing is
  in place: `remeshChunk(cx,cy,cz)` rebuilds one chunk's geometry and swaps its
  GPU buffers, leaving the rest untouched. `World::setBlock` returns exactly the
  chunk coordinates to remesh (the edited chunk plus any neighbour sharing the
  touched face). **Block editing** is built on this seam: `App::editBlocks`
  raycasts the crosshair (`world/Raycast`), edits with `World::setBlock`, and
  remeshes only the returned chunks. Chunk streaming is the remaining consumer.
- **`ChunkMesher`** only handles cube blocks. Non-cube render types (cross,
  custom model) should be detected via a future `BlockProperties::renderType` and
  emit their own geometry, bypassing the greedy pass.
- **`Block::metadata`** is reserved for orientation/state.

### Application
- **`App`** constructs subsystems in dependency order (window → Vulkan →
  swapchain → renderer → world → world renderer → input → player). New
  long-lived systems (asset manager, entity manager, UI) become members here.

---

## Deferred-feature design notes

### Custom block models (e.g. a furnace)
The block registry will carry a *render type* per block (`Cube`, `Cross`,
`Model`, …). The chunk mesher will branch on it: `Cube` blocks go through greedy
meshing; non-cube blocks emit their own geometry and are skipped by the greedy
pass. This keeps the common case fast while leaving room for special blocks.

### Block metadata
`Block::metadata` is reserved now so orientation/state (furnace lit, log axis,
…) can be stored without changing the chunk storage format later.

---

*Update this file at the end of each milestone as new seams are introduced.*
