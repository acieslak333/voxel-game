#include "render/Buffer.h"

#include "render/VulkanContext.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace vg {

Buffer::Buffer(VulkanContext& ctx, VkDeviceSize size, VkBufferUsageFlags usage,
               VkMemoryPropertyFlags properties)
    : ctx_(&ctx), size_(size) {
    VkBufferCreateInfo info{};
    info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size        = size;
    info.usage       = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(ctx_->device(), &info, nullptr, &buffer_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer");
    }

    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(ctx_->device(), buffer_, &memReq);

    // Sub-allocate from the shared pool instead of a private vkAllocateMemory, and
    // bind at the sub-allocation's offset (it may be mid-block).
    alloc_ = ctx_->allocator().allocate(memReq.size, memReq.alignment,
                                        memReq.memoryTypeBits, properties);
    vkBindBufferMemory(ctx_->device(), buffer_, alloc_.memory, alloc_.offset);
}

Buffer::~Buffer() {
    destroy();
}

void Buffer::destroy() {
    if (!ctx_) {
        return;
    }
    // Destroy the buffer first (it references the memory), then return its range to
    // the pool. The pool keeps the block mapped, so there is no per-buffer unmap.
    if (buffer_) {
        vkDestroyBuffer(ctx_->device(), buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    if (alloc_.valid()) {
        ctx_->allocator().free(alloc_);
        alloc_ = {};
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : ctx_(other.ctx_), buffer_(other.buffer_), alloc_(other.alloc_),
      size_(other.size_) {
    other.ctx_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.alloc_ = {};
    other.size_ = 0;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        ctx_    = other.ctx_;
        buffer_ = other.buffer_;
        alloc_  = other.alloc_;
        size_   = other.size_;
        other.ctx_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.alloc_ = {};
        other.size_ = 0;
    }
    return *this;
}

void Buffer::upload(const void* src, VkDeviceSize size) {
    void* dst = map();
    std::memcpy(dst, src, static_cast<size_t>(size));
    // Note: relies on HOST_COHERENT memory so no explicit flush is needed.
}

void* Buffer::map() {
    // Host-visible blocks are persistently mapped by the pool; the sub-allocation's
    // pointer is computed at allocate() time. (Null here means a non-host-visible
    // buffer was mapped — a usage error.)
    return alloc_.mapped;
}

void Buffer::unmap() {
    // No-op: the pool owns the persistent mapping for the whole block.
}

Buffer Buffer::createDeviceLocal(VulkanContext& ctx, const void* data,
                                 VkDeviceSize size, VkBufferUsageFlags usage) {
    // 1. Host-visible staging buffer we can memcpy into.
    Buffer staging(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.upload(data, size);

    // 2. Device-local destination buffer (fast for the GPU to read).
    Buffer local(ctx, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // 3. Copy staging -> device-local on the GPU.
    VkCommandBuffer cmd = ctx.beginSingleTimeCommands();
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging.handle(), local.handle(), 1, &copy);
    ctx.endSingleTimeCommands(cmd);

    return local; // staging is destroyed here by RAII
}

} // namespace vg
