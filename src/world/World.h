#pragma once

/**
 * @file World.h
 * @brief The loaded voxel world: chunk storage, worldgen, lighting, and streaming.
 *
 * World owns the BlockRegistry, a ring-buffer of Chunks, and two per-block light
 * fields (sky and block). It is the single authority for block queries, edits,
 * and streaming. Only the main thread may mutate it; mesh workers hold const
 * references. generateColumnInto() is `const` specifically so the pregen
 * background thread can call it safely.
 * @warning setBlock, recenter, and relight are main-thread-only mutators.
 *          Call streamBarrier() before any of them when workers are running.
 * @see docs/CODE_INDEX.md
 */

#include "world/BlockRegistry.h"
#include "world/Chunk.h"
#include "world/Shape.h"
#include "world/WorldConfig.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  World
// -----------------------------------------------------------------------------
//  A fixed grid of chunks generated from noise (Milestone 3). Owns the block
//  registry and all chunk data, and answers world-space queries (solidity,
//  surface height) used by the renderer and the player.
//
//  Terrain uses two noise layers: one for the base surface *height*, and a
//  second for *material/biome* variation (rocky vs grassy, dirt depth). Island
//  shaping and chunk streaming are deliberately left for later milestones; the
//  grid here is generated once around the origin.
// -----------------------------------------------------------------------------
class World {
public:
    // Build the world from a generation config (size, seed, terrain knobs — see
    // assets/world.yaml) and a block-definition file.
    World(const WorldConfig& config, const std::string& blocksFile);
    ~World(); // flushes edited chunks to disk (streaming persistence; see Stage 3)

    [[nodiscard]] glm::ivec3 chunkCounts()  const { return counts_; }
    // Min chunk coordinate of the loaded window (moves with the player when
    // streaming; {0,0,0} otherwise). The renderer needs it to iterate the window.
    [[nodiscard]] glm::ivec3 chunkOrigin()  const { return originChunk_; }
    [[nodiscard]] glm::ivec3 sizeInBlocks() const { return counts_ * Chunk::kSize; }
    [[nodiscard]] const BlockRegistry& registry() const { return registry_; }
    // World seed (kept for save-path naming and any deterministic per-world hashing).
    [[nodiscard]] uint32_t seed() const { return config_.seed; }
    // The full generation/streaming config (assets/world.yaml). The renderer and app
    // read the streaming/liquid tuning knobs from here (REVIEW R7).
    [[nodiscard]] const WorldConfig& config() const { return config_; }
    [[nodiscard]] const Chunk& chunk(int cx, int cy, int cz) const;

    // The block at world coords; air (the default Block) outside world bounds.
    // This is the seam the mesher samples across chunk boundaries.
    [[nodiscard]] Block blockAt(int wx, int wy, int wz) const;

    // Sky-light level (0..15) at world coords: 15 where the block can see the sky,
    // falling off by one per step into shadowed/enclosed space (so caves and deep
    // pits go dark). Outside the world counts as open sky (15). The mesher folds
    // this into face brightness; the field is rebuilt by computeSkyLight().
    [[nodiscard]] uint8_t skyLightAt(int wx, int wy, int wz) const;

    // Block-light level (0..15) at world coords: emitted by glowing blocks (see
    // BlockProperties::emission) and flooding outward through non-opaque space,
    // losing one level per step. Zero outside the world (no sky fallback).
    [[nodiscard]] uint8_t blockLightAt(int wx, int wy, int wz) const;

    // Linear-RGB hue of the block light at world coords: the colour of the
    // dominant (brightest-reaching) emitter, carried alongside blockLightAt() by
    // the same flood. Black outside the window / where no emitter reaches. The
    // mesher bakes this per-vertex so torches/lava glow in their own colours.
    [[nodiscard]] glm::vec3 blockLightColorAt(int wx, int wy, int wz) const;

    // Light the mesher actually shades with: the brighter of sky and block light.
    [[nodiscard]] uint8_t lightAt(int wx, int wy, int wz) const;

