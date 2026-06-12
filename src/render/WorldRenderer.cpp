#include "render/WorldRenderer.h"

#include "render/Pipeline.h"
#include "render/TextureArray.h"
#include "render/VulkanContext.h"
#include "world/ChunkMesher.h"
#include "world/World.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <execution>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace vg {

namespace {

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

// Floor-modulo (b > 0): maps an absolute chunk coord to its ring-buffer slot,
// matching World's chunk store so a streamed-in chunk reuses the departed
// chunk's mesh slot. Identity over [0, b).
int floormod(int a, int b) {
    const int r = a % b;
    return (r < 0) ? r + b : r;
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
    pipeline_ = std::make_unique<Pipeline>(ctx_, renderPass,
                                           shaderDir + "/chunk.vert.spv",
                                           shaderDir + "/chunk.frag.spv");
    // Same shaders, but the translucent flavor (alpha blend on, depth-write off)
    // for the second, water pass — drawn after opaque so the seabed shows through.
    waterPipeline_ = std::make_unique<Pipeline>(ctx_, renderPass,
                                                shaderDir + "/chunk.vert.spv",
                                                shaderDir + "/chunk.frag.spv",
                                                /*translucent=*/true);
    buildMeshes();
    createUniformBuffers(framesInFlight);
    createDescriptorSets(framesInFlight);
    framesInFlight_ = framesInFlight;
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

VkImageView WorldRenderer::blockTextureView() const { return textures_->view(); }
VkSampler   WorldRenderer::blockTextureSampler() const { return textures_->sampler(); }

void WorldRenderer::uploadChunkMesh(int cx, int cy, int cz) {
    // Smooth lighting (per-corner AO + light) is always on; the mesher's flat
    // mode remains only as a debugging path. Main-thread meshing → free old
    // buffers immediately (callers issue a device-idle wait first).
    MeshData mesh = ChunkMesher::greedyMesh(world_.chunk(cx, cy, cz), world_.registry(),
                                            makeSampler(cx, cy, cz),
                                            makeLightSampler(cx, cy, cz), true,
                                            glm::ivec3(cx, cy, cz) * kChunkSize,
                                            makeTintSampler(cx, cy, cz));
    installMesh(cx, cy, cz, std::move(mesh), /*deferOldBuffers=*/false);
}

void WorldRenderer::swapChunkBuffers(int cx, int cy, int cz, Buffer&& meshBuffer,
                                     VkDeviceSize indexOffset, uint32_t indexCount,
                                     VkDeviceSize waterIndexOffset, uint32_t waterIndexCount,
                                     int32_t firstWaterVertex, bool deferOldBuffers) {
    ChunkMesh& cm = meshes_[chunkIndex(cx, cy, cz)];

    // Drop this slot's previous contribution before rebuilding it. A slot counts
    // as a drawn chunk if it has either opaque or water geometry.
    const bool hadGeometry = cm.indexCount > 0 || cm.waterIndexCount > 0;
    totalTriangles_ -= (cm.indexCount + cm.waterIndexCount) / 3;
    if (hadGeometry) {
        --drawnChunks_;
        if (deferOldBuffers) {
            // An in-flight frame may still reference the old buffer, and the worker
            // path can't issue a per-frame device wait — retire it for a few frames
            // instead of freeing now. tickRetired() reaps it.
            retired_.emplace_back(static_cast<int>(framesInFlight_) + 1, std::move(cm.meshBuffer));
        }
    }
    cm.meshBuffer       = std::move(meshBuffer);
    cm.indexOffset      = indexOffset;
    cm.indexCount       = indexCount;
    cm.waterIndexOffset = waterIndexOffset;
    cm.waterIndexCount  = waterIndexCount;
    cm.firstWaterVertex = firstWaterVertex;
    if (indexCount > 0 || waterIndexCount > 0) {
        ++drawnChunks_;
    }
    cm.worldPos = glm::vec3(cx, cy, cz) * static_cast<float>(Chunk::kSize);
    totalTriangles_ += (cm.indexCount + cm.waterIndexCount) / 3;
}

void WorldRenderer::installMesh(int cx, int cy, int cz, MeshData&& mesh, bool deferOldBuffers) {
    // Main-thread immediate path (one GPU submit+wait in createDeviceLocal) — fine
    // for the rare, small sync remeshes (block edits, falloff). The streaming path
    // uses installMeshBatch(). Vertices + indices share ONE allocation (halves the
    // device allocation count vs a buffer each — see docs/WORLD_GEN_AGENT_TIPS.md §5).
    if (mesh.empty()) {
        swapChunkBuffers(cx, cy, cz, Buffer{}, 0, 0, 0, 0, 0, deferOldBuffers);
        return;
    }
    const MeshLayout L = computeLayout(mesh);
    std::vector<uint8_t> blob(static_cast<size_t>(L.total));
    writeBlob(reinterpret_cast<char*>(blob.data()), mesh, L);
    Buffer mb = Buffer::createDeviceLocal(
        ctx_, blob.data(), L.total,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    swapChunkBuffers(cx, cy, cz, std::move(mb), L.indexOffset, L.indexCount,
                     L.waterIndexOffset, L.waterIndexCount, L.firstWaterVertex,
                     deferOldBuffers);
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
            swapChunkBuffers(r.cx, r.cy, r.cz, Buffer{}, 0, 0, 0, 0, 0, /*deferOldBuffers=*/true);
            continue;
        }
        const MeshLayout L = computeLayout(r.data);
        // One host staging buffer holding all vertices then all indices.
        Buffer stage(ctx_, L.total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        writeBlob(static_cast<char*>(stage.map()), r.data, L);
        // One device buffer used as both vertex and index source.
        Buffer dev(ctx_, L.total,
                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        PendingUpload up;
        up.dst   = dev.handle(); // stable handle; the slot owns the buffer after the swap
        up.size  = L.total;
        up.stage = std::move(stage);
        pendingUploads_.push_back(std::move(up));

        swapChunkBuffers(r.cx, r.cy, r.cz, std::move(dev), L.indexOffset, L.indexCount,
                         L.waterIndexOffset, L.waterIndexCount, L.firstWaterVertex,
                         /*deferOldBuffers=*/true);
    }
}

void WorldRenderer::recordPendingUploads(VkCommandBuffer cmd) {
    if (pendingUploads_.empty()) {
        return;
    }
    for (PendingUpload& p : pendingUploads_) {
        VkBufferCopy copy{};
        copy.size = p.size;
        vkCmdCopyBuffer(cmd, p.stage.handle(), p.dst, 1, &copy);
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
    if (retired_.empty()) {
        return;
    }
    for (auto& r : retired_) {
        --r.first;
    }
    // Erasing frees the Buffer (RAII): safe now — framesInFlight_+1 frames have
    // elapsed, so every frame that could reference it has completed.
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [](const std::pair<int, Buffer>& r) { return r.first <= 0; }),
                   retired_.end());
}

void WorldRenderer::buildMeshes() {
    counts_ = world_.chunkCounts();
    meshes_.resize(static_cast<size_t>(counts_.x) * counts_.y * counts_.z);

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
    constexpr int kCoreRadius = 5;
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
                       return ChunkMesher::greedyMesh(
                           world_.chunk(c.x, c.y, c.z), world_.registry(),
                           makeSampler(c.x, c.y, c.z), makeLightSampler(c.x, c.y, c.z), true,
                           c * kChunkSize, makeTintSampler(c.x, c.y, c.z));
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
    constexpr size_t kSlice = 384;
    for (size_t start = 0; start < coords.size(); start += kSlice) {
        const size_t end = std::min(start + kSlice, coords.size());
        std::vector<Buffer> staging; // freed at slice end (after the copy completes)
        staging.reserve(end - start);
        VkCommandBuffer cmd = ctx_.beginSingleTimeCommands();
        for (size_t i = start; i < end; ++i) {
            const glm::ivec3& c = coords[i];
            MeshData& m = meshed[i];
            if (m.empty()) {
                swapChunkBuffers(c.x, c.y, c.z, Buffer{}, 0, 0, 0, 0, 0, /*deferOldBuffers=*/false);
                continue;
            }
            const MeshLayout L = computeLayout(m);
            Buffer stage(ctx_, L.total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            writeBlob(static_cast<char*>(stage.map()), m, L);
            Buffer dev(ctx_, L.total,
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkBufferCopy copy{};
            copy.size = L.total;
            vkCmdCopyBuffer(cmd, stage.handle(), dev.handle(), 1, &copy);
            staging.push_back(std::move(stage));
            swapChunkBuffers(c.x, c.y, c.z, std::move(dev), L.indexOffset, L.indexCount,
                             L.waterIndexOffset, L.waterIndexCount, L.firstWaterVertex,
                             /*deferOldBuffers=*/false);
        }
        ctx_.endSingleTimeCommands(cmd); // one submit+wait for the whole slice
    }
    if (_timeMesh) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - _t1).count();
        std::printf("[mesh] GPU upload: %lldms\n", static_cast<long long>(ms));
    }
    std::cout << "[world] meshed " << drawnChunks_ << " non-empty chunks, "
              << totalTriangles_ << " triangles";
    if (!deferredStartup_.empty()) {
        std::cout << " (" << deferredStartup_.size() << " outer chunks streaming in)";
    }
    std::cout << "\n";
}

void WorldRenderer::remeshChunk(int cx, int cy, int cz) {
    // The chunk's old buffers may still be referenced by frames in flight, so wait
    // for the GPU to drain before we free and replace them. Remeshing is a rare,
    // discrete event (a block edit, a chunk streaming in), so a full device wait
    // here is acceptable and keeps buffer lifetime trivial.
    vkDeviceWaitIdle(ctx_.device());
    if (!meshVersion_.empty()) {
        ++meshVersion_[static_cast<size_t>(chunkIndex(cx, cy, cz))];
    }
    uploadChunkMesh(cx, cy, cz);
}

void WorldRenderer::remeshChunks(const std::vector<glm::ivec3>& chunks) {
    if (chunks.empty()) {
        return;
    }
    vkDeviceWaitIdle(ctx_.device()); // drain once for the whole batch
    for (const glm::ivec3& c : chunks) {
        // Bump the slot's version so any in-flight worker result for it is treated
        // as stale and discarded (this immediate remesh is the newest state).
        if (!meshVersion_.empty()) {
            ++meshVersion_[static_cast<size_t>(chunkIndex(c.x, c.y, c.z))];
        }
        uploadChunkMesh(c.x, c.y, c.z);
    }
}

void WorldRenderer::remeshAll() {
    // Lighting is baked into the vertex data, so a global lighting change (e.g.
    // a new falloff) means every loaded chunk must be rebuilt. One drain for the
    // lot. Iterate the window's absolute coords — it may have streamed away from 0.
    vkDeviceWaitIdle(ctx_.device());
    const glm::ivec3 o = world_.chunkOrigin();
    for (int cz = o.z; cz < o.z + counts_.z; ++cz) {
        for (int cy = o.y; cy < o.y + counts_.y; ++cy) {
            for (int cx = o.x; cx < o.x + counts_.x; ++cx) {
                if (!meshVersion_.empty()) {
                    ++meshVersion_[static_cast<size_t>(chunkIndex(cx, cy, cz))];
                }
                uploadChunkMesh(cx, cy, cz);
            }
        }
    }
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
    // Pop a frame's slice and mesh it on the main thread, but install via the SAME
    // deferred-buffer + frame-integrated upload path the worker pool uses
    // (installMeshBatch -> recordPendingUploads), NOT the draining remeshChunks.
    // This is the no-workers streaming fallback; routing it through the deferred
    // path means per-tick liquid flow never issues a vkDeviceWaitIdle here either,
    // so the flow-time stutter is gone with or without worker threads.
    std::vector<MeshResult> batch;
    batch.reserve(static_cast<size_t>(budget));
    while (!pendingRemesh_.empty() && static_cast<int>(batch.size()) < budget) {
        const glm::ivec3 c = pendingRemesh_.front();
        pendingRemesh_.pop_front();
        // A chunk queued by several liquid ticks before this drain only needs
        // meshing once — skip a coord already in this batch.
        bool dup = false;
        for (const MeshResult& r : batch) {
            if (r.cx == c.x && r.cy == c.y && r.cz == c.z) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        const std::uint64_t v = ++meshVersion_[static_cast<size_t>(chunkIndex(c.x, c.y, c.z))];
        MeshData md = ChunkMesher::greedyMesh(
            world_.chunk(c.x, c.y, c.z), world_.registry(),
            makeSampler(c.x, c.y, c.z), makeLightSampler(c.x, c.y, c.z), true,
            c * kChunkSize, makeTintSampler(c.x, c.y, c.z));
        batch.push_back(MeshResult{c.x, c.y, c.z, v, std::move(md)});
    }
    if (!batch.empty()) {
        installMeshBatch(batch); // deferred buffers + queued staging copies, no drain
    }
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
        MeshData md = ChunkMesher::greedyMesh(world_.chunk(job.cx, job.cy, job.cz),
                                              world_.registry(),
                                              makeSampler(job.cx, job.cy, job.cz),
                                              makeLightSampler(job.cx, job.cy, job.cz), true,
                                              glm::ivec3(job.cx, job.cy, job.cz) * kChunkSize,
                                              makeTintSampler(job.cx, job.cy, job.cz));
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
            budget = std::max(budget, 64);
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

void WorldRenderer::createDescriptorSets(uint32_t n) {
    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n};

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

        std::array<VkWriteDescriptorSet, 2> writes{};
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

        vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void WorldRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                           const glm::mat4& view, const glm::mat4& proj,
                           const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity) {
    tickRetired(); // reap deferred buffer frees whose in-flight frames have completed

    // Advance the animation clock one frame (~60fps). Used by the vertex shader for
    // foliage sway + water waves; wrapped so the float stays small over long sessions.
    animTime_ += 1.0f / 60.0f;
    if (animTime_ > 3600.0f) animTime_ -= 3600.0f;
    CameraUBO ubo{view, proj, sunDirAmbient, sunColIntensity,
                  glm::vec4(animTime_, 0.0f, 0.0f, 0.0f)};
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

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_->layout(),
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);

    // One draw per non-empty chunk, each translated to its world position via a
    // push constant. Empty slots (air chunks) carry no geometry and are skipped,
    // as are chunks whose bounding box falls entirely outside the view frustum
    // (frustum culling — the GPU already discards back faces, but skipping the
    // draw call entirely also spares vertex processing for off-screen chunks).
    const std::array<glm::vec4, 6> frustum = extractFrustum(proj * view);
    const glm::vec3 chunkExtent(static_cast<float>(Chunk::kSize));

    // --- Pass 1: opaque terrain (writes colour + depth) ----------------------
    for (const ChunkMesh& m : meshes_) {
        if (m.indexCount == 0) {
            continue;
        }
        if (!aabbInFrustum(frustum, m.worldPos, m.worldPos + chunkExtent)) {
            continue;
        }
        PushConstants pc;
        pc.model      = glm::translate(glm::mat4(1.0f), m.worldPos);
        pc.params.x   = 1.0f; // fully opaque
        vkCmdPushConstants(cmd, pipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           Pipeline::kPushConstantSize, &pc);

        VkBuffer     vb   = m.meshBuffer.handle();
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
        vkCmdBindIndexBuffer(cmd, vb, m.indexOffset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }

    // --- Pass 2: translucent water (depth test on, depth write off) ----------
    // Drawn after all opaque geometry so each water surface alpha-blends over the
    // seabed/terrain already in the colour buffer. The viewport/scissor are
    // dynamic and persist; the descriptor set layout is identical, but rebind it
    // against the water pipeline's layout to be explicit.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipeline_->handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, waterPipeline_->layout(),
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);
    for (const ChunkMesh& m : meshes_) {
        if (m.waterIndexCount == 0) {
            continue;
        }
        if (!aabbInFrustum(frustum, m.worldPos, m.worldPos + chunkExtent)) {
            continue;
        }
        PushConstants pc;
        pc.model    = glm::translate(glm::mat4(1.0f), m.worldPos);
        pc.params.x = 0.7f; // ~70% opacity — see the seabed through it
        vkCmdPushConstants(cmd, waterPipeline_->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                           Pipeline::kPushConstantSize, &pc);

        VkBuffer     vb   = m.meshBuffer.handle();
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
        vkCmdBindIndexBuffer(cmd, vb, m.waterIndexOffset, VK_INDEX_TYPE_UINT32);
        // Water indices are 0-based into the water vertices, which sit after the
        // opaque vertices in the same buffer — firstWaterVertex is the offset.
        vkCmdDrawIndexed(cmd, m.waterIndexCount, 1, 0, m.firstWaterVertex, 0);
    }
}

} // namespace vg
