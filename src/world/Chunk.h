#pragma once

#include "world/Block.h"

#include <array>
#include <cstddef>

namespace vg {

// Edge length of a chunk in blocks. Compile-time constant so the storage is a
// fixed-size array and the index math is trivially optimisable.
inline constexpr int kChunkSize = 16;

// -----------------------------------------------------------------------------
//  Chunk
// -----------------------------------------------------------------------------
//  A fixed 16x16x16 block of voxels stored in a flat, contiguous array (good
//  for cache behaviour while meshing). A chunk knows how to get/set blocks; the
//  ChunkMesher (separate class) turns it into renderable geometry.
// -----------------------------------------------------------------------------
class Chunk {
public:
    static constexpr int   kSize   = kChunkSize;
    static constexpr size_t kVolume = static_cast<size_t>(kSize) * kSize * kSize;

    Chunk() { blocks_.fill(Block{}); } // all air

    // Flat index for (x, y, z). Layout is x-major within a row, then y, then z:
    //   index = x + SIZE * (y + SIZE * z)
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

    // Like get(), but returns air for out-of-bounds coordinates. The mesher uses
    // this so chunk-boundary faces are emitted (the chunk is meshed as if
    // surrounded by air).
    // TODO(future): in multi-chunk worlds, sample the neighbouring chunk here
    // instead of returning air, so internal faces between chunks are culled.
    [[nodiscard]] Block getOrAir(int x, int y, int z) const {
        return inBounds(x, y, z) ? blocks_[index(x, y, z)] : Block{};
    }

private:
    std::array<Block, kVolume> blocks_{};
};

} // namespace vg
