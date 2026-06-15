#pragma once

#include "world/Noise.h"
#include "world/NoiseMask.h"
#include "world/NoiseStack.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
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

// Which tree species a biome grows (its `tree:` key). The far-terrain LOD renderer
// reads the ROOT column's species and shapes the impostor accordingly (oak/birch =
// rounded, pine = conical). Oak is the default.
enum class TreeKind : uint8_t { Oak, Birch, Pine };

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
    // Weighted surface/filler palettes (like a feature's block list). One entry = a
    // plain single block; multiple = a per-column weighted mix (speckled surfaces).
    std::vector<std::pair<uint16_t, float>> top;     // surface blocks (grass/sand/…)
    std::vector<std::pair<uint16_t, float>> filler;  // sub-surface blocks (dirt/sand/…)
    bool   snow         = false;  // surface is snow regardless of altitude
    float  treeDensity  = 0.0f;   // per-column tree probability (far-terrain LOD impostors)
    TreeKind  tree       = TreeKind::Oak;   // which tree species this biome grows
    glm::vec3 vegTint{1.0f, 1.0f, 1.0f};    // multiplies grass/leaf albedo (white = none)
    // Per-biome overrides of the global terrain3d knobs. Each is a sentinel meaning
    // "inherit the global value" (negative for the floats, -1 for the tri-state ints),
    // so a biome that authors none leaves the world byte-identical. The scalar ones
    // blend across biome borders (biomeBlendCells_); see biomeParamsAt.
    float  amp3d         = -1.0f; // terrain3d.amplitude   (<0 = inherit)
    int    en3d          = -1;    // terrain3d.enabled      (-1 inherit, 0 off, 1 on)
    float  floatThresh   = -1.0f; // terrain3d.float_threshold (<0 = inherit)
    int    floatReach    = -1;    // terrain3d.float_reach  (<0 = inherit)
    int    coastOn       = -1;    // terrain3d.coast_flatten.enabled (-1 inherit, 0/1)
    float  coastRange    = -1.0f; // terrain3d.coast_flatten.range (<0 = inherit)
    float  coastMin      = -1.0f; // terrain3d.coast_flatten.min   (<0 = inherit)
    // Per-biome 3D density noise stack (terrain3d.density.layers under the biome). When
    // non-empty it replaces the global density blend for columns of this biome — each
    // biome can author its own unlimited-length layer stack. Empty = inherit global.
    NoiseStack densityStack;
    bool   hasDensityStack = false;
    // Noise-driven surface block patches (`surface_masks:` under the biome): each is a
    // block + a NoiseMask; the first whose weight passes overrides the surface block for
    // that column (stone outcrops through grass, mossy bands, flower fields…). Tested in
    // order; empty = the plain palette pick. Dry land only (water/snow still override).
    struct BlockMask { uint16_t id = 0; NoiseMask mask; };
    std::vector<BlockMask> surfaceMasks;
    // Biome SELECTION mask (`mask:` under the biome). With climate gone, this is how a
    // biome varies WITHIN its elevation band: the biome is only chosen where its mask
    // passes (weight >= 0.5), so an earlier-listed biome with a mask (e.g. birch patches)
    // carves out of a later catch-all (oak). Empty = matches the whole band.
    NoiseMask selMask;
};

// The terrain3d/coast knobs resolved (and border-blended) for one column — either the
// global values, or a biome's overrides where it authors them. Booleans are blended as
// a 0/1 average then thresholded at the border. Built by biomeParamsAt(); the no-override
// fast path returns the globals so the default world stays byte-identical.
struct BiomeParams {
    float amp;          // densityAmp_ (or per-biome)
    bool  en3d;         // 3D volumetric terrain on for this column
    float floatThresh;
    int   floatReach;
    bool  coastOn;
    float coastRange;
    float coastMin;
};

// What generateColumn needs to know about a single (x, z) column.
struct ColumnInfo {
    int      height     = 0; // surface block Y
    int      waterLevel = 0; // fill water up to this Y (sea level)
    int      biome      = 0; // index into the biome table (for debug / future use)
    uint16_t topId      = 0; // resolved surface block
    uint16_t fillerId   = 0; // resolved sub-surface block
    float    treeDensity = 0.0f;            // far-terrain LOD tree probability
    TreeKind  treeKind   = TreeKind::Oak;   // resolved tree species for this column
    glm::vec3 vegTint{1.0f, 1.0f, 1.0f};    // biome vegetation tint for this column
};

