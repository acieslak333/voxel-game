#include "render/GpuAllocator.h"

#include "render/VulkanContext.h"

#include <algorithm>
#include <stdexcept>

namespace vg {

namespace {
// Vulkan memory/buffer alignments are always powers of two.
inline VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) & ~(a - 1);
}
constexpr VkDeviceSize kDefaultBlockSize = 64ull * 1024 * 1024; // 64 MiB
} // namespace

GpuAllocator::GpuAllocator(VulkanContext& ctx)
    : ctx_(ctx), blockSize_(kDefaultBlockSize) {}

GpuAllocator::~GpuAllocator() {
    for (auto& b : blocks_) {
        if (!b || b->memory == VK_NULL_HANDLE) {
            continue;
        }
        if (b->mapped) {
            vkUnmapMemory(ctx_.device(), b->memory);
        }
        vkFreeMemory(ctx_.device(), b->memory, nullptr);
    }
}

uint32_t GpuAllocator::createBlock(VkDeviceSize minSize, uint32_t memoryTypeIndex,
                                   bool hostVisible) {
    auto blk = std::make_unique<Block>();
    blk->size            = std::max(blockSize_, minSize);
    blk->memoryTypeIndex = memoryTypeIndex;

    VkMemoryAllocateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize  = blk->size;
    info.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(ctx_.device(), &info, nullptr, &blk->memory) != VK_SUCCESS) {
        throw std::runtime_error("GpuAllocator: vkAllocateMemory failed (out of device memory)");
    }
    if (hostVisible) {
        // Persistently map the whole block once; sub-allocations point inside it.
        if (vkMapMemory(ctx_.device(), blk->memory, 0, VK_WHOLE_SIZE, 0, &blk->mapped) !=
            VK_SUCCESS) {
            vkFreeMemory(ctx_.device(), blk->memory, nullptr);
            throw std::runtime_error("GpuAllocator: vkMapMemory failed");
        }
    }
    blk->freeList.push_back({0, blk->size});
    blocks_.push_back(std::move(blk));
    return static_cast<uint32_t>(blocks_.size() - 1);
}

GpuAlloc GpuAllocator::carve(uint32_t blockId, size_t rangeIdx, VkDeviceSize size,
                             VkDeviceSize alignment) {
    Block& b        = *blocks_[blockId];
    const Range r   = b.freeList[rangeIdx];
    const VkDeviceSize aligned = alignUp(r.offset, alignment);
    const VkDeviceSize span    = (aligned - r.offset) + size; // absorb front pad

    // Shrink/remove the free range we carved from.
    if (span < r.size) {
        b.freeList[rangeIdx] = {r.offset + span, r.size - span};
    } else {
        b.freeList.erase(b.freeList.begin() + static_cast<std::ptrdiff_t>(rangeIdx));
    }
    b.reserved += span;

    GpuAlloc a;
    a.memory     = b.memory;
    a.offset     = aligned;
    a.blockId    = blockId;
    a.spanOffset = r.offset;
    a.spanSize   = span;
    a.mapped     = b.mapped ? static_cast<char*>(b.mapped) + aligned : nullptr;
    return a;
}

GpuAlloc GpuAllocator::allocate(VkDeviceSize size, VkDeviceSize alignment,
                                uint32_t memoryTypeBits, VkMemoryPropertyFlags properties) {
    if (alignment == 0) {
        alignment = 1;
    }
    const uint32_t typeIndex   = ctx_.findMemoryType(memoryTypeBits, properties);
    const bool     hostVisible = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    // First fit: scan existing blocks of this memory type for a range that holds
    // `size` after alignment.
    for (uint32_t id = 0; id < blocks_.size(); ++id) {
        Block& b = *blocks_[id];
        if (b.memoryTypeIndex != typeIndex) {
            continue;
        }
        for (size_t i = 0; i < b.freeList.size(); ++i) {
            const Range r = b.freeList[i];
            const VkDeviceSize span = (alignUp(r.offset, alignment) - r.offset) + size;
            if (span <= r.size) {
                return carve(id, i, size, alignment);
            }
        }
    }
    // No room anywhere: grow by a fresh block (sized up to fit an oversized request).
    const uint32_t id = createBlock(size, typeIndex, hostVisible);
    return carve(id, 0, size, alignment);
}

void GpuAllocator::free(const GpuAlloc& a) {
    if (!a.valid()) {
        return;
    }
    Block& b = *blocks_[a.blockId];
    b.reserved -= a.spanSize;

    // Return the span and coalesce: sort by offset, then merge adjacent ranges in
    // one pass. Per-block free-lists stay small during steady streaming, so this is
    // cheap and obviously correct.
    b.freeList.push_back({a.spanOffset, a.spanSize});
    std::sort(b.freeList.begin(), b.freeList.end(),
              [](const Range& x, const Range& y) { return x.offset < y.offset; });
    std::vector<Range> merged;
    merged.reserve(b.freeList.size());
    for (const Range& r : b.freeList) {
        if (!merged.empty() && merged.back().offset + merged.back().size == r.offset) {
            merged.back().size += r.size;
        } else {
            merged.push_back(r);
        }
    }
    b.freeList.swap(merged);
}

VkDeviceSize GpuAllocator::bytesReserved() const {
    VkDeviceSize n = 0;
    for (const auto& b : blocks_) {
        n += b->reserved;
    }
    return n;
}

VkDeviceSize GpuAllocator::bytesAllocated() const {
    VkDeviceSize n = 0;
    for (const auto& b : blocks_) {
        n += b->size;
    }
    return n;
}

uint32_t GpuAllocator::blockCount() const {
    return static_cast<uint32_t>(blocks_.size());
}

} // namespace vg
