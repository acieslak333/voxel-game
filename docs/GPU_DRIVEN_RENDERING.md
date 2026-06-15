# GPU-driven chunk rendering (arena + indirect draw + compute cull)

Status: **in progress** on `claude/chunk-rendering-optimization-ybh5cc`.
Goal: replace the per-chunk *bind + push-constant + draw* loop in
`WorldRenderer::record()` with **one buffer bind + one `vkCmdDrawIndexedIndirect`
per pass**, and (stage 2) move frustum culling onto the GPU. This removes the
dominant CPU draw-recording cost when the view is `rec`-bound (measure first —
see "Profiling" below).

This document is the contract. The C++ structs, the SSBO layouts, and the two
shaders (`shaders/chunk_indirect.vert`, `shaders/chunk_cull.comp`) must all agree
on the byte layouts described here.

---

## Why the current renderer can't just call indirect draw

Today each chunk owns its **own** `VkBuffer` (`ChunkMesh::meshBuffer`,
`WorldRenderer.h`) and `record()` rebinds vertex+index buffers per chunk
(`WorldRenderer.cpp`). A single `vkCmdDrawIndexedIndirect` draws every command
from the **one** currently-bound vertex buffer + index buffer. So step one is to
move all chunk geometry into a shared **arena**: two big device-local buffers
(all vertices, all indices), with each chunk occupying a sub-range.

```
vertex arena (one VkBuffer):   [ chunkA verts | chunkB verts | ... ]   VERTEX|TRANSFER_DST
index  arena (one VkBuffer):   [ chunkA idx   | chunkB idx   | ... ]   INDEX |TRANSFER_DST
```

Per chunk, within its vertex span: opaque vertices first, then water vertices.
Within its index span: opaque indices first, then water indices. Indices stay
0-based into the chunk's own vertices; the draw command's `vertexOffset` rebases
them (so opaque `vertexOffset = baseVertex`, water `vertexOffset = baseVertex +
opaqueVertexCount`). This mirrors today's `firstWaterVertex` trick.

## Profiling (do this first — part A, already landed)

`VG_FRAME_TIME=1` now prints `cull: <vis> vis <culled> culled <calls> calls`
alongside the `wait`/`rec` split. Read it before committing to stage 2:

- **`rec` high, many `calls`** → CPU draw-recording-bound. The arena + single
  indirect draw (stage 1) is the win. **This is the case this design targets.**
- **`wait` high** → GPU-bound. Indirect draw barely helps; the real levers are
  occlusion culling and better far-LOD. Don't bother with stage 2; revisit scope.

## The `SpanAllocator` (landed, unit-tested)

`src/render/SpanAllocator.{h,cpp}` — a pure-CPU free-list byte sub-allocator over
`[0, capacity)`, coalescing on free, alignment-aware. It is the bookkeeping behind
each arena buffer (which byte ranges are free vs live). Verified standalone via
`tests/span_allocator_test.cpp`. No Vulkan dependency, so it is the one piece of
the migration provable without a GPU.

`MeshArena` (to build) wraps two `SpanAllocator`s + two device `VkBuffer`s and
maps a chunk's geometry to (vertexByteOffset, indexByteOffset). On overflow it
**grows**: allocate a larger buffer, copy live ranges over with a one-off
`beginSingleTimeCommands` blit, swap. Growth is rare (working set is bounded) so a
single submit+wait there is acceptable; size the initial capacity from a config
knob (see `assets/world.yaml` stream tuning, REVIEW R7) to avoid it in practice.

---

## GPU data contract (byte layouts — keep all three in sync)

### Per-chunk draw data SSBO (set 0, binding 2) — used by `chunk_indirect.vert`

```cpp
struct ChunkDrawGPU { glm::vec4 posPad; }; // xyz = chunk world translation, w = 0
```
One entry per mesh slot (`meshes_.size()`). Indexed in the shader by
`gl_InstanceIndex`, which equals the command's `firstInstance`, which the renderer
sets to the chunk's slot. `std430` array stride = 16 bytes.

### Per-slot mesh metadata SSBO (set 0, binding 0 of the cull pass) — `chunk_cull.comp`

```cpp
struct ChunkMetaGPU {
    glm::vec4  aabbMin;  // xyz world min, w pad
    glm::vec4  aabbMax;  // xyz world max, w pad
    glm::uvec4 opaque;   // x=indexCount y=firstIndex z=vertexOffset w=slot(firstInstance)
    glm::uvec4 water;    // x=indexCount y=firstIndex z=vertexOffset w=slot
};
```
`std430` stride = 64 bytes. `firstIndex`/`vertexOffset` are **element** counts
(indices / vertices), not bytes — they index the arena as bound (UINT32 index
buffer, `sizeof(Vertex)` stride).

### Indirect command buffer — matches `VkDrawIndexedIndirectCommand` exactly

```
{ uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; }
```
20 bytes, `std430` stride 20. Two arrays sized to the slot count: one opaque, one
water. `instanceCount` is the cull result (0 = culled/empty no-op, 1 = draw).

---

## record() rewrite

### Stage 1 — CPU fills commands, GPU indirect draw (lowest risk)

