#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>

namespace vg {

namespace {
// Floor division / modulo — correct for negative operands. Truncating `/` and
// `%` round toward zero, which mis-addresses chunks/blocks west or south of the
// origin once the streaming window can sit at negative world coords.
constexpr int floordiv(int a, int b) {
    const int q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
constexpr int floormod(int a, int b) {
    const int r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

// Block-light hue packing: a linear RGB colour (0..1) <-> RGBA8 with R in the low
// byte, matching the GPU vertex format (render/Vertex.h packColorRGBA8). The flood
// carries the dominant emitter's packed colour alongside the intensity level.
inline uint32_t packLightColor(const glm::vec3& c) {
    auto q = [](float v) -> uint32_t {
        const float cl = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint32_t>(cl * 255.0f + 0.5f);
    };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (0xFFu << 24);
}
inline glm::vec3 unpackLightColor(uint32_t p) {
    return {static_cast<float>(p & 0xFF) / 255.0f,
            static_cast<float>((p >> 8) & 0xFF) / 255.0f,
            static_cast<float>((p >> 16) & 0xFF) / 255.0f};
}

// --- Chunk persistence (save-to-disk for edited chunks) ----------------------
// One little binary file per *edited* chunk (unedited chunks regenerate from the
// seed, so they are never written). Format: magic + version + edge length, then
// id(u16)+metadata(u8) per voxel in the chunk's storage order. See docs/STREAMING.md.
constexpr uint32_t kChunkMagic   = 0x4B4E4843u; // 'CHNK'
constexpr uint32_t kChunkVersion = 1u;

std::string chunkPath(const std::string& dir, int cx, int cy, int cz) {
    return dir + "/c." + std::to_string(cx) + '.' + std::to_string(cy) + '.' +
           std::to_string(cz) + ".bin";
}

bool loadChunkFile(const std::string& path, Chunk& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    uint32_t magic = 0, ver = 0;
    int32_t edge = 0;
    f.read(reinterpret_cast<char*>(&magic), sizeof magic);
    f.read(reinterpret_cast<char*>(&ver), sizeof ver);
    f.read(reinterpret_cast<char*>(&edge), sizeof edge);
    if (!f || magic != kChunkMagic || ver != kChunkVersion || edge != Chunk::kSize) {
        return false;
    }
    for (int z = 0; z < Chunk::kSize; ++z) {
        for (int y = 0; y < Chunk::kSize; ++y) {
            for (int x = 0; x < Chunk::kSize; ++x) {
                uint16_t id = 0;
                uint8_t meta = 0;
                f.read(reinterpret_cast<char*>(&id), sizeof id);
                f.read(reinterpret_cast<char*>(&meta), sizeof meta);
                if (!f) {
                    return false; // truncated/corrupt: fall back to regeneration
                }
                out.set(x, y, z, Block{id, meta});
            }
        }
    }
    return true;
}

void saveChunkFile(const std::string& path, const Chunk& c) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return;
    }
    const uint32_t magic = kChunkMagic, ver = kChunkVersion;
    const int32_t edge = Chunk::kSize;
    f.write(reinterpret_cast<const char*>(&magic), sizeof magic);
    f.write(reinterpret_cast<const char*>(&ver), sizeof ver);
    f.write(reinterpret_cast<const char*>(&edge), sizeof edge);
    for (int z = 0; z < Chunk::kSize; ++z) {
        for (int y = 0; y < Chunk::kSize; ++y) {
            for (int x = 0; x < Chunk::kSize; ++x) {
                const Block b = c.get(x, y, z);
                f.write(reinterpret_cast<const char*>(&b.id), sizeof b.id);
                f.write(reinterpret_cast<const char*>(&b.metadata), sizeof b.metadata);
            }
        }
    }
}
} // namespace

World::World(const WorldConfig& config, const std::string& blocksFile)
    : config_(config),
      counts_(config.chunksX, config.chunksY, config.chunksZ),
      registry_(blocksFile),
      heightNoise_(config.seed),
      // Offset the material noise's seed so it is independent of the height noise.
      materialNoise_(config.seed * 2654435761u + 1u),
      caveNoise_(config.seed * 40503u + 0x9e37u),
      // Data-driven shape + biome pipeline. Reads assets/biomes.yaml from the same
      // asset dir as blocks.yaml; worldHeight is the vertical block extent.
      gen_(config.seed, registry_,
           std::filesystem::path(blocksFile).parent_path().string(),
           config.chunksY * Chunk::kSize) {
    // Resolve the block types the generator places (throws if a name is absent
    // from the block-definition file).
    grassId_  = registry_.idByName("grass");
    dirtId_   = registry_.idByName("dirt");
    stoneId_  = registry_.idByName("stone");
    sandId_   = registry_.idByName("sand");
    gravelId_ = registry_.idByName("gravel");
    cobbleId_ = registry_.idByName("cobblestone");
    logId_    = registry_.idByName("oak_log");
    glowId_   = registry_.idByName("glowstone");
    trunkId_  = registry_.idByName("oak_trunk");
    leavesId_ = registry_.idByName("oak_leaves");
    bushId_   = registry_.idByName("bush");
    waterId_  = registry_.idByName("water");
    lavaId_   = registry_.idByName("lava");
    coalId_    = registry_.idByName("coal_ore");
    ironId_    = registry_.idByName("iron_ore");
    goldId_    = registry_.idByName("gold_ore");
    rubyId_    = registry_.idByName("ruby_ore");
    emeraldId_ = registry_.idByName("emerald_ore");
    mythrilId_ = registry_.idByName("mythril_ore");

    // Sanity-check the configured vertical extent: columnHeight() clamps to the
    // world's top, so any terrain taller than chunks.y*kSize gets sheared flat.
    // Warn (don't silently rewrite the user's world.yaml) so plateaued hilltops
    // are not a mystery.
    const int worldTop = counts_.y * Chunk::kSize;
    if (config_.baseHeight + config_.heightAmplitude >= worldTop) {
        std::cerr << "[world] warning: base_height + height_amplitude ("
                  << (config_.baseHeight + config_.heightAmplitude)
                  << ") >= world height (" << worldTop << " = chunks.y * " << Chunk::kSize
                  << "); hilltops will be clamped flat. Raise chunks.y in assets/world.yaml.\n";
    }

    chunks_.resize(static_cast<size_t>(counts_.x) * counts_.y * counts_.z);
    chunkDirty_.assign(chunks_.size(), 0);
    // Per-seed save directory for persisted edits. Streaming only; generate()
    // below loads any saved chunk in place of regenerating it from noise.
    if (config_.streaming) {
        savePath_ = config_.saveDir + "/" + std::to_string(config_.seed);
        try {
            std::filesystem::create_directories(savePath_);
        } catch (const std::exception&) {
            savePath_.clear(); // can't create it: run without persistence
        }
    }
    generate();
    computeSkyLight();
    computeBlockLight();
}

World::~World() {
    // Flush edited chunks still in the window so they survive to next launch.
    try {
        saveDirtyWindow();
    } catch (...) {
        // never throw from a destructor
    }
}

int World::chunkIndex(int cx, int cy, int cz) const {
    // Ring buffer: an absolute chunk coord maps to a fixed slot via floormod, so
    // the window can advance one column without moving the other columns' data —
    // the departing chunk and the entering chunk share a slot. The caller checks
    // inChunkBounds() to know the slot currently holds (cx,cy,cz) and not its
    // wrapped-around neighbour. At origin {0,0,0} (streaming off) floormod is the
    // identity over [0,counts), so this is unchanged.
    const int sx = floormod(cx, counts_.x);
    const int sy = floormod(cy, counts_.y);
    const int sz = floormod(cz, counts_.z);
    return sx + counts_.x * (sy + counts_.y * sz);
}

bool World::inChunkBounds(int cx, int cy, int cz) const {
    return cx >= originChunk_.x && cx < originChunk_.x + counts_.x &&
           cy >= originChunk_.y && cy < originChunk_.y + counts_.y &&
           cz >= originChunk_.z && cz < originChunk_.z + counts_.z;
}

const Chunk& World::chunk(int cx, int cy, int cz) const {
    return chunks_[chunkIndex(cx, cy, cz)];
}

namespace {
// Deterministic per-column pseudo-random in [0,1) from integer coords + a salt
// (so the same world column always grows the same feature). A small integer
// hash — fmix-style avalanche — keeps scatter cheap and seed-stable.
float hash01(int x, int z, uint32_t salt) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^
                 static_cast<uint32_t>(z) * 0xd8163841u ^ (salt * 0x9e3779b9u);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

// 3D variant — deterministic [0,1) from integer (x,y,z) + salt. Used to scatter
// ore veins: the caller hashes a coarsened cell so a small block cluster shares
// one roll (a little vein) rather than every voxel rolling independently.
float hash01(int x, int y, int z, uint32_t salt) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^
                 static_cast<uint32_t>(y) * 0xcb1ab31fu ^
                 static_cast<uint32_t>(z) * 0xd8163841u ^ (salt * 0x9e3779b9u);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}
} // namespace

