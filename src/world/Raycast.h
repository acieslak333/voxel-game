#pragma once

/**
 * @file Raycast.h
 * @brief Amanatides & Woo DDA voxel raycast used for block targeting.
 *
 * raycastVoxel() marches through the grid one voxel at a time and reports the
 * first solid block the ray enters. An optional BoxesFn refines the hit against
 * per-cell sub-boxes (slabs, stairs, thin posts) so aiming past a shaped surface
 * targets the block behind it. The returned normal points back at the ray origin
 * (the face the ray came in through), used to determine where a placed block lands.
 * @see docs/CODE_INDEX.md
 */

#include "world/Shape.h"

#include <glm/glm.hpp>

#include <functional>

namespace vg {

// -----------------------------------------------------------------------------
//  Voxel raycast
// -----------------------------------------------------------------------------
//  Marches a ray through the voxel grid (Amanatides & Woo DDA) and reports the
//  first solid block it enters. Used for "what block am I looking at?" — block
//  breaking targets `block`, placing targets `block + normal` (the empty cell on
//  the face the ray came in through).
//
//  Solidity is queried through a predicate so this has no dependency on how the
//  world is stored, mirroring PlayerController's SolidFn seam.
// -----------------------------------------------------------------------------
/**
 * @brief Result of a voxel raycast.
 *
 * When hit is false all other fields are zero-initialised and must not be used.
 */
struct RaycastHit {
    bool       hit = false;
    glm::ivec3 block{0};  // the solid voxel the ray entered
    glm::ivec3 normal{0}; // face it entered through, pointing back at the origin
    glm::vec3  point{0};  // world-space hit position (on the entered face / sub-box)
};

using SolidFn = std::function<bool(int x, int y, int z)>;
// Optional: the collision boxes of a cell, in WORLD coordinates (vg::shapeBoxes);
// returns the count (0 = no sub-box, treat as a full cell). When supplied, the ray
// must actually pierce one of the boxes to count as a hit — so aiming past a thin
// post/slab/stair targets the block behind it, and `normal`/`point` describe the
// sub-box face that was struck (used to orient a placed shape). A cell that is
// targetable but returns 0 boxes (e.g. cut-out foliage) is hit as a full cell.
using BoxesFn = std::function<int(int x, int y, int z, ShapeBox out[])>;

/**
 * @brief March a ray through the voxel grid and return the first solid hit.
 *
 * @param origin      Ray start in world space.
 * @param dir         Ray direction (need not be normalised; zero -> no hit).
 * @param maxDistance Maximum reach in world units.
 * @param solid       Predicate: true if (x,y,z) is a targetable block.
 * @param boxes       Optional sub-box provider for shaped blocks. When supplied,
 *                    the ray must pierce an actual sub-box to score a hit;
 *                    cells returning 0 boxes are treated as full-cell targets.
 * @return Hit descriptor; hit=false when nothing was found within maxDistance.
 */
[[nodiscard]] RaycastHit raycastVoxel(const glm::vec3& origin, const glm::vec3& dir,
                                      float maxDistance, const SolidFn& solid,
                                      const BoxesFn& boxes = {});

} // namespace vg
