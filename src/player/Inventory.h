#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace vg {

// -----------------------------------------------------------------------------
//  ItemStack
// -----------------------------------------------------------------------------
//  One inventory cell: a block type and how many of it. blockId 0 (air) or a zero
//  count both mean "empty". Items are blocks for now; a future item registry could
//  widen this to tools/food without changing the slot layout.
// -----------------------------------------------------------------------------
struct ItemStack {
    uint16_t blockId = 0;
    uint16_t count   = 0;

    [[nodiscard]] bool empty() const { return blockId == 0 || count == 0; }
    void clear() { blockId = 0; count = 0; }
};

// -----------------------------------------------------------------------------
//  Inventory
// -----------------------------------------------------------------------------
//  The player's item storage: a 9-slot hotbar (slots 0..8, one always "selected")
//  plus a 27-slot backpack (slots 9..35), exactly like the classic layout. Mining
//  a block calls add(); placing the held item calls takeFromSelected(). The full
//  grid is shown/edited by the inventory screen (App::buildInventory); the hotbar
//  row is always on the HUD.
//
//  This is pure data — no dependency on the world or renderer — so it is trivially
//  serialisable later (save/load) and testable in isolation.
// -----------------------------------------------------------------------------
class Inventory {
public:
    static constexpr int kHotbarSlots  = 9;
    static constexpr int kStorageRows  = 3;
    static constexpr int kStorageCols  = 9;
    static constexpr int kStorageSlots = kStorageRows * kStorageCols; // 27
    static constexpr int kSlots        = kHotbarSlots + kStorageSlots; // 36
    static constexpr uint16_t kMaxStack = 99;

    [[nodiscard]] ItemStack&       slot(int i)       { return slots_[static_cast<size_t>(i)]; }
    [[nodiscard]] const ItemStack& slot(int i) const { return slots_[static_cast<size_t>(i)]; }

    // The selected hotbar slot (0..kHotbarSlots-1) and the stack it points at.
    [[nodiscard]] int selected() const { return selected_; }
    void setSelected(int i) {
        selected_ = ((i % kHotbarSlots) + kHotbarSlots) % kHotbarSlots; // wrap
    }
    void scrollSelected(int delta) { setSelected(selected_ + delta); }
    [[nodiscard]] const ItemStack& selectedStack() const { return slots_[static_cast<size_t>(selected_)]; }

    // Add `count` of `blockId`, topping up matching stacks first, then filling empty
    // slots (hotbar before backpack). Returns the number that didn't fit (0 = all in).
    int add(uint16_t blockId, int count = 1);

    // Consume one item from the selected hotbar slot; returns its blockId, or 0 if
    // the slot was empty (nothing to place).
    uint16_t takeFromSelected();

private:
    std::array<ItemStack, kSlots> slots_{};
    int selected_ = 0;
};

} // namespace vg
