#pragma once

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;

// One UI vertex: screen-pixel position, UV, RGBA colour, and a texture-array
// layer (< 0 => sample the font atlas for text/solid quads; >= 0 => sample that
// layer of the block texture array, with `color` used as a shade multiplier).
struct UiVertex {
    glm::vec2 pos;
    glm::vec2 uv;
    glm::vec4 color;
    float     layer;
};

// -----------------------------------------------------------------------------
//  UiRenderer
// -----------------------------------------------------------------------------
//  A small immediate-mode 2D renderer for the HUD and menus. Bakes a TTF font to
//  an atlas (stb_truetype), then each frame you call begin(), emit rect()/text()
//  primitives in pixel coordinates, and record() batches them into one draw in
//  the UI render pass. Solid rects sample a white texel in the atlas so the same
//  pipeline draws both panels and anti-aliased text.
// -----------------------------------------------------------------------------
class UiRenderer {
public:
    UiRenderer(VulkanContext& ctx, VkRenderPass uiRenderPass, uint32_t framesInFlight,
               const std::string& fontPath, float fontPixelHeight,
               VkImageView blockTexView, VkSampler blockTexSampler);
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // Start a fresh batch for this frame.
    void begin(VkExtent2D screen);

    // Filled rectangle (top-left x,y; size w,h) in RGBA (sRGB, alpha 0..1).
    void rect(float x, float y, float w, float h, const glm::vec4& color);

    // Filled solid-colour triangle (three pixel-space corners). Lets the HUD draw
    // non-axis-aligned shapes such as the parallelogram faces of an iso block.
    void triangle(const glm::vec2& a, const glm::vec2& b, const glm::vec2& c,
                  const glm::vec4& color);

    // One textured face of a block icon: a parallelogram whose four corners (given
    // clockwise from the top-left) are mapped to the unit texture square, sampling
    // `layer` of the block texture array. `shade` tints it (rgb multiplier, alpha).
    void blockFace(const glm::vec2& c0, const glm::vec2& c1, const glm::vec2& c2,
                   const glm::vec2& c3, uint32_t layer, const glm::vec4& shade);

    // Draw a string with its top-left at (x,y); `scale` multiplies the baked font
    // size. Returns the advance width in pixels.
    float text(float x, float y, const std::string& s, const glm::vec4& color,
               float scale = 1.0f);

    [[nodiscard]] float textWidth(const std::string& s, float scale = 1.0f) const;
    [[nodiscard]] float lineHeight(float scale = 1.0f) const { return pixelHeight_ * scale; }

    // Upload the batch and draw it. Call inside the UI render pass.
    void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent);

    // Swap the font at runtime (re-bakes the atlas and repoints the descriptor).
    // The block-icon binding is left untouched.
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
