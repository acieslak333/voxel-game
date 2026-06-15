#pragma once

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  CompositeRenderer
// -----------------------------------------------------------------------------
//  The full-screen "post" pass that replaces the old upscale blit. It samples the
//  low-res offscreen scene (NEAREST -> the chunky pixelation) and applies one
//  ordered-dither posterisation over the whole frame, so the sky and the world
//  share the same dithered look. Drawn first in the swapchain (UI) render pass;
//  the UI then blends on top.
//
//  Owns its pipeline + a single descriptor pointing at the offscreen colour image,
//  which is re-pointed via setSource() whenever the offscreen is rebuilt (resize /
//  pixel-scale change).
// -----------------------------------------------------------------------------
class CompositeRenderer {
public:
    CompositeRenderer(VulkanContext& ctx, VkRenderPass targetRenderPass);
    ~CompositeRenderer();

    CompositeRenderer(const CompositeRenderer&) = delete;
    CompositeRenderer& operator=(const CompositeRenderer&) = delete;

    // Per-frame fog inputs (issue #10 E), consumed by composite.frag (push const).
    struct Fog {
        glm::mat4 invViewProj;   // NDC -> world (inverse of the scene's proj*view)
        glm::vec4 camPos;        // xyz camera world position
        glm::vec4 color;         // rgb haze colour, w = distance-fog density
        glm::vec4 params;        // x heightFalloff, y groundFogDensity, z fogTopY, w maxFog
        float     noiseAmount = 0.0f; // dark-grain max opacity (0 = off)
        float     noiseTime   = 0.0f; // seconds, reseeds the grain so it flickers
        float     submerged = 0.0f; // 0 = above water, 1 = camera underwater (blue murk)
        // Bloom: a blurred glow added from the scene's bright areas. Written to the
        // post UBO each frame (see CompositeRenderer.cpp). intensity 0 = off.
        float     bloomIntensity = 0.0f;  // glow strength added back to the frame
        float     bloomThreshold = 0.75f; // brightness above which a pixel blooms
        float     bloomRadius    = 3.0f;  // blur radius in low-res (offscreen) texels
        // God rays: screen-space crepuscular shafts marched toward the sun. Also in
        // the post UBO. intensity 0 = off; sun position/visibility come from the CPU.
        float     godrayIntensity = 0.0f; // final shaft strength (0 = off)
        float     godrayDensity   = 0.9f; // how far the shafts reach (sample spacing)
        float     godrayDecay     = 0.96f;// per-sample falloff along a shaft
        glm::vec2 sunScreen{0.5f, 0.5f};  // sun position in screen UV (0..1)
        float     sunInFront    = 0.0f;   // 1 = sun is in front of the camera, else 0
        float     sunVisibility = 0.0f;   // 0 at night/below horizon .. 1 bright noon
        // Retro (PS1/PS2) post. All 0 = off (modern, byte-identical output).
        float     retroLevels    = 0.0f;  // colour levels/channel (0 = off, 32 = 5-bit)
        float     retroDither    = 0.0f;  // ordered-dither amount (0..1)
        float     retroInterlace = 0.0f;  // scanline dim amount (0..1, PS2)
        float     retroSoft      = 0.0f;  // PS2 soft-blur blend (0..1)
        float     retroParity    = 0.0f;  // 0/1 frame parity, flips the interlace field
        float     retroBilinear  = 0.0f;  // 1 = PS2 smooth base, 0 = PS1 nearest base
        float     retroSoftRadius= 1.0f;  // soft-blur radius in offscreen texels
    };
    // Point the pass at the offscreen colour + depth images to read this frame.
    // Call once up front and again after the offscreen is recreated.
    void setSource(VkImageView sceneView, VkImageView depthView);

    // Bind a retro colour palette (sRGB swatches): the whole frame is remapped to
    // the nearest entry in composite.frag. Pass an empty list to disable (fall back
    // to the per-channel "Colour bits" quantiser). Stored and re-uploaded into the
    // post UBO each frame; only changes when the player picks a palette. Extra
    // swatches beyond kMaxPaletteColors are dropped.
    void setPalette(const std::vector<glm::vec3>& srgbColors);

    // Draw the full-screen composite into the bound render pass. `screen` is the
    // swapchain extent; `lowRes` is the offscreen extent (drives the dither cell).
    void record(VkCommandBuffer cmd, VkExtent2D screen, VkExtent2D lowRes, const Fog& fog);

private:
    void createSampler();
    void createDescriptor();
    void createPipeline(VkRenderPass targetRenderPass);

    VulkanContext& ctx_;

    VkSampler             sampler_       = VK_NULL_HANDLE; // NEAREST: crisp pixelation
    VkSampler             linearSampler_ = VK_NULL_HANDLE; // LINEAR: smooth bloom taps
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
