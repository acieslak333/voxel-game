#pragma once

#include "world/BlockRegistry.h"
#include "world/Chunk.h"
#include "world/Noise.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  World
// -----------------------------------------------------------------------------
//  A fixed grid of chunks generated from noise (Milestone 3). Owns the block
//  registry and all chunk data, and answers world-space queries (solidity,
//  surface height) used by the renderer and the player.
//
//  Terrain uses two noise layers: one for the base surface *height*, and a
//  second for *material/biome* variation (rocky vs grassy, dirt depth). Island
//  shaping and chunk streaming are deliberately left for later milestones; the
//  grid here is generated once around the origin.
// -----------------------------------------------------------------------------
class World {
public:
    World(uint32_t seed, int chunksX, int chunksY, int chunksZ);

    [[nodiscard]] glm::ivec3 chunkCounts()  const { return counts_; }
    [[nodiscard]] glm::ivec3 sizeInBlocks() const { return counts_ * Chunk::kSize; }
    [[nodiscard]] const BlockRegistry& registry() const { return registry_; }
    [[nodiscard]] const Chunk& chunk(int cx, int cy, int cz) const;

    // Is the block at world coords solid? Air (false) outside the world bounds.
    [[nodiscard]] bool isSolid(int wx, int wy, int wz) const;

    // Y to stand on at world column (wx, wz): one above the topmost solid block.
    [[nodiscard]] int surfaceHeight(int wx, int wz) const;

private:
    void generate();
    void generateChunk(int cx, int cy, int cz);

    // Topmost solid block's Y at a world column, from the height noise.
    [[nodiscard]] int columnHeight(int wx, int wz) const;

    [[nodiscard]] int  chunkIndex(int cx, int cy, int cz) const;
    [[nodiscard]] bool inChunkBounds(int cx, int cy, int cz) const;

    glm::ivec3    counts_;     // number of chunks along x, y, z
    BlockRegistry registry_;
    Noise         heightNoise_;
    Noise         materialNoise_;
    std::vector<Chunk> chunks_; // flat, indexed by chunkIndex()
};

} // namespace vg
