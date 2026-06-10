#pragma once

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
};

using SolidFn = std::function<bool(int x, int y, int z)>;
// Optional: the X/Z inset of a thin (Model) block's box at a cell (0 = full cell,
// e.g. 0.3 for a slender trunk). When supplied, the ray must actually pierce the
// centred column to count as a hit — so aiming past a trunk targets the block
// behind it instead of grabbing the trunk's mostly-empty cell.
using InsetFn = std::function<float(int x, int y, int z)>;

[[nodiscard]] RaycastHit raycastVoxel(const glm::vec3& origin, const glm::vec3& dir,
                                      float maxDistance, const SolidFn& solid,
                                      const InsetFn& inset = {});

} // namespace vg
