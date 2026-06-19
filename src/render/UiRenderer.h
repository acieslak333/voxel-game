#pragma once

/**
 * @file UiRenderer.h
 * @brief Declares UiRenderer, the immediate-mode 2D HUD and menu renderer.
 *
 * Bakes a TTF font to an atlas via stb_truetype at construction. Each frame the
 * caller emits rect/text/blockFace/sprite primitives in pixel coordinates; record()
 * batches them into one draw in the UI render pass. A shared white texel in the
 * atlas lets the same pipeline handle solid panels and anti-aliased text.
 * @see docs/CODE_INDEX.md
 */

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;

/**
 * @brief Per-vertex data for the UI batch.
 *
 * layer < 0 samples the font atlas (text + solid quads); layer >= 0 samples
 * that layer of the block texture array with @c color as a shade multiplier.
 */
struct UiVertex {
    glm::vec2 pos;
    glm::vec2 uv;
    glm::vec4 color;
    float     layer;
};

/**
 * @brief Immediate-mode 2D renderer for the HUD and menus.
 *
 * Usage per frame: begin() -> rect()/text()/blockFace()/sprite() -> record().
 * All primitives are batched into a single vertex buffer upload and one draw call.
 * The block texture array is bound as binding 1 so block icons can be drawn with
 * their real textures alongside the font atlas at binding 0.
 */
class UiRenderer {
public:
    /**
     * @brief Bake the font atlas and create the UI pipeline.
     * @param ctx              Vulkan device context.
     * @param uiRenderPass     Render pass the UI draws into (swapchain / UI pass).
     * @param framesInFlight   Number of frames in flight.
     * @param fontPath         Path to a TTF font file.
     * @param fontPixelHeight  Font size in pixels for atlas baking.
     * @param blockTexView     Block texture array view (binding 1, not owned).
     * @param blockTexSampler  Sampler for the block texture array.
     */
    UiRenderer(VulkanContext& ctx, VkRenderPass uiRenderPass, uint32_t framesInFlight,
               const std::string& fontPath, float fontPixelHeight,
               VkImageView blockTexView, VkSampler blockTexSampler);
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    /// Clear the vertex batch for this frame. Must be called before any primitive emit.
    void begin(VkExtent2D screen);

    /// Emit a filled rectangle (top-left x,y; size w,h) in RGBA (sRGB, alpha 0..1).
    void rect(float x, float y, float w, float h, const glm::vec4& color);

    /**
     * @brief Emit a filled solid-colour triangle (pixel-space corners).
     *
     * Samples the white atlas texel so the pipeline handles this identically to rect().
     * Useful for non-axis-aligned HUD shapes such as isometric block faces.
     */
    void triangle(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
                  const glm::vec4& color);

    /**
     * @brief Emit one textured face of a block icon.
     * @param c0..c3  Four corners clockwise from top-left in pixel space.
     * @param layer   Block texture array layer index.
     * @param shade   RGB shade multiplier + alpha.
     */
    void blockFace(const glm::vec2& c0, const glm::vec2& c1, const glm::vec2& c2,
                   const glm::vec2& c3, uint32_t layer, const glm::vec4& shade);

    /**
     * @brief Emit an axis-aligned textured quad from a sub-rect of a block array layer.
     * @param layer  Block texture array layer index.
     * @param tint   RGBA tint applied as a multiplier.
     */
    void sprite(float x, float y, float w, float h, uint32_t layer,
                float u0, float v0, float u1, float v1, const glm::vec4& tint);

    /**
     * @brief Emit a text string with its top-left at (x,y).
     * @param scale  Multiplier on the baked font size.
     * @return Advance width in pixels.
     */
    float text(float x, float y, const std::string& s, const glm::vec4& color,
               float scale = 1.0f);

    /// Return the pixel advance width of @p s at @p scale without emitting geometry.
    [[nodiscard]] float textWidth(const std::string& s, float scale = 1.0f) const;
    /// Return the line height in pixels at @p scale.
    [[nodiscard]] float lineHeight(float scale = 1.0f) const { return pixelHeight_ * scale; }

    /// Upload the vertex batch and record a single draw call. Must be inside the UI render pass.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent);

    /**
     * @brief Swap the font at runtime (re-bakes the atlas; block-icon binding untouched).
     * @param fontPath  Path to the new TTF font.
     * @note Issues vkDeviceWaitIdle since the old atlas may be in use by an in-flight frame.
     */
    void setFont(const std::string& fontPath);

private:
    // Per-glyph atlas data, derived from stb_truetype's baked chars (kept in the
    // .cpp so stb_truetype stays out of headers).
    struct Glyph {
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // atlas UVs
        float xoff = 0, yoff = 0;             // offset from pen, baked pixels
        float w = 0, h = 0;                   // quad size, baked pixels
        float xadvance = 0;                   // pen advance, baked pixels
    };

    void bakeFont(const std::string& fontPath);
    void createFontTexture(const std::vector<uint8_t>& pixels, int w, int h);
    void createDescriptor();
    void createPipeline(VkRenderPass uiRenderPass);
    void createVertexBuffers(uint32_t framesInFlight);
    void pushQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                  float v1, const glm::vec4& c);

    static constexpr int      kFirstChar = 32;
    static constexpr int      kCharCount = 95; // 32..126
    static constexpr uint32_t kMaxVerts  = 1u << 16;

    VulkanContext& ctx_;

    float pixelHeight_ = 0.0f;
    float ascent_      = 0.0f;            // baked-pixel ascent (top to baseline)
    float whiteU_ = 0.0f, whiteV_ = 0.0f; // UV of a white texel for solid quads
    std::array<Glyph, kCharCount> glyphs_{};

    VkImage        fontImage_  = VK_NULL_HANDLE;
    VkDeviceMemory fontMemory_ = VK_NULL_HANDLE;
    VkImageView    fontView_   = VK_NULL_HANDLE;
    VkSampler      fontSampler_ = VK_NULL_HANDLE;

    // Block texture array (owned by the world renderer), bound at set 0 binding 1
    // so block icons can be drawn with their real textures. Not owned here.
    VkImageView    blockView_    = VK_NULL_HANDLE;
    VkSampler      blockSampler_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_      = VK_NULL_HANDLE;
    VkDescriptorSet       set_       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;

    std::vector<UiVertex> verts_;          // CPU batch for the current frame
    std::vector<Buffer>   vertexBuffers_;  // one host-visible buffer per frame
};

} // namespace vg
