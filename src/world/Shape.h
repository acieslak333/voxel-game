#pragma once

/**
 * @file Shape.h
 * @brief Sub-voxel shape system for hammer-reshaped blocks (slabs, stairs, posts, walls).
 *
 * ShapeKind and orientation are packed into Block::metadata (bits 0-2 = kind,
 * bits 3-5 = orient). shapeBoxes() is the single source of truth for both the
 * mesher (geometry) and collision/raycasting (solid volume) — they can never
 * disagree. metadata 0 decodes to {Cube, orient 0} so all existing saved chunks
 * remain full cubes (backward compatible).
 * @see docs/CODE_INDEX.md
 */

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

/**
 * @brief Number of distinct stored orientations for `kind` (Wall = 1; connections
 *        are derived from neighbour state, not stored in metadata).
 */
[[nodiscard]] int shapeOrientCount(ShapeKind kind);

/**
 * @brief Write the axis-aligned box union for a shape into `out`.
 *
 * @param kind     The shape variant.
 * @param orient   Orientation bits from Block::metadata (see shapeOrientOf).
 * @param wallMask For Wall only: bitmask of connected arms (0=-Z,1=+X,2=+Z,3=-X).
 *                 Pass 0 for all other shapes.
 * @param out      Output buffer; must have capacity >= kMaxShapeBoxes.
 * @return Number of boxes written (1 for Cube, up to kMaxShapeBoxes for Wall).
 * @note This is the single source of truth shared by the mesher and by
 *       collision / raycasting — the two subsystems can never disagree.
 */
int shapeBoxes(ShapeKind kind, uint8_t orient, uint8_t wallMask, ShapeBox out[]);

// Convenience wrapper that fills a vector (cleared first).
void shapeBoxes(ShapeKind kind, uint8_t orient, uint8_t wallMask,
                std::vector<ShapeBox>& out);

} // namespace vg
