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
} // namespace

FarTerrainRenderer::FarTerrainRenderer(VulkanContext& ctx, VkRenderPass renderPass,
                                       uint32_t framesInFlight, const std::string& shaderDir,
                                       VkImageView textureView, VkSampler textureSampler,
                                       const Config& config)
    : ctx_(ctx), config_(config), framesInFlight_(framesInFlight) {
    if (!config_.enabled) {
        return; // inert: no pipeline/buffers created
    }
    createPipeline(renderPass, shaderDir);
    createUniformBuffers(framesInFlight);
    vertexBuffers_.reserve(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        vertexBuffers_.emplace_back(ctx_, kMaxVerts * sizeof(FarVertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
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
        s.tint  = world.isVegTintable(ci.topId) ? packTint(ci.vegTint) : kNoTint;
    }
    return s;
}

int FarTerrainRenderer::outerExtentBlocks(const World& world) const {
    if (!config_.enabled) return 0;
    const int base = std::max(1, config_.baseStep);
    const glm::ivec3 counts = world.chunkCounts();
    const int windowHalf = (counts.x / 2) * Chunk::kSize;
    int holeHalf = windowHalf - config_.underlap;
    if (holeHalf < 0) holeHalf = 0;
    holeHalf = ((holeHalf + base - 1) / base) * base;
    int inner = holeHalf;
    for (int L = 0; L < config_.ringCount; ++L) {
        const int step = base << L;
        const int innerL = ((inner + step - 1) / step) * step;
        inner = innerL + config_.ringCells * step;
    }
    return inner; // outermost ring's outer half-extent
}

void FarTerrainRenderer::update(const World& world, const glm::vec3& camPos) {
    if (!config_.enabled) {
        return;
    }
    if (!waterResolved_) {
        try {
            waterLayer_ = world.registry().faceLayer(world.registry().idByName("water"), FacePosY);
        } catch (...) {
            waterLayer_ = 0;
        }
        waterResolved_ = true;
    }
    // Collect a finished background rebuild (swap on the main thread; record() also
    // runs on the main thread, so mesh_ is never read mid-swap).
    if (buildFuture_.valid() &&
        buildFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        mesh_ = buildFuture_.get();
    }
    const int base = std::max(1, config_.baseStep);
    const glm::ivec2 c{floorDiv(static_cast<int>(std::floor(camPos.x)), base) * base,
                       floorDiv(static_cast<int>(std::floor(camPos.z)), base) * base};
    // Kick a rebuild when we've crossed a base cell and none is already in flight.
    // The generator/registry are immutable, so the worker only reads constant data
    // — it never races the main thread's chunk/light mutations.
    if ((!built_ || c != lastCenter_) && !buildFuture_.valid()) {
        lastCenter_ = c;
        built_ = true;
        buildFuture_ = std::async(std::launch::async,
                                  [this, &world, c] { return buildMesh(world, c); });
    }
}

std::vector<FarTerrainRenderer::FarVertex> FarTerrainRenderer::buildMesh(const World& world,
                                                                         glm::ivec2 center) const {
    std::vector<FarVertex> mesh;
    const int base = std::max(1, config_.baseStep);
    const int centerX = center.x;
    const int centerZ = center.y;

    // Start the shell just outside the voxel window (minus a small underlap so the
    // near chunks always cover the seam). The window is (2*view_radius+1) chunks.
    const glm::ivec3 counts = world.chunkCounts();
    const int windowHalf = (counts.x / 2) * Chunk::kSize;
    int holeHalf = windowHalf - config_.underlap;
    if (holeHalf < 0) holeHalf = 0;
    holeHalf = ((holeHalf + base - 1) / base) * base;

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

    int inner = holeHalf;
    for (int L = 0; L < config_.ringCount; ++L) {
        const int step = base << L;
        const int innerL = ((inner + step - 1) / step) * step; // snap to this ring's grid
        const int outerL = innerL + config_.ringCells * step;
        inner = outerL; // the next coarser ring takes over here

        auto inAnnulus = [&](int cellX, int cellZ) -> bool {
            const int ccx = cellX + step / 2, ccz = cellZ + step / 2;
            const int cheb = std::max(std::abs(ccx - centerX), std::abs(ccz - centerZ));
            return cheb >= innerL && cheb < outerL;
        };

        const int startX = floorDiv(centerX - outerL, step) * step;
        const int startZ = floorDiv(centerZ - outerL, step) * step;
        for (int cx = startX; cx < centerX + outerL; cx += step) {
            for (int cz = startZ; cz < centerZ + outerL; cz += step) {
                if (!inAnnulus(cx, cz)) continue;

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
                if (!inAnnulus(cx - step, cz)) skirt(pa, pd); // -X
                if (!inAnnulus(cx + step, cz)) skirt(pb, pc); // +X
                if (!inAnnulus(cx, cz - step)) skirt(pa, pb); // -Z
                if (!inAnnulus(cx, cz + step)) skirt(pd, pc); // +Z
            }
        }
    }
    return mesh;
}

// --- Recording ---------------------------------------------------------------

void FarTerrainRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                                const glm::mat4& view, const glm::mat4& proj,
                                const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity) {
    if (!config_.enabled || mesh_.empty()) return;

    CameraUBO ubo{view, proj, sunDirAmbient, sunColIntensity};
    uniformBuffers_[frameIndex].upload(&ubo, sizeof(ubo));

    const size_t n = std::min(static_cast<size_t>(kMaxVerts), mesh_.size());
    vertexBuffers_[frameIndex].upload(mesh_.data(), n * sizeof(FarVertex));

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
    VkBuffer     vb   = vertexBuffers_[frameIndex].handle();
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
    vkCmdDraw(cmd, static_cast<uint32_t>(n), 1, 0, 0);
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
    depthStencil.depthCompareOp   = VK_COMPARE_OP_LESS;

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
