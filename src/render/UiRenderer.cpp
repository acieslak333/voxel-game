/**
 * @file UiRenderer.cpp
 * @brief UiRenderer implementation: TTF atlas baking, primitive emit helpers,
 *        pipeline setup, and per-frame vertex upload + draw recording.
 *
 * bakeFont() uses stb_truetype to rasterise glyphs into a 512x512 R8 atlas;
 * a 2x2 white block in the bottom-right corner is reserved for solid-colour
 * quads so the same pipeline handles text, panels, and block icons.
 */
#include "render/UiRenderer.h"

#include "render/VulkanContext.h"
#include "render/VulkanUtils.h"

// stb_truetype: single-header TTF rasteriser. Define the implementation here.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4456 4457)
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

#ifndef VG_SHADER_DIR
#define VG_SHADER_DIR "shaders"
#endif

namespace vg {

namespace {
std::vector<char> readFile(const std::string& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("UiRenderer: failed to open " + path);
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
        throw std::runtime_error("UiRenderer: bad shader module " + path);
    }
    return m;
}
} // namespace

UiRenderer::UiRenderer(VulkanContext& ctx, VkRenderPass uiRenderPass, uint32_t framesInFlight,
                       const std::string& fontPath, float fontPixelHeight,
                       VkImageView blockTexView, VkSampler blockTexSampler)
    : ctx_(ctx), pixelHeight_(fontPixelHeight),
      blockView_(blockTexView), blockSampler_(blockTexSampler) {
    bakeFont(fontPath);
    createDescriptor();
    createPipeline(uiRenderPass);
    createVertexBuffers(framesInFlight);
    verts_.reserve(kMaxVerts);
}

UiRenderer::~UiRenderer() {
    VkDevice device = ctx_.device();
    if (pipeline_)       vkDestroyPipeline(device, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
    if (pool_)           vkDestroyDescriptorPool(device, pool_, nullptr);
    if (setLayout_)      vkDestroyDescriptorSetLayout(device, setLayout_, nullptr);
    if (fontSampler_)    vkDestroySampler(device, fontSampler_, nullptr);
    if (fontView_)       vkDestroyImageView(device, fontView_, nullptr);
    if (fontImage_)      vkDestroyImage(device, fontImage_, nullptr);
    if (fontMemory_)     vkFreeMemory(device, fontMemory_, nullptr);
}

void UiRenderer::bakeFont(const std::string& fontPath) {
    std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("UiRenderer: failed to open font " + fontPath);
    }
    const auto size = static_cast<size_t>(file.tellg());
    std::vector<unsigned char> ttf(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(ttf.data()), static_cast<std::streamsize>(size));

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
        throw std::runtime_error("UiRenderer: invalid font " + fontPath);
    }
    const float scale = stbtt_ScaleForPixelHeight(&info, pixelHeight_);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    ascent_ = static_cast<float>(asc) * scale;

    constexpr int W = 512, H = 512;
    std::vector<uint8_t> bitmap(static_cast<size_t>(W) * H, 0);
    std::array<stbtt_bakedchar, kCharCount> baked{};
    const int res = stbtt_BakeFontBitmap(ttf.data(), 0, pixelHeight_, bitmap.data(), W, H,
                                         kFirstChar, kCharCount, baked.data());
    if (res == 0) {
        throw std::runtime_error("UiRenderer: font atlas too small to bake");
    }

    // Reserve a 2x2 white block (bottom-right corner) for solid-colour quads.
    for (int y = H - 2; y < H; ++y) {
        for (int x = W - 2; x < W; ++x) {
            bitmap[static_cast<size_t>(y) * W + x] = 255;
        }
    }
    whiteU_ = (W - 1.0f) / W;
    whiteV_ = (H - 1.0f) / H;

    for (int i = 0; i < kCharCount; ++i) {
        const stbtt_bakedchar& b = baked[static_cast<size_t>(i)];
        Glyph& g = glyphs_[static_cast<size_t>(i)];
        g.u0 = b.x0 / static_cast<float>(W);
        g.v0 = b.y0 / static_cast<float>(H);
        g.u1 = b.x1 / static_cast<float>(W);
        g.v1 = b.y1 / static_cast<float>(H);
        g.xoff = b.xoff;
        g.yoff = b.yoff;
        g.w = static_cast<float>(b.x1 - b.x0);
        g.h = static_cast<float>(b.y1 - b.y0);
        g.xadvance = b.xadvance;
    }

    createFontTexture(bitmap, W, H);
}

