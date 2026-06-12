#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg {

class BlockRegistry;
class Noise;

// -----------------------------------------------------------------------------
//  Feature  (procedural scatter object)
// -----------------------------------------------------------------------------
//  A data-driven object stamped into the world during generation — the procedural
//  cousin of Structure. Where a Structure is a fixed hand-drawn voxel grid, a
//  Feature is a list of SHAPE OPS (box / sphere / cylinder / cone / column / line)
//  whose sizes, heights and block choices can be RANDOMIZED, plus noise/shell
//  fills. Authored as YAML under assets/features/ (see Feature.cpp for the format)
//  and edited with tools/feature_tool.py.
//
//  Seam-safety: a feature instance is a PURE FUNCTION of its origin cell + world
//  seed. Every random value is derived from hash(origin, seed, op-salt), so each
//  streamed chunk that overlaps the feature re-derives identical voxels — exactly
//  like Structure stamping and trees. blockAt() is that pure function.
// -----------------------------------------------------------------------------

// A randomizable scalar: a fixed value, or [a,b] resolved per-instance from a hash.
struct RandF {
    float a = 0.0f, b = 0.0f;
    bool  fixed = true;
    [[nodiscard]] float atf(uint32_t h) const;  // fixed -> a, else lerp(a,b,hash01(h))
    [[nodiscard]] int   ati(uint32_t h) const;  // ...rounded to an int
};

// Where a feature is scattered (its own grid, gates) — each feature owns this,
// unlike the single global structures density.
struct FeatureScatter {
    enum class Dist { Grid, Noise };

    float density = 0.2f;          // chance per candidate grid cell
    int   spacing = 48;            // grid cell size in blocks
    bool  surface = true;          // only root on dry land
    int   minElevation = -1000000; // origin column height vs sea level
    int   maxElevation =  1000000;
    std::vector<int> biomeIds;     // allow-list of biome indices (empty = any)

    // --- placement modifiers (atypical scatter) --------------------------------
    Dist  dist        = Dist::Grid; // Grid = even spread; Noise = organic clumps
    float noiseFreq   = 0.02f;      // Noise dist: lower = bigger clumps
    float noiseThresh = 0.30f;      // Noise dist: higher = rarer/tighter clumps
    int   minSlope    = 0;          // terrain steepness gate (max surface delta over
    int   maxSlope    = 100000;     //   a small neighbourhood, in blocks)
    bool  onWater     = false;      // root on the WATER surface (lilypads) not the land
    int   nearWater   = 0;          // 0 = off; else only within this many blocks of water
};

struct Feature {
    static constexpr uint16_t kSkip = 0xFFFF; // "leave the existing block"

    enum class Shape { Box, Sphere, Ellipsoid, Cylinder, Cone, Column, Line,
                       Torus, Arch, Spiral, Voxels };

    // Pack an op-local cell (each axis in [-128,127]) into a map key for Voxels ops.
    static uint32_t voxelKey(int x, int y, int z) {
        return (static_cast<uint32_t>(x + 128) << 16) |
               (static_cast<uint32_t>(y + 128) << 8) | static_cast<uint32_t>(z + 128);
    }
    enum class Fill  { Solid, Shell, Noise, Scatter };
    enum class Pick  { PerCell, PerInstance };
    enum class Place { Force, AirOnly };

    struct WBlock { uint16_t id = 0; float w = 1.0f; };

    struct Op {
        Shape shape = Shape::Box;
        glm::ivec3 at{0};                 // offset from the anchor (x,z centred; y up)
        RandF radius, height;             // sphere/cyl/cone radius; col/cyl/cone/line len;
                                          // torus/arch/spiral major radius; spiral height
        RandF sizeX, sizeY, sizeZ;        // box / ellipsoid extents
        RandF thickness;                  // torus/arch/spiral tube radius (default 1.5)
        float turns  = 2.0f;              // spiral: revolutions over its height
        float scatter = 0.5f;             // fill: scatter -> fraction of cells kept (0..1)
        std::vector<WBlock> blocks;       // weighted palette (>=1)
        float totalW = 0.0f;
        Pick  pick   = Pick::PerCell;
        Fill  fill   = Fill::Solid;
        float nFreq  = 0.2f, nThresh = 0.0f; // fill: noise
        Place place  = Place::Force;
        // Voxels op: hand-placed cells (op-local key -> block id, or 0xFFFE = use the
        // op's palette). Authored in the feature tool's sculpt mode.
        std::unordered_map<uint32_t, uint16_t> voxels;
        uint32_t salt = 0;                // decorrelates this op's randomness
    };

    // Result of evaluating a cell: a block id (or kSkip), and whether to overwrite.
    struct Cell { uint16_t id = kSkip; bool force = true; };

    std::string    name;
    FeatureScatter scatter;
    glm::ivec3     size{1};   // max bounding box (x,y,z) — gather reach + anchor default
    glm::ivec3     anchor{0}; // local cell meeting the surface (x,z centre; y = ground)
    std::vector<Op> ops;                   // the default form (variant 0)
    // Alternative forms: each is a full op-list. An instance picks one (ops = #0,
    // variants[i] = #i+1) by a per-origin hash, so e.g. one feature can be "3 distinct
    // trees" or several rubble shapes that vary across the world. Seam-safe.
    std::vector<std::vector<Op>> variants;

    // Block at local cell (lx,ly,lz) of the instance rooted with hash `originSeed`,
    // at world (wx,wy,wz) (for noise + per-cell rolls). kSkip = leave terrain.
    [[nodiscard]] Cell at(uint32_t originSeed, int lx, int ly, int lz,
                          const Noise& noise, int wx, int wy, int wz) const;

    [[nodiscard]] int index(int x, int y, int z) const {
        return (y * size.z + z) * size.x + x;
    }
};

// -----------------------------------------------------------------------------
//  FeatureSet — every feature template under assets/features/.
// -----------------------------------------------------------------------------
class FeatureSet {
public:
    // Load every *.yaml under `dir` (missing dir -> empty, features off). Block
    // names resolve via the registry; biome names via the supplied lookup.
    FeatureSet(const std::string& dir, const BlockRegistry& registry,
               const std::vector<std::string>& biomeNames);

    [[nodiscard]] bool empty() const { return features_.empty(); }
    [[nodiscard]] const std::vector<Feature>& all() const { return features_; }

private:
    std::vector<Feature> features_;
};

} // namespace vg
