#include "render/EntityRenderer.h"

#include "render/VulkanContext.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <stdexcept>

namespace vg {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) throw std::runtime_error("EntityRenderer: cannot open " + path);
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<char> buf(size);
    file.seekg(0);
    file.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}
} // namespace

EntityRenderer::EntityRenderer(VulkanContext& ctx, VkRenderPass renderPass,
                               uint32_t framesInFlight, const std::string& shaderDir,
                               VkImageView textureView, VkSampler textureSampler,
                               VkImageView skinView, VkSampler skinSampler)
    : ctx_(ctx), framesInFlight_(framesInFlight) {
    createPipeline(renderPass, shaderDir);
    createUniformBuffers(framesInFlight);
    // One persistent host-visible vertex buffer per frame in flight (re-filled each
    // frame; never reallocated, so an in-flight frame's buffer is never freed).
    vertexBuffers_.reserve(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        vertexBuffers_.emplace_back(ctx_, kMaxVerts * sizeof(EntityVertex),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    createDescriptorSets(framesInFlight, textureView, textureSampler, skinView, skinSampler);
}

EntityRenderer::~EntityRenderer() {
    VkDevice device = ctx_.device();
    if (descriptorPool_)      vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
    if (pipeline_)            vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_)      vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
}

VkShaderModule EntityRenderer::loadShader(const std::string& path) const {
    std::vector<char> code = readFile(path);
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx_.device(), &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("EntityRenderer: shader module failed: " + path);
    }
    return module;
}

void EntityRenderer::createPipeline(VkRenderPass renderPass, const std::string& shaderDir) {
    // Descriptor layout: camera UBO (binding 0) + the block texture array (binding 1),
    // identical to the chunk pipeline so entities can sample the same textures.
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
    VkDescriptorSetLayoutBinding skinBinding{};       // binding 2: Blockbench skin atlas
    skinBinding.binding            = 2;
    skinBinding.descriptorType     = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    skinBinding.descriptorCount    = 1;
    skinBinding.stageFlags         = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutBinding bindings[] = {uboBinding, samplerBinding, skinBinding};
    VkDescriptorSetLayoutCreateInfo dslInfo{};
    dslInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslInfo.bindingCount = 3;
    dslInfo.pBindings    = bindings;
    if (vkCreateDescriptorSetLayout(ctx_.device(), &dslInfo, nullptr, &descriptorSetLayout_) !=
        VK_SUCCESS) {
        throw std::runtime_error("EntityRenderer: descriptor set layout failed");
    }

    VkShaderModule vert = loadShader(shaderDir + "/entity.vert.spv");
    VkShaderModule frag = loadShader(shaderDir + "/entity.frag.spv");
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

    // Vertex input: interleaved EntityVertex (pos, normal, uv, layer).
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(EntityVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 4> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(EntityVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(EntityVertex, normal)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(EntityVertex, uv)};
    attrs[3] = {3, 0, VK_FORMAT_R32_UINT,         offsetof(EntityVertex, layer)};
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
    raster.cullMode    = VK_CULL_MODE_BACK_BIT;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE; // bakeMesh emits CCW/outward
    raster.depthClampEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable  = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE; // entities are opaque
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

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = sizeof(glm::mat4) + sizeof(uint32_t); // model matrix + useSkin flag
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount         = 1;
    layoutInfo.pSetLayouts            = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges    = &pushRange;
    if (vkCreatePipelineLayout(ctx_.device(), &layoutInfo, nullptr, &pipelineLayout_) !=
        VK_SUCCESS) {
        throw std::runtime_error("EntityRenderer: pipeline layout failed");
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
    if (r != VK_SUCCESS) throw std::runtime_error("EntityRenderer: pipeline failed");
}

void EntityRenderer::createUniformBuffers(uint32_t n) {
    uniformBuffers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uniformBuffers_.emplace_back(ctx_, sizeof(CameraUBO),
                                     VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

void EntityRenderer::createDescriptorSets(uint32_t n, VkImageView view, VkSampler sampler,
                                          VkImageView skinView, VkSampler skinSampler) {
    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * n}; // block + skin per set
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes    = sizes.data();
    poolInfo.maxSets       = n;
    if (vkCreateDescriptorPool(ctx_.device(), &poolInfo, nullptr, &descriptorPool_) !=
        VK_SUCCESS) {
        throw std::runtime_error("EntityRenderer: descriptor pool failed");
    }
    std::vector<VkDescriptorSetLayout> layouts(n, descriptorSetLayout_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool     = descriptorPool_;
    alloc.descriptorSetCount = n;
    alloc.pSetLayouts        = layouts.data();
    descriptorSets_.resize(n);
    if (vkAllocateDescriptorSets(ctx_.device(), &alloc, descriptorSets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("EntityRenderer: descriptor set alloc failed");
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
        VkDescriptorImageInfo skinInfo{};
        skinInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        skinInfo.imageView   = skinView;
        skinInfo.sampler     = skinSampler;
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
        writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[2].descriptorCount = 1;
        writes[2].pImageInfo      = &skinInfo;
        vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void EntityRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                            const glm::mat4& view, const glm::mat4& proj,
                            const glm::vec4& sunDirAmbient, const glm::vec4& sunColIntensity,
                            const std::vector<Draw>& draws) {
    if (draws.empty()) return;

    CameraUBO ubo{view, proj, sunDirAmbient, sunColIntensity,
                  glm::vec4(0.0f)};
    uniformBuffers_[frameIndex].upload(&ubo, sizeof(ubo));

    // Concatenate every entity's baked mesh into this frame's vertex buffer,
    // remembering each draw's first vertex + count. Clamp to the buffer cap.
    struct Span { uint32_t first; uint32_t count; glm::mat4 model; uint32_t useSkin; };
    std::vector<Span> spans;
    spans.reserve(draws.size());
    std::vector<EntityVertex> all;
    for (const Draw& d : draws) {
        if (!d.mesh || d.mesh->empty()) continue;
        if (all.size() + d.mesh->size() > kMaxVerts) break; // drop overflow
        const uint32_t first = static_cast<uint32_t>(all.size());
        all.insert(all.end(), d.mesh->begin(), d.mesh->end());
        spans.push_back({first, static_cast<uint32_t>(d.mesh->size()), d.model, d.useSkin});
    }
    if (all.empty()) return;
    vertexBuffers_[frameIndex].upload(all.data(), all.size() * sizeof(EntityVertex));

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
    const VkShaderStageFlags pcStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (const Span& s : spans) {
        // Push the mat4 then the uint flag separately so the bytes are tightly packed
        // (a {mat4,uint} struct would pad to 80B and overrun the 68B push range).
        vkCmdPushConstants(cmd, pipelineLayout_, pcStages, 0, sizeof(glm::mat4), &s.model);
        vkCmdPushConstants(cmd, pipelineLayout_, pcStages, sizeof(glm::mat4),
                           sizeof(uint32_t), &s.useSkin);
        vkCmdDraw(cmd, s.count, 1, s.first, 0);
    }
}

} // namespace vg
