#include "world/World.h"

#include "utilities/hash/Hash.h"
#include "utilities/noise/Noise.h"

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
#include <iterator>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace vg {

namespace {
// floordiv/floormod/hash01 now live in world/Hash.h (the canonical worldgen hashes,
// shared so copies can't drift); used here unqualified via the vg namespace.

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
constexpr uint32_t kChunkVersion = 17u; // bump: coherent low-freq fBm field (S1, replaces anisotropic speckle)

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
      registry_(blocksFile) {
    // Resolve the block types the flat generator places (throws if a name is
    // absent from the block-definition file).
    grassId_  = registry_.idByName("grass");
    dirtId_   = registry_.idByName("dirt");
    stoneId_  = registry_.idByName("stone");
    cobbleId_ = registry_.idByName("cobblestone");

    buildLightColorPalette();

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

// (hash01 2D/3D moved to world/Hash.h — shared canonical worldgen hashes.)

int World::columnHeight(int wx, int wz) const {
    // Flat world: scan the actual voxels for the topmost solid block so the result
    // reflects player edits. The generated base surface is grass at Y=16.
    const glm::ivec3 o = originBlock();
    for (int wy = o.y + sizeInBlocks().y - 1; wy >= o.y; --wy) {
        if (isSolid(wx, wy, wz)) return wy;
    }
    return 16; // flat grass surface (nothing solid found / out of window)
}

// Biome vegetation tinting is gone with the worldgen overhaul: the flat world has
// no biomes, so foliage keeps its authored texture colour (a white multiplier).
bool World::isVegTintable(uint16_t /*id*/) const { return false; }

glm::vec3 World::vegTintAt(int /*wx*/, int /*wz*/) const { return glm::vec3(1.0f); }

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

    // --- 3D Perlin solid/air (multi-layer) ---------------------------------------
    // The world IS a blend of several 3D Perlin fields. Each layer samples its own
    // independently-seeded Perlin at the voxel's world position, remapped from
    // ~[-1,1] to [0,1]; the layers are combined by a WEIGHTED AVERAGE (so the blend
    // stays in [0,1]) and a single threshold decides solid vs air. Every layer is a
    // pure function of (seed, world x/y/z), so chunk seams agree across threads/time
    // (streaming-safe). Add/remove/retune layers here — the threshold logic below is
    // untouched. Every tunable is spelled out below:
    //   type          — which noise primitive (all that vg::Noise exposes):
    //                     Perlin  raw rolling field (~[-1,1]).
    //                     Ridged  1-2|perlin| -> sharp ridgelines / canyons.
    //                     Billow  2|perlin|-1 -> rounded puffy blobs.
    //                     Voronoi cellular/Worley -> cells, cracks, fluting
    //                             (honours `metric` + `cell` below).
    //   seedOffset    — added to the world seed; this is the ONLY thing vg::Noise
    //                   accepts (its ctor is Noise(uint32_t seed); frequency is
    //                   locked to 1 internally and interpolation is Quintic — we
    //                   control zoom via freq{X,Y,Z} here, not in the engine).
    //                   Distinct offsets -> independent fields; reuse one -> same field.
    //   freq{X,Y,Z}   — per-axis noise zoom (smaller -> larger blobs along that
    //                   axis; e.g. low freqY -> tall vertical structures).
    //   weight        — relative contribution to the weighted-average blend.
    //   metric, cell  — Voronoi only (ignored otherwise): distance function
    //                   (Euclidean/Manhattan/Chebyshev) and which value to return
    //                   (F1 nearest -> rounded cells, F2, or F2mF1 -> cell-edge cracks).
    enum class NType { Perlin, Ridged, Billow, Voronoi, Fbm, RidgedFbm };
    using Metric = vg::Noise::Metric;
    using Cell   = vg::Noise::Cell;
    struct Layer {
        NType    type;       // which noise primitive to sample
        uint32_t seedOffset; // + config_.seed -> this layer's independent field
        float    freqX;      // coordinate scale on X applied before sampling
        float    freqY;      // coordinate scale on Y (smaller -> taller features)
        float    freqZ;      // coordinate scale on Z
        float    weight;     // relative contribution to the blended [0,1] value
        int      octaves;    // Fbm/RidgedFbm only: fractal octave count (ignored otherwise)
        Metric   metric;     // Voronoi distance function (ignored by other types)
        Cell     cell;       // Voronoi return mode      (ignored by other types)
    };
    constexpr Layer kLayers[] = {
        // type          seedOffset  freqX   freqY   freqZ  weight oct  metric             cell
        // A single low-frequency, ISOTROPIC Perlin field. Low frequency (0.02 = ~50-
        // block features) carves large CONTIGUOUS solid/air masses, so greedy meshing
        // finds big coplanar runs to merge instead of per-voxel speckle. The old
        // anisotropic freqY=0.4 oscillated solid/air every ~2.5 blocks vertically =
        // maximal exposed surface = the slow-load "speckle"; making the frequency
        // isotropic and low cut triangles ~72% and mesh time ~60% (measured).
        //   For more surface detail at low cost, switch type to NType::Fbm and raise
        //   `oct` (octave count) — falling-amplitude octaves add roughness without
        //   re-fragmenting the field; the gen cost (one Perlin call per octave) is
        //   what the upcoming SIMD batch-sampling step (S2) is meant to absorb.
        {NType::Perlin,        202u,  0.02f,  0.02f,  0.02f,  1.00f, 1,  Metric::Euclidean, Cell::F1},
    };
    constexpr float kThreshold = 0.5f; // [0,1] cutoff: >= solid, < air

    // Build one Perlin field per layer (seed is vg::Noise's sole parameter).
    std::vector<vg::Noise> fields;
    fields.reserve(std::size(kLayers));
    float weightSum = 0.0f;
    for (const Layer& L : kLayers) {
        fields.emplace_back(config_.seed + L.seedOffset);
        weightSum += L.weight;
    }

    for (int lz = 0; lz < N; ++lz) {
        for (int lx = 0; lx < N; ++lx) {
            const int wx = cx * N + lx; // world coordinates of this column
            const int wz = cz * N + lz;
            for (int wy = 0; wy < worldTop; ++wy) {
                const int cy = wy / N;
                if (!needNoise[static_cast<size_t>(cy)]) { continue; }
                // Weighted blend of every layer, each remapped [-1,1] -> [0,1].
                float v = 0.0f;
                for (size_t i = 0; i < fields.size(); ++i) {
                    const Layer&     L  = kLayers[i];
                    const vg::Noise& fn = fields[i];
                    const float sx = static_cast<float>(wx) * L.freqX;
                    const float sy = static_cast<float>(wy) * L.freqY;
                    const float sz = static_cast<float>(wz) * L.freqZ;
                    // Sample the chosen primitive as a raw ~[-1,1] value. Ridged/Billow
                    // are the standard shape transforms of perlin (see NoiseStack::shape).
                    float r;
                    switch (L.type) {
                        case NType::Ridged:    r = 1.0f - 2.0f * std::fabs(fn.perlin(sx, sy, sz)); break;
                        case NType::Billow:    r = 2.0f * std::fabs(fn.perlin(sx, sy, sz)) - 1.0f; break;
                        case NType::Voronoi:   r = fn.worley(sx, sy, sz, L.metric, L.cell);        break;
                        case NType::Fbm:       r = fn.fbm(sx, sy, sz, L.octaves);                  break;
                        case NType::RidgedFbm: r = 1.0f - 2.0f * std::fabs(fn.fbm(sx, sy, sz, L.octaves)); break;
                        case NType::Perlin:
                        default:               r = fn.perlin(sx, sy, sz);                          break;
                    }
                    const float n = r * 0.5f + 0.5f; // [-1,1] -> [0,1]
                    v += L.weight * n;
                }
                v /= weightSum; // normalise back to [0,1]
                if (v >= kThreshold) {
                    stack[static_cast<size_t>(cy)]->set(lx, wy % N, lz, Block{stoneId_, 0});
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
    return unpackLightColor(
        lightColorPalette_[blockLightColor_[static_cast<size_t>(lightIndex(wx, wy, wz))]]);
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

void World::buildLightColorPalette() {
    // Entry 0 is "no colour" (unlit cells / non-emitters). Every distinct emitter
    // hue gets one palette slot; emissionColorIndex_[id] maps a block to its slot.
    // Distinct emitter colours are a handful, so a linear find-or-add is fine and
    // the index fits a byte (guarded below).
    lightColorPalette_.assign(1, 0u);
    emissionColorIndex_.assign(registry_.blockCount(), 0);
    for (uint16_t id = 1; id < registry_.blockCount(); ++id) {
        if (registry_.emission(id) == 0) continue;
        const uint32_t packed = packLightColor(registry_.emissionColor(id));
        auto it = std::find(lightColorPalette_.begin(), lightColorPalette_.end(), packed);
        size_t idx = static_cast<size_t>(std::distance(lightColorPalette_.begin(), it));
        if (it == lightColorPalette_.end()) {
            lightColorPalette_.push_back(packed);
        }
        // Palette index is stored per-cell as a uint8_t; >255 distinct emitter
        // colours would alias. Far beyond any real blocks.yaml, but clamp loudly.
        if (idx > 255) {
            std::cerr << "[world] warning: >255 distinct emitter colours; "
                         "block-light hue palette overflow (using index 0).\n";
            idx = 0;
        }
        emissionColorIndex_[id] = static_cast<uint8_t>(idx);
    }
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
                    blockLightColor_[li] = emissionColorIndex_[id];
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
                         int y0, int y1, int z0, int z1) {
    const glm::ivec3 s = sizeInBlocks();
    const glm::ivec3 o = originBlock();
    const int sy = s.y;
    const int yTop = o.y + sy - 1; // topmost loaded cell (window has no vertical streaming)
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
    auto colorAt = [&](int x, int y, int z) -> uint8_t& {
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

    // Clear the box.
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                fieldAt(x, y, z) = 0;
                if (emitterSeed) colorAt(x, y, z) = 0;
            }

    std::vector<glm::ivec3> frontier;
    if (!emitterSeed) {
        // Sky: each column descends from the top, dimmed by every block's opacity
        // (foliage shadow), ending at a fully-blocking block — exactly the model in
        // computeSkyLight. When the box top is below the world top, the sky value
        // arriving at y1 is the unchanged value just above (vertical sky is lossless
        // through air, costing only opacity), so inherit it instead of assuming open
        // sky. Edits never brighten cells ABOVE them, so that above-box value is
        // still correct.
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                int sky = (y1 >= yTop)
                    ? 15
                    : std::max(0, static_cast<int>(fieldAt(x, y1 + 1, z)) -
                                      skyOpacityAt(x, y1 + 1, z));
                for (int y = y1; y >= y0; --y) {
                    const int op = skyOpacityAt(x, y, z);
                    if (op >= 15) break;
                    if (sky <= 0) break;
                    fieldAt(x, y, z) = static_cast<uint8_t>(sky);
                    frontier.push_back({x, y, z});
                    sky -= op;
                }
            }
    } else {
        // Block light: seed every emissive block (emitters may be opaque).
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const uint16_t id =
                        chunkAtBlock(x, y, z).get(floormod(x, N), floormod(y, N), floormod(z, N)).id;
                    const uint8_t e = registry_.emission(id);
                    if (e > 0) {
                        fieldAt(x, y, z) = e;
                        colorAt(x, y, z) = emissionColorIndex_[id];
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
    for (int y = y0; y <= y1; ++y) {
        if (x0 > o.x)           for (int z = z0; z <= z1; ++z) borderSeed(x0, y, z, x0 - 1, y, z);
        if (x1 < o.x + s.x - 1) for (int z = z0; z <= z1; ++z) borderSeed(x1, y, z, x1 + 1, y, z);
        if (z0 > o.z)           for (int x = x0; x <= x1; ++x) borderSeed(x, y, z0, x, y, z0 - 1);
        if (z1 < o.z + s.z - 1) for (int x = x0; x <= x1; ++x) borderSeed(x, y, z1, x, y, z1 + 1);
    }
    // Top/bottom Y faces — only for block light, which is isotropic and can enter
    // the clamped box vertically from outside. The sky pass already seeds its top
    // face via the column inherit above (and never receives sky from below), so it
    // skips this.
    if (emitterSeed) {
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                if (y0 > o.y)  borderSeed(x, y0, z, x, y0 - 1, z);
                if (y1 < yTop) borderSeed(x, y1, z, x, y1 + 1, z);
            }
    }

    // Flood fill, clamped to the box (X/Z) and the world height (Y).
    const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (size_t head = 0; head < frontier.size(); ++head) {
        const glm::ivec3 p = frontier[head];
        const int level = fieldAt(p.x, p.y, p.z);
        if (level <= falloff) continue;
        for (const auto& d : dirs) {
            const int nx = p.x + d[0], ny = p.y + d[1], nz = p.z + d[2];
            if (nx < x0 || nx > x1 || nz < z0 || nz > z1 || ny < y0 || ny > y1) continue;
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
    // both lighting fields — but only within light's 15-block reach. Relight just
    // that neighbourhood (a full recompute here is what made editing stutter on
    // bigger worlds).
    constexpr int N = Chunk::kSize;
    constexpr int kLightRadius = 16; // one past light's 15-block reach
    const int x0 = std::max(o.x, wx - kLightRadius), x1 = std::min(o.x + size.x - 1, wx + kLightRadius);
    const int z0 = std::max(o.z, wz - kLightRadius), z1 = std::min(o.z + size.z - 1, wz + kLightRadius);

    // Lighting is baked into chunk vertices, so any chunk whose light changed must
    // be remeshed. Remeshing EVERY chunk overlapping the box wastes most of the
    // work — a typical edit only perturbs light in a small neighbourhood. So
    // snapshot each field's box, relight, then mark only the chunks that actually
    // changed (plus the edited chunk and any face-adjacent chunk across a shared
    // boundary, whose geometry/culling changed regardless).
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

    // Relight one field over its own Y-box, then mark every chunk whose baked light
    // changed. The Y extent differs per field: block light is isotropic (≤15 in
    // every axis incl. Y), so it clamps to wy±16; sky descends a column, so its box
    // spans the full height for now (step 2 clamps it to the affected band).
    auto relightAndMark = [&](std::vector<uint8_t>& field, bool emitter, int y0, int y1) {
        const int bw = x1 - x0 + 1, bh = y1 - y0 + 1, bd = z1 - z0 + 1;
        std::vector<uint8_t> old(static_cast<size_t>(bw) * bh * bd);
        auto bIdx = [&](int x, int y, int z) {
            return static_cast<size_t>((x - x0) + bw * ((y - y0) + bh * (z - z0)));
        };
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    old[bIdx(x, y, z)] = field[static_cast<size_t>(lightIndex(x, y, z))];
        relightField(field, emitter, x0, x1, y0, y1, z0, z1);
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (field[static_cast<size_t>(lightIndex(x, y, z))] != old[bIdx(x, y, z)])
                        mark(floordiv(x, N), floordiv(y, N), floordiv(z, N));
    };

    // Sky light travels DOWN a column losslessly and its value at any cell depends on
    // the SURROUNDING terrain height, so a small wy±radius box is wrong over rolling
    // terrain: a neighbour hill taller than wy+radius (whose shadow reaches into the
    // box) or a deep open shaft below the edit both fall outside it — the classic
    // box-too-small relight bug. Use the SAME bounds the recenter path (relightBox)
    // proved correct: anchor the box BOTTOM at the window floor, and cap the TOP just
    // above the highest sky-blocker / sub-15 cell anywhere in the box (a cheap top-
    // down per-column scan that early-outs at the surface, so the open sky above costs
    // little). Over flat terrain this is the few-chunks-tall box as before; over
    // hills/shafts it grows to stay byte-identical to a full computeSkyLight.
    int skyTop = o.y;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            for (int y = o.y + size.y - 1; y > skyTop; --y) {
                if (skyLightAt(x, y, z) < 15 ||
                    registry_.lightOpacity(blockAt(x, y, z).id) > 0) { skyTop = y; break; }
            }
    const int skyY0 = o.y;
    const int skyY1 = std::min(o.y + size.y - 1, skyTop + kLightRadius);
    const int blkY0 = std::max(o.y, wy - kLightRadius);
    const int blkY1 = std::min(o.y + size.y - 1, wy + kLightRadius);
    relightAndMark(skyLight_, false, skyY0, skyY1); // sky: full-depth, terrain-bounded top
    relightAndMark(blockLight_, true, blkY0, blkY1); // block: ±16 around the edit

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
    relightField(skyLight_, false, x0, x1, 0, sy - 1, z0, z1);
    relightField(blockLight_, true, x0, x1, 0, sy - 1, z0, z1);

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
    constexpr int margin = N; // one past light's 15-block reach
    const glm::ivec3 s = sizeInBlocks();
    const int sy = s.y; // originChunk_.y is always 0 (no vertical streaming)
    const int bw = x1 - x0 + 1, bd = z1 - z0 + 1;

    // Height-bound the SKY relight to the band that can actually change, instead of
    // the full world column. Above the highest cell that is either (a) a sky-blocker
    // in the freshly generated terrain, or (b) still carrying stale sub-15 sky from
    // the column that last occupied this ring slot, every cell is already open-sky 15
    // in BOTH the old and the new contents — so relighting it is a guaranteed no-op.
    // A cheap top-down per-column scan finds that ceiling (it early-outs at the
    // surface, reading only the empty sky above), letting the clear/descent/flood skip
    // the air that dominates a 256-tall column. This is the recenter analogue of
    // setBlock's clamped sky box; UNLIKE setBlock, the entering column's light is
    // stale (belongs to the departing column), hence the extra "stale sub-15" term —
    // dropping it is exactly the box-too-small bug that broke floating-island relight.
    // Block light stays full height: its flood is already empty when there are no
    // emitters, so the win there is not worth a second, non-early-out scan.
    int skyTop = 0;
    for (int z = z0; z <= z1; ++z) {
        for (int x = x0; x <= x1; ++x) {
            for (int y = sy - 1; y > skyTop; --y) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                if (skyLight_[li] < 15 ||
                    registry_.lightOpacity(blockAt(x, y, z).id) > 0) {
                    skyTop = y;
                    break;
                }
            }
        }
    }
    // skyRelightFullHeight_ is a test/verify hook (default off): forcing the full
    // column lets a logictest prove the bounded box is byte-identical to full height.
    const int skyY1 = skyRelightFullHeight_ ? (sy - 1) : std::min(sy - 1, skyTop + margin);

    // Snapshot each field over its own Y-box, relight, then diff old-vs-new to remesh
    // only the chunks whose baked light changed (same approach as setBlock). The two
    // fields use different Y extents, so each keeps its own snapshot buffer.
    std::vector<uint8_t> oldSky(static_cast<size_t>(bw) * (skyY1 + 1) * bd);
    std::vector<uint8_t> oldBlk(static_cast<size_t>(bw) * sy * bd);
    auto skyI = [&](int x, int y, int z) {
        return static_cast<size_t>((x - x0) + bw * (y + (skyY1 + 1) * (z - z0)));
    };
    auto blkI = [&](int x, int y, int z) {
        return static_cast<size_t>((x - x0) + bw * (y + sy * (z - z0)));
    };
    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                if (y <= skyY1) oldSky[skyI(x, y, z)] = skyLight_[li];
                oldBlk[blkI(x, y, z)] = blockLight_[li];
            }
        }
    }

    // Sky and block light are independent fields (separate arrays; both only READ
    // the chunks), so relight them concurrently when threading is enabled.
    if (config_.streamWorkers > 0) {
        std::future<void> sky = std::async(std::launch::async, [&] {
            relightField(skyLight_, false, x0, x1, 0, skyY1, z0, z1);
        });
        relightField(blockLight_, true, x0, x1, 0, sy - 1, z0, z1);
        sky.get();
    } else {
        relightField(skyLight_, false, x0, x1, 0, skyY1, z0, z1);
        relightField(blockLight_, true, x0, x1, 0, sy - 1, z0, z1);
    }

    const int cx0 = floordiv(x0, N), cx1 = floordiv(x1, N);
    const int cz0 = floordiv(z0, N), cz1 = floordiv(z1, N);
    const int ncx = cx1 - cx0 + 1, ncz = cz1 - cz0 + 1;
    std::vector<char> grid(static_cast<size_t>(ncx) * counts_.y * ncz, 0);
    auto markChanged = [&](int x, int y, int z) {
        const int gcx = floordiv(x, N) - cx0, gcy = floordiv(y, N), gcz = floordiv(z, N) - cz0;
        grid[static_cast<size_t>(gcx + ncx * (gcy + counts_.y * gcz))] = 1;
    };
    for (int z = z0; z <= z1; ++z) {
        for (int y = 0; y < sy; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const size_t li = static_cast<size_t>(lightIndex(x, y, z));
                if ((y <= skyY1 && skyLight_[li] != oldSky[skyI(x, y, z)]) ||
                    blockLight_[li] != oldBlk[blkI(x, y, z)]) {
                    markChanged(x, y, z);
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

World::PregenStrip World::pregenStrip(int dir, bool alongX, int fromX, int fromZ) const {
    PregenStrip s;
    s.dir    = dir;
    s.alongX = alongX;
    // The strip steps from the VIRTUAL origin (fromX,fromZ): the window origin this
    // strip will be applied at. For the next column step that is the current origin;
    // for a strip staged 2+ columns ahead it is the origin after the earlier queued
    // strips apply (App stages a small queue so the window can advance several columns
    // per second without the per-column pregen landing on the critical path).
    s.origin = {fromX, fromZ};

    // The entering edge column AFTER the one-column step this strip is for —
    // mirrors shiftColumn's edge computation (which runs post-move, so +dir here).
    int edge;
    if (alongX) {
        edge = (dir > 0) ? fromX + dir + counts_.x - 1 : fromX + dir;
    } else {
        edge = (dir > 0) ? fromZ + dir + counts_.z - 1 : fromZ + dir;
    }
    // Same column order shiftColumn gathers, so the apply is a straight move.
    std::vector<glm::ivec2> columns;
    columns.reserve(static_cast<size_t>(alongX ? counts_.z : counts_.x));
    if (alongX) {
        for (int cz = fromZ; cz < fromZ + counts_.z; ++cz) {
            columns.push_back({edge, cz});
        }
    } else {
        for (int cx = fromX; cx < fromX + counts_.x; ++cx) {
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
