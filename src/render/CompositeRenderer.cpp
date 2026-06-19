/**
 * @file CompositeRenderer.cpp
 * @brief CompositeRenderer implementation: NEAREST sampler, descriptor/pipeline
 *        creation, and per-frame fog + retro post-effect recording.
 *
 * record() pushes fog + grain + submerge + pixel-scale constants, memcpys the
 * retro/palette UBO into the persistently-mapped bloom buffer, and issues
 * vkCmdDraw(3) for the fullscreen triangle.
 */
#include "render/CompositeRenderer.h"

#include "core/ColorPalette.h"
#include "render/VulkanContext.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#ifndef VG_SHADER_DIR
#define VG_SHADER_DIR "shaders"
#endif

namespace vg {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("CompositeRenderer: failed to open " + path);
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

VkShaderModule loadModule(VkDevice device, const std::string& path) {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &m) != VK_SUCCESS) {
        throw std::runtime_error("CompositeRenderer: bad shader module " + path);
    }
    return m;
}

// Matches composite.frag's push block exactly (std430). 124 bytes (< 128 min).
struct PushConstants {
    glm::mat4 invViewProj;
    glm::vec4 camPos;
    glm::vec4 fogColor;
    glm::vec4 fogParams;
    float     noise[2];  // x = dark-grain max opacity (0 = off), y = time (s)
    float     submerged; // 0 = above water, 1 = camera underwater
    float     pixel;     // grain cell size in screen px (= pixelate block size)
};

// Matches composite.frag's `Post` UBO (std140).
struct PostUbo {
    float retro[4];  // x levels/channel (0=off), y dither, z interlace
    float retro2[4]; // x frame parity (0/1)
    // Selectable retro palette. palMeta.x = swatch count (0 = off). palette holds
    // kMaxPaletteColors sRGB swatches as vec4 (std140 array stride = 16 bytes).
    float palMeta[4];
    float palette[kMaxPaletteColors][4];
};
} // namespace

CompositeRenderer::CompositeRenderer(VulkanContext& ctx, VkRenderPass targetRenderPass)
    : ctx_(ctx) {
    createSampler();
    // Bloom params live in a tiny host-visible UBO (the push block is already full
    // at 128 bytes). Persistently mapped; we just memcpy the latest values each frame.
    bloomUbo_ = Buffer(ctx_, sizeof(PostUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    bloomMapped_ = bloomUbo_.map();
    createDescriptor();
    createPipeline(targetRenderPass);
}

CompositeRenderer::~CompositeRenderer() {
    VkDevice device = ctx_.device();
    if (pipeline_)       vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (pool_)           vkDestroyDescriptorPool(device, pool_, nullptr);
    if (setLayout_)      vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    if (sampler_)        vkDestroySampler(device, sampler_, nullptr);
}

void CompositeRenderer::createSampler() {
    VkSamplerCreateInfo s{};
    s.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // NEAREST is what turns one low-res texel into a crisp block of pixels (the
    // pixelation the old blit did with VK_FILTER_NEAREST).
    s.magFilter    = VK_FILTER_NEAREST;
    s.minFilter    = VK_FILTER_NEAREST;
    s.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(ctx_.device(), &s, nullptr, &sampler_) != VK_SUCCESS) {
        throw std::runtime_error("CompositeRenderer: failed to create sampler");
    }
}

void CompositeRenderer::createDescriptor() {
    // Bindings: 0 scene colour (nearest), 1 depth, 2 post-FX UBO.
    std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = (i == 2) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(ctx_.device(), &li, nullptr, &setLayout_) != VK_SUCCESS) {
        throw std::runtime_error("CompositeRenderer: descriptor set layout failed");
    }

    std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, // scene-nearest, depth
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1}, // post-FX params
    }};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes    = sizes.data();
    pi.maxSets       = 1;
    if (vkCreateDescriptorPool(ctx_.device(), &pi, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("CompositeRenderer: descriptor pool failed");
    }

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &setLayout_;
    if (vkAllocateDescriptorSets(ctx_.device(), &ai, &set_) != VK_SUCCESS) {
        throw std::runtime_error("CompositeRenderer: descriptor set alloc failed");
    }
}

void CompositeRenderer::setPalette(const std::vector<glm::vec3>& srgbColors) {
    palette_ = srgbColors;
    if (static_cast<int>(palette_.size()) > kMaxPaletteColors) {
        palette_.resize(kMaxPaletteColors); // shader array is fixed-size
    }
}

