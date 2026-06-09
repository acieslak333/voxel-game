#pragma once

#include <vulkan/vulkan.h>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  Buffer
// -----------------------------------------------------------------------------
//  RAII wrapper over a VkBuffer + its VkDeviceMemory. Move-only (owns GPU
//  resources). Two common ways to build one:
//    * the constructor, for a raw buffer you fill yourself (e.g. host-visible
//      uniform buffers updated every frame), and
//    * createDeviceLocal(), which uploads CPU data into fast device-local memory
//      via a temporary staging buffer (e.g. static vertex/index buffers).
// -----------------------------------------------------------------------------
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

    // Persistent map/unmap for buffers updated frequently (e.g. per-frame UBOs).
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
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkDeviceSize   size_   = 0;
    void*          mapped_ = nullptr;
};

} // namespace vg
