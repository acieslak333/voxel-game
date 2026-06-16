#include "render/WorldRenderer.h"

#include "render/Pipeline.h"
#include "render/TextureArray.h"
#include "render/VulkanContext.h"
#include "world/ChunkMesher.h"
#include "world/World.h"
#include "utilities/hash/Hash.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <execution>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace vg {

namespace {

// Drop the calling (mesh-worker) thread below normal scheduling priority. A
// window-step enqueues a burst of greedy-mesh jobs; with the workers at normal
// priority they saturate every core and deschedule the main thread, which was
// most of the residual ~20-30 ms chunk-boundary frame spike (REVIEW O4). Below
// the main thread the OS hands cores back to rendering and the workers simply
// fill the slack. Windows-only for now (the measured platform); a no-op
// elsewhere — POSIX has no portable per-thread nice, and the builds there are
// untested for this knob.
void lowerWorkerThreadPriority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

// Six frustum planes (a,b,c,d) extracted from a clip matrix (Gribb-Hartmann),
// each oriented so a point is inside the half-space when a*x+b*y+c*z+d >= 0.
// The near plane is `row2` because the build forces Vulkan's [0,1] depth
// (GLM_FORCE_DEPTH_ZERO_TO_ONE); the Y-flip in `proj` only relabels top/bottom,
// which is harmless since all six planes are tested. Planes are left unnormalised
// — only the sign of the half-space test matters, so the scale is irrelevant.
std::array<glm::vec4, 6> extractFrustum(const glm::mat4& clip) {
    auto row = [&](int i) { return glm::vec4(clip[0][i], clip[1][i], clip[2][i], clip[3][i]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    return {r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2};
}

// Conservative AABB-vs-frustum test: the box is culled only when it lies wholly
// outside one plane (its "positive vertex" — the corner farthest along the plane
// normal — is still on the outside). Never culls a box that's even partly
// visible, so it can't pop on-screen geometry.
bool aabbInFrustum(const std::array<glm::vec4, 6>& frustum, const glm::vec3& mn,
                   const glm::vec3& mx) {
    for (const glm::vec4& p : frustum) {
        const glm::vec3 pv(p.x >= 0.0f ? mx.x : mn.x, p.y >= 0.0f ? mx.y : mn.y,
                           p.z >= 0.0f ? mx.z : mn.z);
        if (p.x * pv.x + p.y * pv.y + p.z * pv.z + p.w < 0.0f) {
            return false;
        }
    }
    return true;
}

// One chunk's combined buffer holds, in order: opaque vertices, water vertices,
// opaque indices, water indices. Both index ranges live in the same allocation;
// the water draw passes firstWaterVertex as its vertexOffset (its indices are
// 0-based into the water vertices). This computes the byte offsets/counts.
struct MeshLayout {
    VkDeviceSize indexOffset      = 0; // opaque indices (4-byte aligned past the vertices)
    VkDeviceSize waterIndexOffset = 0; // water indices (right after the opaque indices)
    VkDeviceSize total            = 0;
    uint32_t     indexCount       = 0;
    uint32_t     waterIndexCount  = 0;
    int32_t      firstWaterVertex = 0;
};

MeshLayout computeLayout(const MeshData& m) {
    MeshLayout L{};
    const VkDeviceSize ov = sizeof(Vertex) * m.vertices.size();
    const VkDeviceSize wv = sizeof(Vertex) * m.waterVertices.size();
    L.indexOffset      = (ov + wv + 3) & ~VkDeviceSize(3); // 4-byte index alignment
    L.waterIndexOffset = L.indexOffset + sizeof(uint32_t) * m.indices.size();
    L.total            = L.waterIndexOffset + sizeof(uint32_t) * m.waterIndices.size();
    L.indexCount       = static_cast<uint32_t>(m.indices.size());
    L.waterIndexCount  = static_cast<uint32_t>(m.waterIndices.size());
    L.firstWaterVertex = static_cast<int32_t>(m.vertices.size());
    return L;
}

// Lay the four sub-arrays into a destination buffer per `L` (each copy guarded
// so an empty sub-array doesn't memcpy from a null data()).
void writeBlob(char* p, const MeshData& m, const MeshLayout& L) {
    const VkDeviceSize ov = sizeof(Vertex) * m.vertices.size();
    const VkDeviceSize wv = sizeof(Vertex) * m.waterVertices.size();
    if (ov) std::memcpy(p, m.vertices.data(), static_cast<size_t>(ov));
    if (wv) std::memcpy(p + ov, m.waterVertices.data(), static_cast<size_t>(wv));
    if (!m.indices.empty()) {
        std::memcpy(p + L.indexOffset, m.indices.data(), sizeof(uint32_t) * m.indices.size());
    }
    if (!m.waterIndices.empty()) {
        std::memcpy(p + L.waterIndexOffset, m.waterIndices.data(),
                    sizeof(uint32_t) * m.waterIndices.size());
    }
}

} // namespace

WorldRenderer::WorldRenderer(VulkanContext& ctx, VkRenderPass renderPass,
                             uint32_t framesInFlight, const World& world,
                             const std::string& shaderDir, const std::string& textureDir)
    : ctx_(ctx), world_(world) {
    textures_ = std::make_unique<TextureArray>(ctx_, world_.registry().texturePaths(),
                                               textureDir);
    // GPU-driven (indirect) vertex shader: per-chunk translation comes from the
    // draw-data SSBO (binding 2) via gl_InstanceIndex, not a push-constant model.
    pipeline_ = std::make_unique<Pipeline>(ctx_, renderPass,
                                           shaderDir + "/chunk_indirect.vert.spv",
                                           shaderDir + "/chunk.frag.spv");
    // Same shaders, but the translucent flavor (alpha blend on, depth-write off)
    // for the second, water pass — drawn after opaque so the seabed shows through.
    waterPipeline_ = std::make_unique<Pipeline>(ctx_, renderPass,
                                                shaderDir + "/chunk_indirect.vert.spv",
                                                shaderDir + "/chunk.frag.spv",
                                                /*translucent=*/true);
    framesInFlight_ = framesInFlight;
    buildMeshes(); // creates arena_, sizes it from counts_, uploads the core window
    createUniformBuffers(framesInFlight);
    createGpuDrivenBuffers(framesInFlight); // draw-data SSBO + indirect buffers
    createDescriptorSets(framesInFlight);   // references drawDataBuffers_
    meshVersion_.assign(meshes_.size(), 0);
    if (world_.streaming() && world_.streamWorkers() > 0) {
        startWorkers(world_.streamWorkers());
    }
    // Hand the outer rings buildMeshes() skipped to the streaming pipeline (worker
    // pool, or the main-thread backlog without workers). streamPump() pumps at a
    // boosted budget until this initial backlog drains.
    if (!deferredStartup_.empty()) {
        streamRemesh(deferredStartup_);
        deferredStartup_.clear();
        deferredStartup_.shrink_to_fit();
        startupMelt_ = true;
    }
}

WorldRenderer::~WorldRenderer() {
    stopWorkers(); // join threads before tearing down anything they read
    if (descriptorPool_) {
        vkDestroyDescriptorPool(ctx_.device(), descriptorPool_, nullptr);
    }
}

int WorldRenderer::chunkIndex(int cx, int cy, int cz) const {
    return floormod(cx, counts_.x) +
           counts_.x * (floormod(cy, counts_.y) + counts_.y * floormod(cz, counts_.z));
}

ChunkMesher::NeighborSampler WorldRenderer::makeSampler(int cx, int cy, int cz) const {
    const int baseX = cx * Chunk::kSize;
    const int baseY = cy * Chunk::kSize;
    const int baseZ = cz * Chunk::kSize;
    // The sampler is consumed synchronously by greedyMesh(), so capturing the
    // world by reference (via this) is safe.
    return [this, baseX, baseY, baseZ](int lx, int ly, int lz) {
        return world_.blockAt(baseX + lx, baseY + ly, baseZ + lz);
    };
}

ChunkMesher::LightSampler WorldRenderer::makeLightSampler(int cx, int cy, int cz) const {
    const int baseX = cx * Chunk::kSize;
    const int baseY = cy * Chunk::kSize;
    const int baseZ = cz * Chunk::kSize;
    return [this, baseX, baseY, baseZ](int lx, int ly, int lz) -> ChunkMesher::LightSample {
        const int wx = baseX + lx, wy = baseY + ly, wz = baseZ + lz;
        return {world_.skyLightAt(wx, wy, wz), world_.blockLightAt(wx, wy, wz),
                world_.blockLightColorAt(wx, wy, wz)};
    };
}

ChunkMesher::TintSampler WorldRenderer::makeTintSampler(int cx, int /*cy*/, int cz) const {
    const int baseX = cx * Chunk::kSize;
    const int baseZ = cz * Chunk::kSize;
    return [this, baseX, baseZ](int lx, int lz, uint16_t id) -> glm::vec3 {
        if (!world_.isVegTintable(id)) return glm::vec3(1.0f); // common case: no tint
        return world_.vegTintAt(baseX + lx, baseZ + lz);
    };
}

MeshData WorldRenderer::meshChunkData(int cx, int cy, int cz) const {
    return ChunkMesher::greedyMesh(world_.chunk(cx, cy, cz), world_.registry(),
                                   makeSampler(cx, cy, cz), makeLightSampler(cx, cy, cz),
                                   /*smoothLighting=*/true,
                                   glm::ivec3(cx, cy, cz) * kChunkSize,
                                   makeTintSampler(cx, cy, cz));
}

VkImageView WorldRenderer::blockTextureView() const { return textures_->view(); }
VkSampler   WorldRenderer::blockTextureSampler() const { return textures_->sampler(); }

void WorldRenderer::uploadChunkMesh(int cx, int cy, int cz) {
    // Smooth lighting (per-corner AO + light) is always on; the mesher's flat
    // mode remains only as a debugging path. Main-thread meshing → free old
    // buffers immediately (callers issue a device-idle wait first).
    MeshData mesh = meshChunkData(cx, cy, cz);
    installMesh(cx, cy, cz, std::move(mesh), /*deferOldBuffers=*/false);
}

// Allocate an arena span for `mesh`, fill `stage` with its blob, and report the
// placement + the two copy regions (vertices -> vertex arena, indices -> index
// arena) the caller records. Element offsets in the placement are exactly the
// indirect draw's vertexOffset/firstIndex. `mesh` must be non-empty.
WorldRenderer::MeshPlacement WorldRenderer::stageMesh(
    const MeshData& mesh, Buffer& stage, VkDeviceSize& vtxDst, VkDeviceSize& vtxSize,
    VkDeviceSize& idxSrc, VkDeviceSize& idxDst, VkDeviceSize& idxSize) {
    const MeshLayout L = computeLayout(mesh);
    const uint32_t totalVerts =
        static_cast<uint32_t>(mesh.vertices.size() + mesh.waterVertices.size());
    const uint32_t totalIdx =
        static_cast<uint32_t>(mesh.indices.size() + mesh.waterIndices.size());

    const MeshArena::Alloc a = arena_->allocate(totalVerts, totalIdx); // throws if full
    MeshPlacement p;
    p.arena           = a;
    p.indexCount      = L.indexCount;
    p.firstIndex      = a.firstIndex;
    p.baseVertex      = static_cast<int32_t>(a.baseVertex);
    p.waterIndexCount = L.waterIndexCount;
    p.waterFirstIndex = a.firstIndex + L.indexCount;
    p.waterBaseVertex = a.baseVertex + L.firstWaterVertex;

    // Staging blob is [opaqueV | waterV | pad | opaqueI | waterI]; the vertices
    // occupy [0, L.indexOffset minus pad) and the indices [L.indexOffset, L.total).
    stage = Buffer(ctx_, L.total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    writeBlob(static_cast<char*>(stage.map()), mesh, L);
    vtxDst  = arena_->vertexByteOffset(a.baseVertex);
    vtxSize = static_cast<VkDeviceSize>(totalVerts) * MeshArena::kVertexStride;
    idxSrc  = L.indexOffset;
    idxDst  = arena_->indexByteOffset(a.firstIndex);
    idxSize = static_cast<VkDeviceSize>(totalIdx) * MeshArena::kIndexStride;
    return p;
}

void WorldRenderer::swapChunkBuffers(int cx, int cy, int cz, const MeshPlacement& place,
                                     bool deferOldSpan) {
    const uint32_t slot = static_cast<uint32_t>(chunkIndex(cx, cy, cz));
    ChunkMesh& cm = meshes_[slot];

    // Drop this slot's previous contribution before rebuilding it. A slot counts
    // as a drawn chunk if it has either opaque or water geometry.
    const bool hadGeometry = cm.indexCount > 0 || cm.waterIndexCount > 0;
    const bool hadWater    = cm.waterIndexCount > 0;
    totalTriangles_ -= (cm.indexCount + cm.waterIndexCount) / 3;
    if (cm.arena.valid) {
        if (deferOldSpan) {
            // An in-flight frame may still read the old span; retire it for a few
            // frames instead of freeing now. tickRetired() reaps it.
            retiredAllocs_.emplace_back(static_cast<int>(framesInFlight_) + 1, cm.arena);
        } else {
            arena_->free(cm.arena);
        }
    }
    if (hadGeometry) {
        --drawnChunks_;
    }
    cm.arena           = place.arena;
    cm.indexCount      = place.indexCount;
    cm.firstIndex      = place.firstIndex;
    cm.baseVertex      = place.baseVertex;
    cm.waterIndexCount = place.waterIndexCount;
    cm.waterFirstIndex = place.waterFirstIndex;
    cm.waterBaseVertex = place.waterBaseVertex;
    const bool nowGeometry = place.indexCount > 0 || place.waterIndexCount > 0;
    const bool nowWater    = place.waterIndexCount > 0;
    if (nowGeometry) {
        ++drawnChunks_;
    }
    drawnWaterChunks_ += static_cast<std::size_t>(nowWater) - static_cast<std::size_t>(hadWater);
    // Maintain the compact draw list (REVIEW R8): add this slot when it gains
    // geometry, swap-remove it when it loses geometry. drawListPos is its index in
    // drawList_ so removal is O(1).
    if (nowGeometry && cm.drawListPos < 0) {
        cm.drawListPos = static_cast<int>(drawList_.size());
        drawList_.push_back(slot);
    } else if (!nowGeometry && cm.drawListPos >= 0) {
        const int pos = cm.drawListPos;
        const uint32_t moved = drawList_.back();
        drawList_[static_cast<size_t>(pos)] = moved;
        meshes_[moved].drawListPos = pos;
        drawList_.pop_back();
        cm.drawListPos = -1;
    }
    cm.worldPos = glm::vec3(cx, cy, cz) * static_cast<float>(Chunk::kSize);
    // Keep the draw-data mirror (uploaded to the per-frame SSBO in record()) in
    // sync; the indirect vertex shader reads this by slot via gl_InstanceIndex.
    // Sized in buildMeshes() before any swap, so the index is always valid.
    if (slot < drawDataCpu_.size()) {
        drawDataCpu_[slot] = glm::vec4(cm.worldPos, 0.0f);
    }
    totalTriangles_ += (cm.indexCount + cm.waterIndexCount) / 3;
}

void WorldRenderer::installMesh(int cx, int cy, int cz, MeshData&& mesh, bool deferOldBuffers) {
    // Main-thread immediate path (one GPU submit+wait) — fine for the rare, small
    // sync remeshes (block edits, falloff). The streaming path uses installMeshBatch().
    // Geometry is copied into the shared arena at the placement's offsets.
    if (mesh.empty()) {
        swapChunkBuffers(cx, cy, cz, MeshPlacement{}, deferOldBuffers);
        return;
    }
    Buffer stage;
    VkDeviceSize vtxDst, vtxSize, idxSrc, idxDst, idxSize;
    const MeshPlacement p = stageMesh(mesh, stage, vtxDst, vtxSize, idxSrc, idxDst, idxSize);
    VkCommandBuffer cmd = ctx_.beginSingleTimeCommands();
    if (vtxSize) {
        VkBufferCopy c{}; c.srcOffset = 0;      c.dstOffset = vtxDst; c.size = vtxSize;
        vkCmdCopyBuffer(cmd, stage.handle(), arena_->vertexBuffer(), 1, &c);
    }
    if (idxSize) {
        VkBufferCopy c{}; c.srcOffset = idxSrc; c.dstOffset = idxDst; c.size = idxSize;
        vkCmdCopyBuffer(cmd, stage.handle(), arena_->indexBuffer(), 1, &c);
    }
    ctx_.endSingleTimeCommands(cmd); // submit+wait, then stage is freed (RAII)
    swapChunkBuffers(cx, cy, cz, p, deferOldBuffers);
}

void WorldRenderer::installMeshBatch(std::vector<MeshResult>& batch) {
    // Build the device buffers + host staging now (no GPU work yet) and swap the
    // device buffers into their slots. The staging->device COPY is deferred to
    // recordPendingUploads(), which records it into the frame's own command buffer
    // before the render pass — so the upload rides the frame's submit with zero
    // extra device sync. (createDeviceLocal's per-buffer submit+wait was the lag.)
    for (MeshResult& r : batch) {
        const size_t slot = static_cast<size_t>(chunkIndex(r.cx, r.cy, r.cz));
        if (meshVersion_[slot] != r.version) {
            continue; // superseded by a newer request — discard this stale mesh
        }
        if (r.data.empty()) {
            swapChunkBuffers(r.cx, r.cy, r.cz, MeshPlacement{}, /*deferOldSpan=*/true);
            continue;
        }
        // Stage the blob + reserve the arena span now (no GPU work yet); the
        // staging->arena copies are recorded into the frame's command buffer by
        // recordPendingUploads(), so the upload rides the frame's own submit.
        PendingUpload up;
        const MeshPlacement p =
            stageMesh(r.data, up.stage, up.vtxDst, up.vtxSize, up.idxSrc, up.idxDst, up.idxSize);
        pendingUploads_.push_back(std::move(up));
        swapChunkBuffers(r.cx, r.cy, r.cz, p, /*deferOldSpan=*/true);
    }
}

void WorldRenderer::recordPendingUploads(VkCommandBuffer cmd) {
    if (pendingUploads_.empty()) {
        return;
    }
    for (PendingUpload& p : pendingUploads_) {
        // Two copies per chunk: vertices -> vertex arena, indices -> index arena.
        if (p.vtxSize) {
            VkBufferCopy c{}; c.srcOffset = 0;      c.dstOffset = p.vtxDst; c.size = p.vtxSize;
            vkCmdCopyBuffer(cmd, p.stage.handle(), arena_->vertexBuffer(), 1, &c);
        }
        if (p.idxSize) {
            VkBufferCopy c{}; c.srcOffset = p.idxSrc; c.dstOffset = p.idxDst; c.size = p.idxSize;
            vkCmdCopyBuffer(cmd, p.stage.handle(), arena_->indexBuffer(), 1, &c);
        }
    }
    // One barrier so the transfer writes are visible to the vertex/index fetch in
    // the render pass that follows in this same command buffer.
    VkMemoryBarrier barrier{};
    barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    // Keep the staging buffers alive until this frame's GPU work (the copy) is done.
    const int life = static_cast<int>(framesInFlight_) + 1;
    for (PendingUpload& p : pendingUploads_) {
        retired_.emplace_back(life, std::move(p.stage));
    }
    pendingUploads_.clear();
}

void WorldRenderer::tickRetired() {
    // Age + reap deferred staging-buffer frees (RAII releases the Buffer).
    if (!retired_.empty()) {
        for (auto& r : retired_) --r.first;
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                      [](const std::pair<int, Buffer>& r) { return r.first <= 0; }),
                       retired_.end());
    }
    // Age + reap deferred arena-span frees: safe to return to the arena now that
    // framesInFlight_+1 frames have elapsed (no in-flight frame still reads them).
    if (!retiredAllocs_.empty()) {
        for (auto& r : retiredAllocs_) --r.first;
        retiredAllocs_.erase(
            std::remove_if(retiredAllocs_.begin(), retiredAllocs_.end(),
                           [this](std::pair<int, MeshArena::Alloc>& r) {
                               if (r.first <= 0) { arena_->free(r.second); return true; }
                               return false;
                           }),
            retiredAllocs_.end());
    }
}

void WorldRenderer::buildMeshes() {
    counts_ = world_.chunkCounts();
    const size_t numSlots = static_cast<size_t>(counts_.x) * counts_.y * counts_.z;
    meshes_.resize(numSlots);
    drawDataCpu_.assign(numSlots, glm::vec4(0.0f)); // per-slot world pos for the SSBO

    // Size the shared arena from the slot count. These per-slot budgets are the one
    // OOM risk of the GPU-driven path: allocate() throws if a window's live geometry
    // exceeds them (dense terrain + large view_radius). kArenaVertsPerSlot/IdxPerSlot
    // are generous averages (most slots are empty sky or solid interior with few
    // faces); raise them if MeshArena throws. TODO(R7): move to world.yaml arena_tuning.
    constexpr uint32_t kArenaVertsPerSlot = 1024;
    constexpr uint32_t kArenaIdxPerSlot   = 1536; // 1.5 indices per vertex (6 per quad / 4 verts)
    arena_ = std::make_unique<MeshArena>(
        ctx_, static_cast<uint32_t>(numSlots) * kArenaVertsPerSlot,
        static_cast<uint32_t>(numSlots) * kArenaIdxPerSlot);

    // Greedy-meshing thousands of chunks one-at-a-time on the main thread was the
    // bulk of startup. greedyMesh() only READS the world, so mesh every chunk in
    // parallel into CPU mesh data first, then install serially (Vulkan is main-
    // thread only). The window is generated at origin {0,0,0} at construction.
    // When streaming, only the columns within kCoreRadius of the window centre
    // (where the player spawns) are meshed + uploaded synchronously here; the
    // outer rings are handed to the streaming pipeline at the end of the ctor
    // (nearest-first) and melt in over the first frames — the same machinery a
    // window recenter uses. The full window still fills in either way; startup
    // just stops paying ~85% of the mesh+upload bill before the first frame.
    const int kCoreRadius = world_.config().streamCoreRadius; // world.yaml stream_tuning (R7)
    const glm::ivec3 c0 = counts_ / 2;
    std::vector<glm::ivec3> coords;
    coords.reserve(meshes_.size());
    for (int cz = 0; cz < counts_.z; ++cz) {
        for (int cy = 0; cy < counts_.y; ++cy) {
            for (int cx = 0; cx < counts_.x; ++cx) {
                const bool core = std::max(std::abs(cx - c0.x), std::abs(cz - c0.z)) <=
                                  kCoreRadius;
                if (world_.streaming() && !core) {
                    deferredStartup_.push_back({cx, cy, cz});
                } else {
                    coords.push_back({cx, cy, cz});
                }
            }
        }
    }
    // Nearest columns first, so the melt-in grows outward from the player.
    std::sort(deferredStartup_.begin(), deferredStartup_.end(),
              [c0](const glm::ivec3& a, const glm::ivec3& b) {
                  const int da = std::max(std::abs(a.x - c0.x), std::abs(a.z - c0.z));
                  const int db = std::max(std::abs(b.x - c0.x), std::abs(b.z - c0.z));
                  return da < db;
              });
    std::vector<MeshData> meshed(coords.size());
    const auto _t0 = std::chrono::steady_clock::now();
    std::transform(std::execution::par, coords.begin(), coords.end(), meshed.begin(),
                   [this](const glm::ivec3& c) {
                       return meshChunkData(c.x, c.y, c.z);
                   });
    const bool _timeMesh = std::getenv("VG_MESH_TIME") != nullptr;
    const auto _t1 = std::chrono::steady_clock::now();
    if (_timeMesh) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(_t1 - _t0).count();
        std::printf("[mesh] parallel greedyMesh: %lldms (%zu chunks)\n",
                    static_cast<long long>(ms), coords.size());
    }

