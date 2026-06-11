#include "render/CompositeRenderer.h"

#include "render/VulkanContext.h"

#include <array>
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
    float     lowRes[2];
    float     submerged; // 0 = above water, 1 = camera underwater
};
} // namespace

CompositeRenderer::CompositeRenderer(VulkanContext& ctx, VkRenderPass targetRenderPass)
    : ctx_(ctx) {
    createSampler();
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
    // Bindings: 0 scene colour, 1 depth.
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &size;
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

void CompositeRenderer::setSource(VkImageView sceneView, VkImageView depthView) {
    VkDescriptorImageInfo imgs[2]{};
    imgs[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgs[0].imageView   = sceneView;
    imgs[0].sampler     = sampler_;
    imgs[1].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    imgs[1].imageView   = depthView;
    imgs[1].sampler     = sampler_;

    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set_;
        writes[i].dstBinding      = i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = &imgs[i];
    }
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
    pc.lowRes[0]   = static_cast<float>(lowRes.width);
    pc.lowRes[1]   = static_cast<float>(lowRes.height);
    pc.submerged   = fog.submerged;
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set_,
                            0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace vg
