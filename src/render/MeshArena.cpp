#include "render/MeshArena.h"

#include "render/VulkanContext.h"

#include <stdexcept>
#include <string>

namespace vg {

MeshArena::MeshArena(VulkanContext& ctx, uint32_t vertexCapacity, uint32_t indexCapacity)
    : vtx_(ctx, static_cast<VkDeviceSize>(vertexCapacity) * kVertexStride,
           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      idx_(ctx, static_cast<VkDeviceSize>(indexCapacity) * kIndexStride,
           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      vtxAlloc_(vertexCapacity),
      idxAlloc_(indexCapacity) {}

MeshArena::Alloc MeshArena::allocate(uint32_t vertexCount, uint32_t indexCount) {
    Alloc a;
    a.vertexCount = vertexCount;
    a.indexCount  = indexCount;
    // Element-addressed: alignment 1 (every element offset is valid). A 0-count
    // side reserves nothing and keeps offset 0 (never dereferenced — its draw
    // command carries indexCount 0).
    if (vertexCount > 0) {
        const uint64_t off = vtxAlloc_.allocate(vertexCount, 1);
        if (off == SpanAllocator::kInvalid) {
            throw std::runtime_error(
                "MeshArena: vertex arena full (" +
                std::to_string(vtxAlloc_.used()) + "/" +
                std::to_string(vtxAlloc_.capacity()) + " verts used, largest free " +
                std::to_string(vtxAlloc_.largestFreeBlock()) + ", needed " +
                std::to_string(vertexCount) +
                ") — raise settings.yaml arenaVertsPerSlot or lower renderDistance; "
                "see docs/GPU_DRIVEN_RENDERING.md");
        }
        a.baseVertex = static_cast<uint32_t>(off);
    }
    if (indexCount > 0) {
        const uint64_t off = idxAlloc_.allocate(indexCount, 1);
        if (off == SpanAllocator::kInvalid) {
            // Roll back the vertex reservation so a partial alloc doesn't leak.
            if (vertexCount > 0) vtxAlloc_.free(a.baseVertex);
            throw std::runtime_error(
                "MeshArena: index arena full (" +
                std::to_string(idxAlloc_.used()) + "/" +
                std::to_string(idxAlloc_.capacity()) + " indices used, needed " +
                std::to_string(indexCount) +
                ") — raise settings.yaml arenaVertsPerSlot or lower renderDistance; "
                "see docs/GPU_DRIVEN_RENDERING.md");
        }
        a.firstIndex = static_cast<uint32_t>(off);
    }
    a.valid = true;
    return a;
}

void MeshArena::free(const Alloc& a) {
    if (!a.valid) return;
    if (a.vertexCount > 0) vtxAlloc_.free(a.baseVertex);
    if (a.indexCount  > 0) idxAlloc_.free(a.firstIndex);
}

} // namespace vg
