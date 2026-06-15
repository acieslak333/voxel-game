#include "render/FarTerrainRenderer.h"

#include "render/VulkanContext.h"
#include "world/Block.h"
#include "world/Chunk.h"
#include "world/World.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace vg {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("FarTerrainRenderer: cannot open " + path);
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

// Floor division (correct for negative world coordinates), so the cell grid is
// continuous across x/z = 0.
int floorDiv(int a, int b) {
    int q = a / b, r = a % b;
    if (r != 0 && (r < 0) != (b < 0)) --q;
    return q;
}

uint32_t packTint(const glm::vec3& t) {
    auto c = [](float v) {
        return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return c(t.r) | (c(t.g) << 8) | (c(t.b) << 16) | (0xFFu << 24);
}
constexpr uint32_t kNoTint = 0xFFFFFFFFu;

// Deterministic [0,1) hash — MUST match World.cpp's hash01 so the far-terrain
// tree impostors land on exactly the columns the voxel generator roots trees on.
float hash01(int x, int z, uint32_t salt) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^
                 static_cast<uint32_t>(z) * 0xd8163841u ^ (salt * 0x9e3779b9u);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}
} // namespace

FarTerrainRenderer::FarTerrainRenderer(VulkanContext& ctx, VkRenderPass renderPass,
                                       uint32_t framesInFlight, const std::string& shaderDir,
                                       VkImageView textureView, VkSampler textureSampler,
                                       const Config& config)
    : ctx_(ctx), config_(config), framesInFlight_(framesInFlight) {
    // Always build the GPU resources (even when starting disabled) so the LOD
    // on/off toggle can flip config_.enabled at runtime without lazy pipeline
    // creation. config_.enabled gates update()/record()/outerExtentBlocks().
    createPipeline(renderPass, shaderDir);
    createUniformBuffers(framesInFlight);
    createDescriptorSets(framesInFlight, textureView, textureSampler);
}

FarTerrainRenderer::~FarTerrainRenderer() {
    VkDevice device = ctx_.device();
    if (descriptorPool_)      vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (pipeline_)            vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_)      vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
}

// --- Mesh build --------------------------------------------------------------

FarTerrainRenderer::Surf FarTerrainRenderer::sampleSurf(const World& world, int wx, int wz) const {
    const ColumnInfo ci = world.generator().columnInfo(wx, wz);
    const BlockRegistry& reg = world.registry();
    Surf s;
    if (ci.height < ci.waterLevel) {
        // Submerged: show the water surface (oceans/lakes read as water far out).
        s.y     = static_cast<float>(ci.waterLevel + 1) - config_.yBias;
        s.layer = waterLayer_;
        s.tint  = kNoTint;
    } else {
        s.y     = static_cast<float>(ci.height + 1) - config_.yBias;
        s.layer = reg.faceLayer(ci.topId, FacePosY);
        glm::vec3 tint = world.isVegTintable(ci.topId) ? ci.vegTint : glm::vec3(1.0f);
        // Forest tint: darken vegetation-tinted ground toward `forestTint` where the
        // biome is dense forest, so distant woods read as dark masses even between
        // the tree impostors (and beyond the impostor band) instead of bare grass.
        if (world.isVegTintable(ci.topId) && ci.treeDensity > 0.0f) {
            const float f = std::clamp(ci.treeDensity * 5.0f, 0.0f, 1.0f);
            tint *= glm::mix(1.0f, config_.forestTint, f);
        }
        s.tint = packTint(tint);
    }
    return s;
}

int FarTerrainRenderer::outerExtentBlocks(const World& world) const {
    if (!config_.enabled) return 0;
    const int base = std::max(1, config_.baseStep);
    const glm::ivec3 counts = world.chunkCounts();
    const int windowHalf = (counts.x / 2) * Chunk::kSize;
    // Mirror buildMesh's rings: inner starts at 0; ring 0 spans past the window
    // (+ a safety margin for the player being off-centre during fast flight).
    int inner = 0;
    for (int L = 0; L < config_.ringCount; ++L) {
        const int step = base << L;
        const int innerL = ((inner + step - 1) / step) * step;
        const int span = (L == 0) ? (windowHalf + config_.ringCells * base + 96)
                                   : config_.ringCells * step;
        inner = innerL + span;
    }
    return inner; // outermost ring's outer half-extent
}