    // --- Biome vegetation tinting --------------------------------------------
    // True if this block's albedo should be multiplied by the biome tint (the
    // green foliage: grass, leaves of any species, tall grass, fern, bush). Other
    // blocks — incl. flowers/mushrooms — keep their authored colour.
    [[nodiscard]] bool isVegTintable(uint16_t id) const;
    // The biome vegetation tint at a column (white if uninteresting). Used by the
    // mesher (via WorldRenderer's tint sampler) to colour tintable faces.
    [[nodiscard]] glm::vec3 vegTintAt(int wx, int wz) const;

    // Is the block at world coords solid? Air (false) outside the world bounds.
    [[nodiscard]] bool isSolid(int wx, int wy, int wz) const;

    // Can the crosshair target (break/place against) the block here? True for
    // solids AND see-through foliage (leaves/bush) so the canopy is breakable, but
    // false for air and for opaque liquids (you don't target water/lava).
    [[nodiscard]] bool isTargetable(int wx, int wy, int wz) const;

    // X/Z inset of a thin (Model) block's box here (0 for full-cell blocks). Lets
    // the raycast and player collision treat a slender trunk as its real column.
    [[nodiscard]] float modelInsetAt(int wx, int wy, int wz) const;

    // Collision/raycast boxes for the block here, in WORLD coordinates (already
    // offset by the cell). Returns the count (0 = not solid); writes up to
    // kMaxShapeBoxes boxes. Full cubes -> one box; thin Model posts and reshaped
    // blocks (slab/stairs/post/wall) -> their shape's box union. The single source
    // of truth shared with the mesher (vg::shapeBoxes).
    [[nodiscard]] int collisionBoxesAt(int wx, int wy, int wz, ShapeBox out[]) const;

    // Y to stand on at world column (wx, wz): one above the topmost solid block.
    [[nodiscard]] int surfaceHeight(int wx, int wz) const;

    // Place a block at world coords. Relights the affected box and returns exactly
    // the chunk coordinates whose meshes must be rebuilt: the containing chunk, any
    // neighbour chunk sharing the touched face (its boundary faces are culled
    // against this block), and every chunk whose baked sky/block light actually
    // changed (found by diffing the relit box, so unchanged chunks aren't remeshed).
    // An out-of-world write changes nothing and returns an empty list.
    std::vector<glm::ivec3> setBlock(int wx, int wy, int wz, Block b);

    // Batched edit (liquid flow): write many blocks, then relight the *single*
    // union box once and diff it — so N nearby flow fills cost one light flood
    // instead of N. Same return contract as setBlock (the chunks to remesh).
    std::vector<glm::ivec3> setBlocksBatch(const std::vector<std::pair<glm::ivec3, Block>>& edits);

    // Change how fast light decays per spread step (levels lost per block, see
    // Settings) and recompute both light fields. Values clamp to 1..15. Returns
    // true if anything changed — the caller must then remesh every chunk, since
    // lighting is baked into the vertex data.
    bool setLightFalloff(int skyFalloff, int blockFalloff);

    // Recompute BOTH light fields from scratch over the whole current window.
    // recenter()'s incremental shiftColumn path defers its relight to relightBoxes()
    // and records boxes in intermediate-origin coords; a startup recenter that steps
    // multiple columns in X then Z can't have those boxes replayed correctly against
    // the final window, so the caller does a single full recompute instead (one-time
    // startup cost). The caller must streamBarrier() first and remesh afterwards,
    // since lighting is sampled per-pixel from the per-chunk light atlas.
    void recomputeLight();

    // Recenter the loaded window so chunk column (centerChunkX, centerChunkZ) is in
    // the middle, streaming new chunks in and old ones out. Returns the chunk
    // coordinates whose meshes must be (re)built — newly generated columns plus any
    // chunk whose light changed at the seam — to hand to WorldRenderer::remeshChunks.
    // No-op (empty) when streaming is off or the window hasn't moved. The caller
    // passes the player's chunk column each frame. See docs/STREAMING.md.
    // Advance the loaded window to centre on chunk (cx,cz): generate the entering
    // columns and move the window origin (the SYNCHRONOUS part), returning the
    // chunks dirtied by generation and appending the relight boxes to process to
    // `relightBoxesOut`. Follow with relightBoxes() — inline for synchronous
    // streaming, or on a background thread when streamAsync() is on. No-op (empty,
    // no boxes) when streaming is off or the window hasn't moved.
    std::vector<glm::ivec3> recenter(int centerChunkX, int centerChunkZ,
                                     std::vector<glm::ivec4>& relightBoxesOut);