    // Upload in slices: record every chunk's staging->device copy into ONE command
    // buffer and submit+wait once per slice, instead of installMesh()'s submit+wait
    // PER chunk (thousands of GPU round-trips were the rest of the startup cost).
    // The slice bound keeps live allocations (accumulating device buffers + this
    // slice's staging) under Vulkan's maxMemoryAllocationCount (~4096).
    const size_t kSlice = static_cast<size_t>(world_.config().streamUploadSlice); // R7
    for (size_t start = 0; start < coords.size(); start += kSlice) {
        const size_t end = std::min(start + kSlice, coords.size());
        std::vector<Buffer> staging; // freed at slice end (after the copy completes)
        staging.reserve(end - start);
        VkCommandBuffer cmd = ctx_.beginSingleTimeCommands();
        for (size_t i = start; i < end; ++i) {
            const glm::ivec3& c = coords[i];
            MeshData& m = meshed[i];
            if (m.empty()) {
                swapChunkBuffers(c.x, c.y, c.z, MeshPlacement{}, /*deferOldSpan=*/false);
                continue;
            }
            // Stage + reserve the arena span, then record the two arena copies into
            // this slice's command buffer (vertices -> vertex arena, indices -> index
            // arena). Staging stays alive until endSingleTimeCommands completes.
            Buffer stage;
            VkDeviceSize vtxDst, vtxSize, idxSrc, idxDst, idxSize;
            const MeshPlacement p =
                stageMesh(m, stage, vtxDst, vtxSize, idxSrc, idxDst, idxSize);
            if (vtxSize) {
                VkBufferCopy cp{}; cp.srcOffset = 0;      cp.dstOffset = vtxDst; cp.size = vtxSize;
                vkCmdCopyBuffer(cmd, stage.handle(), arena_->vertexBuffer(), 1, &cp);
            }
            if (idxSize) {
                VkBufferCopy cp{}; cp.srcOffset = idxSrc; cp.dstOffset = idxDst; cp.size = idxSize;
                vkCmdCopyBuffer(cmd, stage.handle(), arena_->indexBuffer(), 1, &cp);
            }
            staging.push_back(std::move(stage));
            swapChunkBuffers(c.x, c.y, c.z, p, /*deferOldSpan=*/false);
        }
        ctx_.endSingleTimeCommands(cmd); // one submit+wait for the whole slice
    }
    if (_timeMesh) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - _t1).count();
        std::printf("[mesh] GPU upload: %lldms\n", static_cast<long long>(ms));
        // Pool stats: thousands of chunk buffers now ride a handful of device
        // allocations (vs one vkAllocateMemory each — the maxMemoryAllocationCount
        // ceiling, REVIEW O5).
        const GpuAllocator& a = ctx_.allocator();
        std::printf("[mesh] gpu pool: %u blocks, %.1f MiB allocated, %.1f MiB live\n",
                    a.blockCount(), static_cast<double>(a.bytesAllocated()) / (1024.0 * 1024.0),
                    static_cast<double>(a.bytesReserved()) / (1024.0 * 1024.0));
        // Arena occupancy: live vs capacity for each of the two shared buffers
        // (watch this approach 100% — that's when allocate() would start throwing).
        std::printf("[mesh] arena: verts %u/%u (%.0f%%), idx %u/%u (%.0f%%)\n",
                    arena_->vertexUsed(), arena_->vertexCapacity(),
                    100.0 * arena_->vertexUsed() / std::max(1u, arena_->vertexCapacity()),
                    arena_->indexUsed(), arena_->indexCapacity(),
                    100.0 * arena_->indexUsed() / std::max(1u, arena_->indexCapacity()));
    }
    std::cout << "[world] meshed " << drawnChunks_ << " non-empty chunks, "
              << totalTriangles_ << " triangles";
    if (!deferredStartup_.empty()) {
        std::cout << " (" << deferredStartup_.size() << " outer chunks streaming in)";
    }
    std::cout << "\n";
}