void FarTerrainRenderer::update(const World& world, const glm::vec3& camPos) {
    if (!config_.enabled) {
        return;
    }
    if (!layersResolved_) {
        const BlockRegistry& reg = world.registry();
        auto layerOf = [&](const char* name, uint32_t fallback) -> uint32_t {
            try { return reg.faceLayer(reg.idByName(name), FacePosY); } catch (...) { return fallback; }
        };
        waterLayer_   = layerOf("water", 0);
        // Indexed by TreeKind: 0 Oak, 1 Birch, 2 Pine.
        leafLayer_[0] = layerOf("oak_leaves", 0);
        leafLayer_[1] = layerOf("birch_leaves", leafLayer_[0]);
        leafLayer_[2] = layerOf("pine_leaves", leafLayer_[0]);
        trunkLayer_   = layerOf("oak_log_side", 0);
        layersResolved_ = true;
    }
    // Collect a finished background rebuild (swap on the main thread; record() also
    // runs on the main thread, so mesh_ is never read mid-swap).
    if (buildFuture_.valid() &&
        buildFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        mesh_ = buildFuture_.get();
        uploadMesh(); // rebuild the device-local vertex buffer (retire the old one)
    }
    // Window half-extent: impostors within this distance are about to be replaced by
    // real voxel trees, so they dissolve there (screen-door fade in record/frag).
    fadeNear_ = static_cast<float>((world.chunkCounts().x / 2) * Chunk::kSize);

    const int base = std::max(1, config_.baseStep);
    const glm::ivec2 c{floorDiv(static_cast<int>(std::floor(camPos.x)), base) * base,
                       floorDiv(static_cast<int>(std::floor(camPos.z)), base) * base};
    // Kick a rebuild when the player has moved a couple of base cells since the last
    // one (the shell is distant + async, so this cadence is imperceptible but halves
    // the rebuild CPU vs every single cell; the window-box hole's underlap covers the
    // staleness). The generator/registry are immutable, so the worker only reads
    // constant data — it never races the main thread's chunk/light mutations.
    const int moved = std::max(std::abs(c.x - lastCenter_.x), std::abs(c.y - lastCenter_.y));
    if ((!built_ || moved >= 2 * base) && !buildFuture_.valid()) {
        lastCenter_ = c;
        built_ = true;
        // Snapshot the loaded window box (block bounds) on the main thread — the
        // worker must not read world.chunkOrigin() while recenter() may write it.
        const glm::ivec3 o = world.chunkOrigin();
        const glm::ivec3 n = world.chunkCounts();
        const glm::ivec4 winBox{o.x * Chunk::kSize, o.z * Chunk::kSize,
                                (o.x + n.x) * Chunk::kSize, (o.z + n.z) * Chunk::kSize};
        buildFuture_ = std::async(std::launch::async,
                                  [this, &world, c, winBox] { return buildMesh(world, c, winBox); });
    }
}

