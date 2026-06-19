# `src/render/` — Vulkan, frame graph, GPU memory, world rendering

All Vulkan: device bring-up, the per-frame graph, the chunk world renderer
(meshing + streaming + GPU-driven draw), the scene renderers (sky/entity/ui/
composite), and the shared GPU-memory pool.

> Part of the [**Code Index**](../../docs/CODE_INDEX.md) (see *render/*,
> [Rendering & GPU memory](../../docs/CODE_INDEX.md#rendering--gpu-memory)).
> Exhaustive symbols: [`docs/index/SYMBOLS.md`](../../docs/index/SYMBOLS.md).
> Deep dive: [`docs/GPU_DRIVEN_RENDERING.md`](../../docs/GPU_DRIVEN_RENDERING.md).

## Files

**Bring-up & frame graph**
| File | Responsibility |
|---|---|
| `VulkanContext.{h,cpp}` | Instance/surface/device/queues + the shared `GpuAllocator`; transient command helper. |
| `Swapchain.{h,cpp}` | Swapchain images, render pass, depth; recreated on resize. |
| `Renderer.{h,cpp}` | Per-frame orchestrator: `drawFrame(recordPre, recordScene, recordUi)`, 2 frames in flight, offscreen target, `phaseTimes()`. |
| `OffscreenTarget.{h,cpp}` | Low-res colour+depth target the scene draws into (pixelation source). |

**GPU memory (REVIEW O5)**
| File | Responsibility |
|---|---|
| `GpuAllocator.{h,cpp}` | Block sub-allocator — all device memory from a few large blocks (not per-buffer `vkAllocateMemory`). **Main-thread only.** |
| `Buffer.{h,cpp}` | RAII `VkBuffer` owning one `GpuAllocator` sub-allocation. |
| `MeshArena.{h,cpp}` | Shared device-local vertex+index arenas — all chunk geometry lives in two big buffers, addressed by element count. |
| `Pipeline.{h,cpp}` | Graphics pipeline + descriptor/pipeline layout (opaque or translucent). |
| `Vertex.h` | Packed 24-byte chunk vertex; layout **must** match `attributeDescriptions()`. |

**Textures & light**
| File | Responsibility |
|---|---|
| `TextureArray.{h,cpp}` | `2D_ARRAY` texture (one layer/texture), REPEAT sampler, mip chain. |
| `LightAtlas.{h,cpp}` | 3D light texture, `PAD³=18³` per chunk slot; deferred slot recycling (S7). |
| `VulkanUtils.{h,cpp}` | Image/format helpers (create image/view, layout transitions, buffer→image). |

**Scene renderers**
| File | Responsibility |
|---|---|
| `WorldRenderer.{h,cpp}` | Chunk meshing + streaming + worker pool + GPU-driven indirect draw + light writes. |
| `SkyRenderer.{h,cpp}` | Procedural sky (atmosphere/sun/moon/stars/clouds) as a fullscreen triangle. |
| `EntityRenderer.{h,cpp}` | Animated box-rig entities + first-person held items. |
| `UiRenderer.{h,cpp}` | 2D overlay: TTF font atlas + block-face sprites in one batched draw. |
| `CompositeRenderer.{h,cpp}` | Post-pass: upscale low-res + ordered dither + fog + retro palette/interlace. |
| `Screenshot.{h,cpp}` | Frame read-back → PNG. |

## Invariants (read before editing)

- **`WorldRenderer` is the threading hot-spot.** A worker pool meshes chunks
  (workers only **read** the World → produce CPU `MeshData`; the main thread does
  *all* Vulkan). Safety: `streamBarrier()` drains workers before any world
  mutation; per-slot `meshVersion_` discards stale results; retired buffers/arena
  spans free **deferred** (`framesInFlight+1` frames); `recordPendingUploads()`
  rides uploads on the frame's own submit (no `vkQueueWaitIdle`). See
  [Threading model](../../docs/CODE_INDEX.md#threading-model--invariants).
- **GPU resource create/destroy is main-thread only** (`GpuAllocator`).
- **Reversed-Z** projection (near = depth 1, far = depth 0).
- `VG_MESH_TIME=1` prints the `GpuAllocator` pool summary; `VG_FRAME_TIME=1`
  prints the `wait`/`rec` draw split.

## Read first

`Renderer.h` → `WorldRenderer.h` → `VulkanContext.h` + `GpuAllocator.h` →
`MeshArena.h` → `Vertex.h`.
