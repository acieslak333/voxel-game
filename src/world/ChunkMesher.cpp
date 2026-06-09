#include "world/ChunkMesher.h"

#include "world/BlockRegistry.h"
#include "world/Chunk.h"

#include <array>

namespace vg {

namespace {

constexpr int N = kChunkSize;

// Cheap directional shading so the geometry reads without real lighting:
// top faces brightest, bottom darkest, sides in between (and X != Z so adjacent
// walls are distinguishable).
float faceShade(int axis, bool positive) {
    if (axis == 1) return positive ? 1.0f : 0.45f; // +Y top / -Y bottom
    if (axis == 0) return 0.62f;                    // X faces
    return 0.80f;                                   // Z faces
}

// Map (axis, sign) to the Face enum used by the block registry.
int faceIndex(int axis, bool positive) {
    return axis * 2 + (positive ? 1 : 0);
}

// One cell of the 2D mask used while sweeping a slice. Two cells may be merged
// into the same quad only if every field matches.
struct Mask {
    bool     present  = false;
    uint16_t block    = 0;
    bool     positive = false; // face normal points along +axis?
    uint32_t layer    = 0;
    float    shade    = 0.0f;

    bool operator==(const Mask& o) const {
        return present == o.present && block == o.block && positive == o.positive &&
               layer == o.layer && shade == o.shade;
    }
    bool operator!=(const Mask& o) const { return !(*this == o); }
};

} // namespace

MeshData ChunkMesher::greedyMesh(const Chunk& chunk, const BlockRegistry& reg) {
    MeshData mesh;

    auto opaqueAt = [&](int x, int y, int z) {
        return reg.isOpaque(chunk.getOrAir(x, y, z).id);
    };
    auto idAt = [&](int x, int y, int z) { return chunk.getOrAir(x, y, z).id; };

    // Append a w x h quad lying on a plane perpendicular to `d`, with its lower
    // corner at (base in u,v) and the given face attributes.
    auto addQuad = [&](int d, int u, int v, int plane, int i, int j, int w, int h,
                       const Mask& m) {
        float du[3] = {0, 0, 0}; du[u] = static_cast<float>(w);
        float dv[3] = {0, 0, 0}; dv[v] = static_cast<float>(h);
        float base[3] = {0, 0, 0};
        base[d] = static_cast<float>(plane);
        base[u] = static_cast<float>(i);
        base[v] = static_cast<float>(j);

        const glm::vec3 p0(base[0], base[1], base[2]);
        const glm::vec3 p1(base[0] + du[0], base[1] + du[1], base[2] + du[2]);
        const glm::vec3 p2(base[0] + du[0] + dv[0], base[1] + du[1] + dv[1],
                           base[2] + du[2] + dv[2]);
        const glm::vec3 p3(base[0] + dv[0], base[1] + dv[1], base[2] + dv[2]);

        // UVs in *block units* (w x h) so the texture repeats once per block
        // under REPEAT addressing instead of stretching over the merged quad.
        const float fw = static_cast<float>(w);
        const float fh = static_cast<float>(h);

        const auto base_index = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({p0, {0.f, 0.f}, m.layer, m.shade});
        mesh.vertices.push_back({p1, {fw,  0.f}, m.layer, m.shade});
        mesh.vertices.push_back({p2, {fw,  fh }, m.layer, m.shade});
        mesh.vertices.push_back({p3, {0.f, fh }, m.layer, m.shade});

        // Winding: (u, v, d) form a right-handed basis, so p0->p1->p2->p3 is
        // counter-clockwise seen from the +d side. Positive faces use that
        // order; negative faces are reversed so their front side faces -d.
        if (m.positive) {
            mesh.indices.insert(mesh.indices.end(),
                {base_index + 0, base_index + 1, base_index + 2,
                 base_index + 0, base_index + 2, base_index + 3});
        } else {
            mesh.indices.insert(mesh.indices.end(),
                {base_index + 0, base_index + 2, base_index + 1,
                 base_index + 0, base_index + 3, base_index + 2});
        }
    };

    // Sweep each of the three axes.
    for (int d = 0; d < 3; ++d) {
        const int u = (d + 1) % 3;
        const int v = (d + 2) % 3;

        int x[3] = {0, 0, 0};
        int q[3] = {0, 0, 0};
        q[d] = 1; // step of one block along the sweep axis

        std::vector<Mask> mask(static_cast<size_t>(N) * N);

        // x[d] walks the boundaries between slices, from the -1|0 boundary up to
        // the (N-1)|N boundary, so both outer faces of the chunk are produced.
        for (x[d] = -1; x[d] < N;) {
            // --- Build the mask for this slice boundary ---------------------
            int n = 0;
            for (x[v] = 0; x[v] < N; ++x[v]) {
                for (x[u] = 0; x[u] < N; ++x[u], ++n) {
                    // Compare the block on each side of the boundary. getOrAir
                    // treats outside-the-chunk as air, so chunk-edge faces show.
                    const bool a = opaqueAt(x[0], x[1], x[2]);
                    const bool b = opaqueAt(x[0] + q[0], x[1] + q[1], x[2] + q[2]);

                    Mask m;
                    if (a == b) {
                        // Both solid or both see-through: the face is hidden.
                        m.present = false;
                    } else if (a) {
                        // 'a' is the solid one; its face points toward +d.
                        const uint16_t blk = idAt(x[0], x[1], x[2]);
                        m = {true, blk, true, reg.faceLayer(blk, faceIndex(d, true)),
                             faceShade(d, true)};
                    } else {
                        // 'b' is the solid one; its face points toward -d.
                        const uint16_t blk = idAt(x[0] + q[0], x[1] + q[1], x[2] + q[2]);
                        m = {true, blk, false, reg.faceLayer(blk, faceIndex(d, false)),
                             faceShade(d, false)};
                    }
                    mask[n] = m;
                }
            }

            ++x[d]; // mask now describes faces lying on plane x[d]

            // --- Greedily merge equal mask cells into maximal rectangles ----
            n = 0;
            for (int j = 0; j < N; ++j) {
                for (int i = 0; i < N;) {
                    if (!mask[n].present) {
                        ++i;
                        ++n;
                        continue;
                    }

                    // Grow width along u while cells match.
                    int w = 1;
                    while (i + w < N && mask[n + w] == mask[n]) {
                        ++w;
                    }

                    // Grow height along v while every cell in the next row matches.
                    int h = 1;
                    bool stop = false;
                    while (j + h < N && !stop) {
                        for (int k = 0; k < w; ++k) {
                            if (mask[n + k + h * N] != mask[n]) {
                                stop = true;
                                break;
                            }
                        }
                        if (!stop) ++h;
                    }

                    addQuad(d, u, v, x[d], i, j, w, h, mask[n]);

                    // Mark the merged region consumed.
                    for (int l = 0; l < h; ++l) {
                        for (int k = 0; k < w; ++k) {
                            mask[n + k + l * N].present = false;
                        }
                    }

                    i += w;
                    n += w;
                }
            }
        }
    }

    return mesh;
}

} // namespace vg
