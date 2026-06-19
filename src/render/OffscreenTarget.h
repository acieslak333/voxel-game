#pragma once

/**
 * @file OffscreenTarget.h
 * @brief Low-resolution colour + depth render target for the pixelation effect.
 *
 * The world scene is drawn at a reduced resolution (window / pixelScale) into this
 * target; the CompositeRenderer then nearest-upscales the colour image onto the
 * full-resolution swapchain framebuffer, producing the PS1-style chunky pixel look.
 * The depth image is also sampled by the composite pass for distance fog (issue #10 E).
 * The render pass transitions colour to SHADER_READ_ONLY_OPTIMAL on completion so
 * the composite fragment shader can sample it immediately.
 * @see docs/CODE_INDEX.md
 */

#include <vulkan/vulkan.h>

namespace vg {

class VulkanContext;

// -----------------------------------------------------------------------------
//  OffscreenTarget
// -----------------------------------------------------------------------------
//  A low-resolution colour + depth render target. The world is drawn into this
//  at a fraction of the window resolution, then its colour image is nearest-
//  upscaled (blitted) onto the swapchain — the chunky, PS1-style pixelation.
//
//  The colour image is created with TRANSFER_SRC usage and the render pass leaves
//  it in TRANSFER_SRC_OPTIMAL, so it can be blitted the moment the pass ends.
// -----------------------------------------------------------------------------
/**
 * @brief Low-res colour+depth framebuffer used as the scene render target.
 *
 * Recreated by Renderer whenever pixelScale changes or the swapchain is rebuilt.
 */
class OffscreenTarget {
public:
    OffscreenTarget(VulkanContext& ctx, VkExtent2D extent, VkFormat colorFormat);
    ~OffscreenTarget();

    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    [[nodiscard]] VkRenderPass  renderPass()  const { return renderPass_; }
    [[nodiscard]] VkFramebuffer framebuffer() const { return framebuffer_; }
    [[nodiscard]] VkImage       colorImage()  const { return colorImage_; }
    [[nodiscard]] VkImageView   colorView()   const { return colorView_; }
    [[nodiscard]] VkImageView   depthView()   const { return depthView_; }
    [[nodiscard]] VkExtent2D    extent()      const { return extent_; }

private:
    VulkanContext& ctx_;
    VkExtent2D     extent_;

    VkImage        colorImage_  = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory_ = VK_NULL_HANDLE;
    VkImageView    colorView_   = VK_NULL_HANDLE;

    VkImage        depthImage_  = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkImageView    depthView_   = VK_NULL_HANDLE;

    VkRenderPass   renderPass_  = VK_NULL_HANDLE;
    VkFramebuffer  framebuffer_ = VK_NULL_HANDLE;
};

} // namespace vg