std::vector<FarTerrainRenderer::FarVertex> FarTerrainRenderer::buildMesh(const World& world,
                                                                         glm::ivec2 center,
                                                                         glm::ivec4 winBox) const {
    std::vector<FarVertex> mesh;
    const int base = std::max(1, config_.baseStep);
    const int centerX = center.x;
    const int centerZ = center.y;

    const glm::ivec3 counts = world.chunkCounts();
    const int windowHalf = (counts.x / 2) * Chunk::kSize;

    // The HOLE (where the shell is skipped because voxel chunks cover it) is the
    // loaded window box shrunk by `underlap` — NOT a player-centred radius. Tying
    // it to the actual window keeps it aligned with the real chunks even when the
    // player is off-centre during fast flight, and the `underlap` band means the
    // shell still fills the window's leading edge while those chunks are streaming
    // in: a chunk being meshed shows coarse ground (not void), then draws ON TOP of
    // the shell (overwrite, not delete). A skirt rings the hole to hide the seam.
    const int hx0 = winBox.x + config_.underlap, hx1 = winBox.z - config_.underlap;
    const int hz0 = winBox.y + config_.underlap, hz1 = winBox.w - config_.underlap;
    auto inHole = [&](int cellX, int cellZ, int step) -> bool {
        const int ccx = cellX + step / 2, ccz = cellZ + step / 2;
        return ccx >= hx0 && ccx < hx1 && ccz >= hz0 && ccz < hz1;
    };
    // How far the player sits from the farthest window-box edge (Chebyshev): ring 0
    // must reach past this so it bridges to the chunks on every side, off-centre or not.
    const int winReach = std::max(std::max(std::abs(winBox.x - centerX), std::abs(winBox.z - centerX)),
                                  std::max(std::abs(winBox.y - centerZ), std::abs(winBox.w - centerZ)));

    // Per-build cache: corners are shared between adjacent cells, so memoise the
    // (expensive) generator sample per world column.
    std::unordered_map<uint64_t, Surf> cache;
    cache.reserve(1 << 14);
    auto H = [&](int wx, int wz) -> Surf {
        const uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(wx)) << 32) |
                           static_cast<uint32_t>(wz);
        auto it = cache.find(k);
        if (it != cache.end()) return it->second;
        Surf s = sampleSurf(world, wx, wz);
        cache.emplace(k, s);
        return s;
    };

    auto pushTri = [&](const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                       uint32_t layer, uint32_t tint, bool faceUp) {
        if (mesh.size() + 3 > kMaxVerts) return;
        glm::vec3 n = glm::cross(p1 - p0, p2 - p0);
        const float len = glm::length(n);
        n = len > 1e-6f ? n / len : glm::vec3(0.0f, 1.0f, 0.0f);
        if (faceUp && n.y < 0.0f) n = -n; // top faces always light from above
        mesh.push_back({p0, n, glm::vec2(p0.x, p0.z), layer, tint});
        mesh.push_back({p1, n, glm::vec2(p1.x, p1.z), layer, tint});
        mesh.push_back({p2, n, glm::vec2(p2.x, p2.z), layer, tint});
    };

    int inner = 0;
    for (int L = 0; L < config_.ringCount; ++L) {
        const int step = base << L;
        const int innerL = ((inner + step - 1) / step) * step; // snap to this ring's grid
        // Ring 0 spans from the player out past the window box (+ one ring band), so
        // the fine shell always bridges to the chunks; coarser rings add a band each.
        const int span = (L == 0) ? (winReach + config_.ringCells * base) : config_.ringCells * step;
        const int outerL = innerL + span;
        inner = outerL; // the next coarser ring takes over here

        // A cell is emitted if it's in this ring's distance band AND not in the hole
        // (the loaded window). Skirts fire wherever a neighbour is NOT emitted — so
        // a skirt also rings the hole, hiding the shell/voxel seam.
        auto emit = [&](int cellX, int cellZ) -> bool {
            const int ccx = cellX + step / 2, ccz = cellZ + step / 2;
            const int cheb = std::max(std::abs(ccx - centerX), std::abs(ccz - centerZ));
            if (cheb < innerL || cheb >= outerL) return false;
            return !inHole(cellX, cellZ, step);
        };

        const int startX = floorDiv(centerX - outerL, step) * step;
        const int startZ = floorDiv(centerZ - outerL, step) * step;
        for (int cx = startX; cx < centerX + outerL; cx += step) {
            for (int cz = startZ; cz < centerZ + outerL; cz += step) {
                if (!emit(cx, cz)) continue;

                const Surf a = H(cx, cz);
                const Surf b = H(cx + step, cz);
                const Surf cc = H(cx + step, cz + step);
                const Surf d = H(cx, cz + step);
                const uint32_t layer = a.layer, tint = a.tint;

                const glm::vec3 pa(cx, a.y, cz);
                const glm::vec3 pb(cx + step, b.y, cz);
                const glm::vec3 pc(cx + step, cc.y, cz + step);
                const glm::vec3 pd(cx, d.y, cz + step);
                pushTri(pa, pb, pc, layer, tint, true);
                pushTri(pa, pc, pd, layer, tint, true);

                // Drop a vertical skirt on any edge that borders a cell NOT in this
                // ring (the inner hole, the outer rim, or a coarser/finer ring) so
                // the inevitable height mismatch at a ring seam shows ground, not sky.
                const float sd = config_.skirtDepth;
                auto skirt = [&](const glm::vec3& t0, const glm::vec3& t1) {
                    const glm::vec3 b0(t0.x, t0.y - sd, t0.z);
                    const glm::vec3 b1(t1.x, t1.y - sd, t1.z);
                    pushTri(t0, t1, b1, layer, tint, false);
                    pushTri(t0, b1, b0, layer, tint, false);
                };
                if (!emit(cx - step, cz)) skirt(pa, pd); // -X
                if (!emit(cx + step, cz)) skirt(pb, pc); // +X
                if (!emit(cx, cz - step)) skirt(pa, pb); // -Z
                if (!emit(cx, cz + step)) skirt(pd, pc); // +Z
            }
        }
    }

    // --- Low-poly tree impostors -------------------------------------------------
    // Real 3D geometry (a cone for conifers, an octahedron blob for round canopies),
    // NOT billboards — so they read correctly from any angle and never swim. They
    // are scattered with the EXACT tree gate World::generateColumn uses (hash01 vs
    // the column's treeDensity, species from treeKind), so a distant impostor sits
    // on the same column and at matching size as the real voxel tree — as the player
    // approaches, the window edge reaches it and the voxel tree takes over seamlessly.
    if (config_.trees) {
        const uint32_t seed = world.seed();
        const float maxTreeD = world.generator().maxTreeDensity();
        const int treeOuter = windowHalf + config_.treeDist; // Chebyshev reach from player
        constexpr int kCap = 6000;                           // impostor budget per rebuild
        int placed = 0;

        // A cone (conifer): apex up, N-gon base ring. Outward-ish per-face normals.
        auto cone = [&](glm::vec3 base, float r, float h, uint32_t layer, uint32_t tint) {
            constexpr int N = 7;
            const glm::vec3 apex = base + glm::vec3(0.0f, h, 0.0f);
            for (int i = 0; i < N; ++i) {
                const float a0 = (float(i) / N) * 6.2831853f;
                const float a1 = (float(i + 1) / N) * 6.2831853f;
                const glm::vec3 p0 = base + glm::vec3(std::cos(a0) * r, 0.0f, std::sin(a0) * r);
                const glm::vec3 p1 = base + glm::vec3(std::cos(a1) * r, 0.0f, std::sin(a1) * r);
                pushTri(p0, p1, apex, layer, tint, false);
            }
        };
        // An octahedron blob (round/drooping canopy).
        auto blob = [&](glm::vec3 c, float rh, float rv, uint32_t layer, uint32_t tint) {
            const glm::vec3 top = c + glm::vec3(0.0f, rv, 0.0f);
            const glm::vec3 bot = c - glm::vec3(0.0f, rv, 0.0f);
            const glm::vec3 e[4] = {c + glm::vec3(rh, 0, 0), c + glm::vec3(0, 0, rh),
                                    c - glm::vec3(rh, 0, 0), c - glm::vec3(0, 0, rh)};
            for (int i = 0; i < 4; ++i) {
                const glm::vec3& a = e[i];
                const glm::vec3& b = e[(i + 1) % 4];
                pushTri(a, b, top, layer, tint, false);
                pushTri(b, a, bot, layer, tint, false);
            }
        };
        // A thin trunk box (4 side faces).
        auto trunk = [&](float bx, float bz, float y0, float y1, float half) {
            const glm::vec3 A(bx - half, 0, bz - half), B(bx + half, 0, bz - half);
            const glm::vec3 C(bx + half, 0, bz + half), D(bx - half, 0, bz + half);
            const glm::vec3 corners[4] = {A, B, C, D};
            for (int i = 0; i < 4; ++i) {
                glm::vec3 p = corners[i], q = corners[(i + 1) % 4];
                const glm::vec3 t0(p.x, y0, p.z), t1(q.x, y0, q.z);
                const glm::vec3 u0(p.x, y1, p.z), u1(q.x, y1, q.z);
                pushTri(t0, t1, u1, trunkLayer_, 0x00FFFFFFu, false); // alpha 0 = impostor
                pushTri(t0, u1, u0, trunkLayer_, 0x00FFFFFFu, false);
            }
        };

        for (int oz = centerZ - treeOuter; oz <= centerZ + treeOuter && placed < kCap; ++oz) {
            for (int ox = centerX - treeOuter; ox <= centerX + treeOuter; ++ox) {
                const int cheb = std::max(std::abs(ox - centerX), std::abs(oz - centerZ));
                if (cheb >= treeOuter) continue; // beyond the impostor reach
                // Skip columns inside the loaded voxel window: real trees live there,
                // so an impostor would double them (z-fighting). Outside it, impostors
                // start exactly at the window edge — no overlap, no tree-less ring.
                if (ox >= winBox.x && ox < winBox.z && oz >= winBox.y && oz < winBox.w) continue;
                const float th = hash01(ox, oz, seed ^ 0x7233u);
                if (th >= maxTreeD) continue; // cheap reject before the costly columnInfo
                const ColumnInfo oc = world.generator().columnInfo(ox, oz);
                if (th >= oc.treeDensity) continue;       // no tree roots here
                if (oc.height < oc.waterLevel) continue;  // not in water

                const float csz = hash01(ox, oz, seed ^ 0x7241u); // canopy-size roll
                const float hh  = hash01(ox, oz, seed ^ 0x7234u); // trunk-height roll
                const int k = std::clamp(static_cast<int>(oc.treeKind), 0, 2);
                const uint32_t leaf = leafLayer_[k];
                // Alpha 0 marks impostor geometry so the shader can screen-door
                // dissolve it near the window edge (the trunk lambda matches).
                const uint32_t tint = packTint(oc.vegTint) & 0x00FFFFFFu;
                const float bx = ox + 0.5f, bz = oz + 0.5f;
                const float oh = static_cast<float>(oc.height) + 1.0f;

                switch (oc.treeKind) {
                    case TreeKind::Pine: {
                        const float trunkH = 8.0f + hh * 7.0f;
                        const float r = 2.0f + csz * 2.0f + 0.6f;
                        trunk(bx, bz, oh, oh + trunkH * 0.35f, 0.45f);
                        cone({bx, oh + trunkH * 0.12f, bz}, r, trunkH + 1.0f, leaf, tint);
                        break;
                    }
                    default: { // Oak / Birch — round canopy on a trunk
                        const float trunkH = (oc.treeKind == TreeKind::Birch)
                                                 ? 7.0f + hh * 6.0f
                                                 : 5.0f + hh * 6.0f;
                        const float rH = (oc.treeKind == TreeKind::Birch) ? 2.2f : 2.5f + csz * 2.0f;
                        const float rV = rH + 0.5f;
                        trunk(bx, bz, oh, oh + trunkH, 0.45f);
                        blob({bx, oh + trunkH, bz}, rH + 0.5f, rV, leaf, tint);
                        break;
                    }
                }
                if (++placed >= kCap) break;
            }
        }
    }
    return mesh;
}