void WorldRenderer::meshChunksDeferred(const std::vector<glm::ivec3>& chunks) {
    if (chunks.empty()) {
        return;
    }
    // Mesh on the main thread (the edit is visible this frame) but install through
    // the deferred buffer + frame-integrated upload path — NO vkDeviceWaitIdle. The
    // old buffers are retired for framesInFlight_+1 frames (a frame in flight may
    // still reference them) and the staging->device copy rides the next frame's
    // command buffer via recordPendingUploads (REVIEW R4). Bumping each slot's
    // version makes any in-flight worker result for it stale (discarded on install).
    std::vector<MeshResult> batch;
    batch.reserve(chunks.size());
    for (const glm::ivec3& c : chunks) {
        bool dup = false; // a chunk listed twice (e.g. two edits) only needs meshing once
        for (const MeshResult& r : batch) {
            if (r.cx == c.x && r.cy == c.y && r.cz == c.z) { dup = true; break; }
        }
        if (dup || meshVersion_.empty()) {
            continue;
        }
        const std::uint64_t v = ++meshVersion_[static_cast<size_t>(chunkIndex(c.x, c.y, c.z))];
        MeshData md = meshChunkData(c.x, c.y, c.z);
        batch.push_back(MeshResult{c.x, c.y, c.z, v, std::move(md)});
    }
    if (!batch.empty()) {
        installMeshBatch(batch);
    }
}