    // --- Background strip pregeneration (streaming) ---------------------------
    // Generating the entering edge columns is the expensive part of a window step
    // (~90ms of noise/features even in parallel) and recenter() runs it on the
    // main thread — the per-boundary frame spike. pregenStrip() does ONLY that
    // generation, into staging chunks, without touching the window: it reads just
    // the immutable generator + save files, so it is SAFE on a background thread
    // while the game renders (the caller must not move the window or write save
    // files for the entering columns while it runs — see App's orchestration).
    // recenterWithStrip() then performs the single column step the strip was made
    // for by MOVING the staged chunks into their ring slots (a few ms of memcpy)
    // — byte-identical to generating them in place. If the strip no longer
    // matches the step needed (the player turned around), it returns empty and
    // the caller simply pregens the right strip next cycle.
    /**
     * @brief A pre-generated edge strip of chunks for the next streaming step.
     *
     * Computed by pregenStrip() on a background thread and consumed by
     * recenterWithStrip() on the main thread. If the strip no longer matches the
     * step needed it is silently discarded and re-generated next frame.
     * @note Thread-safe to generate: pregenStrip() is `const` and touches only
     *       the immutable generator + save files, never the window ring buffer.
     */
    struct PregenStrip {
        bool       valid  = false;
        int        dir    = 0;     // +1/-1 along the axis
        bool       alongX = true;
        glm::ivec2 origin{0};      // window origin (x,z) the strip was computed for
        std::vector<Chunk> chunks; // entering columns x chunksY, edge order
    };
    /**
     * @brief Pre-generate the entering edge strip for one streaming step.
     *
     * Generates the column(s) that would enter the window if it moved by `dir`
     * (+1/-1) along X (alongX=true) or Z, starting from virtual origin (fromX,fromZ).
     * Pure function of the immutable generator + save files — safe to call on a
     * background thread while the main thread renders.
     * @note This method is `const` precisely to enforce that it cannot touch window state.
     */
    [[nodiscard]] PregenStrip pregenStrip(int dir, bool alongX, int fromX, int fromZ) const;
    std::vector<glm::ivec3> recenterWithStrip(int centerChunkX, int centerChunkZ,
                                              PregenStrip&& strip,
                                              std::vector<glm::ivec4>& relightBoxesOut);

    // Flood-relight the boxes recorded by recenter() and append every chunk whose
    // light changed to `dirty` (then dedups it). SAFE to run on a background thread:
    // it only reads the just-generated edge chunks and writes the edge light slab,
    // both disjoint from the player-area slots the main thread reads for collision.
    // The caller must not mutate the World (setBlock / another recenter) while this
    // runs — see App's streaming orchestration.
    //
    // ACCEPTED DATA RACE (REVIEW R6): the relight box deliberately reaches ~16 blocks
    // into the *retained* interior (shiftColumn's relight-box recording) so light
    // bleeds across the seam. While this background flood writes that margin, a
    // main-thread light READ in the same margin — lightAt() queries, or liquid-tick
    // meshing in the stream_workers:0 config — can observe a torn uint8 light level.
    // It is benign by construction: light levels are single bytes (a torn read is
    // still a valid 0..15 value, never a wild pointer), and the post-relight remesh
    // of every dirtied chunk overwrites the transient value, so it self-corrects
    // within a frame. It is left as-is rather than locked because a per-cell lock on
    // the light fields would cost far more than the worst-case one-frame flicker. A
    // thread sanitizer WILL flag it; if that ever matters, gate the margin behind a
    // "relight-pending" flag the main-thread light paths treat as off-limits.
    void relightBoxes(const std::vector<glm::ivec4>& boxes, std::vector<glm::ivec3>& dirty);