// --- Device buffer upload + retire -------------------------------------------

void FarTerrainRenderer::uploadMesh() {
    // Retire the current buffer (in-flight frames may still reference it) and build a
    // fresh device-local one from the new mesh. createDeviceLocal stages + copies +
    // waits, so it's ready for this frame's draw. Rare (every few blocks), so the
    // one-off submit is far cheaper than re-fetching ~400k verts from host memory
    // EVERY frame, which was the far terrain's whole per-frame cost.
    if (deviceVertCount_ > 0) {
        retired_.emplace_back(static_cast<int>(framesInFlight_) + 1, std::move(deviceVB_));
    }
    deviceVertCount_ = 0;
    if (mesh_.empty()) {
        return;
    }
    const size_t n = std::min(static_cast<size_t>(kMaxVerts), mesh_.size());
    deviceVB_ = Buffer::createDeviceLocal(ctx_, mesh_.data(), n * sizeof(FarVertex),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    deviceVertCount_ = static_cast<uint32_t>(n);
}

void FarTerrainRenderer::tickRetired() {
    if (retired_.empty()) return;
    for (auto& r : retired_) --r.first;
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [](const std::pair<int, Buffer>& r) { return r.first <= 0; }),
                   retired_.end());
}

// --- Recording ---------------------------------------------------------------

void FarTerrainRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                                const glm::mat4& view, const glm::mat4& proj,
                                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity,
                                const glm::vec3& camPos, const glm::vec3& hazeColor,
                                float fadeStart, float fadeEnd) {
    tickRetired(); // reap deferred buffer frees whose in-flight frames have completed
    if (!config_.enabled || deviceVertCount_ == 0) return;

    CameraUBO ubo{view, proj, sunDirAmbient, sunColIntensity,
                  glm::vec4(camPos, fadeStart), glm::vec4(hazeColor, fadeEnd),
                  glm::vec4(fadeNear_, 56.0f, 0.0f, 0.0f)};
    uniformBuffers_[frameIndex].upload(&ubo, sizeof(ubo));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &descriptorSets_[frameIndex], 0, nullptr);
    VkBuffer     vb   = deviceVB_.handle();
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdDraw(cmd, deviceVertCount_, 1, 0, 0);
}

// --- Vulkan setup (mirrors EntityRenderer) -----------------------------------

VkShaderModule FarTerrainRenderer::loadShader(const std::string& path) const {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx_.device(), &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("FarTerrainRenderer: shader module failed: " + path);
    }
    return module;
}

void FarTerrainRenderer::createPipeline(VkRenderPass renderPass, const std::string& shaderDir) {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding         = 0;
    uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding         = 1;
    samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding bindings[] = {uboBinding, samplerBinding};
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 2;
    dslInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx_.device(), &dslInfo, nullptr, &descriptorSetLayout_) !=
        VK_SUCCESS) {
        throw std::runtime_error("FarTerrainRenderer: descriptor set layout failed");
    }

    VkShaderModule vert = loadShader(shaderDir + "/farterrain.vert.spv");
    VkShaderModule frag = loadShader(shaderDir + "/farterrain.frag.spv");
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vert;
    vertStage.pName  = "main";
    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = frag;
    fragStage.pName  = "main";
    VkPipelineShaderStageCreateInfo stages[] = {vertStage, fragStage};

    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(FarVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 5> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FarVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(FarVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(FarVertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32_UINT,         offsetof(FarVertex, layer)};
    attrs[4] = {4, 0, VK_FORMAT_R8G8B8A8_UNORM,   offsetof(FarVertex, tint)};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertexInput.pVertexAttributeDescriptions    = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth   = 1.0f;
    // Double-sided: the heightmap is seen from above and the skirts from outside;
    // not culling sidesteps every winding question (normals are computed per-tri).
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthClampEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_GREATER; // reversed-Z (near=1, far=0)

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts    = &descriptorSetLayout_;
    if (vkCreatePipelineLayout(ctx_.device(), &layoutInfo, nullptr, &pipelineLayout_) !=
        VK_SUCCESS) {
        throw std::runtime_error("FarTerrainRenderer: pipeline layout failed");
    }

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState      = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState   = &multisample;
    info.pDepthStencilState  = &depthStencil;
    info.pColorBlendState    = &colorBlend;
    info.pDynamicState       = &dynamicState;
    info.layout              = pipelineLayout_;
    info.renderPass          = renderPass;
    info.subpass             = 0;
    const VkResult r = vkCreateGraphicsPipelines(ctx_.device(), VK_NULL_HANDLE, 1, &info,
                                                 nullptr, &pipeline_);
    vkDestroyShaderModule(ctx_.device(), vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), frag, nullptr);
    if (r != VK_SUCCESS) throw std::runtime_error("FarTerrainRenderer: pipeline failed");
}

void FarTerrainRenderer::createUniformBuffers(uint32_t n) {
    uniformBuffers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uniformBuffers_.emplace_back(ctx_, sizeof(CameraUBO),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

void FarTerrainRenderer::createDescriptorSets(uint32_t n, VkImageView view, VkSampler sampler) {
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
        throw std::runtime_error("FarTerrainRenderer: descriptor pool failed");
    }
    std::vector<VkDescriptorSetLayout> layouts(n, descriptorSetLayout_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = descriptorPool_;
    alloc.descriptorSetCount = n;
    alloc.pSetLayouts        = layouts.data();
    descriptorSets_.resize(n);
    if (vkAllocateDescriptorSets(ctx_.device(), &alloc, descriptorSets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("FarTerrainRenderer: descriptor set alloc failed");
    }
    for (uint32_t i = 0; i < n; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniformBuffers_[i].handle();
        bufferInfo.offset = 0;
        bufferInfo.range  = sizeof(CameraUBO);
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView   = view;
        imageInfo.sampler     = sampler;
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

} // namespace vg
