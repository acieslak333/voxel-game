#pragma once

#include "render/Buffer.h"
#include "render/MeshArena.h"
#include "world/ChunkMesher.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vg {

class VulkanContext;
class Pipeline;
class TextureArray;
class World;

// -----------------------------------------------------------------------------
//  WorldRenderer
// -----------------------------------------------------------------------------
//  Renders a whole World: greedy-meshes every chunk once, keeps a vertex/index
//  buffer per chunk (indexed by chunk coordinate), and draws each non-empty one
//  each frame with a per-chunk model matrix (its world-space translation). The
//  pipeline, texture array and per-frame camera UBO/descriptor sets are shared
//  across all chunks.
//
//  remeshChunk() rebuilds a single chunk's geometry in place — the seam for
//  block editing and chunk streaming, so neither has to re-mesh the whole world.
//
//  TODO(future): stream chunks in/out as the player moves rather than meshing
//  the entire (fixed) world up front.
// -----------------------------------------------------------------------------
class WorldRenderer {
public:
    WorldRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                  const World& world, const std::string& shaderDir,
                  const std::string& textureDir);
    ~WorldRenderer();

    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    // `sunDirAmbient` = xyz: direction toward the active celestial light (sun by
    // day, moon at night), w: ambient floor. `sunColIntensity` = rgb: linear
    // light tint, a: sky-light intensity (1 noon .. ~0.16 midnight). Both come
    // from DayNight::state() and feed the chunk shader's directional lighting.
    // `heldLight` is a dynamic point light that travels with the player when they
    // hold an emitter (a lit torch, glowstone, ...): xyz = world position, w =
    // radius in blocks (0 = no held light). `heldLightCol` = rgb linear colour,
    // a = intensity (0..1). Lit per-fragment in the chunk shader (no relight).
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity,
                const glm::vec4& heldLight, const glm::vec4& heldLightCol);

    // PS1 retro geometry knobs (folded into the chunk UBO's `misc`): `jitter` is the
    // vertex-snap grid resolution (0 = off), `affine` toggles affine texture warp.
    void setRetro(float jitter, float affine) { retroJitter_ = jitter; retroAffine_ = affine; }

    // Rebuild one chunk's mesh and swap its GPU buffers, leaving every other
    // chunk untouched. Call after the world's blocks change (see World::setBlock,
    // which returns exactly the chunk coordinates to pass here). Meshes on the
    // main thread (immediate, low-latency) and installs via the deferred path —
    // no GPU drain (REVIEW R4).
    void remeshChunk(int cx, int cy, int cz);

    // Remesh several chunks after an edit. Meshes them on the main thread now (so
    // the edit shows this frame) and installs via the deferred buffer + frame-
    // integrated upload path (installMeshBatch -> recordPendingUploads), so there
    // is NO vkDeviceWaitIdle per edit (REVIEW R4). Pass the list World::setBlock
    // returns after an edit.
    void remeshChunks(const std::vector<glm::ivec3>& chunks);

    // Rebuild every chunk's mesh (one GPU drain for the lot). Needed when
    // something baked into the vertex data changes globally — e.g. the light
    // falloff (lighting lives in per-vertex attributes, not a shader uniform).
    void remeshAll();

    // --- Streaming remesh API (App calls these) ------------------------------
    // A window step dirties a whole edge column (~2*chunksZ*chunksY chunks);
    // remeshing them all at once freezes the frame. These spread the work:
    // with stream_workers > 0 greedy meshing runs on background threads and the
    // main thread only uploads finished meshes; with 0 it falls back to a single-
    // thread backlog drained a budget at a time. Block edits stay immediate
    // (remeshChunks) — a handful of chunks.
    //
    //   streamBarrier(): block until no worker is mid-read of the World. MUST be
    //     called before any World mutation (recenter / setBlock) so workers never
    //     read torn chunk/light data. No-op without workers.
    //   streamRemesh(chunks): (re)mesh these chunks (worker jobs or backlog).
    //   streamPump(budget): apply up to `budget` finished meshes this frame.
    void streamBarrier();
    void streamRemesh(const std::vector<glm::ivec3>& chunks);
    void streamPump(int budget);

    // True when no mesh worker has an outstanding job. The app gates recenter() on
    // this so streamBarrier() never actually *blocks* the frame (it only drains an
    // already-idle pool) — the per-boundary hitch was the barrier waiting out the
    // previous crossing's backlog. Always true (idle) when there are no workers.
    [[nodiscard]] bool streamWorkersIdle() const { return jobsOutstanding_.load() == 0; }

    // Record the queued streamed-mesh staging->device copies (built by streamPump)
    // into `cmd`. MUST be called before any render pass — it's the frame's pre-pass
    // hook (Renderer::drawFrame's recordPre), so uploads ride the frame's own submit
    // with zero extra GPU sync. No-op if nothing is queued.
    void recordPendingUploads(VkCommandBuffer cmd);

    [[nodiscard]] std::size_t drawnChunkCount() const { return drawnChunks_; }
    [[nodiscard]] std::size_t triangleCount()   const { return totalTriangles_; }
    // Per-frame culling telemetry (updated by record(), read by the VG_FRAME_TIME
    // profiler). `visibleChunks` = slots that survived frustum culling and issued a
    // draw this frame; `culledChunks` = slots in drawList_ skipped as off-screen;
    // `drawCalls` = vkCmdDrawIndexed/Indirect calls recorded (opaque + water). These
    // are what tell you whether record() is paying for too many draws (rec-bound) —
    // drawnChunkCount() counts resident geometry, this counts what actually drew.
    [[nodiscard]] std::size_t visibleChunkCount() const { return lastVisibleChunks_; }
    [[nodiscard]] std::size_t culledChunkCount()  const { return lastCulledChunks_; }
    [[nodiscard]] std::size_t drawCallCount()     const { return lastDrawCalls_; }

    // The block texture array's image view + sampler, so other passes (the HUD,
    // which draws isometric block icons) can sample the same textures.
    [[nodiscard]] VkImageView blockTextureView() const;
    [[nodiscard]] VkSampler   blockTextureSampler() const;

