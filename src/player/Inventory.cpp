#include "player/Inventory.h"

namespace vg {

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
