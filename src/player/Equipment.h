#pragma once

#include "player/Inventory.h" // ItemStack
#include "world/BlockRegistry.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace vg {

// -----------------------------------------------------------------------------
//  Equipment (ISSUES #13B — armour + trinkets)
// -----------------------------------------------------------------------------
//  The player's equipped items: four armour slots (head/chest/legs/feet) and four
//  generic trinket/accessory slots, Terraria-style. Pure data + a pure stat
//  aggregator (computeStats) so it has no renderer/world dependency and is
//  headlessly testable. App pushes the resulting scalars onto PlayerController.
// -----------------------------------------------------------------------------
struct Equipment {
    static constexpr int kArmorSlots   = 4; // 0=head 1=chest 2=legs 3=feet
    static constexpr int kTrinketSlots = 4;
    static constexpr int kSlots        = kArmorSlots + kTrinketSlots; // 8

    std::array<ItemStack, kSlots> slots{};

    // Which EquipSlot a given slot index accepts (the four armour pieces in order,
    // then Trinket for the rest). Used to validate what the UI may drop into a slot.
    [[nodiscard]] static EquipSlot accepts(int slotIndex) {
        switch (slotIndex) {
            case 0: return EquipSlot::Head;
            case 1: return EquipSlot::Chest;
            case 2: return EquipSlot::Legs;
            case 3: return EquipSlot::Feet;
            default: return EquipSlot::Trinket;
        }
    }
    // Can item `id` go in slot `slotIndex`? Empty (id 0) always allowed (clearing).
    [[nodiscard]] static bool fits(const BlockRegistry& reg, int slotIndex, uint16_t id) {
        return id == 0 || reg.equip(id) == accepts(slotIndex);
    }

    // Aggregated effect of everything equipped.
    struct Stats {
        float armorReduction = 0.0f; // damage-reduction FRACTION (0..0.8), already capped
        float speedMul       = 1.0f;
        float jumpMul        = 1.0f;
        float regenBonus     = 0.0f; // extra HP/s
    };

    [[nodiscard]] Stats computeStats(const BlockRegistry& reg) const {
        float armorPoints = 0.0f;
        Stats s;
        for (const ItemStack& it : slots) {
            if (it.empty()) continue;
            const BlockProperties& p = reg.get(it.blockId);
            armorPoints += p.armor;
            s.speedMul   *= p.speedMul;
            s.jumpMul    *= p.jumpMul;
            s.regenBonus += p.regenBonus;
        }
        // 1 armour point = 1% reduction, capped at 80% so you're never invincible.
        s.armorReduction = std::min(0.8f, armorPoints / 100.0f);
        return s;
    }
};

} // namespace vg
