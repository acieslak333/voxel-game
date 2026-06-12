#include "world/World.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <filesystem>
#include <fstream>
#include <future>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <numeric>
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
           config.chunksY * Chunk::kSize),
      structures_((std::filesystem::path(blocksFile).parent_path() / "structures").string(),
                  registry_),
      featureNoise_(config.seed * 2246822519u + 0xFEA7u),
      features_((std::filesystem::path(blocksFile).parent_path() / "features").string(),
                registry_, gen_.biomeNames()) {
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
    tallGrassId_    = registry_.idByName("tall_grass");
    fernId_         = registry_.idByName("fern");
    flowerRedId_    = registry_.idByName("flower_red");
    flowerYellowId_ = registry_.idByName("flower_yellow");
    redMushroomId_  = registry_.idByName("red_mushroom");
    cactusId_       = registry_.idByName("cactus");
    vineId_         = registry_.idByName("vine");
    lilypadId_      = registry_.idByName("lilypad");
    birchTrunkId_   = registry_.idByName("birch_trunk");
    birchLeavesId_  = registry_.idByName("birch_leaves");
    pineTrunkId_    = registry_.idByName("pine_trunk");
    pineLeavesId_   = registry_.idByName("pine_leaves");
    mapleTrunkId_   = registry_.idByName("maple_trunk");
    mapleLeavesId_  = registry_.idByName("maple_leaves");
    willowTrunkId_  = registry_.idByName("willow_trunk");
    willowLeavesId_ = registry_.idByName("willow_leaves");
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
    const bool _t = std::getenv("VG_MESH_TIME") != nullptr;
    auto _stamp = [&](const char* what, std::chrono::steady_clock::time_point a) {
        if (_t) std::printf("[world] %s: %lldms\n", what,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - a).count()));
    };
    auto _a = std::chrono::steady_clock::now();
    generate();          _stamp("generate", _a);   _a = std::chrono::steady_clock::now();
    computeSkyLight();   _stamp("skyLight", _a);    _a = std::chrono::steady_clock::now();
    computeBlockLight(); _stamp("blockLight", _a);
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
    // Worldgen v2: caves shrunk (~half-width tunnels) so the dramatic 3D surface
    // isn't undercut into swiss-cheese — and to afford the heavier density gen.
    const float t = config_.caveThreshold * surfaceTaper * 0.55f;
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
    // Ravines: a long, narrow, deep slot canyon. Where a low-frequency 2D winding
    // noise sits inside a thin band, carve the whole tall span — so the canyon is a
    // continuous vertical slit (not blobs) that can breach a hillside. Independent
    // of surfaceTaper so it DOES open at the top (its narrowness keeps it rare).
    if (config_.ravineWidth > 0.0f && wy > config_.ravineFloor && wy < config_.ravineMaxY) {
        const float rf = config_.ravineFrequency;
        const float r = caveNoise_.fbm((wx + 911) * rf, (wz - 733) * rf, 3);
        if (std::abs(r) < config_.ravineWidth) {
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

bool World::isVegTintable(uint16_t id) const {
    return id == grassId_ || id == leavesId_ || id == bushId_ ||
           id == tallGrassId_ || id == fernId_ || id == vineId_ ||
           id == birchLeavesId_ || id == pineLeavesId_ ||
           id == mapleLeavesId_ || id == willowLeavesId_;
}

glm::vec3 World::vegTintAt(int wx, int wz) const {
    return gen_.columnInfo(wx, wz).vegTint;
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
    // Destinations = this column's ring slots (and their dirty flags); the actual
    // work lives in generateColumnInto() so pregenStrip() can aim the same code at
    // staging chunks on a background thread instead.
    std::vector<Chunk*>   stack(static_cast<size_t>(counts_.y));
    std::vector<uint8_t*> dirty(static_cast<size_t>(counts_.y));
    for (int cy = 0; cy < counts_.y; ++cy) {
        const size_t slot = static_cast<size_t>(chunkIndex(cx, cy, cz));
        stack[static_cast<size_t>(cy)] = &chunks_[slot];
        dirty[static_cast<size_t>(cy)] = &chunkDirty_[slot];
    }
    generateColumnInto(cx, cz, stack.data(), dirty.data());
}

void World::generateColumnInto(int cx, int cz, Chunk* const* stack,
                               uint8_t* const* dirtyFlags) const {
    constexpr int N = Chunk::kSize;
    const int worldTop = counts_.y * N;

    // Clear each vertical chunk; load any persisted ones in place of noise.
    // Remember which chunks still need generating.
    std::vector<uint8_t> needNoise(static_cast<size_t>(counts_.y), 1);
    bool any = false;
    for (int cy = 0; cy < counts_.y; ++cy) {
        Chunk& c = *stack[static_cast<size_t>(cy)];
        c = Chunk{}; // a reused ring slot may still hold the departed chunk's blocks
        if (!savePath_.empty() && loadChunkFile(chunkPath(savePath_, cx, cy, cz), c)) {
            *dirtyFlags[static_cast<size_t>(cy)] = 0;
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
            const int h = ci.height; // heightmap base — drives the 3D density gradient
            // The ACTUAL 3D ground surface (the density raises/lowers the land by up
            // to `amplitude`, so h is NOT where the surface is). Features — trees,
            // plants, lanterns, cairns — must sit on THIS, or they end up buried on
            // raised terrain (the "trees don't spawn high up" bug) or float on lowered.
            //
            // The density stack is the most expensive noise in worldgen, so the cells
            // it decides are evaluated ONCE into mainCol — the topmost set bit scanning
            // down from the band top is exactly gen_.surfaceY(wx, wz) (same pure
            // function, same scan), and the solid-mask fill below reuses the bits
            // instead of re-evaluating the band a second time.
            const int scanTop = gen_.surfaceScanTop(h);
            std::vector<uint8_t> mainCol(static_cast<size_t>(scanTop) + 1, 0);
            for (int wy = 0; wy <= scanTop; ++wy) {
                mainCol[static_cast<size_t>(wy)] =
                    gen_.mainTerrainSolid(h, wx, wy, wz) ? 1 : 0;
            }
            int colSurf = 1;
            for (int wy = scanTop; wy >= 1; --wy) {
                if (mainCol[static_cast<size_t>(wy)]) { colSurf = wy; break; }
            }

            // Biome-driven surface treatment (data-driven; see TerrainGenerator /
            // assets/biomes.yaml): the biome picks the surface + filler blocks and
            // snow. Submerged columns get an ocean/lake floor and grow nothing.
            const uint16_t topId = ci.topId;
            const uint16_t subId = ci.fillerId;
            const int dirtDepth  = 4;                  // filler blocks under the surface
            const int seaLevel   = gen_.seaLevel();
            const bool onLand    = h > seaLevel;       // above the sea surface
            const bool grassy    = ci.treeDensity > 0.0f || ci.bushDensity > 0.0f;

            // Cliffs: a column whose real 3D surface drops sharply to a neighbour. The
            // exposed face is (a) rendered as bare ROCK (a sand/grass skin down a vertical
            // wall looks wrong) and (b) eroded geometrically in the fill loop below — the
            // rock is carved away with a smooth 3D noise, more toward the base, so cliffs
            // round off / undercut instead of standing as flat walls. Detected by asking
            // whether a neighbour is AIR kCliffDrop blocks below our surface (cheap — one
            // density eval, no full surface scan); only above the waterline so beaches keep
            // their sand. The neighbour heightmap heights are cached for the carve.
            constexpr int kCliffDrop = 6; // vertical drop per block across that counts as cliff
            int  nhPX = 0, nhNX = 0, nhPZ = 0, nhNZ = 0; // neighbour heightmap heights (cliff only)
            bool cliff = false;
            if (colSurf - kCliffDrop > seaLevel) {
                nhPX = gen_.height(wx + 1, wz); nhNX = gen_.height(wx - 1, wz);
                nhPZ = gen_.height(wx, wz + 1); nhNZ = gen_.height(wx, wz - 1);
                const int probe = colSurf - kCliffDrop;
                cliff = !gen_.isSolid(nhPX, wx + 1, probe, wz) ||
                        !gen_.isSolid(nhNX, wx - 1, probe, wz) ||
                        !gen_.isSolid(nhPZ, wx, probe, wz + 1) ||
                        !gen_.isSolid(nhNZ, wx, probe, wz - 1);
            }

            // --- Whimsical scatter (single-column, so chunk-seam safe) ---------
            // A buried glowstone geode a few blocks down in the stone.
            int geodeY = -1;
            if (onLand && hash01(wx, wz, seed ^ 0x6e0deu) < config_.geodeDensity) {
                geodeY = colSurf - (3 + static_cast<int>(hash01(wx, wz, seed ^ 0x6e1u) * 6.0f));
                if (geodeY <= 1) geodeY = -1;
            }
            // Above-surface features: a glowing lantern-tree, else a cobble cairn.
            int stalkTop = -1, capY = -1, cairnTop = -1;
            if (grassy && hash01(wx, wz, seed ^ 0x1a27u) < config_.lanternDensity) {
                const int trunk = 3 + static_cast<int>(hash01(wx, wz, seed ^ 0x1a28u) * 5.0f);
                stalkTop = colSurf + trunk; // oak-log colSurf+1..stalkTop
                capY     = stalkTop + 1; // glowstone cap
            } else if (onLand && hash01(wx, wz, seed ^ 0xca12u) < config_.cairnDensity) {
                cairnTop = colSurf + 1 + static_cast<int>(hash01(wx, wz, seed ^ 0xca13u) * 3.0f);
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
            // Worldgen v2: 3D VOLUMETRIC fill. A cell is solid where the heightmap
            // gradient + a 3D weighted-noise perturbation is positive (overhangs /
            // cliffs / ledges), plus sparse floating islands above (gen_.isSolid). The
            // fill extends past the heightmap surface to catch overhang/float tops.
            const int top = std::min(std::max({featureTop, waterLevel, h + gen_.overhangReach()}),
                                     worldTop - 1);
            // Precompute solidity so the surface layering can cheaply see the cells
            // above (an overhang's TOP grasses over; its interior is filler/stone). The
            // deep column is always solid, so caves there stay in the solid+carve path
            // below and never flood — only near-surface air reaches the water fill.
            std::vector<uint8_t> solidCol(static_cast<size_t>(top) + 2, 0);
            for (int wy = 0; wy <= top; ++wy) {
                if (needNoise[static_cast<size_t>(wy / N)]) {
                    // isSolid == mainTerrainSolid || floatSolid; the main-terrain bit
                    // was precomputed into mainCol above (cells past scanTop are above
                    // the density band, where mainTerrainSolid is a cheap early-out).
                    const bool m = wy <= scanTop
                                       ? mainCol[static_cast<size_t>(wy)] != 0
                                       : gen_.mainTerrainSolid(h, wx, wy, wz);
                    solidCol[static_cast<size_t>(wy)] =
                        (m || gen_.floatSolid(h, wx, wy, wz)) ? 1 : 0;
                }
            }
            uint16_t belowId = 0; // for the cave-pool floor test (0xFFFF = unknown)
            for (int wy = 0; wy <= top; ++wy) {
                const int cy = wy / N;
                if (!needNoise[static_cast<size_t>(cy)]) { belowId = 0xFFFF; continue; }
                uint16_t id = 0; // air
                if (solidCol[static_cast<size_t>(wy)]) {
                    // Depth below the LOCAL surface: solid cells above this one before
                    // the first air (0 = a surface block). Bounded scan into the
                    // precomputed column so overhang tops grass over correctly.
                    int sd = 0;
                    while (sd <= dirtDepth && wy + 1 + sd <= top &&
                           solidCol[static_cast<size_t>(wy + 1 + sd)]) {
                        ++sd;
                    }
                    uint16_t mat;
                    bool isStone = false;
                    if (wy == geodeY)            mat = glowId_;
                    else if (wy >= 1 && wy <= 2) mat = lavaId_;  // deep lava floor (bedrock)
                    else if (cliff)             { mat = stoneId_; isStone = true; } // cliff face -> bare rock
                    else if (sd == 0)            mat = topId;     // surface block
                    else if (sd <= dirtDepth)    mat = subId;     // filler under the surface
                    else { mat = stoneId_; isStone = true; }

                    bool carve = false;
                    if (mat != lavaId_ && wy != geodeY) {
                        const int depth = colSurf - wy; // depth below the real surface
                        float taper = 1.0f;
                        if (depth < 5) {
                            taper = submerged
                                        ? 0.0f
                                        : (0.3f + 0.14f * static_cast<float>(std::max(0, depth)));
                        }
                        if (taper > 0.0f) carve = caveAt(wx, wy, wz, taper);
                    }
                    if (carve) {
                        if (wy <= config_.lavaPoolMaxY) {
                            id = lavaId_;
                        } else if (wy <= config_.caveWaterMaxY && belowId != 0 &&
                                   belowId != 0xFFFF && belowId != waterId_ &&
                                   belowId != lavaId_ &&
                                   hash01(wx, wy, wz, seed ^ 0x7a7eu) < config_.caveWaterChance) {
                            id = waterId_;
                        } else {
                            id = 0;
                        }
                    } else if (isStone) {
                        const uint16_t ore = oreAt(wx, wy, wz);
                        id = ore != 0 ? ore : stoneId_;
                    } else {
                        id = mat;
                    }
                    // Geometric cliff erosion: round off / undercut exposed cliff faces by
                    // carving the rock away with a smooth 3D noise — MORE toward the base
                    // (erode grows with depth down the face), so the foot recedes and the
                    // wall rounds instead of standing as a dead-flat plane. Only carves
                    // cells actually exposed toward the drop, so the interior stays solid.
                    if (cliff && id != 0 && id != waterId_ && id != lavaId_) {
                        const int down = colSurf - wy; // 0 at the top edge, grows downward
                        if (down >= 1 && down < 30) {
                            const bool exposed =
                                !gen_.isSolid(nhPX, wx + 1, wy, wz) ||
                                !gen_.isSolid(nhNX, wx - 1, wy, wz) ||
                                !gen_.isSolid(nhPZ, wx, wy, wz + 1) ||
                                !gen_.isSolid(nhNZ, wx, wy, wz - 1);
                            if (exposed) {
                                const float erode = std::min(1.0f, static_cast<float>(down) / 20.0f);
                                const float n = featureNoise_.fbm(wx * 0.12f, wy * 0.12f,
                                                                  wz * 0.12f, 3);
                                if (n > 0.60f - erode * 0.45f) id = 0; // carve -> rounded, eroded
                            }
                        }
                    }
                } else { // air: features (above the surface), else sea/lake water fill
                    if (stalkTop >= 0 && wy > colSurf && wy <= capY) {
                        id = (wy <= stalkTop) ? logId_ : glowId_; // lantern-tree
                    } else if (cairnTop >= 0 && wy > colSurf && wy <= cairnTop) {
                        id = cobbleId_;                            // cairn
                    } else if (wy <= waterLevel) {
                        id = waterId_;                             // ocean / lake
                    }
                }
                if (id != 0) {
                    stack[static_cast<size_t>(cy)]->set(lx, wy % N, lz, Block{id, 0});
                }
                belowId = id;
            }

            // --- Trees & bushes (sit on top of the terrain) -------------------
            // place() writes `id` into world cell (wx,wy,wz) iff it is currently
            // air and its chunk is being generated, routing it into whichever
            // vertical chunk owns wy. Only ever touches this column (lx,lz).
            auto place = [&](int wy, uint16_t pid) {
                if (wy < 0 || wy >= worldTop) return;
                const int pcy = wy / N;
                if (!needNoise[static_cast<size_t>(pcy)]) return;
                Chunk& pc = *stack[static_cast<size_t>(pcy)];
                if (pc.get(lx, wy % N, lz).id != 0) return; // never overwrite terrain
                pc.set(lx, wy % N, lz, Block{pid, 0});
            };

            // Trees FIRST (before ground plants), so a trunk cell is never pre-filled
            // by a plant. A tree rooted at column (ox,oz) spreads its canopy over the
            // neighbouring columns, so this column gathers contributions from every
            // nearby root within the canopy radius — keeping generation a pure
            // function of world coords (seam-safe, approach-independent). The root's
            // BIOME sets how likely a tree is (dense forests, none in desert/ocean)
            // AND which SPECIES roots (oc.treeKind -> trunk/leaf blocks + crown shape).
            constexpr int kTreeR = 4; // max canopy half-width gathered (broad maples/willows)
            // world.yaml features.tree_density is the MASTER switch for the built-in
            // trees: 0 turns them off (e.g. when trees are authored as features in
            // assets/features/ instead). maxTreeD == 0 makes the cheap-reject below
            // skip every column, so no built-in tree roots.
            const float maxTreeD = config_.treeDensity > 0.0f ? gen_.maxTreeDensity() : 0.0f;
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
                    const int oh = gen_.surfaceY(ox, oz); // the root's REAL 3D surface,
                    // so trees stand on the actual ground even where the density
                    // raised it well above the heightmap (the "no trees up high" fix).

                    const int   dx = wx - ox, dz = wz - oz;
                    const int   d2 = dx * dx + dz * dz;
                    const float csz = hash01(ox, oz, seed ^ 0x7241u); // canopy-size roll
                    const float hh  = hash01(ox, oz, seed ^ 0x7234u); // trunk-height roll

                    // Resolve the SPECIES (from the root biome) to its blocks, trunk
                    // height, crown radii (rH wide, rV tall) and silhouette shape.
                    enum Shape { Rounded, Conical, Drooping };
                    uint16_t trunkBlk = trunkId_, leafBlk = leavesId_;
                    int trunkH, rH, rV, droop = 0;
                    Shape shape = Rounded;
                    switch (oc.treeKind) {
                        case TreeKind::Birch:
                            trunkBlk = birchTrunkId_; leafBlk = birchLeavesId_;
                            trunkH = 7 + static_cast<int>(hh * 5.99f);  // 7..12, slim
                            rH = 2; rV = 3; break;
                        case TreeKind::Pine:
                            trunkBlk = pineTrunkId_; leafBlk = pineLeavesId_;
                            trunkH = 8 + static_cast<int>(hh * 6.99f);  // 8..14, spire
                            rH = 2 + static_cast<int>(csz * 1.99f); rV = trunkH;
                            shape = Conical; break;
                        case TreeKind::Maple:
                            trunkBlk = mapleTrunkId_; leafBlk = mapleLeavesId_;
                            trunkH = 5 + static_cast<int>(hh * 4.99f);  // 5..9
                            rH = 3 + static_cast<int>(csz * 1.99f); rV = rH; break; // broad, round
                        case TreeKind::Willow:
                            trunkBlk = willowTrunkId_; leafBlk = willowLeavesId_;
                            trunkH = 6 + static_cast<int>(hh * 4.99f);  // 6..10
                            rH = 3 + static_cast<int>(csz * 1.99f); rV = rH;
                            droop = 3 + static_cast<int>(csz * 2.99f);  // strand length
                            shape = Drooping; break;
                        case TreeKind::Oak:
                        default:
                            trunkBlk = trunkId_; leafBlk = leavesId_;
                            trunkH = 5 + static_cast<int>(hh * 6.99f);  // 5..11
                            rH = 2 + static_cast<int>(csz * 1.99f); rV = rH + 1; break;
                    }
                    const int topY = oh + trunkH; // topmost trunk block

                    // Trunk occupies only the root column.
                    if (dx == 0 && dz == 0) {
                        for (int wy = oh + 1; wy <= topY; ++wy) place(wy, trunkBlk);
                    }

                    if (shape == Conical) {
                        // Pine: a stack of shrinking rings — radius grows downward from
                        // a one-block spire above the trunk top, in tiered layers.
                        if (d2 > rH * rH + 1) continue;
                        const int tipY  = topY + 1;
                        const int baseY = topY - (trunkH - 2);
                        for (int wy = baseY; wy <= tipY; ++wy) {
                            int r = (tipY - wy) / 2;             // two rows per radius step
                            if (r > rH) r = rH;
                            if (d2 > r * r) continue;
                            if (dx == 0 && dz == 0 && wy <= topY) continue; // keep trunk visible
                            if (((tipY - wy) & 1) && d2 == r * r &&         // thin alt. tiers
                                hash01(wx * 73 + wy, wz * 19, seed ^ 0x7240u) < 0.5f) continue;
                            place(wy, leafBlk);
                        }
                        continue;
                    }

                    // Rounded / Drooping: an ellipsoid crown centred above the trunk
                    // top, ragged at the outer shell so it isn't a perfect ball.
                    const int cyc = topY - 1 + rV / 2; // canopy centre Y
                    if (d2 <= rH * rH + 1) {
                        for (int wy = cyc - rV; wy <= cyc + rV; ++wy) {
                            const int   ddy = wy - cyc;
                            const float e =
                                static_cast<float>(d2) / static_cast<float>(rH * rH) +
                                static_cast<float>(ddy * ddy) / static_cast<float>(rV * rV);
                            if (e > 1.0f) continue;
                            if (dx == 0 && dz == 0 && wy <= topY) continue; // leave the trunk core
                            if (e > 0.72f &&
                                hash01(wx * 73 + wy, wz * 19 + (wy & 7), seed ^ 0x7240u) < 0.28f) {
                                continue;
                            }
                            place(wy, leafBlk);
                        }
                    }

                    // Willow: hang leaf strands down from the crown's outer rim, so the
                    // canopy droops. Only some rim columns get a strand (hashed).
                    if (shape == Drooping && d2 >= (rH - 1) * (rH - 1) && d2 <= rH * rH + 1 &&
                        hash01(wx, wz, seed ^ 0x7242u) < 0.6f) {
                        const int rim = cyc - rV / 2; // just below the crown's middle
                        const int len = droop + static_cast<int>(hash01(wx, wz, seed ^ 0x7243u) * 2.99f);
                        for (int k = 0; k < len; ++k) place(rim - k, leafBlk);
                    }
                }
            }

            // Ground plants AFTER the trees, so they fill the floor AROUND trunks
            // instead of blocking them (place() never overwrites a tree cell). The
            // biome's `plant:` theme picks the family; a variety hash picks the plant.
            // world.yaml features.bush_density is the MASTER switch for built-in
            // ground plants (0 = off, e.g. when authored as features instead).
            if (config_.bushDensity > 0.0f && ci.plantKind != FloraKind::None &&
                hash01(wx, wz, seed ^ 0xb05fu) < ci.bushDensity) {
                const float v = hash01(wx, wz, seed ^ 0x91a3u); // variety roll [0,1)
                switch (ci.plantKind) {
                    case FloraKind::Desert: {
                        const int ch = 1 + static_cast<int>(hash01(wx, wz, seed ^ 0xcac7u) * 2.99f);
                        for (int k = 1; k <= ch; ++k) place(colSurf + k, cactusId_);
                        break;
                    }
                    case FloraKind::GrassFlower:
                        place(colSurf + 1, v < 0.72f ? tallGrassId_
                                   : v < 0.87f ? flowerRedId_ : flowerYellowId_);
                        break;
                    case FloraKind::Forest:
                        place(colSurf + 1, v < 0.40f ? fernId_
                                   : v < 0.62f ? bushId_
                                   : v < 0.82f ? redMushroomId_ : flowerRedId_);
                        break;
                    case FloraKind::Bush:
                    default:
                        place(colSurf + 1, bushId_);
                        break;
                }
            }

            // --- Flora leftovers (#13F): lilypads on still water + hanging vines ---
            // Read a cell in this column (after the fill + trees), routing y -> chunk.
            auto cellId = [&](int wy) -> uint16_t {
                if (wy < 0 || wy >= worldTop) return 0;
                const int rcy = wy / N;
                if (!needNoise[static_cast<size_t>(rcy)]) return 0xFFFFu; // disk-loaded: unknown
                return stack[static_cast<size_t>(rcy)]->get(lx, wy % N, lz).id;
            };
            // Lilypads float on SHALLOW still water (pond / swamp / lake edge — not the
            // deep open ocean), sparsely scattered. place() only writes into air.
            if (submerged && waterLevel - h <= 2 && waterLevel + 1 < worldTop &&
                hash01(wx, wz, seed ^ 0x11abu) < 0.06f) {
                place(waterLevel + 1, lilypadId_);
            }
            // Hanging vines: where a leaf cell sits directly over air, hang a short
            // cross-sprite strand (1-2 cells). Bounded scan of the canopy band.
            auto isLeaf = [&](uint16_t id) {
                return id == leavesId_ || id == birchLeavesId_ || id == pineLeavesId_ ||
                       id == mapleLeavesId_ || id == willowLeavesId_;
            };
            for (int wy = h + 3; wy < h + 26 && wy < worldTop - 1; ++wy) {
                if (!isLeaf(cellId(wy)) || cellId(wy - 1) != 0) continue; // leaf over air
                if (hash01(wx, wy, wz, seed ^ 0x71f3u) >= 0.035f) continue; // sparse strands
                place(wy - 1, vineId_);
                if (hash01(wx, wy, wz, seed ^ 0x71f4u) < 0.4f) place(wy - 2, vineId_);
            }

            // --- Structures: seam-safe stamp from nearby candidate origins -----
            // Candidate origins live on a coarse grid; this column gathers every
            // nearby origin whose footprint covers it and stamps that origin's
            // voxel column. A pure function of world coords, so the structure comes
            // out identical no matter which chunk streams in first (like trees).
            if (!structures_.empty() && config_.structureDensity > 0.0f) {
                const int S = std::max(8, config_.structureSpacing);
                const int cellR = structures_.maxReachXZ() / S + 1;
                auto placeForce = [&](int wy, uint16_t pid) {
                    if (wy < 0 || wy >= worldTop) return;
                    const int pcy = wy / N;
                    if (!needNoise[static_cast<size_t>(pcy)]) return;
                    stack[static_cast<size_t>(pcy)]->set(lx, wy % N, lz, Block{pid, 0});
                };
                const int gx = floordiv(wx, S), gz = floordiv(wz, S);
                for (int cgz = gz - cellR; cgz <= gz + cellR; ++cgz) {
                    for (int cgx = gx - cellR; cgx <= gx + cellR; ++cgx) {
                        if (hash01(cgx, cgz, seed ^ 0x57b1u) >= config_.structureDensity) continue;
                        const int si = structures_.pick(hash01(cgx, cgz, seed ^ 0x57b4u));
                        if (si < 0) continue;
                        const Structure& st = structures_.all()[static_cast<size_t>(si)];
                        const int ox = cgx * S + static_cast<int>(hash01(cgx, cgz, seed ^ 0x57b2u) * (S - 1));
                        const int oz = cgz * S + static_cast<int>(hash01(cgx, cgz, seed ^ 0x57b3u) * (S - 1));
                        const int slx = (wx - ox) + st.anchor.x;
                        const int slz = (wz - oz) + st.anchor.z;
                        if (slx < 0 || slz < 0 || slx >= st.size.x || slz >= st.size.z) continue;
                        const ColumnInfo oc = gen_.columnInfo(ox, oz);
                        if (st.surface && oc.height <= oc.waterLevel) continue; // not in water
                        const int oh = gen_.surfaceY(ox, oz); // sit structures on the real surface
                        for (int ly = 0; ly < st.size.y; ++ly) {
                            const uint16_t bid = st.at(slx, ly, slz);
                            if (bid != Structure::kSkip) placeForce(oh + (ly - st.anchor.y), bid);
                        }
                    }
                }
            }

            // --- Procedural features: seam-safe stamp (assets/features/) -------
            // Like structures, but each feature owns its own scatter grid and its
            // voxels are EVALUATED procedurally (shape brushes + per-instance random
            // + noise) by Feature::at — still a pure function of the origin cell +
            // seed, so it comes out identical regardless of chunk load order.
            if (!features_.empty()) {
                auto fput = [&](int wy, uint16_t pid, bool force) {
                    if (wy < 0 || wy >= worldTop) return;
                    const int pcy = wy / N;
                    if (!needNoise[static_cast<size_t>(pcy)]) return;
                    Chunk& pc = *stack[static_cast<size_t>(pcy)];
                    if (!force && pc.get(lx, wy % N, lz).id != 0) return; // air-only
                    pc.set(lx, wy % N, lz, Block{pid, 0});
                };
                const auto& feats = features_.all();
                for (size_t fi = 0; fi < feats.size(); ++fi) {
                    const Feature& ft = feats[fi];
                    if (ft.scatter.density <= 0.0f) continue;
                    const int S = std::max(4, ft.scatter.spacing);
                    const int reach = std::max({ft.anchor.x, ft.size.x - 1 - ft.anchor.x,
                                                ft.anchor.z, ft.size.z - 1 - ft.anchor.z});
                    const int cellR = reach / S + 1;
                    const uint32_t fsalt = seed ^ (0xF0000000u + static_cast<uint32_t>(fi) * 0x9e3779b9u);
                    const int gx = floordiv(wx, S), gz = floordiv(wz, S);
                    for (int cgz = gz - cellR; cgz <= gz + cellR; ++cgz) {
                        for (int cgx = gx - cellR; cgx <= gx + cellR; ++cgx) {
                            if (hash01(cgx, cgz, fsalt ^ 0x1u) >= ft.scatter.density) continue;
                            const int ox = cgx * S + static_cast<int>(hash01(cgx, cgz, fsalt ^ 0x2u) * (S - 1));
                            const int oz = cgz * S + static_cast<int>(hash01(cgx, cgz, fsalt ^ 0x3u) * (S - 1));
                            // Noise distribution: only root inside the clumps of a low-freq
                            // field, so the feature comes in organic patches not an even grid.
                            if (ft.scatter.dist == FeatureScatter::Dist::Noise &&
                                featureNoise_.perlin(static_cast<float>(ox) * ft.scatter.noiseFreq,
                                                     static_cast<float>(oz) * ft.scatter.noiseFreq)
                                    <= ft.scatter.noiseThresh) continue;
                            const int slx = (wx - ox) + ft.anchor.x;
                            const int slz = (wz - oz) + ft.anchor.z;
                            if (slx < 0 || slz < 0 || slx >= ft.size.x || slz >= ft.size.z) continue;
                            const ColumnInfo oc = gen_.columnInfo(ox, oz);
                            const bool submerged = oc.height < oc.waterLevel;
                            if (ft.scatter.onWater) { if (!submerged) continue; }   // need water below
                            else if (ft.scatter.surface && submerged) continue;     // dry-land only
                            const int relEl = oc.height - oc.waterLevel; // elevation vs sea/lake
                            if (relEl < ft.scatter.minElevation || relEl > ft.scatter.maxElevation) continue;
                            if (!ft.scatter.biomeIds.empty() &&
                                std::find(ft.scatter.biomeIds.begin(), ft.scatter.biomeIds.end(),
                                          oc.biome) == ft.scatter.biomeIds.end()) continue;
                            if (ft.scatter.nearWater > 0) {  // only near a shoreline / lake edge
                                const int nw = ft.scatter.nearWater;
                                const int dxs[4] = {nw, -nw, 0, 0}, dzs[4] = {0, 0, nw, -nw};
                                bool found = false;
                                for (int k = 0; k < 4 && !found; ++k) {
                                    const ColumnInfo nc = gen_.columnInfo(ox + dxs[k], oz + dzs[k]);
                                    if (nc.height < nc.waterLevel) found = true;
                                }
                                if (!found) continue;
                            }
                            // Root on the water surface (lilypads) or the real 3D ground.
                            const int oh = ft.scatter.onWater ? oc.waterLevel : gen_.surfaceY(ox, oz);
                            if (ft.scatter.minSlope > 0 || ft.scatter.maxSlope < 100000) {
                                int lo = oh, hi = oh;  // steepness = surface delta nearby
                                const int dxs[4] = {3, -3, 0, 0}, dzs[4] = {0, 0, 3, -3};
                                for (int k = 0; k < 4; ++k) {
                                    const int s = gen_.surfaceY(ox + dxs[k], oz + dzs[k]);
                                    lo = std::min(lo, s); hi = std::max(hi, s);
                                }
                                const int slope = hi - lo;
                                if (slope < ft.scatter.minSlope || slope > ft.scatter.maxSlope) continue;
                            }
                            const uint32_t oseed = static_cast<uint32_t>(cgx) * 73856093u ^
                                                   static_cast<uint32_t>(cgz) * 19349663u ^
                                                   (fsalt * 2654435761u);
                            for (int ly = 0; ly < ft.size.y; ++ly) {
                                const int wy = oh + (ly - ft.anchor.y);
                                const Feature::Cell c =
                                    ft.at(oseed, slx, ly, slz, featureNoise_, wx, wy, wz);
                                if (c.id != Feature::kSkip) fput(wy, c.id, c.force);
                            }
                        }
                    }
                }
            }
        }
    }
    for (int cy = 0; cy < counts_.y; ++cy) {
        if (needNoise[static_cast<size_t>(cy)]) {
            *dirtyFlags[static_cast<size_t>(cy)] = 0; // fresh from noise == clean
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
           p.renderType == RenderType::LeafCube || p.renderType == RenderType::Flat;
}

float World::modelInsetAt(int wx, int wy, int wz) const {
    const Block b = blockAt(wx, wy, wz);
    return registry_.renderType(b.id) == RenderType::Model ? registry_.modelInset(b.id)
                                                           : 0.0f;
}

int World::collisionBoxesAt(int wx, int wy, int wz, ShapeBox out[]) const {
    if (!isSolid(wx, wy, wz)) return 0;
    const Block b = blockAt(wx, wy, wz);

    // A thin Model post (e.g. a trunk) collides as its centred render column.
    if (registry_.renderType(b.id) == RenderType::Model) {
        const float ins = registry_.modelInset(b.id);
        out[0] = {{wx + ins, static_cast<float>(wy), wz + ins},
                  {wx + 1.0f - ins, wy + 1.0f, wz + 1.0f - ins}};
        return 1;
    }

    ShapeKind kind   = ShapeKind::Cube;
    uint8_t   orient = 0;
    uint8_t   wallMask = 0;
    if (registry_.shapeable(b.id)) {
        kind   = shapeKindOf(b.metadata);
        orient = shapeOrientOf(b.metadata);
        if (kind == ShapeKind::Wall) {
            auto conn = [&](int dx, int dy, int dz) {
                const Block nb = blockAt(wx + dx, wy + dy, wz + dz);
                const bool fullCube = registry_.isOpaque(nb.id) &&
                    (!registry_.shapeable(nb.id) || shapeKindOf(nb.metadata) == ShapeKind::Cube);
                return fullCube || (registry_.shapeable(nb.id) &&
                                    shapeKindOf(nb.metadata) == ShapeKind::Wall);
            };
            if (conn(0, 0, -1)) wallMask |= 0x1;
            if (conn(1, 0, 0))  wallMask |= 0x2;
            if (conn(0, 0, 1))  wallMask |= 0x4;
            if (conn(-1, 0, 0)) wallMask |= 0x8;
        }
    }
    const int n = shapeBoxes(kind, orient, wallMask, out);
    const glm::vec3 origin(static_cast<float>(wx), static_cast<float>(wy),
                           static_cast<float>(wz));
    for (int i = 0; i < n; ++i) {
        out[i].lo += origin;
        out[i].hi += origin;
    }
    return n;
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
    const glm::ivec3 o = originBlock();
    const int falloff = config_.skyFalloff;
    const glm::ivec3 dirs[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    const bool par = config_.streamWorkers > 0;

    // --- Pass A: per-column top-down SEED of the light values (no frontier) ------
    // Walk each column from the top, marking blocks fully lit (15) and dimming by
    // each block's lightOpacity until an opaque block ends the column. Columns are
    // independent (each writes only its own (x,z) cells), so run the Z-slices in
    // parallel. (Previously this also pushed every lit cell — ~48M — to the flood
    // frontier; almost none of them can ever darken a neighbour, so the flood spun
    // over tens of millions of no-op cells. Pass B below seeds only the few that can.)
    std::vector<int> zs(static_cast<size_t>(s.z));
    std::iota(zs.begin(), zs.end(), o.z);
    auto seedColumn = [&](int z) {
        for (int x = o.x; x < o.x + s.x; ++x) {
            int sky = 15;
            for (int y = o.y + s.y - 1; y >= o.y; --y) {
                const int op = registry_.lightOpacity(blockAt(x, y, z).id);
                if (op >= 15) break;
                skyLight_[static_cast<size_t>(lightIndex(x, y, z))] = static_cast<uint8_t>(sky);
                sky -= op;
                if (sky <= 0) break;
            }
        }
    };
    if (par) std::for_each(std::execution::par, zs.begin(), zs.end(), seedColumn);
    else     std::for_each(zs.begin(), zs.end(), seedColumn);

    // --- Pass B: build the INITIAL flood frontier ---------------------------------
    // Only a lit cell that can actually darken a neighbour belongs in the frontier:
    // one with an in-window neighbour whose seeded light is already below this
    // cell's level minus the base falloff. The open-sky interior (all 15, neighbours
    // all 15) relaxes nothing and is skipped. The threshold ignores the neighbour's
    // own opacity, so it can only OVER-include (extra cells relax nothing) — the
    // flood's fixpoint, and thus the light field, is byte-identical to the old seed.
    // Parallel per Z-slice into per-slice buckets, then concatenated.
    std::vector<std::vector<glm::ivec3>> bands(static_cast<size_t>(s.z));
    std::vector<int> zis(static_cast<size_t>(s.z));
    std::iota(zis.begin(), zis.end(), 0);
    auto scanColumn = [&](int zi) {
        const int z = o.z + zi;
        std::vector<glm::ivec3>& out = bands[static_cast<size_t>(zi)];
        for (int x = o.x; x < o.x + s.x; ++x) {
            for (int y = o.y; y < o.y + s.y; ++y) {
                const int v = skyLight_[static_cast<size_t>(lightIndex(x, y, z))];
                if (v <= falloff) continue;
                for (const glm::ivec3& d : dirs) {
                    const int nx = x + d.x, ny = y + d.y, nz = z + d.z;
                    if (nx < o.x || ny < o.y || nz < o.z ||
                        nx >= o.x + s.x || ny >= o.y + s.y || nz >= o.z + s.z) continue;
                    if (skyLight_[static_cast<size_t>(lightIndex(nx, ny, nz))] < v - falloff) {
                        out.push_back({x, y, z});
                        break;
                    }
                }
            }
        }
    };
    if (par) std::for_each(std::execution::par, zis.begin(), zis.end(), scanColumn);
    else     std::for_each(zis.begin(), zis.end(), scanColumn);

    std::vector<glm::ivec3> frontier;
    size_t total = 0;
    for (const auto& b : bands) total += b.size();
    frontier.reserve(total);
    for (auto& b : bands) frontier.insert(frontier.end(), b.begin(), b.end());

    if (std::getenv("VG_MESH_TIME"))
        std::printf("[world]   skyLight frontier: %zu cells (was ~48M)\n", frontier.size());
    const auto _sf = std::chrono::steady_clock::now();
    // --- Flood (unchanged relaxation): light spreads to non-opaque neighbours,
    // losing sky_falloff (+ opacity) per step. Now over only the boundary frontier.
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
            const int op = registry_.lightOpacity(blockAt(nx, ny, nz).id);
            if (op >= 15) {
                continue;
            }
            const int step = falloff + op;
            uint8_t& nl = skyLight_[static_cast<size_t>(lightIndex(nx, ny, nz))];
            if (level > step && nl < level - step) {
                nl = static_cast<uint8_t>(level - step);
                frontier.push_back({nx, ny, nz});
            }
        }
    }
    if (std::getenv("VG_MESH_TIME"))
        std::printf("[world]   skyLight flood: %lldms\n",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - _sf).count()));
}

