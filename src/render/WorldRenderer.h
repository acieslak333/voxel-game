#pragma once

/**
 * @file WorldRenderer.h
 * @brief Declares WorldRenderer, the main chunk-geometry renderer for the voxel world.
 *
 * Owns the shared vertex/index MeshArena, opaque + water indirect-draw pipelines,
 * per-slot ChunkMesh metadata, and the streaming worker pool that greedy-meshes
 * chunks off the main thread. All Vulkan object creation and draw-command recording
 * happen on the main thread; workers only read the World.
 * @see docs/CODE_INDEX.md
 */

#include "render/Buffer.h"
#include "render/MeshArena.h"
#include "world/ChunkMesher.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <array>
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
class LightAtlas;
class World;

/**
 * @brief Renders the voxel world by greedy-meshing every chunk and issuing
 *        GPU-indirect draw calls from a shared MeshArena.
 *
 * Chunk geometry (opaque + water) lives in one shared vertex arena and one index
 * arena; record() binds them once and issues vkCmdDrawIndexedIndirect per pass.
 * A background worker pool (stream_workers > 0 in world.yaml) meshes chunks off
 * the main thread; workers only READ the World — all Vulkan work happens on the
 * main thread.
 *
 * @warning streamBarrier() MUST be called before any World mutation (recenter,
 *          setBlock) to ensure no worker is mid-read of the World at that point.
 * @note    Per-slot meshVersion_ makes the newest request win; stale worker
 *          results are silently discarded, so re-requesting a mesh is always safe.
 * @see     CLAUDE.md "Threading invariants"
 */
