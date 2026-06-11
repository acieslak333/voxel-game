#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  Block shapes
// -----------------------------------------------------------------------------
//  A *shapeable* solid block (BlockRegistry::shapeable) can be reshaped in place
//  by the hammer into a slab, stairs, post or wall instead of a full cube. The
//  shape + orientation is per-PLACED-block state, stored in Block::metadata, so
//  the same block type can appear as many shapes. metadata 0 decodes to
//  { Cube, orient 0 }, so every existing block and saved chunk stays a full cube
//  (backward compatible — liquids, which also use metadata, are not shapeable).
//
//  shapeBoxes() is the SINGLE source of truth: it returns the axis-aligned box
//  union for a shape, consumed identically by the mesher (textured faces) and by
//  collision / raycasting (solid volume). They can never disagree.
// -----------------------------------------------------------------------------
enum class ShapeKind : uint8_t {
    Cube         = 0, // full voxel cube (the default)
    Slab         = 1, // half block on the floor or ceiling (orient bit 0: 0 bottom, 1 top)
    Stairs       = 2, // a step (orient bits 0-1: facing; bit 2: top/bottom half)
    Post         = 3, // a centred column (orient 0-2: axis X/Y/Z) — fences, thin logs
    Wall         = 4, // centre post + arms toward connected neighbours (orient unused)
    VerticalSlab = 5, // half block stood against one horizontal side (orient 0-3)
};

// An axis-aligned sub-box of a unit cell, in local [0,1]^3 coordinates (y up).
struct ShapeBox {
    glm::vec3 lo;
    glm::vec3 hi;
};

// Most boxes any shape needs (a Wall = centre post + up to four arms). Callers
// can stack-allocate a buffer of this size and avoid heap churn in hot paths
// (per-frame collision, raycasting).
constexpr int kMaxShapeBoxes = 5;

// metadata layout for a shapeable solid: bits 0-2 = ShapeKind, bits 3-5 = orient.
constexpr uint8_t packShape(ShapeKind k, uint8_t orient) {
    return static_cast<uint8_t>((static_cast<uint8_t>(k) & 0x7u) |
                                ((orient & 0x7u) << 3));
}
constexpr ShapeKind shapeKindOf(uint8_t metadata) {
    return static_cast<ShapeKind>(metadata & 0x7u);
}
constexpr uint8_t shapeOrientOf(uint8_t metadata) {
    return static_cast<uint8_t>((metadata >> 3) & 0x7u);
}

// Number of distinct orientations a shape cycles through (Wall derives its own
// connections, so it has a single stored orientation).
[[nodiscard]] int shapeOrientCount(ShapeKind kind);

// The box union for a shape, written into `out` (capacity >= kMaxShapeBoxes);
// returns the box count. `wallMask` (bits: 0 -Z, 1 +X, 2 +Z, 3 -X) is only
// consulted for Wall; pass 0 for every other shape. A Cube returns the single
// full-cell box, so callers can treat all shapes uniformly. Coords are local
// to the cell, in [0,1].
int shapeBoxes(ShapeKind kind, uint8_t orient, uint8_t wallMask, ShapeBox out[]);

// Convenience wrapper that fills a vector (cleared first).
void shapeBoxes(ShapeKind kind, uint8_t orient, uint8_t wallMask,
                std::vector<ShapeBox>& out);

} // namespace vg