void WorldRenderer::remeshChunk(int cx, int cy, int cz) {
    meshChunksDeferred({{cx, cy, cz}});
}

void WorldRenderer::remeshChunks(const std::vector<glm::ivec3>& chunks) {
    meshChunksDeferred(chunks);
}

void WorldRenderer::remeshAll() {
    // Lighting is baked into the vertex data, so a global lighting change (e.g. a new
    // falloff) means every loaded chunk must be rebuilt. A 33x33x16 window is
    // thousands of chunks; the old per-chunk submit+wait path froze the frame for
    // seconds (REVIEW R2). Instead reuse the startup melt-in machinery: hand the whole
    // window to streamRemesh (workers mesh in the background, or the no-worker queue
    // drains over frames) and flag startupMelt_ so streamPump uploads the backlog at
    // the boosted rate. The view shows old lighting for the ~1s it takes to melt in,
    // which beats a multi-second stall. streamRemesh stamps each slot's meshVersion_,
    // so any worker result meshed before the lighting change is discarded as stale.
    // The caller is responsible for the streamBarrier + light rewrite that must
    // precede this (App::drainBeforeWorldMutation, then World::setLightFalloff).
    const glm::ivec3 o = world_.chunkOrigin();
    std::vector<glm::ivec3> all;
    all.reserve(static_cast<size_t>(counts_.x) *
                static_cast<size_t>(counts_.y) *
                static_cast<size_t>(counts_.z));
    for (int cz = o.z; cz < o.z + counts_.z; ++cz) {
        for (int cy = o.y; cy < o.y + counts_.y; ++cy) {
            for (int cx = o.x; cx < o.x + counts_.x; ++cx) {
                all.push_back({cx, cy, cz});
            }
        }
    }
    streamRemesh(all);
    startupMelt_ = true; // boost streamPump's per-frame upload budget until drained
}

