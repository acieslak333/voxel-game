#include "world/SurfaceNets.h"

#include <array>

namespace vg {

namespace {

// Cell corner i has offset (i&1, (i>>1)&1, (i>>2)&1): corner 0=(0,0,0)..7=(1,1,1).
constexpr std::array<glm::vec3, 8> kCorner = {
    glm::vec3{0, 0, 0}, glm::vec3{1, 0, 0}, glm::vec3{0, 1, 0}, glm::vec3{1, 1, 0},
    glm::vec3{0, 0, 1}, glm::vec3{1, 0, 1}, glm::vec3{0, 1, 1}, glm::vec3{1, 1, 1}};

// The 12 cell edges as corner-index pairs.
constexpr std::array<std::array<int, 2>, 12> kEdge = {{
    {0, 1}, {2, 3}, {4, 5}, {6, 7},   // along X
    {0, 2}, {1, 3}, {4, 6}, {5, 7},   // along Y
    {0, 4}, {1, 5}, {2, 6}, {3, 7}}}; // along Z

} // namespace

SurfaceMesh surfaceNets(const std::function<float(int, int, int)>& field,
                        glm::ivec3 dim, float cellSize, glm::vec3 origin) {
    SurfaceMesh mesh;
    if (dim.x < 1 || dim.y < 1 || dim.z < 1) return mesh;

    const glm::ivec3 cdim = dim + 1;        // corners per axis
    const int cstride_y = cdim.x;
    const int cstride_z = cdim.x * cdim.y;
    const auto cidx = [&](int x, int y, int z) { return x + cstride_y * y + cstride_z * z; };

    // Sample every corner once (the quad pass re-reads corner signs).
    std::vector<float> corner(static_cast<size_t>(cdim.x) * cdim.y * cdim.z);
    for (int z = 0; z < cdim.z; ++z)
        for (int y = 0; y < cdim.y; ++y)
            for (int x = 0; x < cdim.x; ++x)
                corner[cidx(x, y, z)] = field(x, y, z);

    // -1 = cell has no surface vertex; else index into mesh.positions.
    const int vstride_y = dim.x;
    const int vstride_z = dim.x * dim.y;
    const auto vidx = [&](int x, int y, int z) { return x + vstride_y * y + vstride_z * z; };
    std::vector<int> cellVert(static_cast<size_t>(dim.x) * dim.y * dim.z, -1);

    // Pass 1 — one vertex per surface-straddling cell, at the mean edge crossing.
    for (int z = 0; z < dim.z; ++z) {
        for (int y = 0; y < dim.y; ++y) {
            for (int x = 0; x < dim.x; ++x) {
                std::array<float, 8> s{};
                int mask = 0;
                for (int i = 0; i < 8; ++i) {
                    s[i] = corner[cidx(x + static_cast<int>(kCorner[i].x),
                                       y + static_cast<int>(kCorner[i].y),
                                       z + static_cast<int>(kCorner[i].z))];
                    if (s[i] < 0.0f) mask |= (1 << i);
                }
                if (mask == 0 || mask == 0xFF) continue; // wholly inside/outside

                glm::vec3 sum(0.0f);
                int count = 0;
                for (const auto& e : kEdge) {
                    const float a = s[e[0]], b = s[e[1]];
                    if ((a < 0.0f) == (b < 0.0f)) continue; // no crossing on this edge
                    const float t = a / (a - b);            // linear zero-crossing
                    sum += kCorner[e[0]] + t * (kCorner[e[1]] - kCorner[e[0]]);
                    ++count;
                }
                const glm::vec3 local = sum / static_cast<float>(count); // in [0,1]^3
                cellVert[vidx(x, y, z)] =
                    static_cast<int>(mesh.positions.size());
                mesh.positions.push_back(
                    origin + (glm::vec3(x, y, z) + local) * cellSize);
                mesh.normals.emplace_back(0.0f);
            }
        }
    }

    // Pass 2 — for each grid edge that crosses zero, connect the four cells that
    // share it into a quad. We only look at the three edges leaving a cell's base
    // corner (+X/+Y/+Z) so each interior edge is visited exactly once.
    auto emitQuad = [&](int a, int b, int c, int d, bool flip) {
        if (a < 0 || b < 0 || c < 0 || d < 0) return; // a neighbour cell had no vertex
        if (flip) std::swap(b, d);
        const uint32_t ua = a, ub = b, uc = c, ud = d;
        mesh.indices.insert(mesh.indices.end(), {ua, ub, uc, ua, uc, ud});
    };
    for (int z = 0; z < dim.z; ++z) {
        for (int y = 0; y < dim.y; ++y) {
            for (int x = 0; x < dim.x; ++x) {
                const bool baseInside = corner[cidx(x, y, z)] < 0.0f;
                // +X edge — quad in the Y,Z plane (needs the y-1 / z-1 cells).
                // The `flip` picks the winding so the triangle normal faces OUT of
                // the surface (toward the +sign / outside half).
                if (y >= 1 && z >= 1 &&
                    baseInside != (corner[cidx(x + 1, y, z)] < 0.0f)) {
                    emitQuad(cellVert[vidx(x, y - 1, z - 1)], cellVert[vidx(x, y, z - 1)],
                             cellVert[vidx(x, y, z)],         cellVert[vidx(x, y - 1, z)],
                             !baseInside);
                }
                // +Y edge — quad in the Z,X plane.
                if (x >= 1 && z >= 1 &&
                    baseInside != (corner[cidx(x, y + 1, z)] < 0.0f)) {
                    emitQuad(cellVert[vidx(x - 1, y, z - 1)], cellVert[vidx(x, y, z - 1)],
                             cellVert[vidx(x, y, z)],         cellVert[vidx(x - 1, y, z)],
                             baseInside);
                }
                // +Z edge — quad in the X,Y plane.
                if (x >= 1 && y >= 1 &&
                    baseInside != (corner[cidx(x, y, z + 1)] < 0.0f)) {
                    emitQuad(cellVert[vidx(x - 1, y - 1, z)], cellVert[vidx(x, y - 1, z)],
                             cellVert[vidx(x, y, z)],         cellVert[vidx(x - 1, y, z)],
                             !baseInside);
                }
            }
        }
    }

    // Area-weighted smooth normals: accumulate each triangle's (un-normalised)
    // cross product into its three vertices, then normalise. No field gradient
    // needed, and shared edges average naturally.
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        const glm::vec3 n = glm::cross(mesh.positions[i1] - mesh.positions[i0],
                                       mesh.positions[i2] - mesh.positions[i0]);
        mesh.normals[i0] += n;
        mesh.normals[i1] += n;
        mesh.normals[i2] += n;
    }
    for (glm::vec3& n : mesh.normals) {
        const float len = glm::length(n);
        n = len > 1e-12f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
    }
    return mesh;
}

} // namespace vg