float World::islandFalloff(int wx, int wz) const {
    // Streaming makes the world endless: there is no single centre to ring with
    // sea, so the radial island mask is disabled and terrain is full-height
    // everywhere (roadmap: no island shaping). Only the fixed-grid (streaming
    // off) world keeps the island.
    if (config_.streaming) {
        return 1.0f;
    }
    const glm::ivec3 s = sizeInBlocks();
    const float cxw = s.x * 0.5f, czw = s.z * 0.5f;
    const float rx = std::max(1.0f, cxw), rz = std::max(1.0f, czw);
    const float dx = (wx - cxw) / rx, dz = (wz - czw) / rz;
    float d = std::sqrt(dx * dx + dz * dz); // 0 at centre, ~1 at the edge mid-points
    // Warp the distance so the shoreline grows bays and capes, not a clean circle.
    d += materialNoise_.fbm(wx * 0.011f, wz * 0.011f, 2) * config_.coastWarp;
    // Land (1) in the middle, fading to sea (0) across the falloff band.
    const float m =
        1.0f - glm::smoothstep(config_.islandFalloffStart, config_.islandFalloffEnd, d);
    return std::clamp(m, 0.0f, 1.0f);
}

int World::columnHeight(int wx, int wz) const {
    // The surface height now comes from the data-driven shape pipeline (continental
    // /erosion/peaks splines -> oceans, plains, mountains). See TerrainGenerator and
    // assets/biomes.yaml. The old single-noise + island-mask height was replaced by
    // this; heightNoise_/materialNoise_/islandFalloff remain for other callers.
    return gen_.height(wx, wz);
}

bool World::caveAt(int wx, int wy, int wz, float surfaceTaper) const {
    if (wy <= config_.caveFloor) {
        return false; // keep a solid floor
    }
    const float f = config_.caveFrequency;
    // Spaghetti tunnels = the intersection of two independent 3D fields' zero-
    // crossings: where BOTH are near zero the carve traces a winding line, not a
    // fat blob, so caves snake. `surfaceTaper` shrinks the channel near the surface
    // so only the strongest tunnels breach as cave mouths (caller tapers by depth).
    const float a = caveNoise_.fbm(wx * f, wy * f, wz * f, 2);
    const float b = caveNoise_.fbm((wx + 137) * f, (wy - 91) * f, (wz + 53) * f, 2);
    const float t = config_.caveThreshold * surfaceTaper;
    if (a * a + b * b < t * t) {
        return true;
    }
    // Large caverns: a low-frequency 3D blob, deep only — big open rooms that the
    // tunnels connect into. Tapered the same way so a deep cavern can also surface.
    if (wy < config_.cavernMaxY) {
        const float cf = f * 0.45f;
        const float c = caveNoise_.fbm((wx - 311) * cf, (wy + 177) * cf, (wz + 97) * cf, 2);
        if (c > config_.cavernThreshold + (1.0f - surfaceTaper) * 0.5f) {
            return true;
        }
    }
    return false;
}

uint16_t World::oreAt(int wx, int wy, int wz) const {
    // One roll shared across a 2x2x2 block cell, so an ore "hit" fills a little
    // cluster instead of a single lonely voxel.
    const int cx = floordiv(wx, 2), cy = floordiv(wy, 2), cz = floordiv(wz, 2);
    const uint32_t seed = config_.seed;
    struct OreDef { int maxY; float density; uint16_t id; uint32_t salt; };
    const OreDef ores[] = {
        {config_.mythrilMaxY, config_.mythrilDensity, mythrilId_, 0x4d17u},
        {config_.emeraldMaxY, config_.emeraldDensity, emeraldId_, 0x3e11u},
        {config_.rubyMaxY,    config_.rubyDensity,    rubyId_,    0x2b07u},
        {config_.goldMaxY,    config_.goldDensity,    goldId_,    0x1a93u},
        {config_.ironMaxY,    config_.ironDensity,    ironId_,    0x0c55u},
        {config_.coalMaxY,    config_.coalDensity,    coalId_,    0x0911u},
    };
    for (const OreDef& o : ores) { // rarest-first: a deep block becomes the rarer ore
        if (wy <= o.maxY && hash01(cx, cy, cz, seed ^ o.salt) < o.density) {
            return o.id;
        }
    }
    return 0; // stays stone
}

void World::generate() {
    // Iterate the loaded window's (X,Z) columns and generate each as a unit
    // (generateColumn shares the column shape across the vertical stack). Columns
    // are independent (own ring slots, stateless noise), so generate them in
    // parallel when threading is enabled — speeds up startup and teleports.
    std::vector<glm::ivec2> columns;
    columns.reserve(static_cast<size_t>(counts_.x) * counts_.z);
    for (int cz = originChunk_.z; cz < originChunk_.z + counts_.z; ++cz) {
        for (int cx = originChunk_.x; cx < originChunk_.x + counts_.x; ++cx) {
            columns.push_back({cx, cz});
        }
    }
    if (config_.streamWorkers > 0) {
        std::for_each(std::execution::par, columns.begin(), columns.end(),
                      [this](const glm::ivec2& c) { generateColumn(c.x, c.y); });
    } else {
        for (const glm::ivec2& c : columns) {
            generateColumn(c.x, c.y);
        }
    }
}

