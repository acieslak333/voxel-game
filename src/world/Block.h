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
    uint16_t id = 0;       // block type; 0 == air, always (the default voxel)
    uint8_t  metadata = 0; // TODO(future): orientation / state (furnace lit, log axis, ...)

    [[nodiscard]] bool isAir() const { return id == 0; }
};

// Block *types* are defined in data (assets/blocks.yaml) and loaded by the
// BlockRegistry, which assigns each an id in file order and answers
// idByName("grass") for code that needs a specific block. Id 0 is reserved for
// "air" (the default-constructed Block), which the registry validates on load.

// How a block turns into geometry. Cube goes through greedy meshing; the rest
// emit their own geometry in a second mesher pass (bypassing greedy) — this is
// what lets non-cube blocks (plants, thin posts) exist alongside the voxel grid.
// Non-cube blocks must be non-opaque so the greedy pass treats their cell as open
// space (the ground under them still meshes, and they don't cull neighbour faces).
enum class RenderType : uint8_t {
    Cube  = 0, // full voxel cube, greedy-meshed (the default)
    Cross = 1, // two intersecting vertical quads (an X): leaves/plants. Double-sided cutout.
    Model = 2, // a centred axis-aligned box thinner than the cell: a slender trunk/post.
    // A full voxel cube AND the cross planes, both cutout-textured: dense foliage
    // that reads as solid leaf blocks with extra fronds poking through. Like Cross,
    // it is non-opaque and emitted in the mesher's non-cube pass (cube faces are
    // culled against opaque neighbours and other LeafCube cells of the same id).
    LeafCube = 3,
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
