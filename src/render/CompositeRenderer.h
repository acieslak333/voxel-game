#pragma once

/**
 * @file CompositeRenderer.h
 * @brief Declares CompositeRenderer, the full-screen post-processing pass.
 *
 * Upscales the low-res offscreen scene with NEAREST sampling (chunky pixelation),
 * applies ordered-dither posterisation, fog, a retro palette/interlace effect, and
 * optionally remaps colours to the nearest swatch in a user-chosen palette. Drawn
 * first in the swapchain (UI) render pass; the UI layer then blends on top.
 * @see docs/CODE_INDEX.md
 */

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace vg {

class VulkanContext;

/**
 * @brief Full-screen post pass: upscale + dither + fog + optional retro palette.
 *
 * Owns the composite pipeline and a single descriptor set pointing at the offscreen
 * scene colour and depth images. Call setSource() whenever the offscreen is rebuilt
 * (window resize or pixel-scale change). Fog and retro parameters are passed as a
 * push constant + host-coherent UBO each frame.
 */
class CompositeRenderer {
public:
    CompositeRenderer(VulkanContext& ctx, VkRenderPass targetRenderPass);
    ~CompositeRenderer();

    CompositeRenderer(const CompositeRenderer&) = delete;
    CompositeRenderer& operator=(const CompositeRenderer&) = delete;

    /// Per-frame fog and retro-post parameters consumed by composite.frag as a push constant.
    struct Fog {
        glm::mat4 invViewProj;   // NDC -> world (inverse of the scene's proj*view)
        glm::vec4 camPos;        // xyz camera world position
        glm::vec4 color;         // rgb haze colour, w = distance-fog density
        glm::vec4 params;        // x heightFalloff, y groundFogDensity, z fogTopY, w maxFog
        float     noiseAmount = 0.0f; // dark-grain max opacity (0 = off)
        float     noiseTime   = 0.0f; // seconds, reseeds the grain so it flickers
        float     submerged = 0.0f; // 0 = above water, 1 = camera underwater (blue murk)
        // Retro (PS1/PS2) post. All 0 = off (modern, byte-identical output).
        float     retroLevels    = 0.0f;  // colour levels/channel (0 = off, 32 = 5-bit)
        float     retroDither    = 0.0f;  // ordered-dither amount (0..1)
        float     retroInterlace = 0.0f;  // scanline dim amount (0..1, PS2)
        float     retroParity    = 0.0f;  // 0/1 frame parity, flips the interlace field
    };
    /**
     * @brief Point the descriptor at the offscreen colour and depth images.
     *
     * Must be called once at startup and again after the offscreen is recreated
     * (resize or pixel-scale change).
     *
     * @param sceneView  Low-res offscreen colour image view (NEAREST sampled).
     * @param depthView  Low-res offscreen depth image view (for depth fog).
     */
    void setSource(VkImageView sceneView, VkImageView depthView);

    /**
     * @brief Set the active retro palette (sRGB swatches).
     *
     * When non-empty, composite.frag remaps every pixel to the nearest palette swatch.
     * Pass an empty vector to disable and fall back to the per-channel quantiser.
     * Swatches beyond kMaxPaletteColors are silently dropped.
     *
     * @param srgbColors  Palette swatches in sRGB space.
     */
    void setPalette(const std::vector<glm::vec3>& srgbColors);

    /**
     * @brief Record the full-screen composite draw.
     * @param cmd     Command buffer (inside the swapchain/UI render pass).
     * @param screen  Swapchain extent (output resolution).
     * @param lowRes  Offscreen extent (drives the dither cell size).
     * @param fog     Per-frame fog and retro-post parameters.
     */
    void record(VkCommandBuffer cmd, VkExtent2D screen, VkExtent2D lowRes, const Fog& fog);

private:
    void createSampler();
    void createDescriptor();
    void createPipeline(VkRenderPass targetRenderPass);

    VulkanContext& ctx_;

    VkSampler             sampler_       = VK_NULL_HANDLE; // NEAREST: crisp pixelation
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_      = VK_NULL_HANDLE;
    VkDescriptorSet       set_       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;

    Buffer                bloomUbo_;            // post-FX params (bloom) the push block can't hold
    void*                 bloomMapped_ = nullptr; // persistent map into bloomUbo_

    std::vector<glm::vec3> palette_;            // bound retro palette (sRGB); empty = off
};

} // namespace vg