void World::generateColumn(int cx, int cz) {
    constexpr int N = Chunk::kSize;
    const int worldTop = counts_.y * N;

    // Clear each vertical chunk; load any persisted ones in place of noise. Cache
    // the ring slots and remember which chunks still need generating.
    std::vector<size_t>  slots(static_cast<size_t>(counts_.y));
    std::vector<uint8_t> needNoise(static_cast<size_t>(counts_.y), 1);
    bool any = false;
    for (int cy = 0; cy < counts_.y; ++cy) {
        const size_t slot = static_cast<size_t>(chunkIndex(cx, cy, cz));
        slots[static_cast<size_t>(cy)] = slot;
        Chunk& c = chunks_[slot];
        c = Chunk{}; // a reused ring slot may still hold the departed chunk's blocks
        if (!savePath_.empty() && loadChunkFile(chunkPath(savePath_, cx, cy, cz), c)) {
            chunkDirty_[slot] = 0;
            needNoise[static_cast<size_t>(cy)] = 0;
        } else {
            any = true;
        }
    }
    if (!any) {
        return; // the whole column was loaded from disk
    }

    const uint32_t seed = config_.seed;
    for (int lz = 0; lz < N; ++lz) {
        for (int lx = 0; lx < N; ++lx) {
            const int wx = cx * N + lx;
            const int wz = cz * N + lz;

            // The expensive bit — computed ONCE for the whole vertical column (the
            // surface height, biome, blocks and water level all come from this one
            // call, instead of also calling columnHeight() separately).
            const ColumnInfo ci = gen_.columnInfo(wx, wz);
            const int h = ci.height;

            // Biome-driven surface treatment (data-driven; see TerrainGenerator /
            // assets/biomes.yaml): the biome picks the surface + filler blocks and
            // snow. Submerged columns get an ocean/lake floor and grow nothing.
            const uint16_t topId = ci.topId;
            const uint16_t subId = ci.fillerId;
            const int dirtDepth  = 4;                  // filler blocks under the surface
            const int seaLevel   = gen_.seaLevel();
            const bool onLand    = h > seaLevel;       // above the sea surface
            const bool grassy    = ci.treeDensity > 0.0f || ci.bushDensity > 0.0f;

            // --- Whimsical scatter (single-column, so chunk-seam safe) ---------
            // A buried glowstone geode a few blocks down in the stone.
            int geodeY = -1;
            if (onLand && hash01(wx, wz, seed ^ 0x6e0deu) < config_.geodeDensity) {
                geodeY = h - (3 + static_cast<int>(hash01(wx, wz, seed ^ 0x6e1u) * 6.0f));
                if (geodeY <= 1) geodeY = -1;
            }
            // Above-surface features: a glowing lantern-tree, else a cobble cairn.
            int stalkTop = -1, capY = -1, cairnTop = -1;
            if (grassy && hash01(wx, wz, seed ^ 0x1a27u) < config_.lanternDensity) {
                const int trunk = 3 + static_cast<int>(hash01(wx, wz, seed ^ 0x1a28u) * 5.0f);
                stalkTop = h + trunk; // oak-log h+1..stalkTop
                capY     = stalkTop + 1; // glowstone cap
            } else if (onLand && hash01(wx, wz, seed ^ 0xca12u) < config_.cairnDensity) {
                cairnTop = h + 1 + static_cast<int>(hash01(wx, wz, seed ^ 0xca13u) * 3.0f);
            }
            const int featureTop = std::max({h, capY, cairnTop});

            // Per-column water surface from the generator: sea level for oceans /
            // coasts / rivers, or a perched lake's own (higher) level. Air at/below
            // it over the terrain floods with water; uplands stay dry.
            const int waterLevel = ci.waterLevel;
            const bool submerged = h < waterLevel; // don't breach caves under water

            // Fill the whole vertical column in one pass, routing each block into
            // whichever chunk owns its Y (skipping chunks loaded from disk). This is
            // identical output to the old per-chunk loop, just without recomputing
            // the column shape for every chunk in the stack.
            const int top = std::min(std::max(featureTop, waterLevel), worldTop - 1);
            for (int wy = 0; wy <= top; ++wy) {
                const int cy = wy / N; // wy >= 0, plain division is fine
                if (!needNoise[static_cast<size_t>(cy)]) {
                    continue;
                }
                uint16_t id = 0; // air
                if (wy <= h) {
                    // Base material for this depth.
                    uint16_t mat;
                    bool isStone = false;
                    if (wy == geodeY)             mat = glowId_;
                    else if (wy == h)             mat = topId;
                    else if (wy >= h - dirtDepth) mat = subId;
                    else if (wy >= 1 && wy <= 2)  mat = lavaId_; // deep lava floor (over bedrock)
                    else { mat = stoneId_; isStone = true; }

                    // Caves carve air out of the whole solid column (not just stone),
                    // so tunnels/caverns can breach the surface as cave mouths. The
                    // carve tapers toward the surface so only strong tunnels open up
                    // (no swiss-cheese), and a submerged surface isn't breached (no
                    // air pockets under water). Lava floor + geodes are never carved.
                    bool carve = false;
                    if (mat != lavaId_ && wy != geodeY) {
                        const int depth = h - wy; // 0 at the surface block
                        float taper = 1.0f;
                        if (depth < 5) {
                            taper = submerged ? 0.0f : (0.3f + 0.14f * static_cast<float>(depth));
                        }
                        if (taper > 0.0f) {
                            carve = caveAt(wx, wy, wz, taper);
                        }
                    }
                    if (carve) {
                        id = 0;
                    } else if (isStone) {
                        const uint16_t ore = oreAt(wx, wy, wz);
                        id = ore != 0 ? ore : stoneId_;
                    } else {
                        id = mat;
                    }
                } else if (stalkTop >= 0) {       // lantern-tree
                    if (wy <= stalkTop)            id = logId_;
                    else if (wy == capY)           id = glowId_;
                } else if (cairnTop >= 0 && wy <= cairnTop) {
                    id = cobbleId_;                // cairn
                } else if (wy <= waterLevel) {     // sea-level water fill (above terrain)
                    id = waterId_;
                }
                if (id != 0) {
                    chunks_[slots[static_cast<size_t>(cy)]].set(lx, wy % N, lz, Block{id, 0});
                }
            }

            // --- Trees & bushes (sit on top of the terrain) -------------------
            // place() writes `id` into world cell (wx,wy,wz) iff it is currently
            // air and its chunk is being generated, routing it into whichever
            // vertical chunk owns wy. Only ever touches this column (lx,lz).
            auto place = [&](int wy, uint16_t pid) {
                if (wy < 0 || wy >= worldTop) return;
                const int pcy = wy / N;
                if (!needNoise[static_cast<size_t>(pcy)]) return;
                Chunk& pc = chunks_[slots[static_cast<size_t>(pcy)]];
                if (pc.get(lx, wy % N, lz).id != 0) return; // never overwrite terrain
                pc.set(lx, wy % N, lz, Block{pid, 0});
            };

            // A single-cell shrub, at the biome's bush density (0 underwater/desert-rock).
            if (hash01(wx, wz, seed ^ 0xb05fu) < ci.bushDensity) {
                place(h + 1, bushId_);
            }

            // Oak trees. A tree rooted at column (ox,oz) spreads its canopy over the
            // neighbouring columns, so this column gathers contributions from every
            // nearby root within the canopy radius — keeping generation a pure
            // function of world coords (seam-safe, approach-independent). The root's
            // BIOME sets how likely a tree is (dense forests, none in desert/ocean).
            constexpr int kTreeR = 3; // max canopy half-width gathered, in blocks
            const float maxTreeD = gen_.maxTreeDensity();
            for (int oz = wz - kTreeR; oz <= wz + kTreeR; ++oz) {
                for (int ox = wx - kTreeR; ox <= wx + kTreeR; ++ox) {
                    // CHEAP reject first: most columns can't root a tree, and the
                    // biome lookup (columnInfo) is expensive, so gate on the hash vs
                    // the densest possible biome before paying for it. The same hash
                    // is then re-checked against the root biome's actual density.
                    const float th = hash01(ox, oz, seed ^ 0x7233u);
                    if (th >= maxTreeD) continue;
                    const ColumnInfo oc = gen_.columnInfo(ox, oz);
                    if (th >= oc.treeDensity) continue;
                    const int oh = oc.height;

                    // Varied trunk height (5..11), hashed per root, so canopy tops
                    // sit at different levels across the forest.
                    const int trunkH =
                        5 + static_cast<int>(hash01(ox, oz, seed ^ 0x7234u) * 6.99f); // 5..11
                    const int topY = oh + trunkH; // topmost trunk block
                    const int dx = wx - ox, dz = wz - oz;

                    // Trunk occupies only the root column.
                    if (dx == 0 && dz == 0) {
                        for (int wy = oh + 1; wy <= topY; ++wy) place(wy, trunkId_);
                    }

                    // Ellipsoidal canopy: a rounded crown wrapping the trunk top.
                    // Horizontal radius rH (2..3) with a taller vertical radius rV
                    // for an oval crown; size hashed per root for variety. The trunk
                    // pokes up into the hollow core.
                    const int   d2  = dx * dx + dz * dz;
                    const float csz = hash01(ox, oz, seed ^ 0x7241u);
                    const int   rH  = 2 + static_cast<int>(csz * 1.99f); // 2..3
                    const int   rV  = rH + 1;                            // taller -> oval
                    if (d2 > rH * rH + 1) continue;     // outside the crown footprint
                    const int cyc = topY - 1 + rV / 2;  // canopy centre Y
                    for (int wy = cyc - rV; wy <= cyc + rV; ++wy) {
                        const int   ddy = wy - cyc;
                        const float e =
                            static_cast<float>(d2) / static_cast<float>(rH * rH) +
                            static_cast<float>(ddy * ddy) / static_cast<float>(rV * rV);
                        if (e > 1.0f) continue;                         // outside the ellipsoid
                        if (dx == 0 && dz == 0 && wy <= topY) continue; // leave the trunk core
                        // Ragged the silhouette: thin the outer shell so the crown
                        // isn't a perfect ball.
                        if (e > 0.72f &&
                            hash01(wx * 73 + wy, wz * 19 + (wy & 7), seed ^ 0x7240u) < 0.28f) {
                            continue;
                        }
                        place(wy, leavesId_);
                    }
                }
            }
        }
    }
    for (int cy = 0; cy < counts_.y; ++cy) {
        if (needNoise[static_cast<size_t>(cy)]) {
            chunkDirty_[slots[static_cast<size_t>(cy)]] = 0; // fresh from noise == clean
        }
    }
}