    // Test/verify hook: force the recenter sky relight (relightBox) to flood the full
    // world column instead of the height-bounded band, so a logictest can prove the
    // bounded box is byte-identical to full height. Off in production (bounded).
    void setSkyRelightFullHeight(bool on) { skyRelightFullHeight_ = on; }

    // Streaming knobs the renderer/app need (read from the world config).
    [[nodiscard]] bool streaming()    const { return config_.streaming; }
    [[nodiscard]] int  streamWorkers() const { return config_.streamWorkers; }
    // Per-seed persistence directory (<saveDir>/<seed>), or empty when persistence
    // is off. The same folder that holds saved chunks; the player save lives here.
    [[nodiscard]] const std::string& savePath() const { return savePath_; }
    // Async streaming: relight the streamed-in edge on a background thread so the
    // main thread never blocks on the light flood (the residual streaming spike).
    [[nodiscard]] bool streamAsync()  const { return config_.streaming && config_.asyncStreaming; }
    // True if recenter(cx,cz) would move the window — a cheap pre-check so the app
    // only pays the worker-drain barrier on the rare frames that actually mutate.
    [[nodiscard]] bool needsRecenter(int centerChunkX, int centerChunkZ) const;

private:
    void generate();
    // Generate the whole vertical stack of chunks at chunk column (cx,cz) as a unit
    // — the surface/material/feature noise is computed once per (X,Z) and shared
    // across the stack (≈chunksY× fewer expensive columnHeight calls). Persisted
    // chunks load from disk instead. Each call writes only its own ring slots, so
    // distinct columns generate in parallel safely.
    void generateColumn(int cx, int cz);
    // The actual column generation, writing through caller-provided destinations:
    // stack[cy] = the chunk to fill, dirtyFlags[cy] = its dirty flag to clear.
    // generateColumn() passes the ring slots; pregenStrip() passes staging chunks
    // — which is why this is const (it touches NO window state, so it can run on
    // a background thread while the main thread reads the window).
    void generateColumnInto(int cx, int cz, Chunk* const* stack,
                            uint8_t* const* dirtyFlags) const;

    // (Re)compute the sky-light field from the current blocks (flood fill from
    // sky-exposed columns). Called after generation and after every edit.
    void computeSkyLight();
    // (Re)compute the block-light field (flood fill from emissive blocks). Same
    // call sites as computeSkyLight(); the two fields are combined by lightAt().
    void computeBlockLight();

    // Build the block-light colour palette + per-block colour index from the
    // registry's emission colours, once at construction. Single-threaded so the
    // parallel emitter seed in computeBlockLight() can read it race-free.
    void buildLightColorPalette();

    // Recompute one light field inside the box [x0,x1] x [y0,y1] x [z0,z1].
    // emitterSeed picks the source: false = sky (open columns), true = block
    // emission. Light entering across ALL six open faces is seeded from the
    // current field outside, so a clamped box is correct, not just local. The sky
    // pass seeds its top face by inheriting the (lossless) column value from just
    // above the box; pass y0/y1 spanning the world height for the legacy behaviour.
    void relightField(std::vector<uint8_t>& field, bool emitterSeed, int x0, int x1,
                      int y0, int y1, int z0, int z1);

    // Advance the window one chunk along X (alongX=true) or Z by `dir` (+1/-1):
    // regenerate the entering edge column and relight the seam, appending dirtied
    // chunks to `dirty`. Used by recenter().
    // dir +/-1 along X (alongX) or Z: generate the entering edge column and record
    // its relight box into `relightBoxes` (the flood is deferred to relightBoxes()),
    // appending the dirtied chunks to `dirty`.
    // `strip`, when non-null, is a matching pregenerated edge (pregenStrip): its
    // chunks are moved into the ring slots instead of being generated here.
    void shiftColumn(int dir, bool alongX, std::vector<glm::ivec3>& dirty,
                     std::vector<glm::ivec4>& relightBoxes, PregenStrip* strip = nullptr);
    // Relight both light fields inside the block-box [x0,x1] x [z0,z1] (full height)
    // and append every chunk whose light changed to `dirty` (diffed vs a snapshot).
    void relightBox(int x0, int x1, int z0, int z1, std::vector<glm::ivec3>& dirty);

