#pragma once

#include "world/Noise.h"
#include "world/NoiseStack.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vg {

class BlockRegistry;

// -----------------------------------------------------------------------------
//  Spline — a piecewise-linear curve through sorted (x, y) control points.
//  Clamps to the end values outside the authored range. This is the tunable knob
//  the terrain shape is built from: continentalness/erosion → elevation curves.
// -----------------------------------------------------------------------------
struct Spline {
    std::vector<float> xs, ys;
    void add(float x, float y) { xs.push_back(x); ys.push_back(y); }
    [[nodiscard]] bool empty() const { return xs.empty(); }
    [[nodiscard]] float at(float x) const;
};

// Which family of ground plants a biome scatters (the biome's `plant:` theme).
// World::generateColumn maps the theme + a per-column variety hash to an actual
// plant block, so one theme yields a mix (e.g. GrassFlower = mostly tall grass
// with the odd flower). None = no ground plants (beaches, bare mountains, ocean).
enum class FloraKind : uint8_t { None, Bush, GrassFlower, Forest, Desert };

// Which tree species a biome grows (its `tree:` key). World::generateColumn reads
// the ROOT column's species and shapes the canopy accordingly (oak/birch/maple =
// rounded, pine = conical, willow = drooping). Oak is the default.
enum class TreeKind : uint8_t { Oak, Birch, Pine, Maple, Willow };

// -----------------------------------------------------------------------------
//  BiomeDef — one biome, loaded from assets/biomes.yaml. A biome is selected for
//  a column when the column's climate (temperature, humidity) and elevation (in
//  blocks relative to sea level) all fall inside this biome's ranges. Biomes are
//  tested in file order; the first match wins, so author specific biomes before a
//  catch-all. Only the *surface treatment* is biome-driven (the terrain shape is
//  the climate-independent spline pipeline), so biome borders never cliff.
// -----------------------------------------------------------------------------
struct BiomeDef {
    std::string name;
    float tempMin = -2.0f, tempMax = 2.0f; // climate match window (noise ~[-1,1])
    float humMin  = -2.0f, humMax  = 2.0f;
    int   relMin  = -10000, relMax = 10000; // elevation match, in blocks vs sea level
    uint16_t topId    = 0;  // surface block (grass/sand/snow/stone…)
    uint16_t fillerId = 0;  // a few blocks under the surface (dirt/sand/stone)
    bool   snow         = false;  // surface is snow regardless of altitude
    float  treeDensity  = 0.0f;   // per-column probability a tree roots here
    float  bushDensity   = 0.0f;  // per-column probability of a ground plant
    FloraKind plant      = FloraKind::Bush; // which plant family scatters here
    TreeKind  tree       = TreeKind::Oak;   // which tree species roots here
    glm::vec3 vegTint{1.0f, 1.0f, 1.0f};    // multiplies grass/leaf albedo (white = none)
};

// What generateColumn needs to know about a single (x, z) column.
struct ColumnInfo {
    int      height     = 0; // surface block Y (after river/lake carving)
    int      waterLevel = 0; // fill water up to this Y (sea level, or a perched lake's level)
    int      biome      = 0; // index into the biome table (for debug / future use)
    uint16_t topId      = 0; // resolved surface block
    uint16_t fillerId   = 0; // resolved sub-surface block
    float    treeDensity = 0.0f;
    float    bushDensity = 0.0f;
    FloraKind plantKind  = FloraKind::None; // resolved plant family for this column
    TreeKind  treeKind   = TreeKind::Oak;   // resolved tree species for this column
    glm::vec3 vegTint{1.0f, 1.0f, 1.0f};    // biome vegetation tint for this column
};

// -----------------------------------------------------------------------------
//  TerrainGenerator
// -----------------------------------------------------------------------------
//  The data-driven worldgen backbone (docs/WORLDGEN.md). Separates the terrain
//  *shape* from the *climate*:
//    * shape   — continentalness / erosion / peaks noises mapped through tunable
//                splines give the surface height (oceans → coasts → plains →
//                mountains), independent of biome, so borders never cliff.
//    * climate — temperature / humidity noises select a biome for the surface
//                treatment (which blocks, snow, trees).
//  Everything is a pure function of world coordinates, so it is deterministic and
//  streaming-safe. All knobs load from assets/world.yaml `generation:` and
//  assets/biomes.yaml; sensible defaults are baked in so the game still runs if
//  those are missing. This is the seam a future generation-tuning tool edits.
// -----------------------------------------------------------------------------
class TerrainGenerator {
public:
    // assetDir holds world.yaml (its `generation:` block) and biomes.yaml. The
    // registry resolves biome block names to ids. worldHeight is the vertical
    // extent in blocks (counts.y * chunk size).
    TerrainGenerator(uint32_t seed, const BlockRegistry& registry,
                     const std::string& assetDir, int worldHeight);