Block World::blockAt(int wx, int wy, int wz) const {
    const glm::ivec3 size = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    if (wx < o.x || wy < o.y || wz < o.z ||
        wx >= o.x + size.x || wy >= o.y + size.y || wz >= o.z + size.z) {
        return Block{}; // outside the loaded window is open air
    }
    const int cx = floordiv(wx, Chunk::kSize), lx = floormod(wx, Chunk::kSize);
    const int cy = floordiv(wy, Chunk::kSize), ly = floormod(wy, Chunk::kSize);
    const int cz = floordiv(wz, Chunk::kSize), lz = floormod(wz, Chunk::kSize);
    return chunk(cx, cy, cz).get(lx, ly, lz);
}

bool World::isSolid(int wx, int wy, int wz) const {
    return registry_.isSolid(blockAt(wx, wy, wz).id);
}

bool World::isTargetable(int wx, int wy, int wz) const {
    const Block b = blockAt(wx, wy, wz);
    if (b.isAir()) {
        return false;
    }
    const BlockProperties& p = registry_.get(b.id);
    // Solids (incl. Model trunks) plus see-through foliage (Cross/LeafCube) — but
    // NOT liquids. Water/lava are non-solid Cube blocks; since water became
    // non-opaque (so light reaches the seabed) a plain `!opaque` test would let
    // you mine the water itself, so target only solids + foliage render types.
    return p.solid || p.renderType == RenderType::Cross ||
           p.renderType == RenderType::LeafCube;
}

float World::modelInsetAt(int wx, int wy, int wz) const {
    const Block b = blockAt(wx, wy, wz);
    return registry_.renderType(b.id) == RenderType::Model ? registry_.modelInset(b.id)
                                                           : 0.0f;
}

int World::lightIndex(int wx, int wy, int wz) const {
    // Same ring-buffer mapping as chunkIndex, at block granularity (Y is never
    // wrapped — there is no vertical streaming, so wy stays in [0, s.y)). At
    // origin {0,0,0} floormod is the identity over [0,s), so this is unchanged.
    const glm::ivec3 s = sizeInBlocks();
    return floormod(wx, s.x) + s.x * (floormod(wy, s.y) + s.y * floormod(wz, s.z));
}

uint8_t World::skyLightAt(int wx, int wy, int wz) const {
    const glm::ivec3 s = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    if (wx < o.x || wy < o.y || wz < o.z ||
        wx >= o.x + s.x || wy >= o.y + s.y || wz >= o.z + s.z) {
        return 15; // outside the loaded window is open sky
    }
    return skyLight_[static_cast<size_t>(lightIndex(wx, wy, wz))];
}

uint8_t World::blockLightAt(int wx, int wy, int wz) const {
    const glm::ivec3 s = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    if (wx < o.x || wy < o.y || wz < o.z ||
        wx >= o.x + s.x || wy >= o.y + s.y || wz >= o.z + s.z) {
        return 0; // no emitters outside the loaded window
    }
    return blockLight_[static_cast<size_t>(lightIndex(wx, wy, wz))];
}

glm::vec3 World::blockLightColorAt(int wx, int wy, int wz) const {
    const glm::ivec3 s = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    if (wx < o.x || wy < o.y || wz < o.z ||
        wx >= o.x + s.x || wy >= o.y + s.y || wz >= o.z + s.z) {
        return glm::vec3(0.0f); // no emitters outside the loaded window
    }
    return unpackLightColor(blockLightColor_[static_cast<size_t>(lightIndex(wx, wy, wz))]);
}

uint8_t World::lightAt(int wx, int wy, int wz) const {
    return std::max(skyLightAt(wx, wy, wz), blockLightAt(wx, wy, wz));
}