void World::computeBlockLight() {
    const glm::ivec3 s = sizeInBlocks();
    blockLight_.assign(static_cast<size_t>(s.x) * s.y * s.z, 0);
    // Hue rides along with the intensity: each cell records the colour of whichever
    // emitter reaches it brightest (set together with the level below).
    blockLightColor_.assign(static_cast<size_t>(s.x) * s.y * s.z, 0);

    // Seed: every emissive block starts at its emission level. Emitters may be
    // opaque (glowstone is a solid cube), so seeding ignores opacity — the spread
    // below is what respects it, lighting the open space around the emitter.
    // Scanning every cell for emitters single-threaded was ~0.7s of startup; the
    // Z-slices are independent (each writes only its own cells), so scan them in
    // parallel into per-slice buckets — the same shape as computeSkyLight's passes.
    // Buckets are concatenated in slice order, so the frontier ordering (and with
    // it the flood's first-writer-wins colour tie-breaking) is identical to the
    // sequential scan: the light AND colour fields come out byte-identical.
    const glm::ivec3 o = originBlock();
    const bool par = config_.streamWorkers > 0;
    std::vector<std::vector<glm::ivec3>> bands(static_cast<size_t>(s.z));
    std::vector<int> zis(static_cast<size_t>(s.z));
    std::iota(zis.begin(), zis.end(), 0);
    auto seedSlice = [&](int zi) {
        const int z = o.z + zi;
        std::vector<glm::ivec3>& out = bands[static_cast<size_t>(zi)];
        for (int y = o.y; y < o.y + s.y; ++y) {
            for (int x = o.x; x < o.x + s.x; ++x) {
                const uint16_t id = blockAt(x, y, z).id;
                const uint8_t e = registry_.emission(id);
                if (e > 0) {
                    const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                    blockLight_[li] = e;
                    blockLightColor_[li] = packLightColor(registry_.emissionColor(id));
                    out.push_back({x, y, z});
                }
            }
        }
    };
    if (par) std::for_each(std::execution::par, zis.begin(), zis.end(), seedSlice);
    else     std::for_each(zis.begin(), zis.end(), seedSlice);

    std::vector<glm::ivec3> frontier;
    size_t total = 0;
    for (const auto& b : bands) total += b.size();
    frontier.reserve(total);
    for (auto& b : bands) frontier.insert(frontier.end(), b.begin(), b.end());

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
    // Sky-light opacity (0..15) of the block at a cell — how much sky light it
    // subtracts. Used only on the sky pass so this incremental relight matches
    // computeSkyLight (foliage casts a shadow that survives nearby edits).
    auto skyOpacityAt = [&](int x, int y, int z) {
        return static_cast<int>(registry_.lightOpacity(
            chunkAtBlock(x, y, z).get(floormod(x, N), floormod(y, N), floormod(z, N)).id));
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
        // Sky: each column starts at 15 and is dimmed by every block's opacity as
        // it descends (foliage shadow), ending at a fully-blocking block — exactly
        // the model in computeSkyLight, so an edit relights the box the same way.
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                int sky = 15;
                for (int y = o.y + sy - 1; y >= o.y; --y) {
                    const int op = skyOpacityAt(x, y, z);
                    if (op >= 15) break;
                    fieldAt(x, y, z) = static_cast<uint8_t>(sky);
                    frontier.push_back({x, y, z});
                    sky -= op;
                    if (sky <= 0) break;
                }
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
        // Cost to light this cell: base falloff, plus (sky pass only) the cell's
        // own opacity so light leaks slowly into foliage and not at all through a
        // full blocker — matching the flood below and computeSkyLight.
        int step = falloff;
        if (emitterSeed) {
            if (opaqueAt(ix, iy, iz)) return;
        } else {
            const int op = skyOpacityAt(ix, iy, iz);
            if (op >= 15) return;
            step += op;
        }
        const int v = static_cast<int>(fieldAt(ax, ay, az)) - step;
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
            // Entering a cell costs falloff plus (sky pass only) its opacity; a full
            // blocker stops the spread. Mirrors computeSkyLight's flood so foliage
            // shade is identical whether baked at gen time or after an edit.
            int step = falloff;
            if (emitterSeed) {
                if (opaqueAt(nx, ny, nz)) continue;
            } else {
                const int op = skyOpacityAt(nx, ny, nz);
                if (op >= 15) continue;
                step += op;
            }
            if (level > step && fieldAt(nx, ny, nz) < level - step) {
                fieldAt(nx, ny, nz) = static_cast<uint8_t>(level - step);
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
                        std::vector<glm::ivec4>& relightBoxes, PregenStrip* strip) {
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
    // stateless noise) — the bulk of the per-step CPU cost — unless a matching
    // pregenerated strip provides them (pregenStrip ran the SAME generation on a
    // background thread), in which case the staged chunks are MOVED into the ring
    // slots: a few ms of memcpy instead of ~90ms of noise on the main thread.
    if (strip) {
        for (size_t i = 0; i < columns.size(); ++i) {
            for (int cy = 0; cy < counts_.y; ++cy) {
                const size_t slot = static_cast<size_t>(
                    chunkIndex(columns[i].x, cy, columns[i].y));
                chunks_[slot] = std::move(
                    strip->chunks[i * static_cast<size_t>(counts_.y) +
                                  static_cast<size_t>(cy)]);
                chunkDirty_[slot] = 0; // fresh from noise/disk == clean
            }
        }
    } else if (config_.streamWorkers > 0) {
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

World::PregenStrip World::pregenStrip(int dir, bool alongX) const {
    PregenStrip s;
    s.dir    = dir;
    s.alongX = alongX;
    s.origin = {originChunk_.x, originChunk_.z};

    // The entering edge column AFTER the one-column step this strip is for —
    // mirrors shiftColumn's edge computation (which runs post-move, so +dir here).
    int edge;
    if (alongX) {
        edge = (dir > 0) ? originChunk_.x + dir + counts_.x - 1 : originChunk_.x + dir;
    } else {
        edge = (dir > 0) ? originChunk_.z + dir + counts_.z - 1 : originChunk_.z + dir;
    }
    // Same column order shiftColumn gathers, so the apply is a straight move.
    std::vector<glm::ivec2> columns;
    columns.reserve(static_cast<size_t>(alongX ? counts_.z : counts_.x));
    if (alongX) {
        for (int cz = originChunk_.z; cz < originChunk_.z + counts_.z; ++cz) {
            columns.push_back({edge, cz});
        }
    } else {
        for (int cx = originChunk_.x; cx < originChunk_.x + counts_.x; ++cx) {
            columns.push_back({cx, edge});
        }
    }

    // Generate every column into the staging chunks — the exact code the ring
    // slots get (generateColumnInto), so the applied window is byte-identical to
    // a synchronous shift. Dirty flags land in a scratch buffer; the apply resets
    // the real ones. Parallel like generate(): columns are independent.
    s.chunks.resize(columns.size() * static_cast<size_t>(counts_.y));
    std::vector<uint8_t> dirtyScratch(s.chunks.size(), 0);
    auto genOne = [&](size_t i) {
        std::vector<Chunk*>   stack(static_cast<size_t>(counts_.y));
        std::vector<uint8_t*> dirty(static_cast<size_t>(counts_.y));
        for (int cy = 0; cy < counts_.y; ++cy) {
            const size_t k = i * static_cast<size_t>(counts_.y) + static_cast<size_t>(cy);
            stack[static_cast<size_t>(cy)] = &s.chunks[k];
            dirty[static_cast<size_t>(cy)] = &dirtyScratch[k];
        }
        generateColumnInto(columns[i].x, columns[i].y, stack.data(), dirty.data());
    };
    std::vector<size_t> idx(columns.size());
    std::iota(idx.begin(), idx.end(), size_t{0});
    if (config_.streamWorkers > 0) {
        std::for_each(std::execution::par, idx.begin(), idx.end(), genOne);
    } else {
        std::for_each(idx.begin(), idx.end(), genOne);
    }
    s.valid = true;
    return s;
}

std::vector<glm::ivec3> World::recenterWithStrip(int centerChunkX, int centerChunkZ,
                                                 PregenStrip&& strip,
                                                 std::vector<glm::ivec4>& relightBoxesOut) {
    std::vector<glm::ivec3> dirty;
    if (!config_.streaming || !strip.valid) {
        return dirty;
    }
    // A strip is good for exactly ONE column step from the origin it was made at,
    // and only if that step is still the one needed (the player may have turned
    // around while it generated). On any mismatch: discard — nothing moved, and
    // the next cycle pregens the right strip. X is stepped first, like recenter().
    if (strip.origin.x != originChunk_.x || strip.origin.y != originChunk_.z) {
        return dirty;
    }
    const int targetX = centerChunkX - config_.viewRadius;
    const int targetZ = centerChunkZ - config_.viewRadius;
    int  dir;
    bool alongX;
    if (targetX != originChunk_.x) {
        alongX = true;
        dir    = targetX > originChunk_.x ? 1 : -1;
    } else if (targetZ != originChunk_.z) {
        alongX = false;
        dir    = targetZ > originChunk_.z ? 1 : -1;
    } else {
        return dirty; // window no longer needs to move
    }
    if (dir != strip.dir || alongX != strip.alongX) {
        return dirty;
    }
    // Remaining steps (diagonal / fast movement) are deliberately NOT taken here:
    // each later cycle pregens + applies its own column, so the main thread never
    // pays generation. dirty is deduped by relightBoxes(), as with recenter().
    shiftColumn(dir, alongX, dirty, relightBoxesOut, &strip);
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