// -----------------------------------------------------------------------------
//  TerrainGenerator
// -----------------------------------------------------------------------------
//  The data-driven worldgen backbone (docs/WORLDGEN.md). One concentric island:
//  the shape is a radial rise from the coast to ridged peaks at the core, and the
//  climate (temperature/humidity) plus elevation select a surface biome (beach →
//  forest → highlands → peaks). Everything is a pure function of world coordinates,
//  so it is deterministic and streaming-safe. Knobs load from assets/world.yaml
//  `generation:` and assets/biomes.yaml; sensible defaults are baked in.
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
    // the 3D density below (overhangs perturb this base).
    [[nodiscard]] int height(int wx, int wz) const;

    // --- 3D volumetric terrain ---------------------------------------------------
    // Is the voxel (wx,wy,wz) solid? `surfaceH` is the column's heightmap height.
    // Solid where the heightmap gradient (surfaceH - y) plus a 3D weighted-noise
    // perturbation is positive — giving overhangs/cliffs. A pure function of
    // seed+coords, so streaming-safe. Falls back to a pure heightmap when 3D is off.
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
    // derives BOTH the solid mask and the ground surface from that single pass.
    [[nodiscard]] bool mainTerrainSolid(int surfaceH, int wx, int wy, int wz) const;
    [[nodiscard]] bool floatSolid(int surfaceH, int wx, int wy, int wz) const;

    // The CONTINUOUS main-terrain solidity scalar at a world cell: > 0 inside the
    // land, < 0 in air, ~0 at the surface — its sign matches mainTerrainSolid()
    // everywhere (floating islands excluded). A pure function of (seed, coords).
    [[nodiscard]] float terrainDensity(int wx, int wy, int wz) const;
    // Highest Y surfaceY() would probe for a column of heightmap height `surfaceH`.
    [[nodiscard]] int surfaceScanTop(int surfaceH) const;

    // Full surface description for a column (height + biome + surface blocks +
    // feature densities).
    [[nodiscard]] ColumnInfo columnInfo(int wx, int wz) const;

    [[nodiscard]] int seaLevel() const { return seaLevel_; }
    [[nodiscard]] int worldHeight() const { return worldHeight_; }

    // DEBUG/preview only (headless genmap "solo biome" slice): force every column to
    // resolve to biome `i` — its surface blocks, per-biome terrain3d params and masks —
    // so the tool can show ONE biome generating in isolation. -1 = off (the
    // normal climate/elevation selection). Never set on the live game generator.
    void setForcedBiome(int i) { forcedBiome_ = i; }

    // Biome names in table order (the index ColumnInfo::biome refers to).
    [[nodiscard]] std::vector<std::string> biomeNames() const {
        std::vector<std::string> n;
        n.reserve(biomes_.size());
        for (const BiomeDef& b : biomes_) n.push_back(b.name);
        return n;
    }

    // --- Debug / tuning introspection (headless genmap only; NOT used by the
    //     generation path, so adding/calling this never changes world output). ---
    enum class Field {
        Continentalness, Erosion, Peaks, Temperature, Humidity, Relief, Height
    };
    [[nodiscard]] float fieldValue(Field f, int wx, int wz) const;

    // The densest biome tree probability — lets callers cheaply reject most columns
    // (hash gate) before paying for a full columnInfo() in the tree-scatter loop.
    [[nodiscard]] float maxTreeDensity() const { return maxTreeDensity_; }