void World::computeSkyLight() {
    const glm::ivec3 s = sizeInBlocks();
    skyLight_.assign(static_cast<size_t>(s.x) * s.y * s.z, 0);

    // BFS frontier stored as a flat vector with a moving head (cheaper than a
    // std::queue and the visited check is just "did the stored level rise?").
    std::vector<glm::ivec3> frontier;

    // Seed: walk each column down from the top, marking blocks fully lit (15)
    // until an opaque block blocks the sky. Everything below starts dark (0) and
    // only brightens if light floods in from the side. Iterate the loaded window
    // in absolute world coords (o = its min block corner); lightIndex() ring-maps
    // them to slots. At origin {0,0,0} this is the original [0,s) sweep.
    const glm::ivec3 o = originBlock();
    for (int z = o.z; z < o.z + s.z; ++z) {
        for (int x = o.x; x < o.x + s.x; ++x) {
            for (int y = o.y + s.y - 1; y >= o.y; --y) {
                if (registry_.isOpaque(blockAt(x, y, z).id)) {
                    break;
                }
                skyLight_[static_cast<size_t>(lightIndex(x, y, z))] = 15;
                frontier.push_back({x, y, z});
            }
        }
    }

    // Flood fill: light spreads to non-opaque neighbours, losing sky_falloff
    // levels per step (world.yaml), so shadowed pockets fade to black with
    // distance from the open sky — higher falloff keeps caves darker.
    const int falloff = config_.skyFalloff;
    const glm::ivec3 dirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (size_t head = 0; head < frontier.size(); ++head) {
        const glm::ivec3 p = frontier[head];
        const uint8_t level = skyLight_[static_cast<size_t>(lightIndex(p.x, p.y, p.z))];
        if (level <= falloff) {
            continue;
        }
        for (const glm::ivec3& d : dirs) {
            const int nx = p.x + d.x, ny = p.y + d.y, nz = p.z + d.z;
            if (nx < o.x || ny < o.y || nz < o.z ||
                nx >= o.x + s.x || ny >= o.y + s.y || nz >= o.z + s.z) {
                continue;
            }
            if (registry_.isOpaque(blockAt(nx, ny, nz).id)) {
                continue;
            }
            uint8_t& nl = skyLight_[static_cast<size_t>(lightIndex(nx, ny, nz))];
            if (nl < level - falloff) {
                nl = static_cast<uint8_t>(level - falloff);
                frontier.push_back({nx, ny, nz});
            }
        }
    }
}

void World::computeBlockLight() {
    const glm::ivec3 s = sizeInBlocks();
    blockLight_.assign(static_cast<size_t>(s.x) * s.y * s.z, 0);
    // Hue rides along with the intensity: each cell records the colour of whichever
    // emitter reaches it brightest (set together with the level below).
    blockLightColor_.assign(static_cast<size_t>(s.x) * s.y * s.z, 0);

    std::vector<glm::ivec3> frontier;

    // Seed: every emissive block starts at its emission level. Emitters may be
    // opaque (glowstone is a solid cube), so seeding ignores opacity — the spread
    // below is what respects it, lighting the open space around the emitter.
    const glm::ivec3 o = originBlock();
    for (int z = o.z; z < o.z + s.z; ++z) {
        for (int y = o.y; y < o.y + s.y; ++y) {
            for (int x = o.x; x < o.x + s.x; ++x) {
                const uint16_t id = blockAt(x, y, z).id;
                const uint8_t e = registry_.emission(id);
                if (e > 0) {
                    const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                    blockLight_[li] = e;
                    blockLightColor_[li] = packLightColor(registry_.emissionColor(id));
                    frontier.push_back({x, y, z});
                }
            }
        }
    }

    // Flood fill: light spreads into non-opaque neighbours, dimming by
    // block_falloff levels per step (world.yaml), so a glowstone glow fades
    // smoothly into the surrounding dark. The neighbour inherits the spreading
    // cell's hue whenever it brightens it, so the dominant emitter wins each cell.
    const int falloff = config_.blockFalloff;
    const glm::ivec3 dirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    for (size_t head = 0; head < frontier.size(); ++head) {
        const glm::ivec3 p = frontier[head];
        const size_t pi = static_cast<size_t>(lightIndex(p.x, p.y, p.z));
        const uint8_t level = blockLight_[pi];
        if (level <= falloff) {
            continue;
        }
        for (const glm::ivec3& d : dirs) {
            const int nx = p.x + d.x, ny = p.y + d.y, nz = p.z + d.z;
            if (nx < o.x || ny < o.y || nz < o.z ||
                nx >= o.x + s.x || ny >= o.y + s.y || nz >= o.z + s.z) {
                continue;
            }
            if (registry_.isOpaque(blockAt(nx, ny, nz).id)) {
                continue; // opaque blocks don't transmit light
            }
            const size_t ni = static_cast<size_t>(lightIndex(nx, ny, nz));
            uint8_t& nl = blockLight_[ni];
            if (nl < level - falloff) {
                nl = static_cast<uint8_t>(level - falloff);
                blockLightColor_[ni] = blockLightColor_[pi];
                frontier.push_back({nx, ny, nz});
            }
        }
    }
}

void World::relightField(std::vector<uint8_t>& field, bool emitterSeed, int x0, int x1,
                         int z0, int z1) {
    const glm::ivec3 s = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    const int sy = s.y;
    constexpr int N = Chunk::kSize;

    // Field access is ring-mapped (lightIndex), and opacity/emission are read from
    // the ring chunk store via a floor-correct chunk/block split, so the box may
    // sit anywhere in the window — including negative world coords, and straddling
    // the array seam. Coords passed in/stored are absolute world coords. At origin
    // {0,0,0} all of this reduces to the original contiguous indexing.
    auto fieldAt = [&](int x, int y, int z) -> uint8_t& {
        return field[static_cast<size_t>(lightIndex(x, y, z))];
    };
    // Block-light hue, mirrored alongside `field` only on the emitter pass (the
    // sky pass leaves it untouched, since sky/block relight the same box back to
    // back and the block pass owns the colour). Writes are guarded by emitterSeed.
    auto colorAt = [&](int x, int y, int z) -> uint32_t& {
        return blockLightColor_[static_cast<size_t>(lightIndex(x, y, z))];
    };
    auto chunkAtBlock = [&](int x, int y, int z) -> const Chunk& {
        return chunks_[static_cast<size_t>(
            chunkIndex(floordiv(x, N), floordiv(y, N), floordiv(z, N)))];
    };
    auto opaqueAt = [&](int x, int y, int z) {
        return registry_.isOpaque(
            chunkAtBlock(x, y, z).get(floormod(x, N), floormod(y, N), floormod(z, N)).id);
    };

    // Clear the box (full height).
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            for (int y = o.y; y < o.y + sy; ++y) {
                fieldAt(x, y, z) = 0;
                if (emitterSeed) colorAt(x, y, z) = 0;
            }

    std::vector<glm::ivec3> frontier;
    if (!emitterSeed) {
        // Sky: each column is lit (15) from the top down until an opaque block.
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
                for (int y = o.y + sy - 1; y >= o.y; --y) {
                    if (opaqueAt(x, y, z)) break;
                    fieldAt(x, y, z) = 15;
                    frontier.push_back({x, y, z});
                }
    } else {
        // Block light: seed every emissive block (emitters may be opaque).
        for (int z = z0; z <= z1; ++z)
            for (int y = o.y; y < o.y + sy; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const uint16_t id =
                        chunkAtBlock(x, y, z).get(floormod(x, N), floormod(y, N), floormod(z, N)).id;
                    const uint8_t e = registry_.emission(id);
                    if (e > 0) {
                        fieldAt(x, y, z) = e;
                        colorAt(x, y, z) = packLightColor(registry_.emissionColor(id));
                        frontier.push_back({x, y, z});
                    }
                }
    }

    // Light decays by the per-source falloff (world.yaml lighting:) on every
    // spread step, both across the border seed and inside the box.
    const int falloff = emitterSeed ? config_.blockFalloff : config_.skyFalloff;

    // Seed light entering across the box's open X/Z faces from the (unchanged)
    // field just outside, so the box is correct rather than dark at its edges.
    // Only seed from a face that has a loaded neighbour (inside the window).
    auto borderSeed = [&](int ix, int iy, int iz, int ax, int ay, int az) {
        if (opaqueAt(ix, iy, iz)) return;
        const int v = static_cast<int>(fieldAt(ax, ay, az)) - falloff;
        if (v > static_cast<int>(fieldAt(ix, iy, iz))) {
            fieldAt(ix, iy, iz) = static_cast<uint8_t>(v);
            if (emitterSeed) colorAt(ix, iy, iz) = colorAt(ax, ay, az); // inherit outside hue
            frontier.push_back({ix, iy, iz});
        }
    };
    for (int y = o.y; y < o.y + sy; ++y) {
        if (x0 > o.x)           for (int z = z0; z <= z1; ++z) borderSeed(x0, y, z, x0 - 1, y, z);
        if (x1 < o.x + s.x - 1) for (int z = z0; z <= z1; ++z) borderSeed(x1, y, z, x1 + 1, y, z);
        if (z0 > o.z)           for (int x = x0; x <= x1; ++x) borderSeed(x, y, z0, x, y, z0 - 1);
        if (z1 < o.z + s.z - 1) for (int x = x0; x <= x1; ++x) borderSeed(x, y, z1, x, y, z1 + 1);
    }

    // Flood fill, clamped to the box (X/Z) and the world height (Y).
    const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (size_t head = 0; head < frontier.size(); ++head) {
        const glm::ivec3 p = frontier[head];
        const int level = fieldAt(p.x, p.y, p.z);
        if (level <= falloff) continue;
        for (const auto& d : dirs) {
            const int nx = p.x + d[0], ny = p.y + d[1], nz = p.z + d[2];
            if (nx < x0 || nx > x1 || nz < z0 || nz > z1 || ny < o.y || ny >= o.y + sy) continue;
            if (opaqueAt(nx, ny, nz)) continue;
            if (fieldAt(nx, ny, nz) < level - falloff) {
                fieldAt(nx, ny, nz) = static_cast<uint8_t>(level - falloff);
                if (emitterSeed) colorAt(nx, ny, nz) = colorAt(p.x, p.y, p.z); // inherit parent hue
                frontier.push_back({nx, ny, nz});
            }
        }
    }
}