void WorldRenderer::queueRemesh(const std::vector<glm::ivec3>& chunks) {
    for (const glm::ivec3& c : chunks) {
        pendingRemesh_.push_back(c);
    }
}

void WorldRenderer::processRemeshQueue(int budget) {
    if (pendingRemesh_.empty() || budget <= 0) {
        return;
    }
    // Pop a frame's slice of unique coords and hand them to the shared
    // main-thread-mesh + deferred-install path. This is the no-workers streaming
    // fallback (and liquid-flow remeshes when stream_workers: 0); routing it through
    // meshChunksDeferred means no vkDeviceWaitIdle here either.
    std::vector<glm::ivec3> slice;
    slice.reserve(static_cast<size_t>(budget));
    while (!pendingRemesh_.empty() && static_cast<int>(slice.size()) < budget) {
        const glm::ivec3 c = pendingRemesh_.front();
        pendingRemesh_.pop_front();
        // A chunk queued by several liquid ticks before this drain only needs
        // meshing once — skip a coord already in this slice.
        if (std::find(slice.begin(), slice.end(), c) == slice.end()) {
            slice.push_back(c);
        }
    }
    meshChunksDeferred(slice);
}

// --- Worker-thread streaming meshing -----------------------------------------
// Workers only ever READ the World (to greedy-mesh) and produce CPU MeshData; the
// main thread does all Vulkan (uploads via installMesh). The main thread is the
// sole World mutator and calls drainMeshJobs() (streamBarrier) before mutating, so
// a worker never reads chunk/light data mid-write. Per-slot meshVersion_ makes the
// newest request win: a result is applied only if it's still the latest for its slot.

