#pragma once

/**
 * @file ChunkMesher.h
 * @brief Greedy voxel mesher: turns a Chunk into a renderable MeshData.
 *
 * ChunkMesher::greedyMesh() merges coplanar, same-block faces into large quads
 * (greedy meshing), performs face culling via the NeighborSampler, computes
 * per-corner ambient occlusion, and emits opaque terrain and translucent water in
 * separate index buffers. Per-pixel sky/block light comes from a light atlas rather
 * than being baked into each vertex.
 * @see docs/CODE_INDEX.md
 */

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
/**
 * @brief CPU-side triangle mesh output from ChunkMesher::greedyMesh().
 *
 * Opaque geometry and translucent water surfaces are kept in separate buffers
 * so the renderer can draw them in two distinct passes.
 */
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

    // Biome vegetation tint for a tintable block at chunk-local (x,z): the shader
    // multiplies the albedo by it, so grass/leaves take a per-biome colour. Returns
    // white (1,1,1) for non-tintable blocks (the common case), so the mesher can
    // call it for every face and only foliage actually tints.
    using TintSampler = std::function<glm::vec3(int x, int z, uint16_t id)>;

    /**
     * @brief Generate the full mesh for `chunk`.
     *
     * Runs the greedy merge sweep for opaque terrain and translucent water, then
     * a second non-cube pass for plants, thin posts, flowing liquids, and shaped
     * blocks. The result is ready to upload to the GPU via WorldRenderer.
     *
     * @param chunk       The source voxel data (read-only; mesh workers call this).
     * @param registry    Block-type database for opacity, texture layers, etc.
     * @param neighbor    Sampler for blocks one step outside chunk bounds (for culling).
     * @param light       Sky + block light levels at chunk-local (possibly out-of-bounds) coords.
     * @param smoothLighting True = per-corner AO + soft shading; false = flat directional only.
     * @param worldOrigin Minimum-corner world block coord; seeds the texture-variant hash.
     * @param tint        Biome vegetation tint for tintable block faces.
     * @return Populated MeshData with separate opaque and water buffers.
     * @note Thread-safe: reads only const Chunk and registry data. Called by mesh workers.
     */
    [[nodiscard]] static MeshData greedyMesh(const Chunk& chunk,
                                             const BlockRegistry& registry,
                                             const NeighborSampler& neighbor,
                                             const LightSampler& light,
                                             bool smoothLighting,
                                             const glm::ivec3& worldOrigin,
                                             const TintSampler& tint);
};

} // namespace vg