bool World::setLightFalloff(int skyFalloff, int blockFalloff) {
    const int sky = std::clamp(skyFalloff, 1, 15);
    const int blk = std::clamp(blockFalloff, 1, 15);
    if (sky == config_.skyFalloff && blk == config_.blockFalloff) {
        return false;
    }
    config_.skyFalloff   = sky;
    config_.blockFalloff = blk;
    computeSkyLight();
    computeBlockLight();
    return true;
}

std::vector<glm::ivec3> World::setBlock(int wx, int wy, int wz, Block b) {
    const glm::ivec3 size = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    if (wx < o.x || wy < o.y || wz < o.z ||
        wx >= o.x + size.x || wy >= o.y + size.y || wz >= o.z + size.z) {
        return {}; // ignore writes outside the loaded window
    }
    const int cx = floordiv(wx, Chunk::kSize), lx = floormod(wx, Chunk::kSize);
    const int cy = floordiv(wy, Chunk::kSize), ly = floormod(wy, Chunk::kSize);
    const int cz = floordiv(wz, Chunk::kSize), lz = floormod(wz, Chunk::kSize);
    chunks_[chunkIndex(cx, cy, cz)].set(lx, ly, lz, b);
    chunkDirty_[static_cast<size_t>(chunkIndex(cx, cy, cz))] = 1; // diverged from disk/noise

    // Blocks gate sky light and may add/remove an emitter, so the edit changes
    // both lighting fields — but only within 15 blocks. Relight just that box (a
    // full recompute here is what made editing stutter on bigger worlds).
    constexpr int N = Chunk::kSize;
    constexpr int kLightRadius = 16; // one past light's 15-block reach
    const int x0 = std::max(o.x, wx - kLightRadius), x1 = std::min(o.x + size.x - 1, wx + kLightRadius);
    const int z0 = std::max(o.z, wz - kLightRadius), z1 = std::min(o.z + size.z - 1, wz + kLightRadius);
    const int sy = size.y;

    // Lighting is baked into chunk vertices, so any chunk whose light changed must
    // be remeshed. Remeshing EVERY chunk overlapping the box (3x3 columns over the
    // full height — up to 3*3*chunksY chunks) is what made editing lag: a typical
    // edit only perturbs light in a small neighbourhood, leaving most of those
    // chunks identical. So snapshot the box, relight, then diff and remesh only the
    // chunks that actually changed (plus the edited chunk and any face-adjacent
    // chunk across a shared boundary, whose geometry/culling changed regardless).
    const int bw = x1 - x0 + 1, bd = z1 - z0 + 1;
    std::vector<uint8_t> oldSky(static_cast<size_t>(bw) * sy * bd);
    std::vector<uint8_t> oldBlk(static_cast<size_t>(bw) * sy * bd);
    auto boxIdx = [&](int x, int y, int z) {
        return static_cast<size_t>((x - x0) + bw * (y + sy * (z - z0)));
    };
    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                const size_t bi = boxIdx(x, y, z);
                oldSky[bi] = skyLight_[li];
                oldBlk[bi] = blockLight_[li];
            }
        }
    }

    relightField(skyLight_, false, x0, x1, z0, z1);
    relightField(blockLight_, true, x0, x1, z0, z1);

    // A small dirty-grid over the chunk columns the box spans (at most 3x3 wide,
    // full height). Light can travel at most 15 blocks, so nothing outside the
    // radius-16 box changes — the grid covers every affected chunk.
    const int cx0 = floordiv(x0, N), cx1 = floordiv(x1, N);
    const int cz0 = floordiv(z0, N), cz1 = floordiv(z1, N);
    const int ncx = cx1 - cx0 + 1, ncz = cz1 - cz0 + 1;
    std::vector<char> dirtyGrid(static_cast<size_t>(ncx) * counts_.y * ncz, 0);
    auto mark = [&](int ccx, int ccy, int ccz) {
        if (ccx < cx0 || ccx > cx1 || ccz < cz0 || ccz > cz1 || ccy < 0 || ccy >= counts_.y) {
            return;
        }
        dirtyGrid[static_cast<size_t>((ccx - cx0) + ncx * (ccy + counts_.y * (ccz - cz0)))] = 1;
    };

    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                const size_t bi = boxIdx(x, y, z);
                if (skyLight_[li] != oldSky[bi] || blockLight_[li] != oldBlk[bi]) {
                    mark(floordiv(x, N), floordiv(y, N), floordiv(z, N));
                }
            }
        }
    }

    // The edited block always re-meshes its own chunk; a face-adjacent chunk only
    // when the block sits on the shared boundary (its cross-chunk faces flip).
    mark(cx, cy, cz);
    if (lx == 0)     mark(cx - 1, cy, cz);
    if (lx == N - 1) mark(cx + 1, cy, cz);
    if (ly == 0)     mark(cx, cy - 1, cz);
    if (ly == N - 1) mark(cx, cy + 1, cz);
    if (lz == 0)     mark(cx, cy, cz - 1);
    if (lz == N - 1) mark(cx, cy, cz + 1);

    std::vector<glm::ivec3> dirty;
    for (int dcz = cz0; dcz <= cz1; ++dcz) {
        for (int dcy = 0; dcy < counts_.y; ++dcy) {
            for (int dcx = cx0; dcx <= cx1; ++dcx) {
                if (dirtyGrid[static_cast<size_t>((dcx - cx0) + ncx * (dcy + counts_.y * (dcz - cz0)))]) {
                    dirty.push_back({dcx, dcy, dcz});
                }
            }
        }
    }
    return dirty;
}