void WorldRenderer::startWorkers(int n) {
    stopWorkers_ = false;
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

void WorldRenderer::stopWorkers() {
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        stopWorkers_ = true;
    }
    jobCv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

void WorldRenderer::workerLoop() {
    lowerWorkerThreadPriority(); // yield cores to the main thread (REVIEW O4)
    for (;;) {
        MeshJob job;
        {
            std::unique_lock<std::mutex> lk(jobMutex_);
            jobCv_.wait(lk, [this] { return stopWorkers_ || !jobQueue_.empty(); });
            if (jobQueue_.empty()) {
                if (stopWorkers_) {
                    return;
                }
                continue;
            }
            job = jobQueue_.front();
            jobQueue_.pop_front();
        }

        // Read-only: safe because the main thread drains us before mutating World.
        MeshData md = meshChunkData(job.cx, job.cy, job.cz);
        {
            std::lock_guard<std::mutex> lk(resultMutex_);
            resultQueue_.push_back(MeshResult{job.cx, job.cy, job.cz, job.version, std::move(md)});
        }
        if (jobsOutstanding_.fetch_sub(1) == 1) {
            // Reached zero — wake a thread blocked in drainMeshJobs(). Notify under
            // the barrier mutex so the waiter can't miss it between predicate + wait.
            std::lock_guard<std::mutex> lk(barrierMutex_);
            barrierCv_.notify_all();
        }
    }
}

void WorldRenderer::enqueueMeshJobs(const std::vector<glm::ivec3>& chunks) {
    if (chunks.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(jobMutex_);
        for (const glm::ivec3& c : chunks) {
            const size_t slot = static_cast<size_t>(chunkIndex(c.x, c.y, c.z));
            const std::uint64_t v = ++meshVersion_[slot]; // main-thread-only field
            jobQueue_.push_back(MeshJob{c.x, c.y, c.z, v});
            jobsOutstanding_.fetch_add(1);
        }
    }
    jobCv_.notify_all();
}

void WorldRenderer::drainMeshJobs() {
    std::unique_lock<std::mutex> lk(barrierMutex_);
    barrierCv_.wait(lk, [this] { return jobsOutstanding_.load() == 0; });
}

void WorldRenderer::processMeshResults(int budget) {
    // Pop up to `budget` finished meshes, then upload them all with a single GPU
    // submit+wait (installMeshBatch) — per-buffer device waits here were the load lag.
    std::vector<MeshResult> batch;
    batch.reserve(static_cast<size_t>(budget));
    {
        // Only results with geometry count against the budget — an empty result
        // (an all-air chunk) costs no buffer build or upload, and most of a
        // freshly streamed window edge is open sky. Letting empties ride free
        // keeps the budget meaning "uploads per frame", which is the cost it
        // actually bounds.
        std::lock_guard<std::mutex> lk(resultMutex_);
        int nonEmpty = 0;
        while (nonEmpty < budget && !resultQueue_.empty()) {
            if (!resultQueue_.front().data.empty()) {
                ++nonEmpty;
            }
            batch.push_back(std::move(resultQueue_.front()));
            resultQueue_.pop_front();
        }
    }
    if (batch.empty()) {
        return;
    }
    installMeshBatch(batch);
}

void WorldRenderer::streamBarrier() {
    if (!workers_.empty()) {
        drainMeshJobs();
    }
}

void WorldRenderer::streamRemesh(const std::vector<glm::ivec3>& chunks) {
    if (!workers_.empty()) {
        enqueueMeshJobs(chunks);
    } else {
        queueRemesh(chunks);
    }
}

void WorldRenderer::streamPump(int budget) {
    // While the deferred startup rings are still melting in, pump well above the
    // steady-state budget: the per-frame cost is a few extra buffer uploads, and
    // it's the first seconds after launch — filling the view fast matters more
    // than a perfectly level frame time. Back to the caller's budget for good
    // once the initial backlog has drained.
    if (startupMelt_) {
        bool drained;
        if (!workers_.empty()) {
            std::lock_guard<std::mutex> lk(resultMutex_);
            drained = jobsOutstanding_.load() == 0 && resultQueue_.empty();
        } else {
            drained = pendingRemesh_.empty();
        }
        if (drained) {
            startupMelt_ = false;
        } else {
            budget = std::max(budget, world_.config().streamMeltBudget); // world.yaml (R7)
        }
    }
    if (!workers_.empty()) {
        processMeshResults(budget);
    } else {
        processRemeshQueue(budget);
    }
}

void WorldRenderer::createUniformBuffers(uint32_t n) {
    uniformBuffers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uniformBuffers_.emplace_back(ctx_, sizeof(CameraUBO),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

void WorldRenderer::createGpuDrivenBuffers(uint32_t n) {
    // One draw-data SSBO + opaque/water indirect buffer per frame in flight, all
    // host-visible (the CPU refills them in record(); host writes before the queue
    // submit are visible to the device, so no extra barrier is needed). Sized to
    // the slot count — at most one command per slot per pass.
    const VkDeviceSize slots     = static_cast<VkDeviceSize>(meshes_.size());
    const VkDeviceSize ssboBytes = slots * sizeof(glm::vec4);
    const VkDeviceSize cmdBytes  = slots * sizeof(VkDrawIndexedIndirectCommand);
    const VkMemoryPropertyFlags host =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    drawDataBuffers_.reserve(n);
    opaqueIndirect_.reserve(n);
    waterIndirect_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        drawDataBuffers_.emplace_back(ctx_, ssboBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
        opaqueIndirect_.emplace_back(ctx_, cmdBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, host);
        waterIndirect_.emplace_back(ctx_, cmdBytes, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, host);
    }
}

void WorldRenderer::createDescriptorSets(uint32_t n) {
    std::array<VkDescriptorPoolSize, 3> sizes{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n};
    sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n}; // binding 2: per-chunk draw data

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes    = sizes.data();
    poolInfo.maxSets       = n;
    if (vkCreateDescriptorPool(ctx_.device(), &poolInfo, nullptr, &descriptorPool_) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool");
    }

    std::vector<VkDescriptorSetLayout> layouts(n, pipeline_->descriptorSetLayout());
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = descriptorPool_;
    alloc.descriptorSetCount = n;
    alloc.pSetLayouts        = layouts.data();

    descriptorSets_.resize(n);
    if (vkAllocateDescriptorSets(ctx_.device(), &alloc, descriptorSets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets");
    }

    for (uint32_t i = 0; i < n; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i].handle();
        bufferInfo.offset = 0;
        bufferInfo.range  = sizeof(CameraUBO);

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = textures_->view();
        imageInfo.sampler     = textures_->sampler();

        VkDescriptorBufferInfo drawDataInfo{};
        drawDataInfo.buffer = drawDataBuffers_[i].handle();
        drawDataInfo.offset = 0;
        drawDataInfo.range  = VK_WHOLE_SIZE;

        std::array<VkWriteDescriptorSet, 3> writes{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = descriptorSets_[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &bufferInfo;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = descriptorSets_[i];
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo      = &imageInfo;

        writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet          = descriptorSets_[i];
        writes[2].dstBinding      = 2;
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo     = &drawDataInfo;

        vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void WorldRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                           const glm::mat4& view, const glm::mat4& proj,
                           const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity,
                           const glm::vec4& heldLight, const glm::vec4& heldLightCol) {
    tickRetired(); // reap deferred buffer frees whose in-flight frames have completed

    // Advance the animation clock one frame (~60fps). Used by the vertex shader for
    // foliage sway + water waves; wrapped so the float stays small over long sessions.
    animTime_ += 1.0f / 60.0f;
    if (animTime_ > 3600.0f) animTime_ -= 3600.0f;
    CameraUBO ubo{view, proj, sunDirAmbient, sunColIntensity,
                  glm::vec4(animTime_, 0.0f, retroAffine_, 0.0f),
                  heldLight, heldLightCol};
    uniformBuffers_[frameIndex].upload(&ubo, sizeof(ubo));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->handle());

    VkViewport viewport{};
    viewport.width    = static_cast<float>(extent.width);
    viewport.height   = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Upload this frame's per-chunk draw-data SSBO (chunk world translation per slot,
    // read by the indirect vertex shader via gl_InstanceIndex). swapChunkBuffers keeps
    // the mirror current; re-uploading the whole array is cheap (slots * 16 B).
    if (!drawDataCpu_.empty()) {
        drawDataBuffers_[frameIndex].upload(drawDataCpu_.data(),
                                            drawDataCpu_.size() * sizeof(glm::vec4));
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);

    // Bind the shared arena ONCE for both passes; each chunk's draw addresses into it
    // via its indirect command's vertexOffset/firstIndex. Pipeline binds don't unbind
    // vertex/index buffers, so the water pass reuses these.
    VkBuffer     arenaVtx = arena_->vertexBuffer();
    VkDeviceSize zero     = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &arenaVtx, &zero);
    vkCmdBindIndexBuffer(cmd, arena_->indexBuffer(), 0, VK_INDEX_TYPE_UINT32);

    const std::array<glm::vec4, 6> frustum = extractFrustum(proj * view);
    const glm::vec3 chunkExtent(static_cast<float>(Chunk::kSize));
    std::size_t visible = 0, culled = 0;

    // --- Pass 1: opaque terrain (writes colour + depth) ----------------------
    // CPU frustum cull builds a compact indirect command array (one command per
    // in-frustum opaque chunk; firstInstance = slot so the vertex shader resolves the
    // chunk's world pos). A SINGLE vkCmdDrawIndexedIndirect issues them all — this is
    // the recording-cost win over the old per-chunk bind+push+draw loop. (Stage 2
    // moves this cull/build into chunk_cull.comp — see docs/GPU_DRIVEN_RENDERING.md.)
    cmdScratch_.clear();
    for (const uint32_t slot : drawList_) {
        const ChunkMesh& m = meshes_[slot];
        if (m.indexCount == 0) continue; // water-only chunk
        if (!aabbInFrustum(frustum, m.worldPos, m.worldPos + chunkExtent)) { ++culled; continue; }
        ++visible;
        VkDrawIndexedIndirectCommand c{};
        c.indexCount    = m.indexCount;
        c.instanceCount = 1;
        c.firstIndex    = m.firstIndex;
        c.vertexOffset  = m.baseVertex;
        c.firstInstance = slot;
        cmdScratch_.push_back(c);
    }
    std::size_t calls = cmdScratch_.size();
    if (!cmdScratch_.empty()) {
        opaqueIndirect_[frameIndex].upload(
            cmdScratch_.data(), cmdScratch_.size() * sizeof(VkDrawIndexedIndirectCommand));
        PushConstants pc{}; // model unused in the indirect path; params.x = opaque alpha
        pc.params.x = 1.0f;
        vkCmdPushConstants(cmd, pipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           Pipeline::kPushConstantSize, &pc);
        vkCmdDrawIndexedIndirect(cmd, opaqueIndirect_[frameIndex].handle(), 0,
                                 static_cast<uint32_t>(cmdScratch_.size()),
                                 sizeof(VkDrawIndexedIndirectCommand));
    }

    // --- Pass 2: translucent water (depth test on, depth write off) ----------
    // Skipped entirely when the window has no water (common above-ground case, R8).
    // Drawn after opaque so each surface alpha-blends over the terrain already in the
    // colour buffer.
    if (drawnWaterChunks_ != 0) {
        cmdScratch_.clear();
        for (const uint32_t slot : drawList_) {
            const ChunkMesh& m = meshes_[slot];
            if (m.waterIndexCount == 0) continue;
            if (!aabbInFrustum(frustum, m.worldPos, m.worldPos + chunkExtent)) continue;
            VkDrawIndexedIndirectCommand c{};
            c.indexCount    = m.waterIndexCount;
            c.instanceCount = 1;
            c.firstIndex    = m.waterFirstIndex;
            c.vertexOffset  = m.waterBaseVertex;
            c.firstInstance = slot;
            cmdScratch_.push_back(c);
        }
        if (!cmdScratch_.empty()) {
            waterIndirect_[frameIndex].upload(
                cmdScratch_.data(), cmdScratch_.size() * sizeof(VkDrawIndexedIndirectCommand));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipeline_->handle());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipeline_->layout(),
                                    0, 1, &descriptorSets_[frameIndex], 0, nullptr);
            PushConstants pc{}; pc.params.x = 0.7f; // ~70% opacity — see the seabed through it
            vkCmdPushConstants(cmd, waterPipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                               Pipeline::kPushConstantSize, &pc);
            vkCmdDrawIndexedIndirect(cmd, waterIndirect_[frameIndex].handle(), 0,
                                     static_cast<uint32_t>(cmdScratch_.size()),
                                     sizeof(VkDrawIndexedIndirectCommand));
            calls += cmdScratch_.size();
        }
    }

    lastVisibleChunks_ = visible;
    lastCulledChunks_  = culled;
    lastDrawCalls_     = calls;
}

} // namespace vg
