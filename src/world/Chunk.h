#pragma once

/**
 * @file Chunk.h
 * @brief Fixed 16x16x16 voxel storage unit.
 *
 * A Chunk is the smallest independently meshable volume of the world. It owns
 * a flat, contiguous Block array (good cache behaviour for the mesher) and
 * exposes bounds-checked accessors. ChunkMesher reads chunks; World owns them.
 * @see docs/CODE_INDEX.md
 */

#include "world/Block.h"

#include <array>
#include <cstddef>

namespace vg {

/// Edge length of a chunk in blocks. Compile-time constant so the storage is a
/// fixed-size array and the index math is trivially optimisable.
inline constexpr int kChunkSize = 16;

// -----------------------------------------------------------------------------
//  Chunk
// -----------------------------------------------------------------------------
//  A fixed 16x16x16 block of voxels stored in a flat, contiguous array (good
//  for cache behaviour while meshing). A chunk knows how to get/set blocks; the
//  ChunkMesher (separate class) turns it into renderable geometry.
// -----------------------------------------------------------------------------
/**
 * @brief A 16x16x16 block of voxels in a flat, cache-friendly array.
 *
 * Chunks are owned by World and stored in a ring buffer. Only the main thread
 * may mutate a chunk; mesh workers access chunks read-only. The companion
 * ChunkMesher class converts chunk data into renderable geometry.
 * @warning Mutation is main-thread-only. Call World::streamBarrier() before
 *          any write when stream workers are running.
 */
class Chunk {
public:
    static constexpr int   kSize   = kChunkSize;
    static constexpr size_t kVolume = static_cast<size_t>(kSize) * kSize * kSize;

    Chunk() { blocks_.fill(Block{}); } // all air

    /**
     * @brief Flat array index for local block coordinates.
     *
     * Layout: index = x + kSize * (y + kSize * z). Callers must ensure
     * coordinates are in [0, kSize) — no bounds check is performed.
     */
    [[nodiscard]] static constexpr size_t index(int x, int y, int z) {
        return static_cast<size_t>(x) +
               kSize * (static_cast<size_t>(y) + kSize * static_cast<size_t>(z));
    }

    [[nodiscard]] static constexpr bool inBounds(int x, int y, int z) {
        return x >= 0 && x < kSize && y >= 0 && y < kSize && z >= 0 && z < kSize;
    }

    [[nodiscard]] Block get(int x, int y, int z) const {
        return blocks_[index(x, y, z)];
    }

    void set(int x, int y, int z, Block b) {
        blocks_[index(x, y, z)] = b;
    }

    // Like get(), but returns air for out-of-bounds coordinates. Handy for an
    // isolated, single-chunk mesh. (The world mesher instead samples neighbouring
    // chunks across edges via ChunkMesher::NeighborSampler, so faces between two
    // solid chunks are culled rather than emitted-and-hidden.)
    [[nodiscard]] Block getOrAir(int x, int y, int z) const {
        return inBounds(x, y, z) ? blocks_[index(x, y, z)] : Block{};
    }

private:
    std::array<Block, kVolume> blocks_{};
};

} // namespace vg