std::vector<glm::ivec3> World::setBlocksBatch(
    const std::vector<std::pair<glm::ivec3, Block>>& edits) {
    if (edits.empty()) {
        return {};
    }
    const glm::ivec3 size = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    constexpr int N = Chunk::kSize;
    constexpr int kLightRadius = 16;

    // Write every in-window edit and track the union of their X/Z extents.
    int minX = o.x + size.x, maxX = o.x - 1;
    int minZ = o.z + size.z, maxZ = o.z - 1;
    std::vector<glm::ivec3> placed; // in-window cells, for boundary marking
    placed.reserve(edits.size());
    for (const auto& e : edits) {
        const glm::ivec3 p = e.first;
        if (p.x < o.x || p.y < o.y || p.z < o.z || p.x >= o.x + size.x ||
            p.y >= o.y + size.y || p.z >= o.z + size.z) {
            continue;
        }
        const int cx = floordiv(p.x, N), lx = floormod(p.x, N);
        const int cy = floordiv(p.y, N), ly = floormod(p.y, N);
        const int cz = floordiv(p.z, N), lz = floormod(p.z, N);
        chunks_[chunkIndex(cx, cy, cz)].set(lx, ly, lz, e.second);
        chunkDirty_[static_cast<size_t>(chunkIndex(cx, cy, cz))] = 1;
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
        placed.push_back(p);
    }
    if (placed.empty()) {
        return {};
    }

    // One relight + diff over the union box (vs one per edit in setBlock).
    const int x0 = std::max(o.x, minX - kLightRadius);
    const int x1 = std::min(o.x + size.x - 1, maxX + kLightRadius);
    const int z0 = std::max(o.z, minZ - kLightRadius);
    const int z1 = std::min(o.z + size.z - 1, maxZ + kLightRadius);
    const int sy = size.y;
    const int bw = x1 - x0 + 1, bd = z1 - z0 + 1;

    std::vector<uint8_t> oldSky(static_cast<size_t>(bw) * sy * bd);
    std::vector<uint8_t> oldBlk(static_cast<size_t>(bw) * sy * bd);
    auto boxIdx = [&](int x, int y, int z) {
        return static_cast<size_t>((x - x0) + bw * (y + sy * (z - z0)));
    };
    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                oldSky[boxIdx(x, y, z)] = skyLight_[li];
                oldBlk[boxIdx(x, y, z)] = blockLight_[li];
            }
        }
    }
    relightField(skyLight_, false, x0, x1, z0, z1);
    relightField(blockLight_, true, x0, x1, z0, z1);

    const int cx0 = floordiv(x0, N), cx1 = floordiv(x1, N);
    const int cz0 = floordiv(z0, N), cz1 = floordiv(z1, N);
    const int ncx = cx1 - cx0 + 1, ncz = cz1 - cz0 + 1;
    std::vector<char> dirtyGrid(static_cast<size_t>(ncx) * counts_.y * ncz, 0);
    auto mark = [&](int ccx, int ccy, int ccz) {
        if (ccx < cx0 || ccx > cx1 || ccz < cz0 || ccz > cz1 || ccy < 0 || ccy >= counts_.y) {
            return;
        }
        dirtyGrid[static_cast<size_t>((ccx - cx0) + ncx * (ccy + counts_.y * (ccz - cz0)))] = 1;
    };
    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                if (skyLight_[li] != oldSky[boxIdx(x, y, z)] ||
                    blockLight_[li] != oldBlk[boxIdx(x, y, z)]) {
                    mark(floordiv(x, N), floordiv(y, N), floordiv(z, N));
                }
            }
        }
    }
    for (const glm::ivec3& p : placed) {
        const int cx = floordiv(p.x, N), lx = floormod(p.x, N);
        const int cy = floordiv(p.y, N), ly = floormod(p.y, N);
        const int cz = floordiv(p.z, N), lz = floormod(p.z, N);
        mark(cx, cy, cz);
        if (lx == 0)     mark(cx - 1, cy, cz);
        if (lx == N - 1) mark(cx + 1, cy, cz);
        if (ly == 0)     mark(cx, cy - 1, cz);
        if (ly == N - 1) mark(cx, cy + 1, cz);
        if (lz == 0)     mark(cx, cy, cz - 1);
        if (lz == N - 1) mark(cx, cy, cz + 1);
    }

    std::vector<glm::ivec3> dirty;
    for (int dcz = cz0; dcz <= cz1; ++dcz) {
        for (int dcy = 0; dcy < counts_.y; ++dcy) {
            for (int dcx = cx0; dcx <= cx1; ++dcx) {
                if (dirtyGrid[static_cast<size_t>((dcx - cx0) + ncx * (dcy + counts_.y * (dcz - cz0)))]) {
                    dirty.push_back({dcx, dcy, dcz});
                }
            }
        }
    }
    return dirty;
}

void World::saveChunkIfDirty(int cx, int cy, int cz) {
    if (savePath_.empty()) {
        return;
    }
    const size_t slot = static_cast<size_t>(chunkIndex(cx, cy, cz));
    if (!chunkDirty_[slot]) {
        return;
    }
    saveChunkFile(chunkPath(savePath_, cx, cy, cz), chunks_[slot]);
    chunkDirty_[slot] = 0; // now matches disk
}

void World::saveDirtyWindow() {
    if (savePath_.empty()) {
        return;
    }
    for (int cz = originChunk_.z; cz < originChunk_.z + counts_.z; ++cz) {
        for (int cy = 0; cy < counts_.y; ++cy) {
            for (int cx = originChunk_.x; cx < originChunk_.x + counts_.x; ++cx) {
                saveChunkIfDirty(cx, cy, cz);
            }
        }
    }
}

void World::relightBox(int x0, int x1, int z0, int z1, std::vector<glm::ivec3>& dirty) {
    constexpr int N = Chunk::kSize;
    const glm::ivec3 s = sizeInBlocks();
    const int sy = s.y; // originChunk_.y is always 0 (no vertical streaming)
    const int bw = x1 - x0 + 1, bd = z1 - z0 + 1;

    // Snapshot the box, relight, then diff old-vs-new to remesh only the chunks
    // whose baked light changed (same approach as setBlock).
    std::vector<uint8_t> oldSky(static_cast<size_t>(bw) * sy * bd);
    std::vector<uint8_t> oldBlk(static_cast<size_t>(bw) * sy * bd);
    auto bi = [&](int x, int y, int z) {
        return static_cast<size_t>((x - x0) + bw * (y + sy * (z - z0)));
    };
    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                oldSky[bi(x, y, z)] = skyLight_[li];
                oldBlk[bi(x, y, z)] = blockLight_[li];
            }
        }
    }

    // Sky and block light are independent fields (separate arrays; both only READ
    // the chunks), so relight them concurrently when threading is enabled.
    if (config_.streamWorkers > 0) {
        std::future<void> sky = std::async(std::launch::async, [&] {
            relightField(skyLight_, false, x0, x1, z0, z1);
        });
        relightField(blockLight_, true, x0, x1, z0, z1);
        sky.get();
    } else {
        relightField(skyLight_, false, x0, x1, z0, z1);
        relightField(blockLight_, true, x0, x1, z0, z1);
    }

    const int cx0 = floordiv(x0, N), cx1 = floordiv(x1, N);
    const int cz0 = floordiv(z0, N), cz1 = floordiv(z1, N);
    const int ncx = cx1 - cx0 + 1, ncz = cz1 - cz0 + 1;
    std::vector<char> grid(static_cast<size_t>(ncx) * counts_.y * ncz, 0);
    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                if (skyLight_[li] != oldSky[bi(x, y, z)] || blockLight_[li] != oldBlk[bi(x, y, z)]) {
                    const int gcx = floordiv(x, N) - cx0, gcy = floordiv(y, N), gcz = floordiv(z, N) - cz0;
                    grid[static_cast<size_t>(gcx + ncx * (gcy + counts_.y * gcz))] = 1;
                }
            }
        }
    }
    for (int gcz = 0; gcz < ncz; ++gcz) {
        for (int gcy = 0; gcy < counts_.y; ++gcy) {
            for (int gcx = 0; gcx < ncx; ++gcx) {
                if (grid[static_cast<size_t>(gcx + ncx * (gcy + counts_.y * gcz))]) {
                    dirty.push_back({cx0 + gcx, gcy, cz0 + gcz});
                }
            }
        }
    }
}

