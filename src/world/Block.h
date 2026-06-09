#pragma once

#include <cstdint>

namespace vg {

// -----------------------------------------------------------------------------
//  Block
// -----------------------------------------------------------------------------
//  A single voxel. Deliberately a small struct rather than a bare id so that
//  per-block state can grow later without changing chunk storage layout.
//
//  Keep this struct small (it is stored one-per-voxel, 4096 per chunk): more
//  fields *may* be added, but think hard before doing so.
// -----------------------------------------------------------------------------
struct Block {
    uint16_t id = 0;       // block type; 0 == air (see BlockId)
    uint8_t  metadata = 0; // TODO(future): orientation / state (furnace lit, log axis, ...)

    [[nodiscard]] bool isAir() const { return id == 0; }
};

// Built-in block type ids. The BlockRegistry is the single source of truth for
// their *properties*; this enum just gives the ids readable names.
enum class BlockId : uint16_t {
    Air   = 0,
    Grass = 1,
    Dirt  = 2,
    Stone = 3,
};

// The six faces of a cube, ordered so that index = axis*2 + (positive ? 1 : 0).
// Used to look up per-face texture layers in the block registry.
enum Face : int {
    FaceNegX = 0,
    FacePosX = 1,
    FaceNegY = 2,
    FacePosY = 3,
    FaceNegZ = 4,
    FacePosZ = 5,
    FaceCount = 6,
};

} // namespace vg