    // Persist the chunk at (cx,cy,cz) to disk if it has unsaved edits, then mark it
    // clean. No-op when persistence is off (savePath_ empty / streaming disabled).
    void saveChunkIfDirty(int cx, int cy, int cz);
    // Persist every edited chunk currently in the window (unload / teleport / quit).
    void saveDirtyWindow();

    [[nodiscard]] int lightIndex(int wx, int wy, int wz) const;

    // Topmost solid block's Y at a world column (scans the actual voxels, so it
    // reflects edits; the flat world's base surface is grass at Y=16).
    [[nodiscard]] int columnHeight(int wx, int wz) const;

    [[nodiscard]] int  chunkIndex(int cx, int cy, int cz) const;
    [[nodiscard]] bool inChunkBounds(int cx, int cy, int cz) const;

    // Min block coordinate of the loaded window (originChunk_ * kSize). All
    // per-block storage (chunks_, skyLight_, blockLight_) is addressed relative to
    // this, so the window can move without rewriting absolute world coords. See
    // docs/STREAMING.md.
    [[nodiscard]] glm::ivec3 originBlock() const { return originChunk_ * Chunk::kSize; }

    WorldConfig   config_;     // generation knobs (assets/world.yaml)
    glm::ivec3    counts_;     // number of chunks along x, y, z (== config_.chunks*)
    // Min chunk coordinate of the loaded window. Stays {0,0,0} until the window
    // follows the player (docs/STREAMING.md Stage 2); the full-field light
    // recompute paths (computeSkyLight/computeBlockLight) still assume it is 0 and
    // are reworked into a windowed relight in that stage.
    glm::ivec3    originChunk_{0, 0, 0};
    BlockRegistry registry_;
    std::vector<Chunk>   chunks_;     // flat ring buffer, indexed by chunkIndex()
    std::vector<uint8_t> skyLight_;   // per-block sky light, indexed by lightIndex()
    std::vector<uint8_t> blockLight_; // per-block emitted light, same indexing
    bool skyRelightFullHeight_ = false; // test hook: see setSkyRelightFullHeight()
    // Per-block emitter hue, stored as a 1-byte palette index (same indexing as
    // blockLight_). Emitter colours come from a handful of blocks.yaml entries, so
    // a palette index costs 1 byte/cell instead of a packed RGBA8 uint32 — ~200 MB
    // saved at a 33x33x256 window (REVIEW O3). lightColorPalette_[idx] is the packed
    // RGBA8 colour (entry 0 = none/black); emissionColorIndex_[blockId] is that
    // block's palette index, both built once in buildLightColorPalette().
    std::vector<uint8_t>  blockLightColor_;
    std::vector<uint32_t> lightColorPalette_;   // palette idx -> packed RGBA8 (0 = none)
    std::vector<uint8_t>  emissionColorIndex_;  // block id -> palette idx (0 = non-emitter)
    std::vector<uint8_t> chunkDirty_; // per ring slot: edited since gen/load (needs saving)
    std::string          savePath_;   // <config.saveDir>/<seed>; empty = persistence off

    // Block ids the flat generator places, resolved by name from the registry
    // once at construction so generateColumn() stays a cheap loop.
    uint16_t grassId_;
    uint16_t dirtId_;
    uint16_t stoneId_;
    uint16_t cobbleId_;
    uint16_t sandId_;
    uint16_t waterId_;
    uint16_t snowId_;
    // Tree species (trunk = centred Model post, leaves = LeafCube canopy), placed by
    // the biome pass in generateColumnInto.
    uint16_t oakTrunkId_, oakLeavesId_;
    uint16_t birchTrunkId_, birchLeavesId_;
    uint16_t pineTrunkId_, pineLeavesId_;
};

} // namespace vg