void World::shiftColumn(int dir, bool alongX, std::vector<glm::ivec3>& dirty,
                        std::vector<glm::ivec4>& relightBoxes) {
    constexpr int N = Chunk::kSize;
    // Advance the window origin by one chunk along the axis. The entering edge
    // column reuses the ring slots the leaving column just vacated.
    int edge; // absolute chunk coord of the entering edge column on the moving axis
    if (alongX) {
        originChunk_.x += dir;
        edge = (dir > 0) ? originChunk_.x + counts_.x - 1 : originChunk_.x;
    } else {
        originChunk_.z += dir;
        edge = (dir > 0) ? originChunk_.z + counts_.z - 1 : originChunk_.z;
    }

    // Save departing chunks (serial — they occupy the slots we're about to reuse),
    // gather the entering edge columns, and mark the new column + its now-interior
    // neighbour for remesh. generateColumn() clears each reused slot before filling.
    std::vector<glm::ivec2> columns; // (cx,cz) of the entering edge columns
    columns.reserve(static_cast<size_t>(alongX ? counts_.z : counts_.x));
    if (alongX) {
        for (int cz = originChunk_.z; cz < originChunk_.z + counts_.z; ++cz) {
            columns.push_back({edge, cz});
            for (int cy = 0; cy < counts_.y; ++cy) {
                // The reused slot still holds the departing chunk (same ring slot,
                // counts_.x chunks back along the axis) — persist it if edited.
                saveChunkIfDirty(edge - dir * counts_.x, cy, cz);
                dirty.push_back({edge, cy, cz});
                dirty.push_back({edge - dir, cy, cz}); // now-interior neighbour
            }
        }
    } else {
        for (int cx = originChunk_.x; cx < originChunk_.x + counts_.x; ++cx) {
            columns.push_back({cx, edge});
            for (int cy = 0; cy < counts_.y; ++cy) {
                saveChunkIfDirty(cx, cy, edge - dir * counts_.z);
                dirty.push_back({cx, cy, edge});
                dirty.push_back({cx, cy, edge - dir});
            }
        }
    }

    // Generate the entering columns in parallel (each writes its own ring slots,
    // stateless noise) — the bulk of the per-step CPU cost.
    if (config_.streamWorkers > 0) {
        std::for_each(std::execution::par, columns.begin(), columns.end(),
                      [this](const glm::ivec2& c) { generateColumn(c.x, c.y); });
    } else {
        for (const glm::ivec2& c : columns) {
            generateColumn(c.x, c.y);
        }
    }

    // Relight a slab spanning the entering column plus a 15-block margin into the
    // retained interior (so light bleeds across the seam), clamped to the window.
    const glm::ivec3 s = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    // Record the relight box (the flood itself is deferred to relightBoxes(), so it
    // can run on a background thread). glm::ivec4 packs the box as (x0, x1, z0, z1).
    const int colStart = edge * N;
    if (alongX) {
        const int x0 = (dir > 0) ? std::max(o.x, colStart - 16) : colStart;
        const int x1 = (dir > 0) ? colStart + N - 1 : std::min(o.x + s.x - 1, colStart + N - 1 + 16);
        relightBoxes.push_back({x0, x1, o.z, o.z + s.z - 1});
    } else {
        const int z0 = (dir > 0) ? std::max(o.z, colStart - 16) : colStart;
        const int z1 = (dir > 0) ? colStart + N - 1 : std::min(o.z + s.z - 1, colStart + N - 1 + 16);
        relightBoxes.push_back({o.x, o.x + s.x - 1, z0, z1});
    }
}

void World::relightBoxes(const std::vector<glm::ivec4>& boxes, std::vector<glm::ivec3>& dirty) {
    for (const glm::ivec4& b : boxes) {
        relightBox(b.x, b.y, b.z, b.w, dirty); // (x0, x1, z0, z1)
    }
    // Dedup the dirty set (gen dirty + light-changed chunks may overlap / repeat).
    auto less = [](const glm::ivec3& a, const glm::ivec3& b) {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        return a.z < b.z;
    };
    auto eq = [](const glm::ivec3& a, const glm::ivec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    std::sort(dirty.begin(), dirty.end(), less);
    dirty.erase(std::unique(dirty.begin(), dirty.end(), eq), dirty.end());
}

std::vector<glm::ivec3> World::recenter(int centerChunkX, int centerChunkZ,
                                        std::vector<glm::ivec4>& relightBoxesOut) {
    if (!config_.streaming) {
        return {}; // fixed-grid world: never moves
    }
    const int targetX = centerChunkX - config_.viewRadius;
    const int targetZ = centerChunkZ - config_.viewRadius;
    if (targetX == originChunk_.x && targetZ == originChunk_.z) {
        return {};
    }

    std::vector<glm::ivec3> dirty;

    // Teleport / huge jump: nothing in the old window overlaps the new one, so
    // stepping column-by-column would be wasteful — regenerate the whole window.
    const int dx = targetX - originChunk_.x, dz = targetZ - originChunk_.z;
    if ((dx < 0 ? -dx : dx) >= counts_.x || (dz < 0 ? -dz : dz) >= counts_.z) {
        saveDirtyWindow(); // nothing in the old window is reused — persist edits first
        originChunk_.x = targetX;
        originChunk_.z = targetZ;
        generate();
        computeSkyLight();
        computeBlockLight();
        for (int cz = originChunk_.z; cz < originChunk_.z + counts_.z; ++cz) {
            for (int cy = 0; cy < counts_.y; ++cy) {
                for (int cx = originChunk_.x; cx < originChunk_.x + counts_.x; ++cx) {
                    dirty.push_back({cx, cy, cz});
                }
            }
        }
        return dirty;
    }

    // Otherwise advance one chunk column at a time so each entering column is
    // generated and relit against its already-loaded interior neighbour.
    while (originChunk_.x != targetX) {
        shiftColumn(targetX > originChunk_.x ? 1 : -1, /*alongX=*/true, dirty, relightBoxesOut);
    }
    while (originChunk_.z != targetZ) {
        shiftColumn(targetZ > originChunk_.z ? 1 : -1, /*alongX=*/false, dirty, relightBoxesOut);
    }
    // dirty is deduped by relightBoxes() (called next), after the light-changed
    // chunks have been appended too.
    return dirty;
}

bool World::needsRecenter(int centerChunkX, int centerChunkZ) const {
    if (!config_.streaming) {
        return false;
    }
    return (centerChunkX - config_.viewRadius) != originChunk_.x ||
           (centerChunkZ - config_.viewRadius) != originChunk_.z;
}

int World::surfaceHeight(int wx, int wz) const {
    return columnHeight(wx, wz) + 1;
}

} // namespace vg
