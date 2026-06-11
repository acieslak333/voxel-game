#pragma once

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

[[nodiscard]] RaycastHit raycastVoxel(const glm::vec3& origin, const glm::vec3& dir,
                                      float maxDistance, const SolidFn& solid,
                                      const BoxesFn& boxes = {});

} // namespace vg
