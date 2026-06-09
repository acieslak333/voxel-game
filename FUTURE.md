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
| Extra survival stats   | Not built  | `Player` stats block (alongside health).             |
| Texture mipmapping     | Not built  | Texture array upload.                                 |

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
- **`Window`** owns GLFW and exposes the raw `GLFWwindow*`. Input handling
  (keyboard/mouse) will attach here or in a dedicated `Input` class in
  Milestone 2; the player controller will consume it.

### World / meshing
- **`BlockRegistry`** is the single source of truth for block properties and the
  per-face texture layers. Add a block by adding an id in `Block.h` and one
  `registerBlock(...)` call — `internTexture()` deduplicates texture layers.
- **`Chunk::getOrAir`** is the hook for cross-chunk meshing: it currently returns
  air outside the chunk, but in a multi-chunk world it should sample the
  neighbouring chunk so faces between chunks are culled (see its `TODO(future)`).
- **`ChunkMesher`** only handles cube blocks. Non-cube render types (cross,
  custom model) should be detected via a future `BlockProperties::renderType` and
  emit their own geometry, bypassing the greedy pass.
- **`Block::metadata`** is reserved for orientation/state.

### Application
- **`App`** constructs subsystems in dependency order. New long-lived systems
  (world, player, asset manager) become members here, constructed after the
  renderer. In Milestone 3 the single `ChunkRenderer` chunk + buffers become a
  collection keyed by chunk coordinate.

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
