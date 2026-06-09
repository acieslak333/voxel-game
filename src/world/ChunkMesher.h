#pragma once

#include "render/Vertex.h"

#include <cstdint>
#include <vector>

namespace vg {

class Chunk;
class BlockRegistry;

// CPU-side mesh: interleaved vertices + an index buffer. Uploaded to the GPU by
// the renderer.
struct MeshData {
    std::vector<Vertex>   vertices;
    std::vector<uint32_t> indices;
    [[nodiscard]] bool empty() const { return indices.empty(); }
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
    [[nodiscard]] static MeshData greedyMesh(const Chunk& chunk,
                                             const BlockRegistry& registry);
};

} // namespace vg
