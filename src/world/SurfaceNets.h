#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  Surface Nets (naive / dual contouring of a scalar field)
// -----------------------------------------------------------------------------
//  docs/WORLDGEN.md Layer 2.3: extract a SMOOTH triangle surface from a 3D scalar
//  density/SDF field — one vertex per cell that straddles the surface, positioned
//  at the average of the edge zero-crossings. This is what gives cliffs, the
//  monolithic arch and rocky floating-island bottoms rounded/sharp features that
//  the per-voxel greedy mesher (blocky quads) cannot. Marching Cubes is the
//  blockier fallback; Surface Nets is one vertex per cell, fast and smooth.
//
//  This module is deliberately standalone and game-agnostic: it consumes a scalar
//  field and emits positions/normals/indices, with NO dependency on the chunk
//  meshing, Vertex format or renderer. Wiring it into the live render path is a
//  separate, coordinated step. That keeps the algorithm verifiable in isolation
//  (--logictest) and out of the way of the voxel-face mesher.
//
//  Determinism & seam-safety: the output is a pure function of the sampled field.
//  If two adjacent chunks sample the SAME world-space field over their shared
//  boundary (mesh each chunk with a one-cell halo so cells on the seam see the
//  neighbour's corners), the boundary vertices are bit-identical — no cracks.
// -----------------------------------------------------------------------------
struct SurfaceMesh {
    std::vector<glm::vec3> positions; // world-space vertex positions
    std::vector<glm::vec3> normals;   // smooth per-vertex normals (area-weighted)
    std::vector<uint32_t>  indices;   // triangle list (3 per triangle)
    [[nodiscard]] bool empty() const { return indices.empty(); }
};

// Field convention: field(cx,cy,cz) is the scalar at integer grid CORNER
// (cx,cy,cz), sampled over cx∈[0,dim.x], cy∈[0,dim.y], cz∈[0,dim.z] (so dim is the
// number of CELLS per axis and dim+1 corners are read per axis). NEGATIVE = inside
// the surface, >= 0 = outside (matching vg::sdf and a (surfaceH - y)-style density).
// `cellSize` scales a cell to world units; `origin` is the world position of corner
// (0,0,0). A cell emits a vertex only when its 8 corners are not all one sign.
[[nodiscard]] SurfaceMesh surfaceNets(const std::function<float(int, int, int)>& field,
                                      glm::ivec3 dim, float cellSize, glm::vec3 origin);

} // namespace vg
