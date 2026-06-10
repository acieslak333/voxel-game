# Chunk Streaming — design & staged plan

Roadmap milestone 2 (see `memory/voxel-roadmap.md`). Turns the **fixed up-front
chunk grid** into a **player-centred window** that loads/unloads chunks as the
player moves, with windowed lighting, on-disk persistence of edits, and
worker-thread generation/meshing. Horizontal only; vertical extent stays fixed
(`height_chunks`). Agreed sub-decisions: **1b** windowed global light field,
**2c** save-to-disk persistence, **3b** worker-thread gen+meshing. **No island
shaping** (an infinite world has no single coastline — the island mask is removed
when the window starts moving, Stage 2).

---

## Current model (what we're changing)

| Concern        | Today                                                                 |
|----------------|-----------------------------------------------------------------------|
| Chunk storage  | `World::chunks_` — dense `vector<Chunk>` indexed by absolute `chunkIndex(cx,cy,cz)`, region `[0,counts)`. |
| Lighting       | `skyLight_`/`blockLight_` — global `vector<uint8_t>` in **absolute** world-block coords (`lightIndex(wx,wy,wz)`). |
| Generation     | `generate()` fills all chunks at construction; `generateChunk(cx,cy,cz)` is **stateless** given seed + coords (✓ streaming-ready). |
| Renderer       | `WorldRenderer::meshes_` — dense, parallel to `chunks_`, built once. |
| Terrain        | `islandFalloff()` shapes one island centred on the world middle. |

Everything is addressed by **absolute** coordinates anchored at origin `(0,0,0)`
and bounded by `sizeInBlocks()`. Streaming makes the loaded set a **moving
window**: a `(2R+1) × heightChunks × (2R+1)` box of chunks whose min corner
(`originChunk`) follows the player.

---

## Target model

A **window** = the loaded box of chunks. Two anchors:

- `originChunk_` : min chunk coordinate of the window (moves with the player).
- `windowCounts_` : `{2R+1, heightChunks, 2R+1}` (constant after load).

Storage stays **dense** but is addressed **window-relative**: a chunk at absolute
`(cx,cy,cz)` lives at local `(cx,cy,cz) - originChunk_`, wrapped into a **ring
buffer** in X/Z so the window can advance one column without moving the other
`2R` columns of data. The light fields become the **same window** of blocks
(`windowCounts_ * kSize`), addressed window-relative.

Coordinates can go **negative** (infinite world), so all `coord / kSize` must
become **floor-division** and `coord % kSize` a **floor-mod** (truncating `/`
and `%` are wrong for negatives). A `floordiv`/`floormod` helper pair is added.

---

## Stages (each compiles & is independently verifiable)

### Stage 1 — Window-origin seam *(behaviour-preserving, origin = 0)*
Introduce `originChunk_` (init `{0,0,0}`) and `originBlock_ = originChunk_ * kSize`
and route **all** index/bounds math through them: `chunkIndex`, `inChunkBounds`,
`lightIndex`, and the bounds checks in `blockAt`/`skyLightAt`/`blockLightAt`, plus
the `generate()` loop bounds. With origin `0` every number is unchanged, so the
world is **identical** — this only carves the seam Stage 2 needs.
*Verify: build, world looks/behaves exactly as before.*

### Stage 2 — Window follows the player *(the core change)*
- Add `floordiv`/`floormod`; make all `coord/kSize`, `coord%kSize` floor-correct.
- Store chunks + light in a **ring buffer** wrapped in X/Z; `chunkPtr(cx,cy,cz)`
  returns the chunk if inside the window else `nullptr`.
- `World::recenter(playerChunkXZ)`: when the player crosses a chunk boundary,
  advance `originChunk_` by one column at a time; for each newly-entered column,
  **generate** its chunks (or load from disk in Stage 3) and **relight** the
  border (the windowed sky/block BFS, seeded from the retained interior), and
  return the set of chunks to (re)mesh. Unloaded columns are dropped.
- **Remove island shaping** (`islandFalloff` → return 1; delete the `island:`
  config + `coastWarp`): the world becomes endless rolling terrain.
- `WorldRenderer` becomes window-addressed too (ring-buffer of `ChunkMesh`,
  add/remove meshes per `recenter`). Frustum culling (already in) keeps draw
  counts sane.
- `App` calls `world_.recenter(...)` each frame from the player position and
  feeds the returned dirty/added/removed chunks to the renderer.
*Verify: walk in one direction — terrain keeps generating, old chunks unload, no
seams, lighting correct at the moving edge.*

### Stage 3 — Save-to-disk persistence (2c)
- A region/chunk save format under a world save dir keyed by seed. On **unload**,
  serialize any chunk whose blocks differ from freshly-generated (a dirty flag set
  by `setBlock`); on **load**, if a saved chunk exists, read it instead of
  regenerating. Unedited chunks are never written (regeneration is deterministic).
