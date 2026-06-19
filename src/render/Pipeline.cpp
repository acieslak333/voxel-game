/**
 * @file Pipeline.cpp
 * @brief Implements Pipeline: descriptor set layout, pipeline layout, and VkPipeline creation.
 *
 * createDescriptorSetLayout() wires four bindings (camera UBO, texture array, draw-data
 * SSBO, light atlas). createPipeline() configures vertex input from Vertex::attributeDescriptions(),
 * reversed-Z depth test (GREATER), and optionally alpha blending + depth-write-off for
 * the translucent (water) variant.
 */

#include "render/Pipeline.h"

#include "render/Vertex.h"
#include "render/VulkanContext.h"

#include <fstream>
#include <stdexcept>
#include <vector>

namespace vg {

namespace {

std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path);
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), static_cast<std::streamsize>(size));
    return buffer;
}

} // namespace

Pipeline::Pipeline(VulkanContext& ctx, VkRenderPass renderPass,
                   const std::string& vertSpvPath, const std::string& fragSpvPath,
                   bool translucent)
    : ctx_(&ctx), translucent_(translucent) {
    createDescriptorSetLayout();
    createPipeline(renderPass, vertSpvPath, fragSpvPath);
}

Pipeline::~Pipeline() {
    if (!ctx_) {
        return;
    }
    VkDevice device = ctx_->device();
    if (pipeline_)            vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_)      vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
}

void Pipeline::createDescriptorSetLayout() {
    // binding 0: camera/sun uniform buffer. The vertex shader reads the matrices;
    // the fragment shader reads the sun direction/colour for day-night lighting.
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding         = 0;
    uboBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // binding 1: the block texture array, sampled in the fragment shader.
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding         = 1;
    samplerBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    // binding 2: per-chunk draw-data SSBO (world translation per slot) for the
    // GPU-driven (indirect) vertex shader, read via gl_InstanceIndex.
    VkDescriptorSetLayoutBinding drawDataBinding{};
    drawDataBinding.binding         = 2;
    drawDataBinding.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    drawDataBinding.descriptorCount = 1;
    drawDataBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;

    // binding 3: the per-chunk light atlas (3D texture, S7). The fragment shader
    // samples sky/block/hue here per-pixel instead of reading them interpolated
    // from the vertex, so lighting is decoupled from mesh geometry.
    VkDescriptorSetLayoutBinding lightBinding{};
    lightBinding.binding         = 3;
    lightBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lightBinding.descriptorCount = 1;
    lightBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding bindings[] = {uboBinding, samplerBinding, drawDataBinding,
                                               lightBinding};

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = 4;
    info.pBindings    = bindings;

    if (vkCreateDescriptorSetLayout(ctx_->device(), &info, nullptr, &descriptorSetLayout_) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout");
    }
}

VkShaderModule Pipeline::loadShaderModule(const std::string& path) const {
    std::vector<char> code = readFile(path);

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx_->device(), &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module: " + path);
    }
    return module;
}

void Pipeline::createPipeline(VkRenderPass renderPass, const std::string& vertSpv,
                              const std::string& fragSpv) {
    VkShaderModule vert = loadShaderModule(vertSpv);
    VkShaderModule frag = loadShaderModule(fragSpv);

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

    // Vertex input: one interleaved Vertex stream.
    auto binding    = Vertex::bindingDescription();
    auto attributes = Vertex::attributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount   = 1;
    vertexInput.pVertexBindingDescriptions      = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions    = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Viewport/scissor are dynamic; we only declare their counts here.
    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType            = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode      = VK_POLYGON_MODE_FILL;
    raster.lineWidth        = 1.0f;
    // Water is single-sided in the greedy mesh but should read from underneath
    // too (swimming, or a glass-like surface), so the translucent pass draws both
    // faces; opaque terrain keeps back-face culling.
    raster.cullMode         = translucent_ ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    raster.frontFace        = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.depthClampEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    // Translucent water tests depth (it's hidden behind opaque terrain) but does
    // NOT write it, so overlapping liquid surfaces don't occlude each other and
    // the seabed drawn behind still shows through.
    depthStencil.depthWriteEnable = translucent_ ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp   = VK_COMPARE_OP_GREATER; // reversed-Z (near=1, far=0)

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Standard src-alpha over-blend for the water pass; opaque pass overwrites.
    blendAttachment.blendEnable         = translucent_ ? VK_TRUE : VK_FALSE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments    = &blendAttachment;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates    = dynamicStates;

    // Pipeline layout: descriptor set + a model-matrix push constant.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset     = 0;
    pushRange.size       = kPushConstantSize;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;

    if (vkCreatePipelineLayout(ctx_->device(), &layoutInfo, nullptr, &pipelineLayout_) !=
        VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout");
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

    if (vkCreateGraphicsPipelines(ctx_->device(), VK_NULL_HANDLE, 1, &info, nullptr,
                                  &pipeline_) != VK_SUCCESS) {
        vkDestroyShaderModule(ctx_->device(), vert, nullptr);
        vkDestroyShaderModule(ctx_->device(), frag, nullptr);
        throw std::runtime_error("Failed to create graphics pipeline");
    }

    // Shader modules can be destroyed once the pipeline is built.
    vkDestroyShaderModule(ctx_->device(), vert, nullptr);
    vkDestroyShaderModule(ctx_->device(), frag, nullptr);
}

} // namespace vg