Per frame, per pass (opaque, then water):
1. Walk `drawList_`; for each slot build a `VkDrawIndexedIndirectCommand` from its
   `ChunkMesh` arena offsets; set `instanceCount = aabbInFrustum(...) ? 1 : 0`
   (reuse the existing CPU test). `firstInstance = slot`.
2. Upload the command array to a host-visible per-frame indirect buffer
   (`INDIRECT_BUFFER` usage). Also refresh the per-chunk `ChunkDrawGPU` SSBO when a
   slot's geometry changes (or every frame — it's small).
3. Bind once: arena vertex buffer (binding 0), arena index buffer, descriptor set
   (now including binding 2 = draw-data SSBO).
4. `vkCmdDrawIndexedIndirect(cmd, indirectBuf, 0, slotCount, sizeof(cmd))`.

This already collapses hundreds of `bind+push+draw` triples into one draw call —
the bulk of the `rec` win — while keeping culling on the CPU.

### Stage 2 — compute cull writes the commands (full GPU-driven)

Replace step 1-2 with a compute dispatch:
1. Keep `ChunkMetaGPU[]` updated (on geometry change, not per frame).
2. Each frame: push the 6 frustum planes + slot count to `chunk_cull.comp`,
   `vkCmdDispatch(ceil(slotCount/64),1,1)` into the device-local opaque+water
   command buffers.
3. Barrier: `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` (`SHADER_WRITE`) →
   `VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT` (`INDIRECT_COMMAND_READ`).
4. Same `vkCmdDrawIndexedIndirect` over the command buffer.

The dispatch + barrier happen **before** the render pass (the compute write can't
straddle a render pass), so run it in the existing `recordPendingUploads` pre-pass
hook or just before `vkCmdBeginRenderPass`. CPU per-chunk cost is then zero.

---

## Descriptor / pipeline changes

- Extend the chunk graphics descriptor set layout (`Pipeline`) with **binding 2**:
  `STORAGE_BUFFER`, `VK_SHADER_STAGE_VERTEX_BIT` (the draw-data SSBO). Bump the
  descriptor pool sizes in `createDescriptorSets` (+ one storage buffer per frame).
- New compute pipeline + its own descriptor set layout for `chunk_cull.comp`
  (bindings 0 meta, 1 opaque cmds, 2 water cmds; push constant 6 vec4 + uint).
- Swap the chunk vertex shader to `chunk_indirect.vert.spv`. The water pipeline
  reuses it (params still arrives by push constant, set once per pass).
- Device features `multiDrawIndirect` + `drawIndirectFirstInstance`: **enabled**
  in `VulkanContext::createLogicalDevice` (landed).

## Streaming / install path changes (the fragile part — Threading invariants)

`swapChunkBuffers / installMesh / installMeshBatch / buildMeshes /
recordPendingUploads / tickRetired` change from "create/free a per-chunk `Buffer`"
to "allocate/free an arena span + copy into the arena at that offset". Keep the
existing deferred-retire discipline: a freed arena span must not be reused until
`framesInFlight_+1` frames pass (an in-flight frame may still read it) — so the
`retired_` list holds freed *spans*, not `Buffer`s. All arena alloc/free stays on
the main thread (workers still produce CPU `MeshData` only — invariant #1 intact).

`ChunkMesh` becomes:
```cpp
struct ChunkMesh {
    uint64_t vtxOffset, vtxBytes;   // span in the vertex arena
    uint64_t idxOffset, idxBytes;   // span in the index arena
    uint32_t firstIndex, indexCount;       // opaque (element units)
    uint32_t waterFirstIndex, waterIndexCount;
    int32_t  baseVertex, waterBaseVertex;   // vertexOffset for the two draws
    glm::vec3 worldPos; int drawListPos;
};
```

---

## Hardware bring-up checklist (because none of the GPU path is testable headless)

Run with the Vulkan validation layer on and `VG_FRAME_TIME=1`. Verify in order:

1. **Features present**: device creation succeeds with `multiDrawIndirect` +
   `drawIndirectFirstInstance` (validation errors here = unsupported GPU; gate &
   fall back to the per-chunk path).
2. **Arena upload**: a static scene renders identically to the old path
   (screenshot diff). Catches arena offset / `vertexOffset` mistakes.
3. **Indirect command struct**: if geometry is garbled, suspect the
   `VkDrawIndexedIndirectCommand` field order / std430 stride (must be 20 bytes).
4. **firstInstance lookup**: if chunks render at the wrong world position, the
   `gl_InstanceIndex` → draw-data mapping is off (check `firstInstance = slot`).
5. **Cull barrier (stage 2)**: flicker / missing chunks = missing or wrong
   compute→indirect barrier (`SHADER_WRITE` → `INDIRECT_COMMAND_READ`).
6. **Streaming churn**: walk with `VG_AUTOWALK=8 VG_STREAM_TIME=1`; watch for
   corruption as spans are freed/reused — that exercises the retire timing.
7. **Compare**: `rec` should drop sharply vs the pre-change baseline at equal
   `vis`/`calls`; `wait` unchanged confirms the win is CPU-side as intended.
