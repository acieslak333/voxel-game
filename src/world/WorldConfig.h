#pragma once

#include <cstdint>
#include <string>

namespace vg {

// -----------------------------------------------------------------------------
//  WorldConfig
// -----------------------------------------------------------------------------
//  World-size, seed, and terrain-shaping knobs, loaded from assets/world.yaml.
//  These were previously magic numbers in World.cpp / App.h; per the project
//  convention (docs/CONFIGURATION.md) they now live in documented data. Each
//  field's meaning, units, and effect are described in assets/world.yaml; the
//  defaults below match the values shipped in that file so a missing/!malformed
//  file still produces the same world.
//
//  Authored data, not player settings: unlike vg::Settings this is only read,
//  never written back by the game.
// -----------------------------------------------------------------------------
struct WorldConfig {
    // --- World size & seed -----------------------------------------------------
    std::uint32_t seed    = 1337u; // procedural seed (same seed => same world)

    // Dynamic chunk streaming (docs/STREAMING.md). When true the loaded window
    // follows the player (infinite world, island shaping disabled). When false the
    // world is the fixed (2*viewRadius + 1) square below, generated once — the
    // original behaviour. Default OFF while streaming is stabilised.
    bool streaming = false;

    // Directory for persisted edits when streaming. Per-seed subfolders hold one
    // little file per edited chunk; unedited chunks regenerate from the seed and
    // are never written. Relative to the working directory.
    std::string saveDir = "saves";

    // Background threads that mesh streamed-in chunks (streaming only). 0 = mesh on
    // the main thread, amortised over frames (no worker threads). >0 offloads greedy
    // meshing to that many workers so the main thread only uploads finished meshes —
    // removes the residual streaming stutter. Set 0 if threading ever misbehaves.
    int streamWorkers = 4;

    // Relight the streamed-in edge on a background thread (streaming only). The
    // light flood is the last big per-boundary cost still on the main thread;
    // moving it off-thread removes the residual stutter. Generation + the window
    // move stay synchronous (so there are no window-origin races); the background
    // task only touches the just-generated edge chunks/light, disjoint from the
    // player area. EXPERIMENTAL — set false to relight synchronously on the main
    // thread (the proven path) if it ever misbehaves.
    bool asyncStreaming = false;

    // Streaming window: how many chunks are kept loaded in each horizontal
    // direction around the player (also the fixed world's half-size when streaming
    // is off). The world is a (2*viewRadius + 1) square of chunks of this radius.
    int viewRadius   = 8;
    // Vertical extent of the world, in chunks (there is no vertical streaming).
    int heightChunks = 3;

    // Chunk-grid dimensions World actually builds. DERIVED by load() from the two
    // values above (chunksX == chunksZ == 2*viewRadius + 1; chunksY == heightChunks);
    // do not set these in YAML.
    int chunksX = 17;
    int chunksY = 3;
    int chunksZ = 17;

    // --- Surface height noise --------------------------------------------------
    float heightFrequency = 0.006f; // lower => broader, smoother hills
    int   octaves         = 4;      // fbm octaves for the height field (detail)
    int   baseHeight      = 18;     // lowest surface height, in blocks
    int   heightAmplitude = 32;     // surface spans [baseHeight, baseHeight+amp]

    // --- Material / biome noise ------------------------------------------------
    float materialFrequency = 0.04f; // frequency of the rocky/dirt-depth variation
    int   materialOctaves   = 3;     // fbm octaves for the material field

    // --- Surface materials -----------------------------------------------------
    int   dirtDepthMin           = 3;     // thinnest dirt layer under grass
    int   dirtDepthMax           = 5;     // thickest dirt layer under grass
    int   rockyHeightMargin      = 3;     // within this many blocks of the peak => bare stone
    float rockyMaterialThreshold = 0.45f; // material noise above this => bare stone surface
    int   beachHeightMargin      = 2;     // surfaces <= baseHeight+this become sand (beaches)
    float terrainWarp            = 14.0f; // domain-warp the height field (blocks) => swirly hills

    // --- Island shaping (temporary; revisited when streaming lands) ------------
    // Terrain only rises above sea level (baseHeight) where a radial mask is high,
    // so the world is one island ringed by flat sandy sea-floor. Distances are
    // normalized: 0 at the world centre, ~1 at the edge mid-points.
    float islandFalloffStart = 0.55f; // land is full height inside this radius
    float islandFalloffEnd   = 0.95f; // ...and has sunk to sea level by this radius
    float coastWarp          = 0.10f; // wiggle added to the coastline (fraction of radius)

    // --- Whimsical scatter features (per-column probabilities) -----------------
    float lanternDensity = 0.004f; // glowing lantern-tree (oak-log stalk + glowstone cap)
    float cairnDensity   = 0.003f; // little stacked cobblestone cairn (a trail marker)
    float geodeDensity   = 0.006f; // glowstone geode buried in the stone (glows when dug)
    float treeDensity    = 0.02f;  // oak tree (thin trunk + crossed-quad leaf canopy), grass only
    float bushDensity     = 0.02f; // single-cell crossed-quad shrub on grass

    // --- Caves -----------------------------------------------------------------
    // Tunnels are carved through the stone where two 3D-noise fields are both near
    // zero (their intersection is a 1D-ish curve), so they wind rather than blob.
    float caveFrequency = 0.035f; // spatial frequency of the cave noise (higher => tighter, twistier)
    float caveThreshold = 0.085f; // |noise| band that counts as hollow (bigger => wider/more caves)
    int   caveFloor     = 2;      // never carve at or below this world Y (keeps a solid base)
    // Large caverns: a low-frequency 3D blob carved only below cavernMaxY (deep), so
    // big open rooms form down low. Higher threshold => rarer/smaller caverns.
    float cavernThreshold = 0.52f;
    int   cavernMaxY      = 40;

    // --- Ores ------------------------------------------------------------------
    // Each ore replaces stone in small clusters (a roll shared across a 2x2x2 cell)
    // up to its max world-Y, so rarer ores sit deeper. Checked rarest-first.
    float coalDensity    = 0.020f;  int coalMaxY    = 56;
    float ironDensity    = 0.013f;  int ironMaxY    = 44;
    float goldDensity    = 0.006f;  int goldMaxY    = 28;
    float rubyDensity    = 0.0035f; int rubyMaxY    = 18;
    float emeraldDensity = 0.0030f; int emeraldMaxY = 16;
    float mythrilDensity = 0.0016f; int mythrilMaxY = 9;

    // --- Lighting --------------------------------------------------------------
    // Light lost per block while *spreading* (the BFS flood), per source. Levels
    // run 0..15, so reach ~= 15 / falloff blocks. Sky columns open to the sky are
    // always 15 regardless — falloff only governs how far light leaks sideways
    // into caves/overhangs (sky) or radiates from an emitter (block).
    //
    // Player-facing: NOT read from world.yaml. Driven by vg::Settings
    // ("skyFalloff"/"blockFalloff" in settings.yaml, adjustable in the Esc menu)
    // via World::setLightFalloff; these are the pre-settings defaults.
    int skyFalloff   = 2; // higher => caves go dark faster
    int blockFalloff = 1; // higher => smaller glow radius around emitters

    // Load from `path`; any missing field (or a missing/unreadable file) keeps
    // its default above, so the game still runs with a sensible world.
    [[nodiscard]] static WorldConfig load(const std::string& path);
};

} // namespace vg
