#pragma once

#include "render/Vertex.h"
#include "world/Block.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace vg {

class Chunk;
class BlockRegistry;

// CPU-side mesh: interleaved vertices + an index buffer. Uploaded to the GPU by
// the renderer. Liquid (water) surfaces are kept in a SEPARATE batch so the
// renderer can draw them in a second, alpha-blended pass after the opaque
// geometry — letting the seabed/terrain behind the water show through. The
// water indices are 0-based into `waterVertices` (the renderer offsets them).
struct MeshData {
    std::vector<Vertex>   vertices;       // opaque terrain + plants
    std::vector<uint32_t> indices;
    std::vector<Vertex>   waterVertices;  // translucent water surfaces
    std::vector<uint32_t> waterIndices;
    [[nodiscard]] bool empty() const { return indices.empty() && waterIndices.empty(); }
};

// -----------------------------------------------------------------------------
//  ChunkMesher
// -----------------------------------------------------------------------------
//  Turns a chunk into a triangle mesh using *greedy meshing*: adjacent, coplanar
//  faces of the same block type are merged into a single large quad, drastically
//  cutting triangle count versus one quad per block face. Face culling is built
//  in — a face is only emitted where a solid block borders a non-opaque one.
// -----------------------------------------------------------------------------
class ChunkMesher {
public:
    // Samples a block at chunk-local coords that lie *outside* the chunk (one
    // step over an edge, so coords range over [-1, kSize]). Returning the
    // neighbouring chunk's block here lets the mesher cull faces on the boundary
    // between two solid chunks; returning air (the default) meshes the chunk in
    // isolation, as if surrounded by empty space.
    using NeighborSampler = std::function<Block(int x, int y, int z)>;

    // Sky- and block-light levels (each 0..15) at a world cell. Kept separate so
    // the shader can give them different colours (a warm sun vs a glowstone glow).
    // blockColor is the linear-RGB hue of the block light here (the dominant
    // emitter's colour); meaningful only where block > 0.
    struct LightSample {
        uint8_t   sky   = 15;
        uint8_t   block = 0;
        glm::vec3 blockColor{0.0f};
    };
    // Samples the light at chunk-local coords (which may lie just outside the
    // chunk). Light is a world-level field, so unlike blocks there is no in-chunk
    // fast path — every lookup goes through this.
    using LightSampler = std::function<LightSample(int x, int y, int z)>;

    // smoothLighting: true folds per-corner ambient occlusion + averaged sky
    // light into each vertex (soft, modern look); false uses only the flat
    // directional top/side/bottom shade (the simpler original look).
    // worldOrigin: this chunk's minimum-corner block coordinate in world space
    // (chunkCoord * kChunkSize). Used to seed the per-block texture-variant hash
    // so the choice is stable and seamless across chunk boundaries.
    [[nodiscard]] static MeshData greedyMesh(const Chunk& chunk,
                                             const BlockRegistry& registry,
                                             const NeighborSampler& neighbor,
                                             const LightSampler& light,
                                             bool smoothLighting,
                                             const glm::ivec3& worldOrigin);
};

} // namespace vg
