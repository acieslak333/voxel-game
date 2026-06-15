#pragma once

#include "world/NoiseMask.h"

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

    // --- Streaming tuning (the `stream_tuning:` block in world.yaml) -----------
    // Per-machine performance knobs for the streaming pipeline. Defaults match the
    // values these replaced as inline constants (REVIEW R7); raise on a fast machine
    // to fill the view quicker, lower to keep per-frame cost down. See world.yaml.
    int streamPumpBudget = 12;  // finished-mesh uploads applied per frame (steady state)
    int streamMeltBudget = 64;  // boosted per-frame uploads while a backlog melts in
    int streamCoreRadius = 5;   // chunks around spawn meshed synchronously at startup;
                                // the rest stream in (smaller => faster first frame)
    int streamUploadSlice = 384;// chunks per GPU submit in the startup batch upload
                                // (bounded by Vulkan's maxMemoryAllocationCount ~4096)

    // --- Liquid flow tuning (the `liquids:` block in world.yaml) ---------------
    int liquidMaxFills = 16;    // cells a liquid tick fills before stopping (one relight)
    int liquidScan     = 4096;  // cheap blockAt reads per tick to drain dead queue cells
    int liquidMaxLevel = 3;     // rings a source spreads before the thin edge stops
                                // (level 0 source .. liquidMaxLevel edge); bigger = wider
                                // puddles but more cells filled + remeshed per tick

    // (Removed: the old single-noise height/material fields and the radial island
    //  mask. Terrain shape + surface materials are now authored in assets/biomes.yaml
    //  and driven by vg::TerrainGenerator; columnHeight() calls gen_.height().)

    // (Removed: the built-in lantern/cairn/geode/tree/bush scatter densities. All
    //  surface decoration is now authored as procedural features — assets/features/
    //  *.yaml — so these per-column built-in scatters no longer exist.)

    // --- Structures (assets/structures/*.yaml) --------------------------------
    // Hand-authored voxel templates stamped sparsely on land. Candidate origins sit
    // on a coarse grid: every structureSpacing blocks a cell has a structureDensity
    // chance of rooting one (jittered within the cell, seam-safe via per-column
    // gather). 0 density disables them.
    int   structureSpacing = 80;
    float structureDensity = 0.35f;

    // --- Ore (iron only) -------------------------------------------------------
    // Iron replaces stone in small clusters (a roll shared across a 2x2x2 cell) up
    // to its max world-Y.
    float ironDensity = 0.013f;  int ironMaxY = 44;
    // Optional noise MASK (`ores.iron.mask:` in world.yaml): a multi-layer field +
    // threshold + steepness curve whose [0,1] weight MULTIPLIES the ore density, so
    // iron concentrates into authored ore-rich regions/veins instead of a flat
    // sprinkle. Empty (default) → weight 1 → unchanged. Same primitive + tool editor
    // as the biome surface masks / feature scatter.
    NoiseMask oreMask;

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
