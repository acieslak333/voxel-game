/**
 * @file ChestStore.cpp
 * @brief ChestStore binary serialisation and position-key unpack helper.
 * @see docs/CODE_INDEX.md
 */

#include "player/ChestStore.h"

#include <cstring>

namespace vg {

namespace {
constexpr char     kMagic[4] = {'V', 'G', 'C', 'H'};
constexpr uint32_t kVersion  = 1;

// Recover the signed components from a packed key (sign-extend each 21-bit field).
glm::ivec3 unpack(int64_t k) {
    auto s = [](int64_t v) -> int {
        v &= 0x1FFFFF;
        if (v & 0x100000) v |= ~static_cast<int64_t>(0x1FFFFF); // sign-extend bit 20
        return static_cast<int>(v);
    };
    return {s(k >> 42), s(k >> 21), s(k)};
}
} // namespace

/// Serialise all non-empty chests to a self-describing byte buffer (magic + version + payload).
std::vector<uint8_t> ChestStore::serialize() const {
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) {
        const auto* c = static_cast<const uint8_t*>(p);
        b.insert(b.end(), c, c + n);
    };
    put(kMagic, 4);
    put(&kVersion, sizeof kVersion);
    // Count only chests that actually hold something, so emptied chests don't bloat
    // the file (a fully-empty chest carries no state worth saving).
    uint32_t count = 0;
    for (const auto& [k, chest] : chests_) {
        for (const ItemStack& s : chest) {
            if (!s.empty()) { ++count; break; }
        }
    }
    put(&count, sizeof count);
    for (const auto& [k, chest] : chests_) {
        bool any = false;
        for (const ItemStack& s : chest) if (!s.empty()) { any = true; break; }
        if (!any) continue;
        const glm::ivec3 p = unpack(k);
        put(&p.x, sizeof p.x);
        put(&p.y, sizeof p.y);
        put(&p.z, sizeof p.z);
        for (const ItemStack& s : chest) {
            put(&s.blockId, sizeof s.blockId);
            put(&s.count, sizeof s.count);
        }
    }
    return b;
}

/// Parse a buffer written by serialize(); returns false on bad magic, wrong version, or truncation.
bool ChestStore::deserialize(const uint8_t* data, size_t len) {
    size_t off = 0;
    auto get = [&](void* p, size_t n) {
        if (off + n > len) return false;
        std::memcpy(p, data + off, n);
        off += n;
        return true;
    };
    char magic[4];
    uint32_t version = 0;
    if (!get(magic, 4) || !get(&version, sizeof version)) return false;
    if (std::memcmp(magic, kMagic, 4) != 0 || version != kVersion) return false;
    uint32_t count = 0;
    if (!get(&count, sizeof count)) return false;
    chests_.clear();
    for (uint32_t i = 0; i < count; ++i) {
        glm::ivec3 p;
        if (!get(&p.x, sizeof p.x) || !get(&p.y, sizeof p.y) || !get(&p.z, sizeof p.z)) {
            return false;
        }
        Chest chest{};
        for (int s = 0; s < kSlots; ++s) {
            if (!get(&chest[static_cast<size_t>(s)].blockId, sizeof(uint16_t)) ||
                !get(&chest[static_cast<size_t>(s)].count, sizeof(uint16_t))) {
                return false;
            }
        }
        chests_[key(p)] = chest;
    }
    return true;
}

} // namespace vg
