/**
 * @file Inventory.cpp
 * @brief Inventory add/remove/count operations with two-pass slot filling.
 * @see docs/CODE_INDEX.md
 */

#include "player/Inventory.h"

namespace vg {

/// Two-pass add: top up existing stacks first, then fill empty slots. Returns leftover count.
int Inventory::add(uint16_t blockId, int count) {
    if (blockId == 0 || count <= 0) {
        return count > 0 ? count : 0;
    }
    int remaining = count;

    // Pass 1: top up existing stacks of the same block (hotbar then backpack, which
    // is the natural slot order 0..kSlots-1).
    for (int i = 0; i < kSlots && remaining > 0; ++i) {
        ItemStack& s = slots_[static_cast<size_t>(i)];
        if (s.blockId == blockId && s.count < kMaxStack) {
            const int space = kMaxStack - s.count;
            const int put   = remaining < space ? remaining : space;
            s.count = static_cast<uint16_t>(s.count + put);
            remaining -= put;
        }
    }

    // Pass 2: drop the rest into empty slots.
    for (int i = 0; i < kSlots && remaining > 0; ++i) {
        ItemStack& s = slots_[static_cast<size_t>(i)];
        if (s.empty()) {
            const int put = remaining < kMaxStack ? remaining : kMaxStack;
            s.blockId = blockId;
            s.count   = static_cast<uint16_t>(put);
            remaining -= put;
        }
    }

    return remaining; // 0 = everything fit
}

/// Sum of counts for `blockId` across every slot; used by crafting canCraft checks.
int Inventory::count(uint16_t blockId) const {
    if (blockId == 0) return 0;
    int total = 0;
    for (const ItemStack& s : slots_) {
        if (s.blockId == blockId) total += s.count;
    }
    return total;
}

/// Remove up to `n` of `blockId` (hotbar first); returns how many could NOT be removed.
int Inventory::remove(uint16_t blockId, int n) {
    if (blockId == 0 || n <= 0) return n > 0 ? n : 0;
    for (int i = 0; i < kSlots && n > 0; ++i) {
        ItemStack& s = slots_[static_cast<size_t>(i)];
        if (s.blockId != blockId) continue;
        const int take = n < s.count ? n : s.count;
        s.count = static_cast<uint16_t>(s.count - take);
        n -= take;
        if (s.count == 0) s.clear();
    }
    return n; // 0 = removed everything requested
}

/// Decrement the selected hotbar slot by one and return its blockId; 0 if slot was empty.
uint16_t Inventory::takeFromSelected() {
    ItemStack& s = slots_[static_cast<size_t>(selected_)];
    if (s.empty()) {
        return 0;
    }
    const uint16_t id = s.blockId;
    if (--s.count == 0) {
        s.clear();
    }
    return id;
}

} // namespace vg
