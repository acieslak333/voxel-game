#pragma once

#include "render/Buffer.h"
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
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                const glm::mat4& view, const glm::mat4& proj,
                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity);

    // Rebuild one chunk's mesh and swap its GPU buffers, leaving every other
    // chunk untouched. Call after the world's blocks change (see World::setBlock,
    // which returns exactly the chunk coordinates to pass here).
    void remeshChunk(int cx, int cy, int cz);

    // Remesh several chunks with a single GPU drain (cheaper than calling
    // remeshChunk per chunk, which waits each time). Pass the list World::setBlock
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
    };
    float animTime_ = 0.0f; // accumulated per recorded frame; drives the sway/wave clock
    // Per-draw push constant: chunk model matrix + params (params.x = output
    // alpha — 1 for the opaque pass, < 1 for the translucent water pass).
    struct PushConstants {
        glm::mat4 model;
        glm::vec4 params{1.0f};
    };
    static_assert(sizeof(PushConstants) == 20 * sizeof(float), "matches Pipeline::kPushConstantSize");
    // GPU buffers + placement for one chunk's mesh. An empty chunk keeps a slot
    // here with indexCount == 0 and null buffers (skipped while drawing). One
    // allocation holds, in order: opaque vertices, water vertices, opaque indices,
    // water indices. Water indices are 0-based into the water vertices, so the
    // water draw passes firstWaterVertex as its vkCmdDrawIndexed vertexOffset.
    struct ChunkMesh {
        Buffer       meshBuffer;           // all vertices then all indices, ONE allocation
        VkDeviceSize indexOffset = 0;      // byte offset of the opaque index data
        uint32_t     indexCount = 0;       // opaque index count
        VkDeviceSize waterIndexOffset = 0; // byte offset of the water index data
        uint32_t     waterIndexCount = 0;  // translucent water index count
        int32_t      firstWaterVertex = 0; // vertexOffset for water draws (= opaque vertex count)
        glm::vec3    worldPos{0.0f};
    };
    // A queued mesh job / its finished result, exchanged with the worker pool.
    struct MeshJob    { int cx, cy, cz; std::uint64_t version; };
    struct MeshResult { int cx, cy, cz; std::uint64_t version; MeshData data; };

    void buildMeshes();
    // (Re)mesh chunk (cx,cy,cz) and (re)build its buffers into its slot, updating
    // the triangle/drawn-chunk tallies. Shared by buildMeshes() and remeshChunk().
    void uploadChunkMesh(int cx, int cy, int cz);
    // Apply an already-built mesh to a slot. deferOldBuffers=true retires the old
    // GPU buffers for framesInFlight_+1 frames instead of freeing them now (an
    // in-flight frame may still reference them) — required when uploading from the
    // worker path, which can't issue a device-idle wait every frame.
    void installMesh(int cx, int cy, int cz, MeshData&& mesh, bool deferOldBuffers);
    // Swap an already-built combined mesh buffer into a slot (retire old, update
    // tallies). The buffer holds vertices then indices; indexOffset is where the
    // indices start.
    void swapChunkBuffers(int cx, int cy, int cz, Buffer&& meshBuffer,
                          VkDeviceSize indexOffset, uint32_t indexCount,
                          VkDeviceSize waterIndexOffset, uint32_t waterIndexCount,
                          int32_t firstWaterVertex, bool deferOldBuffers);
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
    [[nodiscard]] int chunkIndex(int cx, int cy, int cz) const;
    void createUniformBuffers(uint32_t n);
    void createDescriptorSets(uint32_t n);

    VulkanContext& ctx_;
    const World&   world_;

    std::unique_ptr<TextureArray> textures_;
    std::unique_ptr<Pipeline>     pipeline_;            // opaque terrain
    std::unique_ptr<Pipeline>     waterPipeline_;       // translucent water (2nd pass)

    std::vector<ChunkMesh> meshes_;          // indexed by chunkIndex(); may be empty
    std::deque<glm::ivec3> pendingRemesh_;    // streaming remesh backlog (drained per frame)
    glm::ivec3             counts_{0};        // chunk grid dimensions
    std::size_t            drawnChunks_    = 0; // slots with geometry
    std::size_t            totalTriangles_ = 0;

    std::vector<Buffer>          uniformBuffers_; // one per frame in flight
    VkDescriptorPool             descriptorPool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> descriptorSets_;

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
        VkBuffer     dst  = VK_NULL_HANDLE; // the chunk's combined device buffer
        VkDeviceSize size = 0;              // total bytes (vertices + indices)
        Buffer       stage;                 // one host staging buffer
    };
    std::vector<PendingUpload> pendingUploads_;
};

} // namespace vg