    // Surface block Y at a world column (clamped to [1, worldHeight-1]). This is the
    // heightmap "base surface" — biome/water/LOD use it; the actual voxels come from
    // the 3D density below (overhangs/floating bits perturb this base).
    [[nodiscard]] int height(int wx, int wz) const;

    // --- Worldgen v2: 3D volumetric terrain --------------------------------------
    // Is the voxel (wx,wy,wz) solid? `surfaceH` is the column's heightmap height
    // (height()/columnInfo().height). Solid where the heightmap gradient
    // (surfaceH - y) plus a 3D weighted-noise perturbation is positive — giving
    // overhangs, cliffs and (sparsely, far above the surface) floating islands.
    // A pure function of seed+coords, so streaming-safe. Falls back to a pure
    // heightmap (y <= surfaceH) when 3D terrain is disabled.
    [[nodiscard]] bool isSolid(int surfaceH, int wx, int wy, int wz) const;
    // How far (blocks) terrain can poke ABOVE the heightmap surface: the column fill
    // must scan up to surfaceH + this for overhang/float tops. 0 when 3D is off.
    [[nodiscard]] int overhangReach() const;
    // The actual 3D GROUND surface Y (topmost main-terrain solid, ignoring floating
    // islands). Use this — NOT height() — to place features (trees/plants), since the
    // 3D density moves the real surface up/down from the heightmap by ±amplitude.
    [[nodiscard]] int surfaceY(int wx, int wz) const;

    // --- Split solidity tests (isSolid == mainTerrainSolid || floatSolid) --------
    // World::generateColumn evaluates the density band ONCE per column cell and
    // derives BOTH the solid mask and the ground surface from that single pass —
    // surfaceY() would re-evaluate the same cells a second time (the density stack
    // is the most expensive noise in worldgen). Same pure functions, same results.
    [[nodiscard]] bool mainTerrainSolid(int surfaceH, int wx, int wy, int wz) const;
    [[nodiscard]] bool floatSolid(int surfaceH, int wx, int wy, int wz) const;
    // Highest Y surfaceY() would probe for a column of heightmap height `surfaceH`:
    // the topmost main-terrain solid cell lies at or below this. The first
    // mainTerrainSolid cell scanning DOWN from here (to 1) IS surfaceY's result.
    [[nodiscard]] int surfaceScanTop(int surfaceH) const;

    // Full surface description for a column (height + biome + surface blocks +
    // feature densities).
    [[nodiscard]] ColumnInfo columnInfo(int wx, int wz) const;

    [[nodiscard]] int seaLevel() const { return seaLevel_; }
    [[nodiscard]] int worldHeight() const { return worldHeight_; }

    // Biome names in table order (the index ColumnInfo::biome refers to). Used to
    // resolve feature scatter biome allow-lists to indices.
    [[nodiscard]] std::vector<std::string> biomeNames() const {
        std::vector<std::string> n;
        n.reserve(biomes_.size());
        for (const BiomeDef& b : biomes_) n.push_back(b.name);
        return n;
    }

    // --- Debug / tuning introspection (headless genmap only; NOT used by the
    //     generation path, so adding/calling this never changes world output). ---
    //  Exposes each raw noise layer (and a couple of derived fields) so the map
    //  tool can visualise every layer that feeds the terrain, not just the result.
    //  Noise fields return ~[-1, 1]; Relief/Height return blocks (vs sea / absolute).
    enum class Field {
        Continentalness, Erosion, Peaks, Temperature, Humidity, River, Relief, Height
    };
    [[nodiscard]] float fieldValue(Field f, int wx, int wz) const;

    // The densest biome tree probability — lets callers cheaply reject most columns
    // (hash gate) before paying for a full columnInfo() in the tree-scatter loop.
    [[nodiscard]] float maxTreeDensity() const { return maxTreeDensity_; }

private:
    void loadConfig(const std::string& assetDir, const BlockRegistry& registry);
    [[nodiscard]] const BiomeDef& selectBiome(float temp, float hum, int relHeight) const;

    // Land height from the shape noises + river carving (no lakes — lakes sample
    // this to pick their level, so it must not recurse back into them).
    [[nodiscard]] int shapeHeight(int wx, int wz) const;

    // Main-terrain (no floating islands) solidity, shared by isSolid() + surfaceY().
    [[nodiscard]] bool mainSolid(int surfaceH, int wx, int wy, int wz) const;

    // A perched lake covering this column, if any. `level` = water surface Y,
    // `bed` = the carved terrain height for this column (the bowl).
    struct LakeInfo { bool in = false; int level = 0; int bed = 0; };
    [[nodiscard]] LakeInfo lakeAt(int wx, int wz) const;

    int worldHeight_;
    int seaLevel_   = 64;
    int snowLineRel_ = 55; // blocks above sea level where any surface gets a snow cap

    // Noise fields (each seeded independently off the world seed).
    Noise contNoise_, eroNoise_, peakNoise_, tempNoise_, humNoise_, riverNoise_;
    float contFreq_ = 0.0016f, eroFreq_ = 0.0032f, peakFreq_ = 0.0065f;
    float tempFreq_ = 0.0009f, humFreq_ = 0.0012f;
    int   contOct_ = 4, eroOct_ = 3, peakOct_ = 4, climOct_ = 2;