private:
    struct CameraUBO {
        glm::mat4 view;
        glm::mat4 proj;
        glm::vec4 sunDir; // xyz: toward the active light, w: ambient floor
        glm::vec4 sunCol; // rgb: linear light tint, a: sky-light intensity
        glm::vec4 misc;   // x: animation time (seconds) for foliage sway / water waves
        glm::vec4 heldLight;    // xyz: held-emitter world pos, w: radius (0 = off)
        glm::vec4 heldLightCol; // rgb: linear colour, a: intensity (0..1)
    };
    float animTime_ = 0.0f; // accumulated per recorded frame; drives the sway/wave clock
    float retroJitter_ = 0.0f; // PS1 vertex-jitter grid resolution (0 = off)
    float retroAffine_ = 0.0f; // PS1 affine texture-warp flag (0/1)
    // Per-draw push constant: chunk model matrix + params (params.x = output
    // alpha — 1 for the opaque pass, < 1 for the translucent water pass).
    struct PushConstants {
        glm::mat4 model;
        glm::vec4 params{1.0f};
    };
    static_assert(sizeof(PushConstants) == 20 * sizeof(float), "matches Pipeline::kPushConstantSize");
    // GPU buffers + placement for one chunk's mesh, now as a span in the shared
    // MeshArena (not a per-chunk VkBuffer). An empty chunk keeps a slot here with
    // indexCount == 0 and an invalid arena alloc (skipped while drawing). Opaque
    // vertices/indices come first in the chunk's arena span, water after; the water
    // draw uses waterBaseVertex/waterFirstIndex (its indices are 0-based into the
    // water vertices). baseVertex/firstIndex are element offsets — exactly what
    // VkDrawIndexedIndirectCommand.vertexOffset / .firstIndex consume.
    struct ChunkMesh {
        MeshArena::Alloc arena;            // span in the shared vertex+index arena
        uint32_t indexCount      = 0;      // opaque index count
        uint32_t firstIndex      = 0;      // opaque first index (= arena.firstIndex)
        int32_t  baseVertex      = 0;      // opaque vertexOffset (= arena.baseVertex)
        uint32_t waterIndexCount = 0;      // translucent water index count
        uint32_t waterFirstIndex = 0;      // water first index
        int32_t  waterBaseVertex = 0;      // water vertexOffset
        glm::vec3 worldPos{0.0f};
        int       drawListPos = -1;        // index in drawList_, or -1 if empty (R8)
    };
    // A chunk's computed arena placement, passed from the install paths to
    // swapChunkBuffers (replaces the old per-chunk Buffer + offsets bundle).
    struct MeshPlacement {
        MeshArena::Alloc arena;
        uint32_t indexCount = 0,      firstIndex = 0;      int32_t baseVertex = 0;
        uint32_t waterIndexCount = 0, waterFirstIndex = 0; int32_t waterBaseVertex = 0;
    };
    // A queued mesh job / its finished result, exchanged with the worker pool.
    struct MeshJob    { int cx, cy, cz; std::uint64_t version; };
    struct MeshResult { int cx, cy, cz; std::uint64_t version; MeshData data; };

    void buildMeshes();
    // (Re)mesh chunk (cx,cy,cz) and (re)build its buffers into its slot, updating
    // the triangle/drawn-chunk tallies. The immediate-drain path: callers issue a
    // device-idle wait first. Now used only by buildMeshes() (startup); edits go
    // through meshChunksDeferred().
    void uploadChunkMesh(int cx, int cy, int cz);
    // Mesh a list of chunks on the MAIN thread (so an edit is visible this frame),
    // dedup, bump each slot's version (discarding any stale worker result), and
    // install via the deferred buffer + frame-integrated upload path — no GPU
    // drain. Shared by remeshChunk/remeshChunks and the no-worker remesh queue
    // (REVIEW R4).
    void meshChunksDeferred(const std::vector<glm::ivec3>& chunks);
    // Apply an already-built mesh to a slot. deferOldBuffers=true retires the old
    // GPU buffers for framesInFlight_+1 frames instead of freeing them now (an
    // in-flight frame may still reference them) — required when uploading from the
    // worker path, which can't issue a device-idle wait every frame.
    void installMesh(int cx, int cy, int cz, MeshData&& mesh, bool deferOldBuffers);
    // Swap an already-arena-resident mesh into a slot (retire the old arena span,
    // update tallies + draw list). deferOldSpan retires the previous span for
    // framesInFlight_+1 frames (an in-flight frame may still reference it) instead
    // of freeing it now. An empty placement (invalid arena, zero counts) clears the
    // slot. The geometry must already be (or be queued to be) uploaded into the
    // arena at the placement's offsets.
    void swapChunkBuffers(int cx, int cy, int cz, const MeshPlacement& place,
                          bool deferOldSpan);
    // Allocate an arena span for `mesh`, fill `stage` with its blob and return the
    // placement + the copy regions to record (vertices -> vertex arena, indices ->
    // index arena). Shared by every upload path. `stage` is left holding the data.
    [[nodiscard]] MeshPlacement stageMesh(const MeshData& mesh, Buffer& stage,
                                          VkDeviceSize& vtxDst, VkDeviceSize& vtxSize,
                                          VkDeviceSize& idxSrc, VkDeviceSize& idxDst,
                                          VkDeviceSize& idxSize);
    // Upload a whole batch of finished worker meshes with a SINGLE GPU submit+wait
    // for every staging copy (vs one device-idle wait per buffer, which is what
    // made loading lag). Discards results superseded by a newer request.
    void installMeshBatch(std::vector<MeshResult>& batch);
    void tickRetired(); // age + free the deferred-deletion list (once per frame)

    // Worker-thread meshing (stream_workers > 0).
    void startWorkers(int n);
    void stopWorkers();
    void workerLoop();
    void enqueueMeshJobs(const std::vector<glm::ivec3>& chunks);
    void drainMeshJobs();
    void processMeshResults(int budget);
    // Single-thread amortised fallback (stream_workers == 0).
    void queueRemesh(const std::vector<glm::ivec3>& chunks);
    void processRemeshQueue(int budget);
    // A sampler that resolves chunk-local coords just outside (cx,cy,cz) to the
    // neighbouring chunk's block, so the mesher can cull cross-chunk faces.
    [[nodiscard]] ChunkMesher::NeighborSampler makeSampler(int cx, int cy, int cz) const;
    // Resolves chunk-local coords to the world's sky-light field for this chunk.
    [[nodiscard]] ChunkMesher::LightSampler makeLightSampler(int cx, int cy, int cz) const;
    // Resolves a tintable block at chunk-local (x,z) to its biome vegetation tint.
    [[nodiscard]] ChunkMesher::TintSampler makeTintSampler(int cx, int cy, int cz) const;

    // The single chunk-mesh chokepoint every build path funnels through: greedy
    // meshing normally, or (VG_SURFACENETS=1) a Surface Nets mesh of the analytic
    // terrain density (docs/WORLDGEN.md Layer 2) — an experimental smooth-terrain
    // preview. Both only READ the world, so this is safe on the worker pool.
    [[nodiscard]] MeshData meshChunkData(int cx, int cy, int cz) const;
    // Build a chunk mesh with Surface Nets over the generator's density field.
    [[nodiscard]] MeshData buildSurfaceNetsMesh(int cx, int cy, int cz) const;

    [[nodiscard]] int chunkIndex(int cx, int cy, int cz) const;
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n);
    // Per-frame GPU-driven draw resources: the chunk draw-data SSBO (binding 2) and
    // the opaque/water indirect command buffers. Created once counts_ is known.
    void createGpuDrivenBuffers(uint32_t n);

    VulkanContext& ctx_;
    const World&   world_;
    bool           surfaceNets_ = false; // VG_SURFACENETS: experimental Layer-2 mesher
    int            landformMode_ = 0;    // VG_LANDFORM: 0 terrain, 1 pillars, 2 arch (SN demo)

    std::unique_ptr<TextureArray> textures_;
    std::unique_ptr<Pipeline>     pipeline_;            // opaque terrain
    std::unique_ptr<Pipeline>     waterPipeline_;       // translucent water (2nd pass)

    std::vector<ChunkMesh> meshes_;          // indexed by chunkIndex(); may be empty
    // Compact list of the meshes_ slots that currently have geometry (opaque or
    // water), maintained by swapChunkBuffers. record() iterates THIS instead of all
    // ~17k slots twice per frame, so its cost scales with drawn chunks not window
    // volume (REVIEW R8). drawnWaterChunks_ lets the whole water pass be skipped when
    // nothing has water (the common above-ground case).
    std::vector<uint32_t>  drawList_;
    std::size_t            drawnWaterChunks_ = 0;
    // Outer-ring chunks buildMeshes() skipped (streaming only): the ctor hands them
    // to the streaming pipeline, nearest-first, so they melt in over the first
    // frames instead of blocking startup. startupMelt_ keeps streamPump() at a
    // boosted budget until that initial backlog has fully drained.
    std::vector<glm::ivec3> deferredStartup_;
    bool                    startupMelt_ = false;
    std::deque<glm::ivec3> pendingRemesh_;    // streaming remesh backlog (drained per frame)
    glm::ivec3             counts_{0};        // chunk grid dimensions
    std::size_t            drawnChunks_    = 0; // slots with geometry
    std::size_t            totalTriangles_ = 0;
    // Per-frame culling telemetry, recomputed each record(). Not part of the draw
    // state — purely for the VG_FRAME_TIME readout (see *ChunkCount() getters).
    std::size_t            lastVisibleChunks_ = 0;
    std::size_t            lastCulledChunks_  = 0;
    std::size_t            lastDrawCalls_     = 0;

    std::vector<Buffer>          uniformBuffers_; // one per frame in flight
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;

    // --- GPU-driven (arena + indirect) draw state ----------------------------
    // All chunk geometry lives in one shared vertex arena + one index arena, so
    // record() binds once and issues a single vkCmdDrawIndexedIndirect per pass.
    std::unique_ptr<MeshArena> arena_;
    // Per frame in flight, host-visible: the per-slot draw-data SSBO (vec4 world
    // pos, indexed by slot via gl_InstanceIndex) and the opaque/water indirect
    // command buffers. Double-buffered so the CPU can refill frame N while the GPU
    // reads frame N-framesInFlight.
    std::vector<Buffer> drawDataBuffers_;   // binding 2 SSBO (numSlots * vec4)
    std::vector<Buffer> opaqueIndirect_;    // numSlots * VkDrawIndexedIndirectCommand
    std::vector<Buffer> waterIndirect_;
    std::vector<glm::vec4> drawDataCpu_;     // mirror of the SSBO (worldPos per slot)
    // Reused per-frame scratch for building the indirect command arrays on the CPU.
    std::vector<VkDrawIndexedIndirectCommand> cmdScratch_;
    // Freed arena spans awaiting reuse (framesInFlight_+1 frames), like retired_.
    std::vector<std::pair<int, MeshArena::Alloc>> retiredAllocs_;

    // --- Streaming worker pool (active only when streaming + stream_workers > 0) -
    uint32_t                   framesInFlight_ = 0;
    std::vector<std::uint64_t> meshVersion_;       // latest requested mesh per slot
    std::vector<std::pair<int, Buffer>> retired_;  // deferred buffer frees (framesLeft, buf)

    std::vector<std::thread>   workers_;
    std::mutex                 jobMutex_;
    std::condition_variable    jobCv_;
    std::deque<MeshJob>        jobQueue_;     // guarded by jobMutex_
    std::mutex                 resultMutex_;
    std::deque<MeshResult>     resultQueue_;  // guarded by resultMutex_
    std::mutex                 barrierMutex_;
    std::condition_variable    barrierCv_;
    std::atomic<int>           jobsOutstanding_{0}; // enqueued but not yet meshed
    std::atomic<bool>          stopWorkers_{false};

    // --- Frame-integrated streaming upload -----------------------------------
    // streamPump builds the device buffers (already swapped into their slots) plus
    // host staging copies and queues them here; recordPendingUploads() records the
    // copies into the frame command buffer before the render pass. The staging
    // buffers are kept alive here until then, then retired for framesInFlight_.
    struct PendingUpload {
        Buffer       stage;                 // one host staging buffer (verts then indices)
        VkDeviceSize vtxDst = 0, vtxSize = 0; // -> arena vertex buffer (src offset 0)
        VkDeviceSize idxSrc = 0, idxDst = 0, idxSize = 0; // -> arena index buffer
    };
    std::vector<PendingUpload> pendingUploads_;
};

} // namespace vg
