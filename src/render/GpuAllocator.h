#pragma once

/**
 * @file GpuAllocator.h
 * @brief Shared block sub-allocator that backs all VkBuffer device memory (REVIEW O5).
 *
 * Vulkan's maxMemoryAllocationCount (typically 4096) limits live vkAllocateMemory
 * calls. GpuAllocator sidesteps this by owning a small number of large VkDeviceMemory
 * blocks and handing out sub-allocations (GpuAlloc) at offsets within them. Thousands
 * of chunk buffers therefore require only a handful of device allocations. Freed ranges
 * coalesce back into the block's free-list; blocks are retained for the allocator's
 * lifetime. Host-visible blocks are persistently mapped.
 *
 * Stats are printed at teardown and when VG_MESH_TIME=1 is set.
 * @warning Must be used on the main thread only (no internal locking).
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  GpuAllocator — block sub-allocator for VkBuffers
// -----------------------------------------------------------------------------
//  Vulkan caps the number of live `vkAllocateMemory` allocations at
//  `maxMemoryAllocationCount` (guaranteed >= 4096, often exactly that). The old
//  one-allocation-per-Buffer model put every chunk mesh + every staging buffer
//  against that ceiling, so a large `view_radius` (thousands of resident chunks)
//  could exhaust it, and streaming churned an allocate/free per upload.
//
//  This allocator hands out *sub-allocations*: it owns a few large VkDeviceMemory
//  blocks (one `vkAllocateMemory` each) and binds many buffers into them at
//  offsets (`vkBindBufferMemory(buf, block, offset)`). VkBuffer creation is NOT
//  capped — only the memory allocations are — so thousands of chunk buffers cost
//  a handful of device allocations. Freed ranges go back to a per-block
//  coalescing free-list and are reused; blocks themselves are kept for the
//  allocator's lifetime (the streaming working set is bounded, so block count
//  reaches a steady high-water and stays well under the ceiling).
//
//  Main-thread only: all buffer create/destroy in this renderer happens on the
//  main thread (workers produce CPU MeshData only), so there is no internal
//  locking.
// -----------------------------------------------------------------------------

/**
 * @brief Handle to one sub-allocation: a byte range inside a shared VkDeviceMemory block.
 *
 * `spanOffset`/`spanSize` describe the full reserved range returned to the free-list on
 * free() (it may start before `offset` when alignment forced front padding, ensuring
 * adjacent freed spans coalesce cleanly). `mapped` is non-null for host-visible sub-
 * allocations and points directly into the block's persistent mapping.
 */
struct GpuAlloc {
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    VkDeviceMemory memory = VK_NULL_HANDLE; // the owning block's memory (shared)
    VkDeviceSize   offset = 0;              // aligned offset to bind the buffer at
    void*          mapped = nullptr;        // host ptr at `offset` (host-visible only)
    uint32_t       blockId = kInvalid;      // index of the owning block
    // The reserved span returned to the free-list on free() — may start a little
    // before `offset` when alignment forced front padding (the pad is absorbed so
    // free ranges coalesce cleanly with no slivers).
    VkDeviceSize   spanOffset = 0;
    VkDeviceSize   spanSize   = 0;

    [[nodiscard]] bool valid() const { return blockId != kInvalid; }
};

/**
 * @brief Block-level GPU memory allocator; all Buffers sub-allocate from its blocks.
 * @warning Main-thread only. No internal locking.
 */
class GpuAllocator {
public:
    explicit GpuAllocator(VulkanContext& ctx);
    ~GpuAllocator();

    GpuAllocator(const GpuAllocator&) = delete;
    GpuAllocator& operator=(const GpuAllocator&) = delete;

    // Sub-allocate `size` bytes aligned to `alignment` (both from the buffer's
    // VkMemoryRequirements) out of a block whose memory type satisfies
    // (`memoryTypeBits` & `properties`). Host-visible blocks are persistently
    // mapped; the returned `mapped` points at the sub-allocation. Throws on an
    // out-of-memory device allocation.
    [[nodiscard]] GpuAlloc allocate(VkDeviceSize size, VkDeviceSize alignment,
                                    uint32_t memoryTypeBits,
                                    VkMemoryPropertyFlags properties);
    void free(const GpuAlloc& a);

    // Stats (gated debug print on teardown / VG_MESH_TIME).
    [[nodiscard]] VkDeviceSize bytesReserved()  const; // sum of live sub-allocations
    [[nodiscard]] VkDeviceSize bytesAllocated() const; // sum of block sizes (device allocs)
    [[nodiscard]] uint32_t     blockCount()     const;

private:
    struct Range { VkDeviceSize offset; VkDeviceSize size; };
    struct Block {
        VkDeviceMemory     memory = VK_NULL_HANDLE;
        VkDeviceSize       size = 0;
        uint32_t           memoryTypeIndex = 0;
        void*              mapped = nullptr; // non-null for host-visible blocks
        std::vector<Range> freeList;         // sorted ascending by offset
        VkDeviceSize       reserved = 0;     // live (sub-allocated) bytes, for stats
    };

    // Carve `size` (aligned) out of freeList[rangeIdx] of `blocks_[blockId]`.
    GpuAlloc carve(uint32_t blockId, size_t rangeIdx, VkDeviceSize size,
                   VkDeviceSize alignment);
    // Allocate a fresh device block of at least `minSize` bytes of the given type.
    uint32_t createBlock(VkDeviceSize minSize, uint32_t memoryTypeIndex,
                         bool hostVisible);

    VulkanContext&                      ctx_;
    std::vector<std::unique_ptr<Block>> blocks_;
    VkDeviceSize                        blockSize_; // default block granularity
};

} // namespace vg