    // Optional data-driven NoiseStack overrides (assets/biomes.yaml `<field>.layers:`).
    // When a field declares `layers:`, its single-fbm sample is replaced by the
    // weighted blend. When absent the stack stays empty and the legacy scalar path
    // above runs unchanged — so adding this feature does NOT alter existing worlds
    // (the --selftest golden is stable until a `layers:` block is authored).
    NoiseStack contStack_, eroStack_, peakStack_, tempStack_, humStack_, riverStack_;

    // --- Worldgen v2: 3D volumetric density (overhangs / cliffs / floating isles) -
    // The main density noise perturbs the heightmap gradient to carve overhangs and
    // cliffs; the float noise sparsely adds solid blobs high above the surface.
    // Each is a weighted-sum NoiseStack when authored in biomes.yaml (`terrain3d:
    // density.layers` / `float.layers`), else a scalar fbm fallback — the user's
    // "sum of weighted noises so we get small and big changes" request.
    bool  density3DEnabled_ = true;
    Noise densityNoise_, floatNoise_;
    NoiseStack densityStack_, floatStack_;
    float densityFreqXZ_ = 0.011f; // horizontal noise frequency (wide features)
    float densityFreqY_  = 0.050f; // vertical: high enough that density folds +/- within
                                   // the band -> the surface is multi-valued = overhangs
    int   densityOct_    = 4;
    float densityAmp_    = 28.0f;  // overhang/cliff/swell intensity, in blocks (dramatic)
    float floatFreq_     = 0.0058f;// lower freq -> bigger, chunkier island masses
    float floatThresh_   = 0.40f;  // lower -> larger contiguous islands (still sparse,
                                   // since the low-freq peaks only clear it in patches)
    int   floatGap_      = 6;      // min blocks above the surface a float isle can start
    int   floatReach_    = 64;     // how far above the surface float isles can appear

    // Sample a noise field: the stack blend if one was authored, else the scalar fbm.
    // All take raw world coords (the per-layer frequency is applied inside the stack).
    [[nodiscard]] float sampleCont(float x, float z) const;
    [[nodiscard]] float sampleEro(float x, float z) const;
    [[nodiscard]] float samplePeak(float x, float z) const;
    [[nodiscard]] float sampleTemp(float x, float z) const;
    [[nodiscard]] float sampleHum(float x, float z) const;
    [[nodiscard]] float sampleRiver(float x, float z) const;

    // Rivers: a winding channel carved toward sea level where the river noise is
    // near zero, only in lowlands (so mountains don't get deep canals).
    uint32_t seed_ = 0;
    float riverFreq_  = 0.0021f;
    float riverWidth_ = 0.06f; // |noise| band counted as channel (wider = broader rivers)
    int   riverDepth_ = 7;     // how far below sea level the channel bed cuts
    int   riverMaxRel_ = 16;   // no rivers where land sits more than this above sea

    // Perched lakes: a coarse grid of candidate lakes; each carves a bowl filled to
    // a deterministic level (the land height at its centre), above sea level.
    int   lakeSpacing_  = 190; // average spacing between candidate lakes (blocks)
    float lakeChance_   = 0.45f;
    int   lakeRadiusMin_ = 11, lakeRadiusMax_ = 24;
    int   lakeDepth_    = 7;
    int   lakeMinRel_   = 3, lakeMaxRel_ = 30; // only on land in this elevation band

    // Single-island mode: a radial mask centred at (islandCx,islandCz) makes ONE
    // landmass that sinks to deep ocean beyond islandRadius_, with a noise-warped
    // irregular coastline. When off, continentalness drives land/ocean (archipelago).
    bool  islandEnabled_   = false;
    float islandCx_ = 0.0f, islandCz_ = 0.0f;
    float islandRadius_    = 900.0f; // distance at which land has fully sunk to ocean
    float islandInner_     = 0.5f;   // fraction of the radius that's full-height land
    float islandCoastWarp_ = 180.0f; // blocks of noise warp on the coastline (irregular)
    float islandLandBase_  = 8.0f;   // base interior land elevation (blocks above sea)
    float islandInteriorVar_ = 7.0f; // continentalness -> interior hills/valleys amplitude
    float islandDeepOcean_ = -46.0f; // sea-floor elevation far out at sea (blocks vs sea)

    // Shape splines: continentalness → base elevation; erosion → mountain
    // amplitude (scaled by the ridged peaks noise).
    Spline contSpline_, eroSpline_;

    std::vector<BiomeDef> biomes_;
    float maxTreeDensity_ = 0.0f; // max treeDensity over all biomes (tree-loop gate)

    // Block ids used for submerged surfaces (ocean/lake floor), resolved once.
    uint16_t oceanFloorId_ = 0, oceanFillerId_ = 0, snowId_ = 0;
};

} // namespace vg
