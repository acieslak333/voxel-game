#pragma once

#include "render/Buffer.h"

#include <vulkan/vulkan.h>

#include <glm/glm.hpp>

#include <memory>

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
    };
    // Point the pass at the offscreen colour + depth images to read this frame.
    // Call once up front and again after the offscreen is recreated.
    void setSource(VkImageView sceneView, VkImageView depthView);

    // Draw the full-screen composite into the bound render pass. `screen` is the
    // swapchain extent; `lowRes` is the offscreen extent (drives the dither cell).
    void record(VkCommandBuffer cmd, VkExtent2D screen, VkExtent2D lowRes, const Fog& fog);

    // Quantisation steps per channel (higher = smoother). Tunable look knob.
    void setLevels(float levels) { levels_ = levels; }

private:
    void createSampler();
    void createDescriptor();
    void createPipeline(VkRenderPass targetRenderPass);

    VulkanContext& ctx_;
    float          levels_ = 8.0f;

    VkSampler             sampler_   = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool      pool_      = VK_NULL_HANDLE;
    VkDescriptorSet       set_       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;
};

} // namespace vg
