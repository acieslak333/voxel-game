#include "render/SkyRenderer.h"

#include "render/VulkanContext.h"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace vg {

namespace {

VkShaderModule loadModule(VkDevice device, const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("SkyRenderer: failed to open " + path);
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<char> code(size);
    file.seekg(0);
    file.read(code.data(), static_cast<std::streamsize>(size));

    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &m) != VK_SUCCESS) {
        throw std::runtime_error("SkyRenderer: bad shader module " + path);
    }
    return m;
}

} // namespace

SkyRenderer::SkyRenderer(VulkanContext& ctx, VkRenderPass sceneRenderPass,
                         uint32_t framesInFlight, const std::string& shaderDir,
                         VkImageView cloudBaseNoise, VkImageView cloudDetailNoise,
                         VkImageView weatherMap, VkSampler cloudSampler)
    : ctx_(ctx) {
    createDescriptors(framesInFlight, cloudBaseNoise, cloudDetailNoise, weatherMap,
                      cloudSampler);
    createPipeline(sceneRenderPass, shaderDir);
}

SkyRenderer::~SkyRenderer() {
    VkDevice device = ctx_.device();
    if (pipeline_)       vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (pool_)           vkDestroyDescriptorPool(device, pool_, nullptr);
    if (setLayout_)      vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
}

void SkyRenderer::createDescriptors(uint32_t n, VkImageView base, VkImageView detail,
                                    VkImageView weather, VkSampler sampler) {
    uniformBuffers_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        uniformBuffers_.emplace_back(ctx_, sizeof(SkyUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }

    // Binding 0: the sky/cloud UBO. Bindings 1-3: cloud base noise (3D), cloud
    // detail noise (3D), and the weather map (2D), all static for the session.
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding         = i;
        bindings[i].descriptorType  = (i == 0) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = static_cast<uint32_t>(bindings.size());
    li.pBindings    = bindings.data();
    if (vkCreateDescriptorSetLayout(ctx_.device(), &li, nullptr, &setLayout_) != VK_SUCCESS) {
        throw std::runtime_error("SkyRenderer: descriptor set layout failed");
    }

    std::array<VkDescriptorPoolSize, 2> sizes{};
    sizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n};
    sizes[1] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n * 3};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pi.pPoolSizes    = sizes.data();
    pi.maxSets       = n;
    if (vkCreateDescriptorPool(ctx_.device(), &pi, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("SkyRenderer: descriptor pool failed");
    }

    std::vector<VkDescriptorSetLayout> layouts(n, setLayout_);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = pool_;
    ai.descriptorSetCount = n;
    ai.pSetLayouts        = layouts.data();
    sets_.resize(n);
    if (vkAllocateDescriptorSets(ctx_.device(), &ai, sets_.data()) != VK_SUCCESS) {
        throw std::runtime_error("SkyRenderer: descriptor set alloc failed");
    }

    for (uint32_t i = 0; i < n; ++i) {
        VkDescriptorBufferInfo info{};
        info.buffer = uniformBuffers_[i].handle();
        info.offset = 0;
        info.range  = sizeof(SkyUBO);

        VkDescriptorImageInfo images[3]{};
        const VkImageView views[3] = {base, detail, weather};
        for (int s = 0; s < 3; ++s) {
            images[s].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            images[s].imageView   = views[s];
            images[s].sampler     = sampler;
        }

        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = sets_[i];
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo     = &info;
        for (uint32_t b = 1; b < 4; ++b) {
            writes[b].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet          = sets_[i];
            writes[b].dstBinding      = b;
            writes[b].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[b].descriptorCount = 1;
            writes[b].pImageInfo      = &images[b - 1];
        }
        vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
}

void SkyRenderer::createPipeline(VkRenderPass renderPass, const std::string& shaderDir) {
    VkShaderModule vert = loadModule(ctx_.device(), shaderDir + "/sky.vert.spv");
    VkShaderModule frag = loadModule(ctx_.device(), shaderDir + "/sky.frag.spv");

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

    // No vertex buffers: the fullscreen triangle comes from gl_VertexIndex.
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

    // The scene pass has a depth attachment, but the sky neither tests nor
    // writes it — the world simply draws over the sky afterwards.
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable = VK_FALSE; // opaque: replaces the clear colour

    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyns{};
    dyns.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyns.dynamicStateCount = 2;
    dyns.pDynamicStates    = dyn;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts    = &setLayout_;
    if (vkCreatePipelineLayout(ctx_.device(), &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        throw std::runtime_error("SkyRenderer: pipeline layout failed");
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
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &dyns;
    info.layout              = pipelineLayout_;
    info.renderPass          = renderPass;
    info.subpass             = 0;
    const VkResult r =
        vkCreateGraphicsPipelines(ctx_.device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_);
    vkDestroyShaderModule(ctx_.device(), vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), frag, nullptr);
    if (r != VK_SUCCESS) {
        throw std::runtime_error("SkyRenderer: pipeline creation failed");
    }
}

void SkyRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent,
                         const glm::mat4& view, const glm::mat4& proj,
                         const glm::vec3& camPos, const DayNight::SkyState& s,
                         const CloudSystem::GpuParams& clouds) {
    // Strip the camera translation: the sky is at infinity, so only the view
    // rotation matters for the per-pixel ray reconstruction. The cloud raymarch
    // does need the real camera position, passed separately in the UBO.
    const glm::mat4 rotOnly = glm::mat4(glm::mat3(view));

    SkyUBO ubo{};
    ubo.invViewProj = glm::inverse(proj * rotOnly);
    ubo.sunDir      = glm::vec4(s.sunDir, s.skyBlend);
    ubo.moonDir     = glm::vec4(s.moonDir, s.exposure);
    ubo.zenith      = glm::vec4(s.zenith, s.analyticSky ? 1.0f : 0.0f);
    ubo.horizon     = glm::vec4(s.horizon, s.sunIntensity);
    ubo.sunDisc     = glm::vec4(s.sunDisc, s.cosSunOuter);
    ubo.moonDisc    = glm::vec4(s.moonDisc, s.cosMoonOuter);
    ubo.params      = glm::vec4(s.glow, s.cosSunInner, s.cosMoonInner, s.mieG);
    ubo.betaR       = glm::vec4(s.betaR, s.betaM);
    ubo.tint        = glm::vec4(s.zenithTint, s.sunsetStrength);
    ubo.sunset      = glm::vec4(s.sunsetColor, s.sunsetAmount);
    ubo.sunsetMid   = glm::vec4(s.sunsetMid, 0.0f);
    ubo.sunsetHigh  = glm::vec4(s.sunsetHigh, 0.0f);
    ubo.ozone       = glm::vec4(s.ozone, s.ozoneStrength);
    ubo.cloudDusk   = glm::vec4(s.cloudDusk, s.cloudDuskAmt);
    ubo.clouds      = clouds;
    ubo.camPos      = glm::vec4(camPos, 0.0f);
    ubo.star        = glm::vec4(s.siderealAngle, s.latitude, s.starBrightness, s.milkyWay);
    ubo.star2       = glm::vec4(s.twinkleSpeed, s.starExtinction, s.planets, s.shootingStars);
    uniformBuffers_[frameIndex].upload(&ubo, sizeof(ubo));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                        static_cast<float>(extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &sets_[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace vg
