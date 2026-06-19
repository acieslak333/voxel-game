#pragma once

/**
 * @file Buffer.h
 * @brief RAII VkBuffer backed by a sub-allocation from the shared GpuAllocator pool.
 *
 * Every Buffer's device memory is a sub-allocation from one of the GpuAllocator's
 * large VkDeviceMemory blocks (not a per-buffer vkAllocateMemory call). Host-visible
 * blocks are kept persistently mapped by the pool, so map() is a pointer lookup and
 * unmap() is a no-op. Move-only; copying is deleted.
 * @warning Must be created and destroyed on the main thread only.
 * @see docs/CODE_INDEX.md
 */

#include "render/GpuAllocator.h"

#include <vulkan/vulkan.h>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  Buffer
// -----------------------------------------------------------------------------
//  RAII wrapper over a VkBuffer whose memory is a sub-allocation from the
//  context's shared GpuAllocator (not a private vkAllocateMemory — that ceiling
//  is exactly what the pool removes). Move-only (owns the VkBuffer + its
//  sub-allocation). Two common ways to build one:
//    * the constructor, for a raw buffer you fill yourself (e.g. host-visible
//      uniform buffers updated every frame), and
//    * createDeviceLocal(), which uploads CPU data into fast device-local memory
//      via a temporary staging buffer (e.g. static vertex/index buffers).
// -----------------------------------------------------------------------------
/**
 * @brief Move-only RAII owner of a VkBuffer and its GpuAllocator sub-allocation.
 */
class Buffer {
public:
    Buffer() = default;
    Buffer(VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
           VkMemoryPropertyFlags properties);
    ~Buffer();

    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    // Copy `size` bytes from `src` into this buffer. Requires host-visible memory.
    void upload(const void* src, VkDeviceSize size);

    // Host pointer to this buffer's memory (host-visible buffers only). The pool
    // keeps host-visible blocks persistently mapped, so map() just returns the
    // sub-allocation's pointer and unmap() is a no-op (kept for call-site symmetry).
    [[nodiscard]] void* map();
    void unmap();

    [[nodiscard]] VkBuffer       handle() const { return buffer_; }
    [[nodiscard]] VkDeviceSize   size()   const { return size_; }
    [[nodiscard]] bool           valid()  const { return buffer_ != VK_NULL_HANDLE; }

    // Build a device-local buffer initialised with `data` (via staging copy).
    static Buffer createDeviceLocal(VulkanContext& ctx, const void* data,
                                    VkDeviceSize size, VkBufferUsageFlags usage);

private:
    void destroy();

    VulkanContext* ctx_    = nullptr;
    VkBuffer       buffer_ = VK_NULL_HANDLE;
    GpuAlloc       alloc_;            // sub-allocation backing buffer_'s memory
    VkDeviceSize   size_   = 0;       // requested size (<= alloc_.spanSize)
};

} // namespace vg
