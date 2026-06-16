#include "utilities/alloc/SpanAllocator.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace vg {

namespace {
// Alignment is always a power of two; round v up to the next multiple of a.
inline uint64_t alignUp(uint64_t v, uint64_t a) {
    return (v + a - 1u) & ~(a - 1u);
}
} // namespace

SpanAllocator::SpanAllocator(uint64_t capacity) {
    reset(capacity);
}

void SpanAllocator::reset(uint64_t capacity) {
    freeList_.clear();
    live_.clear();
    capacity_ = capacity;
    used_     = 0;
    if (capacity > 0) {
        freeList_.emplace(0u, capacity);
    }
}

uint64_t SpanAllocator::allocate(uint64_t size, uint64_t alignment) {
    if (size == 0 || capacity_ == 0) {
        return kInvalid;
    }
    if (alignment == 0) {
        alignment = 1;
    }

    // Best-fit: iterate the free list (sorted by offset) and remember the
    // candidate whose wasted space (span - size - front_pad) is minimised.
    // All entries are visited once; with per-arena lists of O(hundreds) this
    // is well within the hot-path budget.
    auto   bestIt   = freeList_.end();
    uint64_t bestWaste = kInvalid;

    for (auto it = freeList_.begin(); it != freeList_.end(); ++it) {
        const uint64_t rangeStart = it->first;
        const uint64_t rangeSize  = it->second;
        const uint64_t aligned    = alignUp(rangeStart, alignment);
        const uint64_t frontPad   = aligned - rangeStart;
        const uint64_t needed     = frontPad + size; // total bytes consumed from range

        if (needed > rangeSize) {
            continue; // doesn't fit
        }
        const uint64_t waste = rangeSize - needed;
        if (waste < bestWaste) {
            bestWaste = waste;
            bestIt    = it;
            if (waste == 0) {
                break; // perfect fit — can't do better
            }
        }
    }

    if (bestIt == freeList_.end()) {
        return kInvalid; // out of space
    }

    const uint64_t rangeStart = bestIt->first;
    const uint64_t rangeSize  = bestIt->second;
    const uint64_t aligned    = alignUp(rangeStart, alignment);
    const uint64_t frontPad   = aligned - rangeStart;
    const uint64_t spanSize   = frontPad + size; // bytes reserved from arena

    // Remove the free range; if there is a tail remainder put it back.
    freeList_.erase(bestIt);
    if (spanSize < rangeSize) {
        const uint64_t tailStart = rangeStart + spanSize;
        freeList_.emplace(tailStart, rangeSize - spanSize);
    }

    // Record the reserved span keyed by the aligned (returned) offset.
    live_.emplace(aligned, Span{rangeStart, spanSize});
    used_ += spanSize;

    return aligned;
}

void SpanAllocator::free(uint64_t offset) {
    auto liveIt = live_.find(offset);
    // Callers must only free offsets returned by allocate().
    assert(liveIt != live_.end() && "SpanAllocator::free: offset not live");
    if (liveIt == live_.end()) {
        return;
    }

    const Span span = liveIt->second;
    live_.erase(liveIt);
    used_ -= span.size;

    const uint64_t spanEnd = span.start + span.size;

    // Insert into free list and coalesce with neighbours in O(log n) + O(1).
    // Lower bound gives us the first free range whose offset >= span.start;
    // the range immediately before it (if any) may be our left neighbour.
    auto next = freeList_.lower_bound(span.start);

    uint64_t mergedStart = span.start;
    uint64_t mergedSize  = span.size;

    // Coalesce with right neighbour (next) if it is directly adjacent.
    if (next != freeList_.end() && next->first == spanEnd) {
        mergedSize += next->second;
        next = freeList_.erase(next);
    }

    // Coalesce with left neighbour (prev) if it ends exactly at our start.
    if (next != freeList_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == mergedStart) {
            mergedStart  = prev->first;
            mergedSize  += prev->second;
            freeList_.erase(prev);
        }
    }

    freeList_.emplace(mergedStart, mergedSize);
}

uint64_t SpanAllocator::largestFreeBlock() const {
    uint64_t largest = 0;
    for (const auto& [offset, size] : freeList_) {
        if (size > largest) {
            largest = size;
        }
    }
    return largest;
}

} // namespace vg