private:
    void loadConfig(const std::string& assetDir, const BlockRegistry& registry);
    // Index of the biome a column resolves to: forcedBiome_ when set, else the first
    // biome (file order) whose ELEVATION band contains rel AND whose optional selection
    // mask passes at (wx,wz), else the last (catch-all). Climate (temperature/humidity)
    // was removed — biomes are pure elevation rings, varied by noise selection masks.
    // The single source of biome selection — columnInfo() routes through it too.
    [[nodiscard]] int selectBiomeIndex(int wx, int wz, int relHeight) const;
    [[nodiscard]] const BiomeDef& selectBiome(int wx, int wz, int relHeight) const;
    // The biome at a column, selected from the BASE heightmap (which 3D modulation never
    // moves, so no feedback loop). Shared by the per-biome param/stack resolvers.
    [[nodiscard]] const BiomeDef& selectBiomeAt(int wx, int wz) const;

    // Land height from the shape noises.
    [[nodiscard]] int shapeHeight(int wx, int wz) const;

    // Main-terrain (no floating islands) solidity, shared by isSolid() + surfaceY().
    [[nodiscard]] bool mainSolid(int surfaceH, int wx, int wy, int wz) const;

    // The 3D density perturbation at a world cell (the NoiseStack blend, or the
    // scalar fbm fallback) — the single most expensive eval in worldgen.
    [[nodiscard]] float rawDensity(float fx, float fy, float fz) const;
    // Approximate density via a coarse world-aligned lattice (REVIEW O6): sample
    // rawDensity only at lattice corners (cached per worker) and trilinearly
    // interpolate. Opt-in (densityInterp_); a pure fn of (seed, coords) either way.
    [[nodiscard]] float densityInterpolated(float fx, float fy, float fz) const;
    // A single lattice-corner density, memoised in a per-thread cache (pure
    // memoisation — output stays deterministic and worker order can't change it).
    [[nodiscard]] float densityCorner(int lx, int ly, int lz) const;

    int worldHeight_;
    int seaLevel_   = 64;
    int snowLineRel_ = 55; // blocks above sea level where any surface gets a snow cap

    // Noise fields (each seeded independently off the world seed).
    // coastNoise_ is a generic shaping field for the island coastline warp (it once
    // also drove the now-removed river carving, hence its independent seed).
    Noise contNoise_, eroNoise_, peakNoise_, tempNoise_, humNoise_, coastNoise_;
    float contFreq_ = 0.0016f, eroFreq_ = 0.0032f, peakFreq_ = 0.0065f;
    float tempFreq_ = 0.0009f, humFreq_ = 0.0012f;
    int   contOct_ = 4, eroOct_ = 3, peakOct_ = 4, climOct_ = 2;

    // Optional data-driven NoiseStack overrides (assets/biomes.yaml `<field>.layers:`).
    // When a field declares `layers:`, its single-fbm sample is replaced by the
    // weighted blend; absent → the scalar path above runs.
    NoiseStack contStack_, eroStack_, peakStack_, tempStack_, humStack_;

    // --- 3D volumetric density (overhangs / cliffs) ------------------------------
    // The main density noise perturbs the heightmap gradient to carve overhangs and
    // cliffs; the float noise sparsely adds solid blobs high above the surface.
    bool  density3DEnabled_ = true;
    Noise densityNoise_, floatNoise_;
    NoiseStack densityStack_, floatStack_;
    float densityFreqXZ_ = 0.011f;
    float densityFreqY_  = 0.050f;
    int   densityOct_    = 4;
    float densityAmp_    = 28.0f;  // overhang/cliff/swell intensity, in blocks
    // Coast flatten (`terrain3d.coast_flatten`): scale the 3D swell DOWN near sea level
    // so the shoreline/beach is smooth, while inland keeps full cliffs. ampScale ramps
    // from `coastFlattenMin_` at the waterline to 1.0 by `coastFlattenRange_` blocks
    // above/below sea. Off by default.
    bool  coastFlatten_      = false;
    float coastFlattenRange_ = 12.0f;
    float coastFlattenMin_   = 0.2f;
    float floatFreq_     = 0.0058f;
    float floatThresh_   = 0.40f;
    int   floatGap_      = 6;      // min blocks above the surface a float isle can start
    int   floatReach_    = 64;     // how far above the surface float isles can appear

    // Per-biome 3D-amplitude modulation (Option C, docs/WORLDGEN.md). A biome's
    // `terrain3d.amplitude` overrides the global swell only where that biome is,
    // blended over `biomeBlendCells_` blocks so borders don't cliff. anyAmpOverride_
    // false → the global densityAmp_ path runs unchanged (default world byte-identical).
    // maxAmp_ (global + all overrides) bounds the surface scans so a taller biome isn't
    // clipped.
    int   biomeBlendCells_ = 16;
    bool  anyAmpOverride_  = false;  // any biome overrides a blended terrain3d/coast scalar
    float maxAmp_          = 28.0f;
    int   maxFloatReach_   = 64;     // max float_reach over global + all biomes (scan bound)
    // Cheaper-path gate: the per-biome density-stack override lives on hot code that is
    // byte-identical when no biome authors it, so it is guarded by its own flag.
    bool  anyDensityStackOverride_ = false;

    // Coarse-lattice density interpolation (REVIEW O6), opt-in via
    // `terrain3d.interpolate`. World-aligned lattice (seam-consistent), per-thread
    // memoised corner samples; densityEpoch_ disambiguates the cache across instances.
    bool  densityInterp_ = false;
    int   latX_ = 4, latY_ = 8, latZ_ = 4;
    uint32_t densityEpoch_ = 0;

    // Sample a noise field: the stack blend if one was authored, else the scalar fbm.
    [[nodiscard]] float sampleCont(float x, float z) const;
    [[nodiscard]] float sampleEro(float x, float z) const;
    [[nodiscard]] float samplePeak(float x, float z) const;
    [[nodiscard]] float sampleTemp(float x, float z) const;
    [[nodiscard]] float sampleHum(float x, float z) const;
    // Per-biome terrain3d/coast knobs: resolve them for the single biome at a column
    // (no blend), then biomeParamsAt blends a 3x3 box over biomeBlendCells_ and caches
    // the result per column. The no-override fast path returns the globals unchanged, so
    // the default world is byte-identical. See docs/WORLDGEN.md (Option C).
    [[nodiscard]] BiomeParams globalParams() const;   // the no-override defaults
    [[nodiscard]] BiomeParams biomeParamsRaw(int wx, int wz) const;
    [[nodiscard]] BiomeParams biomeParamsAt(int wx, int wz) const;
    [[nodiscard]] float densityAmpAt(int wx, int wz) const; // == biomeParamsAt(..).amp
    // The 3D density noise stack to use for a column: a biome's own terrain3d.density
    // override if it authored one, else the global densityStack_ (nullptr = global fbm
    // fallback / global stack). Cached per column; only consulted when an override exists.
    [[nodiscard]] const NoiseStack* densityStackAt(int wx, int wz) const;

    uint32_t seed_ = 0;

    // Single-island mode: a radial mask centred at (islandCx,islandCz) makes ONE
    // landmass that sinks to deep ocean beyond islandRadius_, rising to a central
    // peak plateau (islandPeakHeight_) so the elevation biome bands read as rings.
    bool  islandEnabled_   = false;
    float islandCx_ = 0.0f, islandCz_ = 0.0f;
    float islandRadius_    = 900.0f; // distance at which land has fully sunk to ocean
    float islandInner_     = 0.5f;   // fraction of the radius that's full-height land
    float islandCoastWarp_ = 180.0f; // blocks of noise warp on the coastline (irregular)
    float islandLandBase_  = 8.0f;   // base interior land elevation (blocks above sea)
    float islandPeakHeight_ = 0.0f;  // smooth radial rise toward the CORE (blocks): the land
                                     // climbs from the coast to a central peak plateau, so the
                                     // elevation biome bands read as concentric rings.
    float islandInteriorVar_ = 7.0f; // continentalness -> interior hills/valleys amplitude
    float islandDeepOcean_ = -46.0f; // sea-floor elevation far out at sea (blocks vs sea)

    // Shape splines: continentalness → base elevation; erosion → mountain amplitude
    // (scaled by the ridged peaks noise).
    Spline contSpline_, eroSpline_;
    // Island radial PROFILE (`island_profile_spline`): x = normalised distance from the
    // core (0 = centre → 1 = coast/ocean), y = the rise multiplier (1 = full peak_height,
    // 0 = sunk to ocean_floor). When authored it REPLACES the built-in smoothstep + inner
    // mechanism, so you can draw the coast→core slope (cliffs, shelves, a ledge per ring).
    // Empty (default) → the smoothstep path runs unchanged (byte-identical).
    Spline profileSpline_;

    std::vector<BiomeDef> biomes_;
    int   forcedBiome_ = -1;      // >=0 -> preview "solo biome" mode (see setForcedBiome)
    float maxTreeDensity_ = 0.0f; // max treeDensity over all biomes (tree-loop gate)

    // Block ids used for submerged surfaces (ocean/lake floor), resolved once.
    uint16_t oceanFloorId_ = 0, oceanFillerId_ = 0, snowId_ = 0;
};

} // namespace vg
