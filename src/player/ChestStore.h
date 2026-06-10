#pragma once

#include "player/Inventory.h" // ItemStack

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <unordered_map>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  ChestStore (ISSUES #13B)
// -----------------------------------------------------------------------------
//  The contents of every placed chest, keyed by world position. Chest BLOCKS are
//  persisted with their chunk (the streaming save already writes edited chunks);
//  this holds the matching slot payloads, saved to <world save dir>/chests.dat so
//  contents survive a reload. Kept separate from the chunk format (and free of
//  file I/O) so it's a small, pure, headlessly round-trip-testable unit; App does
//  the actual file read/write.
//
//  NOTE: the position key packs x/y/z into 21 signed bits each, so it is exact for
//  coordinates within ±2^20 (~1e6) of the origin — far beyond normal play. Outside
//  that, two chests could alias; revisit with a wider key if worlds get that large.
// -----------------------------------------------------------------------------
class ChestStore {
public:
    static constexpr int kSlots = 27; // 3x9 grid, like the backpack
    using Chest = std::array<ItemStack, kSlots>;

    [[nodiscard]] bool has(const glm::ivec3& pos) const {
        return chests_.find(key(pos)) != chests_.end();
    }
    // Get (creating an empty chest if absent) the chest at `pos`.
    [[nodiscard]] Chest& at(const glm::ivec3& pos) { return chests_[key(pos)]; }
    void erase(const glm::ivec3& pos) { chests_.erase(key(pos)); }

    [[nodiscard]] const std::unordered_map<int64_t, Chest>& all() const { return chests_; }

    [[nodiscard]] std::vector<uint8_t> serialize() const;
    bool deserialize(const uint8_t* data, size_t len);

    // Pack/unpack a world position to/from the 64-bit map key (exposed so the file
    // layer can store the unpacked coords).
    [[nodiscard]] static int64_t key(const glm::ivec3& p) {
        auto m = [](int v) -> int64_t { return static_cast<int64_t>(v) & 0x1FFFFF; };
        return (m(p.x) << 42) | (m(p.y) << 21) | m(p.z);
    }

private:
    std::unordered_map<int64_t, Chest> chests_;
};

} // namespace vg
