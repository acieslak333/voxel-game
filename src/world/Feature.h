#pragma once

/**
 * @file Feature.h
 * @brief Data-driven procedural scatter objects stamped into the world during generation.
 *
 * A Feature is a list of geometric ops (box/sphere/cylinder/cone/etc.) whose sizes and
 * block palettes may be randomised per-instance. Every random value is derived from
 * hash(origin, seed, op-salt), making each instance a pure function of its origin cell
 * and the world seed — seam-safe for chunk streaming. Authored as YAML under
 * assets/features/ and edited with tools/feature_tool.py.
 * @note Feature::at() is a pure function of (originSeed, local coords, world coords).
 * @see docs/CODE_INDEX.md
 */

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

/**
 * @brief A scalar that is either fixed or randomised per-instance from a hash.
 *
 * fixed=true -> always returns `a`. fixed=false -> linearly interpolates [a,b]
 * using hash01(h), where `h` is derived from the instance origin and op salt.
 */
struct RandF {
    float a = 0.0f, b = 0.0f;
    bool  fixed = true;
    [[nodiscard]] float atf(uint32_t h) const;  // fixed -> a, else lerp(a,b,hash01(h))
    [[nodiscard]] int   ati(uint32_t h) const;  // ...rounded to an int
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
/**
 * @brief Loaded collection of all Feature templates under assets/features/.
 *
 * Constructed once at startup; each Feature template is placed explicitly by the
 * worldgen code. Automatic scatter/spawn logic was removed with the worldgen
 * overhaul; only the voxel-op data (size/anchor/ops) is retained.
 */
class FeatureSet {
public:
    // Load every *.yaml under `dir` as feature templates (missing dir -> empty).
    // Block names resolve via the registry. Spawn/scatter logic was removed with the
    // worldgen overhaul: this just loads the voxel-op data so a feature can be placed
    // explicitly later. Block names resolve via the registry.
    FeatureSet(const std::string& dir, const BlockRegistry& registry);

    [[nodiscard]] bool empty() const { return features_.empty(); }
    [[nodiscard]] const std::vector<Feature>& all() const { return features_; }

private:
    std::vector<Feature> features_;
};

} // namespace vg