class WorldRenderer {
public:
    /**
     * @brief Construct and upload all chunk meshes. Starts the worker pool if configured.
     * @param ctx            Vulkan device context.
     * @param renderPass     The scene render pass these chunks draw into.
     * @param framesInFlight Number of frames in flight (double/triple buffering).
     * @param world          The world to render; held by reference — must outlive this.
     * @param shaderDir      Directory containing compiled SPIR-V shaders.
     * @param textureDir     Directory containing block texture images.
     */
    WorldRenderer(VulkanContext& ctx, VkRenderPass renderPass, uint32_t framesInFlight,
                  const World& world, const std::string& shaderDir,
                  const std::string& textureDir);
    ~WorldRenderer();

    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    /**
     * @brief Record opaque + water chunk draw commands into @p cmd for this frame.
     *
     * Updates the camera UBO, builds the indirect command arrays, and issues
     * vkCmdDrawIndexedIndirect for the opaque pass then (if needed) the water pass.
     *
     * @param cmd              Command buffer to record into (must be inside the scene render pass).
     * @param frameIndex       Current frame-in-flight index.
     * @param extent           Swapchain / offscreen render extent.
     * @param view             Camera view matrix.
     * @param proj             Camera projection matrix.
     * @param sunDirAmbient    xyz = direction toward the active celestial light; w = ambient floor.
     * @param sunColIntensity  rgb = linear light tint; a = sky-light intensity (1 at noon).
     * @param heldLight        xyz = world position of held emitter; w = radius in blocks (0 = off).
     * @param heldLightCol     rgb = linear colour of held light; a = intensity (0..1).
     */
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity,
                const glm::vec4& heldLight, const glm::vec4& heldLightCol);

    /// Affine texture-warp flag (PS1 style), folded into the chunk UBO's misc.z (0 = off, 1 = on).
    void setRetro(float affine) { retroAffine_ = affine; }

    /**
     * @brief Remesh a single chunk after a block edit. Main-thread, no GPU drain.
     * @param cx  Chunk X coordinate.
     * @param cy  Chunk Y coordinate.
     * @param cz  Chunk Z coordinate.
     * @note Uses the deferred upload path (recordPendingUploads) — no vkDeviceWaitIdle (REVIEW R4).
     */
    void remeshChunk(int cx, int cy, int cz);

    /**
     * @brief Remesh a set of chunks after a block edit. Main-thread, no GPU drain.
     * @param chunks  List of chunk coordinates returned by World::setBlock.
     * @note Uses the deferred upload path — no vkDeviceWaitIdle per edit (REVIEW R4).
     */
    void remeshChunks(const std::vector<glm::ivec3>& chunks);

    /**
     * @brief Rebuild every chunk's mesh with a single GPU drain.
     *
     * Required when baked vertex data changes globally, e.g. light falloff tunables,
     * since lighting lives in per-vertex attributes rather than a shader uniform.
     */
    void remeshAll();

    // --- Streaming remesh API (App calls these) ------------------------------

    /**
     * @brief Block until all mesh workers have finished their current job.
     *
     * MUST be called before any World mutation (recenter, setBlock) to prevent
     * workers reading torn chunk or light data. No-op when no workers are running.
     *
     * @warning Skipping this call before a World mutation is a data race.
     */
    void streamBarrier();

    /**
     * @brief Enqueue chunks for (re)meshing via the worker pool or the single-thread backlog.
     * @param chunks  Chunk coordinates to remesh.
     */
    void streamRemesh(const std::vector<glm::ivec3>& chunks);

    /**
     * @brief Install up to @p budget finished worker meshes into the arena this frame.
     * @param budget  Maximum number of meshes to process this call.
     */
    void streamPump(int budget);

    /**
     * @brief Return true when all worker jobs have completed (pool is idle).
     *
     * App gates recenter() on this so streamBarrier() never blocks the frame —
     * it only drains an already-idle pool. Always true when stream_workers == 0.
     */
    [[nodiscard]] bool streamWorkersIdle() const { return jobsOutstanding_.load() == 0; }

    /**
     * @brief Record pending staging->arena copy commands into @p cmd.
     *
     * Must be called BEFORE the render pass (Renderer::drawFrame's pre-pass hook) so
     * uploads ride the frame's own submit with zero extra GPU sync. No-op if the
     * queue is empty.
     *
     * @param cmd  Pre-pass command buffer, outside any render pass.
     * @note Staging buffers are retired for framesInFlight_+1 frames to avoid
     *       freeing a buffer that an in-flight frame is still reading.
     */
    void recordPendingUploads(VkCommandBuffer cmd);

    /**
     * @brief Dispatch the GPU frustum-cull compute shader (opt-in, VG_GPUCULL).
     *
     * Writes this frame's indirect draw commands by testing each slot's AABB against
     * the frustum stored during the previous record() call. Must be called before
     * the render pass. No-op when GPU cull is disabled.
     *
     * @param cmd        Pre-pass command buffer, outside any render pass.
     * @param frameIndex Current frame-in-flight index.
     */
    void recordCull(VkCommandBuffer cmd, uint32_t frameIndex);

    /// Total slots with resident geometry (opaque or water).
    [[nodiscard]] std::size_t drawnChunkCount() const { return drawnChunks_; }
    /// Total triangle count across all resident chunk meshes.
    [[nodiscard]] std::size_t triangleCount()   const { return totalTriangles_; }
    /// Slots that passed frustum culling and issued a draw this frame.
    [[nodiscard]] std::size_t visibleChunkCount() const { return lastVisibleChunks_; }
    /// Slots skipped as off-screen this frame (VG_FRAME_TIME telemetry).
    [[nodiscard]] std::size_t culledChunkCount()  const { return lastCulledChunks_; }
    /// vkCmdDraw* calls recorded this frame (opaque + water).
    [[nodiscard]] std::size_t drawCallCount()     const { return lastDrawCalls_; }

    /// Block texture array image view, exposed so the UI can draw isometric block icons.
    [[nodiscard]] VkImageView blockTextureView() const;
    /// Block texture array sampler, paired with blockTextureView().
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
        glm::vec4 lightAtlas;   // S7: x = slots/row, yzw = atlas texel dims (frag sampling)
    };
    float animTime_ = 0.0f; // accumulated per recorded frame; drives the sway/wave clock
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
        int       lightSlot   = -1;        // S7: this chunk's slot in the light atlas (-1 = none)
    };
    // Per-slot mesh metadata the GPU cull shader reads (mirrors chunk_cull.comp's
    // ChunkMeta, std430): aabb = world bounds; opaque/water = (indexCount, firstIndex,
    // vertexOffset-as-uint, slot) for that range's indirect command.
    struct ChunkMeta {
        glm::vec4  aabbMin, aabbMax;
        glm::uvec4 opaque, water;
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

    // The single chunk-mesh chokepoint every build path funnels through (greedy
    // meshing). Only READS the world, so this is safe on the worker pool.
    [[nodiscard]] MeshData meshChunkData(int cx, int cy, int cz) const;

    [[nodiscard]] int chunkIndex(int cx, int cy, int cz) const;
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n);
    // Per-frame GPU-driven draw resources: the chunk draw-data SSBO (binding 2) and
    // the opaque/water indirect command buffers. Created once counts_ is known.
    void createGpuDrivenBuffers(uint32_t n);

    VulkanContext& ctx_;
    const World&   world_;

    std::unique_ptr<TextureArray> textures_;
    std::unique_ptr<LightAtlas>   lightAtlas_;          // S7: per-chunk-slot light volume
    std::unique_ptr<Pipeline>     pipeline_;            // opaque terrain
    std::unique_ptr<Pipeline>     waterPipeline_;       // translucent water (2nd pass)

    // S7: chunks whose light block must be (re)written into their atlas slot. Filled
    // by swapChunkBuffers when a chunk gains/changes geometry; flushed into the frame
    // command buffer by recordPendingUploads (so the upload rides the frame's submit).
    struct PendingLight { int slot, cx, cy, cz; };
    std::vector<PendingLight> pendingLightWrites_;
    // Fill `out` (kPad³ RGBA8, x fastest) with chunk (cx,cy,cz)'s padded light block:
    // RGBA = [sky<<4 | block, hueR, hueG, hueB] per voxel, incl. a 1-voxel border.
    void buildLightBlock(int cx, int cy, int cz, unsigned char* out) const;

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

    // --- GPU-driven culling (chunk_cull.comp; opt-in via VG_GPUCULL) ----------
    // A compute pass tests each slot's AABB against the frustum and writes its
    // opaque/water indirect commands (instanceCount 0 = culled/empty), so the draw is
    // one vkCmdDrawIndexedIndirect over the whole dense per-slot array. Default off —
    // the CPU cull above is the proven path and there is no measured win (GPU idle).
    bool                         gpuCull_ = false;
    VkDescriptorSetLayout        cullSetLayout_      = VK_NULL_HANDLE;
    VkPipelineLayout             cullPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline                   cullPipeline_       = VK_NULL_HANDLE;
    VkDescriptorPool             cullPool_           = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> cullSets_;     // per frame in flight
    std::vector<Buffer>          metaBuffers_;  // per-slot ChunkMeta SSBO (per frame)
    std::vector<ChunkMeta>       metaCpu_;      // CPU mirror, refilled by swapChunkBuffers
    std::array<glm::vec4, 6>     lastFrustum_{}; // record() stores it; recordCull reads it
    void createCullResources(const std::string& shaderDir, uint32_t n);
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