void CompositeRenderer::setSource(VkImageView sceneView, VkImageView depthView) {
    // Two image descriptors: scene (nearest, base) + depth.
    VkDescriptorImageInfo imgs[2]{};
    imgs[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgs[0].imageView   = sceneView;
    imgs[0].sampler     = sampler_;
    imgs[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imgs[1].imageView   = depthView;
    imgs[1].sampler     = sampler_;

    VkDescriptorBufferInfo ubo{};
    ubo.buffer = bloomUbo_.handle();
    ubo.offset = 0;
    ubo.range  = sizeof(PostUbo);

    std::array<VkWriteDescriptorSet, 3> writes{};
    auto imageWrite = [&](uint32_t idx, uint32_t binding, const VkDescriptorImageInfo* info) {
        writes[idx].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[idx].dstSet          = set_;
        writes[idx].dstBinding      = binding;
        writes[idx].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[idx].descriptorCount = 1;
        writes[idx].pImageInfo      = info;
    };
    imageWrite(0, 0, &imgs[0]); // binding 0: scene (nearest)
    imageWrite(1, 1, &imgs[1]); // binding 1: depth
    writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet          = set_;
    writes[2].dstBinding      = 2;             // binding 2: post-FX UBO
    writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo     = &ubo;

    vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void CompositeRenderer::createPipeline(VkRenderPass targetRenderPass) {
    VkShaderModule vert = loadModule(ctx_.device(), std::string(VG_SHADER_DIR) + "/composite.vert.spv");
    VkShaderModule frag = loadModule(ctx_.device(), std::string(VG_SHADER_DIR) + "/composite.frag.spv");

    VkPipelineShaderStageCreateInfo vs{};
    vs.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vs.stage  = VK_SHADER_STAGE_VERTEX_BIT;
    vs.module = vert;
    vs.pName  = "main";
    VkPipelineShaderStageCreateInfo fs{};
    fs.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fs.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    fs.module = frag;
    fs.pName  = "main";
    VkPipelineShaderStageCreateInfo stages[] = {vs, fs};

    // No vertex input: the fullscreen triangle is generated from gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth   = 1.0f;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Opaque: the composite overwrites the whole screen.
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates    = dyn;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset     = 0;
    push.size       = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pl{};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &setLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(ctx_.device(), &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        throw std::runtime_error("CompositeRenderer: pipeline layout failed");
    }

    VkGraphicsPipelineCreateInfo info{};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState   = &ms;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &ds;
    info.layout              = pipelineLayout_;
    info.renderPass          = targetRenderPass;
    info.subpass             = 0;
    const VkResult r = vkCreateGraphicsPipelines(ctx_.device(), VK_NULL_HANDLE, 1, &info,
                                                 nullptr, &pipeline_);
    vkDestroyShaderModule(ctx_.device(), vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), frag, nullptr);
    if (r != VK_SUCCESS) {
        throw std::runtime_error("CompositeRenderer: pipeline creation failed");
    }
}

void CompositeRenderer::record(VkCommandBuffer cmd, VkExtent2D screen, VkExtent2D lowRes,
                               const Fog& fog) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(screen.width),
                        static_cast<float>(screen.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, screen};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PushConstants pc{};
    pc.invViewProj = fog.invViewProj;
    pc.camPos      = fog.camPos;
    pc.fogColor    = fog.color;
    pc.fogParams   = fog.params;
    pc.noise[0]    = fog.noiseAmount;
    pc.noise[1]    = fog.noiseTime;
    pc.submerged   = fog.submerged;
    // Screen px per offscreen texel == the pixelate factor, so the grain cells line
    // up with the chunky upscaled blocks rather than being finer 1-px static.
    pc.pixel       = static_cast<float>(screen.width) /
                     static_cast<float>(std::max(1u, lowRes.width));
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    // Refresh the post UBO (quasi-static; a 1-frame-stale value during a slider drag
    // is invisible, so a single coherent buffer is fine — no per-frame copies).
    PostUbo u{};
    u.retro[0]  = fog.retroLevels;
    u.retro[1]  = fog.retroDither;
    u.retro[2]  = fog.retroInterlace;
    u.retro2[0] = fog.retroParity;
    // Retro palette: count + swatches (sRGB). count 0 => composite.frag uses the
    // per-channel quantiser instead. Capped at kMaxPaletteColors by setPalette.
    const int palCount = static_cast<int>(palette_.size());
    u.palMeta[0] = static_cast<float>(palCount);
    for (int i = 0; i < palCount; ++i) {
        u.palette[i][0] = palette_[i].r;
        u.palette[i][1] = palette_[i].g;
        u.palette[i][2] = palette_[i].b;
        u.palette[i][3] = 1.0f;
    }
    std::memcpy(bloomMapped_, &u, sizeof(u));

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set_,
                            0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace vg
