#pragma once

/**
 * @file MeshArena.h
 * @brief Shared vertex+index arena for GPU-driven chunk drawing.
 *
 * All chunk geometry is packed into two large device-local Buffers (one for
 * vertices, one for indices) instead of a separate VkBuffer per chunk. The
 * renderer binds them once and issues a single vkCmdDrawIndexedIndirect per pass.
 *
 * SpanAllocators track free element ranges in units of elements (not bytes), so
 * returned offsets are directly usable as `vertexOffset` / `firstIndex` in draw
 * commands. Capacity is fixed at construction; allocate() throws std::runtime_error
 * on overflow (raise settings.yaml arenaVertsPerSlot or lower renderDistance).
 *
 * @note DRAFT: authored without a live Vulkan toolchain. See docs/GPU_DRIVEN_RENDERING.md.
 * @warning Main-thread only (no internal locking); mesh workers produce CPU data only.
 * @see docs/CODE_INDEX.md
 */

#include "render/Buffer.h"
#include "utilities/alloc/SpanAllocator.h"
#include "render/Vertex.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  MeshArena — shared vertex/index arena for GPU-driven chunk drawing
// -----------------------------------------------------------------------------
//  DRAFT (compile-unverified): authored without a Vulkan toolchain in the build
//  environment. See docs/GPU_DRIVEN_RENDERING.md for the design + bring-up
//  checklist. The SpanAllocator backing it IS unit-tested.
//
//  All chunk geometry lives in two big device-local buffers instead of one
//  VkBuffer per chunk, so the renderer can bind once and issue a single
//  vkCmdDrawIndexedIndirect per pass over a per-slot command array:
//
//    vertex arena (one VkBuffer): [ chunkA verts | chunkB verts | ... ]
//    index  arena (one VkBuffer): [ chunkA idx   | chunkB idx   | ... ]
//
//  The two SpanAllocators count ELEMENTS (vertices / indices), not bytes, so a
//  returned offset is directly the `vertexOffset` / `firstIndex` an indexed draw
//  wants — and we sidestep the fact that sizeof(Vertex) is not a power of two
//  (byte-aligning to it is impossible with the power-of-two SpanAllocator).
//
//  Per chunk: one vertex span (opaque vertices then water vertices) and one index
//  span (opaque indices then water indices). Indices stay 0-based into the chunk's
//  own vertices; the draw command's vertexOffset rebases them.
//
//  Capacity is FIXED at construction (sized from a config budget). allocate()
//  throws on overflow rather than growing — growth (reallocate + blit live spans)
//  is listed as future work in the design doc; the streaming working set is
//  bounded, so a generous budget avoids it. Main-thread only (like GpuAllocator):
//  all alloc/free happens on the main thread; mesh workers only produce CPU data.
// -----------------------------------------------------------------------------
/**
 * @brief Two-arena (vertex + index) GPU buffer manager for all resident chunk meshes.
 * @warning Main-thread only.
 */
class MeshArena {
public:
    // A chunk's placement in the arena. baseVertex/firstIndex are element offsets
    // (what vkCmdDrawIndexedIndirectCommand.vertexOffset / .firstIndex consume).
    struct Alloc {
        uint32_t baseVertex  = 0; // first vertex of this chunk in the vertex arena
        uint32_t vertexCount = 0; // opaque + water vertices
        uint32_t firstIndex  = 0; // first index of this chunk in the index arena
        uint32_t indexCount  = 0; // opaque + water indices
        bool     valid       = false;
    };

    static constexpr VkDeviceSize kVertexStride = sizeof(Vertex);
    static constexpr VkDeviceSize kIndexStride  = sizeof(uint32_t);

    // vertexCapacity/indexCapacity are element counts (not bytes). The two device
    // buffers are sized vertexCapacity*sizeof(Vertex) and indexCapacity*4.
    MeshArena(VulkanContext& ctx, uint32_t vertexCapacity, uint32_t indexCapacity);

    MeshArena(const MeshArena&) = delete;
    MeshArena& operator=(const MeshArena&) = delete;

    // Reserve element ranges for a chunk. Either count may be 0 (e.g. a water-only
    // or all-opaque chunk) — that side gets an empty (offset 0, count 0) range.
    // Throws std::runtime_error if the arena is full (raise the budget).
    [[nodiscard]] Alloc allocate(uint32_t vertexCount, uint32_t indexCount);
    // Release a chunk's ranges back to the free lists (coalescing).
    void free(const Alloc& a);

    [[nodiscard]] VkBuffer vertexBuffer() const { return vtx_.handle(); }
    [[nodiscard]] VkBuffer indexBuffer()  const { return idx_.handle(); }

    [[nodiscard]] VkDeviceSize vertexByteOffset(uint32_t baseVertex) const {
        return static_cast<VkDeviceSize>(baseVertex) * kVertexStride;
    }
    [[nodiscard]] VkDeviceSize indexByteOffset(uint32_t firstIndex) const {
        return static_cast<VkDeviceSize>(firstIndex) * kIndexStride;
    }

    // Stats (VG_MESH_TIME): live elements / capacity for each arena.
    [[nodiscard]] uint32_t vertexUsed()     const { return static_cast<uint32_t>(vtxAlloc_.used()); }
    [[nodiscard]] uint32_t vertexCapacity() const { return static_cast<uint32_t>(vtxAlloc_.capacity()); }
    [[nodiscard]] uint32_t indexUsed()      const { return static_cast<uint32_t>(idxAlloc_.used()); }
    [[nodiscard]] uint32_t indexCapacity()  const { return static_cast<uint32_t>(idxAlloc_.capacity()); }

private:
    Buffer        vtx_;       // device-local VERTEX | TRANSFER_DST
    Buffer        idx_;       // device-local INDEX  | TRANSFER_DST
    SpanAllocator vtxAlloc_;  // counts vertices
    SpanAllocator idxAlloc_;  // counts indices
};

} // namespace vg
