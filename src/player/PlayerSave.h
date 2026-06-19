#pragma once

/**
 * @file PlayerSave.h
 * @brief Versioned binary (de)serialisation for persisted player state.
 *
 * Holds position, look, health, inventory slots, and equipment slots in a
 * self-describing byte buffer (magic "VGPL" + version). No file I/O — App reads
 * and writes the bytes. Header-only (all logic is inline). Headlessly
 * round-trip testable via `--logictest`.
 * @see docs/CODE_INDEX.md
 */

#include <glm/glm.hpp>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  PlayerSave
// -----------------------------------------------------------------------------
//  The persisted player state (position, look, health, inventory, game-mode),
//  saved next to the world's chunks as <save dir>/player.dat (ISSUES #13K). Kept
//  as a plain struct with buffer (de)serialisation — no file I/O — so it is pure
//  and headlessly round-trip testable (--logictest) and App just writes/reads the
//  bytes. Block ids are stored raw (like the chunk save): appending blocks stays
//  compatible, reordering would not.
// -----------------------------------------------------------------------------
/** @brief Serialisable snapshot of all player state that survives a quit/reload. */
struct PlayerSave {
    glm::vec3 feet{0.0f};
    float     yaw    = 0.0f;
    float     pitch  = 0.0f;
    float     health = 100.0f;
    int32_t   selected = 0;
    bool      creative = true;
    // One (blockId, count) per inventory slot, in slot order.
    std::vector<std::pair<uint16_t, uint16_t>> slots;
    // One (blockId, count) per equipment slot (armour + trinkets), in slot order.
    std::vector<std::pair<uint16_t, uint16_t>> equip;

    static constexpr char     kMagic[4] = {'V', 'G', 'P', 'L'};
    static constexpr uint32_t kVersion  = 2; // v2 adds the equipment list

    // Serialise to a self-describing byte buffer (magic + version + payload).
    [[nodiscard]] std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> b;
        auto put = [&](const void* p, size_t n) {
            const auto* c = static_cast<const uint8_t*>(p);
            b.insert(b.end(), c, c + n);
        };
        put(kMagic, 4);
        put(&kVersion, sizeof kVersion);
        put(&feet, sizeof feet);
        put(&yaw, sizeof yaw);
        put(&pitch, sizeof pitch);
        put(&health, sizeof health);
        put(&selected, sizeof selected);
        const uint8_t cr = creative ? 1u : 0u;
        put(&cr, 1);
        const uint16_t n = static_cast<uint16_t>(slots.size());
        put(&n, sizeof n);
        for (const auto& s : slots) {
            put(&s.first, sizeof s.first);
            put(&s.second, sizeof s.second);
        }
        const uint16_t e = static_cast<uint16_t>(equip.size());
        put(&e, sizeof e);
        for (const auto& s : equip) {
            put(&s.first, sizeof s.first);
            put(&s.second, sizeof s.second);
        }
        return b;
    }

    // Parse a buffer written by serialize(); false on bad magic/version/truncation.
    [[nodiscard]] bool deserialize(const uint8_t* data, size_t len) {
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
        if (!get(&feet, sizeof feet) || !get(&yaw, sizeof yaw) ||
            !get(&pitch, sizeof pitch) || !get(&health, sizeof health) ||
            !get(&selected, sizeof selected)) return false;
        uint8_t cr = 1;
        if (!get(&cr, 1)) return false;
        creative = cr != 0;
        uint16_t n = 0;
        if (!get(&n, sizeof n)) return false;
        slots.clear();
        slots.reserve(n);
        for (uint16_t i = 0; i < n; ++i) {
            uint16_t id = 0, count = 0;
            if (!get(&id, sizeof id) || !get(&count, sizeof count)) return false;
            slots.emplace_back(id, count);
        }
        uint16_t e = 0;
        if (!get(&e, sizeof e)) return false;
        equip.clear();
        equip.reserve(e);
        for (uint16_t i = 0; i < e; ++i) {
            uint16_t id = 0, count = 0;
            if (!get(&id, sizeof id) || !get(&count, sizeof count)) return false;
            equip.emplace_back(id, count);
        }
        return true;
    }
};

} // namespace vg
