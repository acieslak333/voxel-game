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

    VkMemoryAllocateInfo alloc{};
    alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize  = memReq.size;
    alloc.memoryTypeIndex = ctx_->findMemoryType(memReq.memoryTypeBits, properties);

    if (vkAllocateMemory(ctx_->device(), &alloc, nullptr, &memory_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory");
    }
    vkBindBufferMemory(ctx_->device(), buffer_, memory_, 0);
}

Buffer::~Buffer() {
    destroy();
}

void Buffer::destroy() {
    if (!ctx_) {
        return;
    }
    if (mapped_) {
        vkUnmapMemory(ctx_->device(), memory_);
        mapped_ = nullptr;
    }
    if (buffer_) {
        vkDestroyBuffer(ctx_->device(), buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
    }
    if (memory_) {
        vkFreeMemory(ctx_->device(), memory_, nullptr);
        memory_ = VK_NULL_HANDLE;
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : ctx_(other.ctx_), buffer_(other.buffer_), memory_(other.memory_),
      size_(other.size_), mapped_(other.mapped_) {
    other.ctx_ = nullptr;
    other.buffer_ = VK_NULL_HANDLE;
    other.memory_ = VK_NULL_HANDLE;
    other.size_ = 0;
    other.mapped_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        destroy();
        ctx_    = other.ctx_;
        buffer_ = other.buffer_;
        memory_ = other.memory_;
        size_   = other.size_;
        mapped_ = other.mapped_;
        other.ctx_ = nullptr;
        other.buffer_ = VK_NULL_HANDLE;
        other.memory_ = VK_NULL_HANDLE;
        other.size_ = 0;
        other.mapped_ = nullptr;
    }
    return *this;
}

void Buffer::upload(const void* src, VkDeviceSize size) {
    void* dst = map();
    std::memcpy(dst, src, static_cast<size_t>(size));
    // Note: relies on HOST_COHERENT memory so no explicit flush is needed.
}

void* Buffer::map() {
    if (!mapped_) {
        vkMapMemory(ctx_->device(), memory_, 0, size_, 0, &mapped_);
    }
    return mapped_;
}

void Buffer::unmap() {
    if (mapped_) {
        vkUnmapMemory(ctx_->device(), memory_);
        mapped_ = nullptr;
    }
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