void UiRenderer::createFontTexture(const std::vector<uint8_t>& pixels, int w, int h) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h;
    Buffer staging(ctx_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    staging.upload(pixels.data(), bytes);

    vkutil::createImage(ctx_, static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1,
                        VK_FORMAT_R8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, fontImage_, fontMemory_);
    vkutil::transitionImageLayout(ctx_, fontImage_, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    vkutil::copyBufferToImage(ctx_, staging.handle(), fontImage_, static_cast<uint32_t>(w),
                              static_cast<uint32_t>(h), 1);
    vkutil::transitionImageLayout(ctx_, fontImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    fontView_ = vkutil::createImageView(ctx_.device(), fontImage_, VK_FORMAT_R8_UNORM,
                                        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, 1);

    VkSamplerCreateInfo s{};
    s.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    s.magFilter    = VK_FILTER_LINEAR;
    s.minFilter    = VK_FILTER_LINEAR;
    s.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    s.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (vkCreateSampler(ctx_.device(), &s, nullptr, &fontSampler_) != VK_SUCCESS) {
        throw std::runtime_error("UiRenderer: failed to create font sampler");
    }
}

void UiRenderer::createDescriptor() {
    // Binding 0: font atlas (text + solid quads). Binding 1: block texture array
    // (block icons). Both are sampled in the fragment shader.
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
        throw std::runtime_error("UiRenderer: descriptor set layout failed");
    }

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &size;
    pi.maxSets       = 1;
    if (vkCreateDescriptorPool(ctx_.device(), &pi, nullptr, &pool_) != VK_SUCCESS) {
        throw std::runtime_error("UiRenderer: descriptor pool failed");
    }

    VkDescriptorSetAllocateInfo ai{};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &setLayout_;
    if (vkAllocateDescriptorSets(ctx_.device(), &ai, &set_) != VK_SUCCESS) {
        throw std::runtime_error("UiRenderer: descriptor set alloc failed");
    }

    VkDescriptorImageInfo fontImg{};
    fontImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    fontImg.imageView   = fontView_;
    fontImg.sampler     = fontSampler_;
    VkDescriptorImageInfo blockImg{};
    blockImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    blockImg.imageView   = blockView_;
    blockImg.sampler     = blockSampler_;

    std::array<VkWriteDescriptorSet, 2> writes{};
    const VkDescriptorImageInfo* infos[2] = {&fontImg, &blockImg};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet          = set_;
        writes[i].dstBinding      = i;
        writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].descriptorCount = 1;
        writes[i].pImageInfo      = infos[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void UiRenderer::setFont(const std::string& fontPath) {
    VkDevice device = ctx_.device();
    vkDeviceWaitIdle(device); // the font atlas may still be in use by a frame

    if (fontSampler_) { vkDestroySampler(device, fontSampler_, nullptr); fontSampler_ = VK_NULL_HANDLE; }
    if (fontView_)    { vkDestroyImageView(device, fontView_, nullptr);  fontView_   = VK_NULL_HANDLE; }
    if (fontImage_)   { vkDestroyImage(device, fontImage_, nullptr);     fontImage_  = VK_NULL_HANDLE; }
    if (fontMemory_)  { vkFreeMemory(device, fontMemory_, nullptr);      fontMemory_ = VK_NULL_HANDLE; }

    bakeFont(fontPath); // recreates the atlas image/view/sampler and glyph metrics

    VkDescriptorImageInfo fontImg{};
    fontImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    fontImg.imageView   = fontView_;
    fontImg.sampler     = fontSampler_;
    VkWriteDescriptorSet write{};
    write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet          = set_;
    write.dstBinding      = 0;
    write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo      = &fontImg;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void UiRenderer::createPipeline(VkRenderPass uiRenderPass) {
    VkShaderModule vert = loadModule(ctx_.device(), std::string(VG_SHADER_DIR) + "/ui.vert.spv");
    VkShaderModule frag = loadModule(ctx_.device(), std::string(VG_SHADER_DIR) + "/ui.frag.spv");

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

    VkVertexInputBindingDescription bind{};
    bind.binding   = 0;
    bind.stride    = sizeof(UiVertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 4> attrs{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UiVertex, uv)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UiVertex, color)};
    attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(UiVertex, layer)};

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &bind;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions    = attrs.data();

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

    // Standard alpha blending for text/translucent panels.
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;

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
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset     = 0;
    push.size       = sizeof(glm::vec2);

    VkPipelineLayoutCreateInfo pl{};
    pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount         = 1;
    pl.pSetLayouts            = &setLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges    = &push;
    if (vkCreatePipelineLayout(ctx_.device(), &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) {
        throw std::runtime_error("UiRenderer: pipeline layout failed");
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
    info.renderPass          = uiRenderPass;
    info.subpass             = 0;
    const VkResult r = vkCreateGraphicsPipelines(ctx_.device(), VK_NULL_HANDLE, 1, &info,
                                                 nullptr, &pipeline_);
    vkDestroyShaderModule(ctx_.device(), vert, nullptr);
    vkDestroyShaderModule(ctx_.device(), frag, nullptr);
    if (r != VK_SUCCESS) {
        throw std::runtime_error("UiRenderer: pipeline creation failed");
    }
}

void UiRenderer::createVertexBuffers(uint32_t framesInFlight) {
    vertexBuffers_.reserve(framesInFlight);
    for (uint32_t i = 0; i < framesInFlight; ++i) {
        vertexBuffers_.emplace_back(ctx_, sizeof(UiVertex) * kMaxVerts,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
}

void UiRenderer::begin(VkExtent2D) {
    verts_.clear();
}

void UiRenderer::pushQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                          float v1, const glm::vec4& c) {
    if (verts_.size() + 6 > kMaxVerts) {
        return; // batch full; drop further geometry this frame
    }
    const UiVertex tl{{x0, y0}, {u0, v0}, c, -1.0f};
    const UiVertex tr{{x1, y0}, {u1, v0}, c, -1.0f};
    const UiVertex br{{x1, y1}, {u1, v1}, c, -1.0f};
    const UiVertex bl{{x0, y1}, {u0, v1}, c, -1.0f};
    verts_.push_back(tl);
    verts_.push_back(tr);
    verts_.push_back(br);
    verts_.push_back(tl);
    verts_.push_back(br);
    verts_.push_back(bl);
}

void UiRenderer::rect(float x, float y, float w, float h, const glm::vec4& color) {
    pushQuad(x, y, x + w, y + h, whiteU_, whiteV_, whiteU_, whiteV_, color);
}

void UiRenderer::triangle(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
                          const glm::vec4& color) {
    if (verts_.size() + 3 > kMaxVerts) {
        return; // batch full; drop further geometry this frame
    }
    // All three corners sample the atlas's white texel, so the solid colour shows
    // through unchanged (same trick rect() uses). Culling is disabled, so winding
    // does not matter. layer = -1 selects the font atlas in the shader.
    const glm::vec2 white{whiteU_, whiteV_};
    verts_.push_back({a, white, color, -1.0f});
    verts_.push_back({b, white, color, -1.0f});
    verts_.push_back({c, white, color, -1.0f});
}

void UiRenderer::blockFace(const glm::vec2& c0, const glm::vec2& c1, const glm::vec2& c2,
                           const glm::vec2& c3, uint32_t layer, const glm::vec4& shade) {
    if (verts_.size() + 6 > kMaxVerts) {
        return; // batch full; drop further geometry this frame
    }
    // Map the four corners to the unit texture square (c0=top-left .. c3=bottom-
    // left) and emit two triangles. layer >= 0 selects the block array sampler.
    const float L = static_cast<float>(layer);
    const UiVertex v0{c0, {0.0f, 0.0f}, shade, L};
    const UiVertex v1{c1, {1.0f, 0.0f}, shade, L};
    const UiVertex v2{c2, {1.0f, 1.0f}, shade, L};
    const UiVertex v3{c3, {0.0f, 1.0f}, shade, L};
    verts_.push_back(v0);
    verts_.push_back(v1);
    verts_.push_back(v2);
    verts_.push_back(v0);
    verts_.push_back(v2);
    verts_.push_back(v3);
}

void UiRenderer::sprite(float x, float y, float w, float h, uint32_t layer,
                        float u0, float v0, float u1, float v1, const glm::vec4& tint) {
    if (verts_.size() + 6 > kMaxVerts) {
        return; // batch full; drop further geometry this frame
    }
    const float L = static_cast<float>(layer); // layer >= 0 -> block array sampler
    const UiVertex tl{{x, y},         {u0, v0}, tint, L};
    const UiVertex tr{{x + w, y},     {u1, v0}, tint, L};
    const UiVertex br{{x + w, y + h}, {u1, v1}, tint, L};
    const UiVertex bl{{x, y + h},     {u0, v1}, tint, L};
    verts_.push_back(tl);
    verts_.push_back(tr);
    verts_.push_back(br);
    verts_.push_back(tl);
    verts_.push_back(br);
    verts_.push_back(bl);
}

float UiRenderer::text(float x, float y, const std::string& s, const glm::vec4& color,
                       float scale) {
    float penX = x;
    const float baseline = y + ascent_ * scale;
    for (char ch : s) {
        if (ch < kFirstChar || ch >= kFirstChar + kCharCount) {
            ch = '?';
        }
        const Glyph& g = glyphs_[static_cast<size_t>(ch - kFirstChar)];
        if (g.w > 0 && g.h > 0) {
            const float gx0 = penX + g.xoff * scale;
            const float gy0 = baseline + g.yoff * scale;
            pushQuad(gx0, gy0, gx0 + g.w * scale, gy0 + g.h * scale, g.u0, g.v0, g.u1, g.v1,
                     color);
        }
        penX += g.xadvance * scale;
    }
    return penX - x;
}

float UiRenderer::textWidth(const std::string& s, float scale) const {
    float w = 0.0f;
    for (char ch : s) {
        if (ch < kFirstChar || ch >= kFirstChar + kCharCount) {
            ch = '?';
        }
        w += glyphs_[static_cast<size_t>(ch - kFirstChar)].xadvance * scale;
    }
    return w;
}

void UiRenderer::record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent) {
    if (verts_.empty()) {
        return;
    }
    const uint32_t count = static_cast<uint32_t>(verts_.size());
    vertexBuffers_[frameIndex].upload(verts_.data(), sizeof(UiVertex) * count);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                        static_cast<float>(extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const glm::vec2 screen(static_cast<float>(extent.width), static_cast<float>(extent.height));
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(screen),
                       &screen);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &set_,
                            0, nullptr);

    VkBuffer vb = vertexBuffers_[frameIndex].handle();
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdDraw(cmd, count, 1, 0, 0);
}

} // namespace vg
