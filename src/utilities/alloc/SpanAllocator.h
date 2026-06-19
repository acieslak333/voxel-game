#pragma once

/**
 * @file SpanAllocator.h
 * @brief Best-fit free-list sub-allocator for a linear byte arena (CPU side).
 *
 * Manages a contiguous [0, capacity) address space without owning backing storage.
 * Supports aligned allocation with front-padding absorbed into the reserved span,
 * and free() with adjacent-range coalescing. Used by GpuAllocator to manage
 * per-VkBuffer chunk mesh arenas. Main-thread only (no thread-safety).
 * @see docs/CODE_INDEX.md
 */

#include <cstdint>
#include <map>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  SpanAllocator — CPU free-list sub-allocator for a linear byte arena
// -----------------------------------------------------------------------------
//  Manages a single contiguous address space [0, capacity) in bytes. Callers
//  request aligned byte ranges ("spans") and receive back an offset; the
//  allocator tracks which regions are live vs free without owning the backing
//  storage itself. The intended consumer is GpuAllocator's per-block free-list
//  logic elevated to a standalone, testable type: each big VkBuffer arena maps
//  to one SpanAllocator, and chunk meshes are placed at the returned offsets.
//
//  Fit strategy: best-fit (smallest free range that accommodates the request
//  after alignment padding). This minimises wasted space in the typical voxel
//  workload where chunk meshes vary widely in size (tiny air chunks vs dense
//  surface chunks) and many short-lived allocations interleave with long-lived
//  ones. Compared to first-fit, best-fit leaves larger contiguous holes for
//  oversized requests at the cost of one extra O(n) scan per alloc; with
//  per-block free-lists of at most a few hundred entries during steady streaming
//  this is negligible.
//
//  Alignment: the caller's requested alignment is always a power of two. If a
//  free range starts below the next aligned boundary, the gap (front padding) is
//  absorbed into the reserved span. On free() the whole reserved span — padding
//  included — is returned to the free list, so no slivers accumulate.
//
//  Coalescing: on every free() the returned span is inserted at its sorted
//  position and merged with any immediately adjacent free ranges (both sides).
//  After all live spans are freed the free list collapses to a single [0,capacity)
//  entry regardless of free order.
//
//  Thread-safety: none. All calls must originate from the same thread (the main
//  thread in this renderer, consistent with GpuAllocator's contract).
// -----------------------------------------------------------------------------

/** @brief Best-fit free-list sub-allocator; callers receive byte offsets, not pointers. */
class SpanAllocator {
public:
    static constexpr uint64_t kInvalid = ~0ull;

    SpanAllocator() = default;
    explicit SpanAllocator(uint64_t capacity);

    // Discard all state and reinitialise with a single free range [0, capacity).
    void reset(uint64_t capacity);

    // Allocate `size` bytes aligned to `alignment` (power of two, >= 1).
    // Returns the aligned start offset, or kInvalid when no contiguous range fits.
    // Front padding required to reach alignment is absorbed into the reserved span;
    // pass the returned offset verbatim to free() to reclaim the whole span.
    [[nodiscard]] uint64_t allocate(uint64_t size, uint64_t alignment);

    // Release the span whose aligned start was returned by allocate().
    // Coalesces with adjacent free ranges in O(log n) + O(1) neighbour merges.
    void free(uint64_t offset);

    [[nodiscard]] uint64_t capacity()  const { return capacity_; }
    [[nodiscard]] uint64_t used()      const { return used_; }
    [[nodiscard]] uint64_t freeBytes() const { return capacity_ - used_; }

    // Size of the largest contiguous free range; 0 when the arena is fully occupied.
    [[nodiscard]] uint64_t largestFreeBlock() const;

private:
    struct Span { uint64_t start; uint64_t size; }; // reserved span for a live alloc

    // Free list: sorted ascending by offset for O(log n) lookup and O(1) coalesce.
    // Key = range start offset, value = range size.
    std::map<uint64_t, uint64_t> freeList_;

    // Live allocation directory: maps the aligned offset returned to the caller
    // back to the reserved [start, size) span (start <= aligned offset because of
    // front padding). Required so free(offset) can reclaim the full span.
    std::map<uint64_t, Span> live_;

    uint64_t capacity_ = 0;
    uint64_t used_     = 0; // sum of reserved span sizes for live allocations
};

} // namespace vg
