#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace vg {

namespace {
// Terrain shaping constants.
constexpr float kHeightFreq   = 0.018f; // lower -> broader hills
constexpr int   kBaseHeight   = 18;     // lowest surface height
constexpr int   kHeightAmp    = 18;     // surface varies over [base, base+amp]
constexpr int   kOctaves      = 4;

constexpr float kMaterialFreq = 0.04f;
} // namespace

World::World(uint32_t seed, int chunksX, int chunksY, int chunksZ)
    : counts_(chunksX, chunksY, chunksZ),
      heightNoise_(seed),
      // Offset the material noise's seed so it is independent of the height noise.
      materialNoise_(seed * 2654435761u + 1u) {
    chunks_.resize(static_cast<size_t>(counts_.x) * counts_.y * counts_.z);
    generate();
}

int World::chunkIndex(int cx, int cy, int cz) const {
    return cx + counts_.x * (cy + counts_.y * cz);
}

bool World::inChunkBounds(int cx, int cy, int cz) const {
    return cx >= 0 && cx < counts_.x && cy >= 0 && cy < counts_.y &&
           cz >= 0 && cz < counts_.z;
}

const Chunk& World::chunk(int cx, int cy, int cz) const {
    return chunks_[chunkIndex(cx, cy, cz)];
}

int World::columnHeight(int wx, int wz) const {
    // fbm returns ~[-1,1]; remap to [0,1] then to [base, base+amp].
    const float n = heightNoise_.fbm(wx * kHeightFreq, wz * kHeightFreq, kOctaves);
    const float t = std::clamp((n + 1.0f) * 0.5f, 0.0f, 1.0f);
    int h = kBaseHeight + static_cast<int>(t * kHeightAmp);
    // Never exceed the world's vertical extent.
    return std::min(h, counts_.y * Chunk::kSize - 1);
}

void World::generate() {
    for (int cz = 0; cz < counts_.z; ++cz) {
        for (int cy = 0; cy < counts_.y; ++cy) {
            for (int cx = 0; cx < counts_.x; ++cx) {
                generateChunk(cx, cy, cz);
            }
        }
    }
}

void World::generateChunk(int cx, int cy, int cz) {
    Chunk& c = chunks_[chunkIndex(cx, cy, cz)];

    for (int lz = 0; lz < Chunk::kSize; ++lz) {
        for (int lx = 0; lx < Chunk::kSize; ++lx) {
            const int wx = cx * Chunk::kSize + lx;
            const int wz = cz * Chunk::kSize + lz;

            const int h = columnHeight(wx, wz);

            // Material layer: dirt thickness + whether the surface is rock.
            const float m = materialNoise_.fbm(wx * kMaterialFreq, wz * kMaterialFreq, 3);
            const int dirtDepth = 3 + static_cast<int>((m + 1.0f) * 0.5f * 2.0f); // 3..5
            const bool rockySurface =
                (h >= kBaseHeight + kHeightAmp - 3) || (m > 0.45f);

            for (int ly = 0; ly < Chunk::kSize; ++ly) {
                const int wy = cy * Chunk::kSize + ly;
                if (wy > h) {
                    continue; // air above the surface
                }

                BlockId id;
                if (wy == h) {
                    id = rockySurface ? BlockId::Stone : BlockId::Grass;
                } else if (wy >= h - dirtDepth) {
                    id = BlockId::Dirt;
                } else {
                    id = BlockId::Stone;
                }
                c.set(lx, ly, lz, Block{static_cast<uint16_t>(id), 0});
            }
        }
    }
}

bool World::isSolid(int wx, int wy, int wz) const {
    const glm::ivec3 size = sizeInBlocks();
    if (wx < 0 || wy < 0 || wz < 0 || wx >= size.x || wy >= size.y || wz >= size.z) {
        return false; // outside the world is open air
    }
    const int cx = wx / Chunk::kSize, lx = wx % Chunk::kSize;
    const int cy = wy / Chunk::kSize, ly = wy % Chunk::kSize;
    const int cz = wz / Chunk::kSize, lz = wz % Chunk::kSize;
    return registry_.isSolid(chunk(cx, cy, cz).get(lx, ly, lz).id);
}

int World::surfaceHeight(int wx, int wz) const {
    return columnHeight(wx, wz) + 1;
}

} // namespace vg