- Flush on quit; load on launch.
*Verify: edit blocks, walk away until they unload, walk back — edits persist;
relaunch — edits persist.*

### Stage 4 — Worker-thread gen + meshing (3b)
- A job queue: background threads run `generateChunk` + `greedyMesh` (both already
  pure given inputs); the main thread only **uploads** finished meshes and applies
  results. `recenter` enqueues jobs instead of doing the work inline.
- Lighting at the seam needs care: relight reads neighbour chunks, so either
  relight on the main thread after a column's chunks land, or snapshot inputs per
  job. Start with main-thread relight (cheap, bounded) + threaded gen/mesh.
*Verify: sprint across the world — no frame hitches as chunks stream in.*

---

## Risks / notes
- **Negative-coordinate math** is the most common streaming bug: audit every
  `/kSize` and `%kSize`. Centralised in floordiv/floormod helpers in Stage 2.
- **Lighting at the moving edge**: the windowed BFS must seed from the retained
  interior so newly-loaded columns aren't dark at the seam (the existing
  `relightField` border-seed logic is the template).
- **Vertical**: unchanged. `skyLightAt` above the window top = open sky (15);
  below the floor stays the current behaviour.
- **Settings/HUD**: `view_radius` already exists; no new player setting needed for
  Stage 1–2. Save-dir path is a new config in Stage 3.
- Island config (`island:` block, `coastWarp`, `islandFalloff*`) is **removed** in
  Stage 2; `world.yaml` and `WorldConfig` get cleaned up then.

---

*Implemented incrementally; this file tracks the plan. Update the stage checkboxes
as each lands.*

- [x] Stage 1 — window-origin seam *(done; origin fixed at {0,0,0}, world identical)*
- [~] Stage 2 — window follows player *(implemented behind `streaming:` flag, default
  OFF; compiles; needs runtime testing. Ring storage + floor math + windowed relight
  + `World::recenter` + `App` wiring + island-off-when-streaming. Known v1 limits:
  per-chunk-boundary hitch (no worker threads yet — Stage 4), edits to unloaded
  chunks are lost (no save — Stage 3), large initial/teleport jumps regenerate the
  whole window.)*
- [~] Stage 3 — save-to-disk persistence *(implemented; compiles; needs runtime
  testing. One binary file per *edited* chunk under `<save_dir>/<seed>/`; a per-slot
  dirty flag (set by setBlock, cleared on gen/load/save). Saved on unload (window
  edge), on teleport, and on quit (World dtor); loaded in generateChunk in place of
  noise. Inert when streaming off. Delete the `saves/` folder to wipe edits.)*
- [x] Stage 4 — de-hitch the streaming edge *(done. v1: amortized incremental
  meshing (no-thread fallback, `stream_workers: 0`). 3b proper: **worker-thread
  meshing** (`stream_workers` > 0, default 4). Workers only READ the World to
  greedy-mesh → CPU MeshData; the main thread does all Vulkan. Safety model:
  (a) `streamBarrier()` drains workers before any World mutation (recenter/setBlock)
  so no worker reads torn data — main is the sole mutator; (b) per-slot
  `meshVersion_` makes the newest request win (stale results discarded);
  (c) deferred buffer deletion (`retired_`, framesInFlight+1 frames, reaped in
  record()) avoids a per-frame device-idle wait. `App` only pays the barrier on
  frames the window actually moves (`World::needsRecenter`). Generation + relight
  remain synchronous on the main thread (small, bounded). Set `stream_workers: 0`
  to fall back to the amortized path if threading misbehaves.
  **Frame-integrated upload:** chunk-mesh staging→device copies are recorded into
  the frame's own command buffer before the render pass (Renderer recordPre hook),
  so uploads ride the frame's submit/fence — zero `vkQueueWaitIdle` (the per-buffer
  device sync in `createDeviceLocal` was the load lag).
  **Async relight (`async_streaming`):** generation + the window-origin move stay
  synchronous on the main thread; the heavy light flood runs on a background thread
  (`recenter()` returns relight boxes; `relightBoxes()` floods them off-thread and
  returns the dirty list; the main thread enqueues the remeshes). Race-free because
  the worker only reads the just-generated edge chunks and writes the edge light
  slab — disjoint from the player-area slots the main thread reads — origin is never
  mutated off-thread, one task at a time, and setBlock/next-recenter serialize with
  it. Set `async_streaming: false` to relight synchronously.
  **Also parallelized the remaining synchronous step:** the entering column's
  generation runs across the parallel STL (`std::execution::par` — each chunk is an
  independent ring slot sampling stateless noise), and the two relight passes
  (sky/block, independent arrays, read-only on chunks) run concurrently via
  `std::async`. Both gated on `stream_workers > 0`. The main thread still *waits*
  on gen+relight (they mutate shared World state, so they can't be fire-and-forget
  without a bigger async-generation pipeline), but the wait is now Nx/2x shorter.)*
