#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace vg {

class VulkanContext;
class Window;

// -----------------------------------------------------------------------------
//  Swapchain
// -----------------------------------------------------------------------------
//  Wraps the swapchain and everything tied to its images: image views, the
//  render pass, and one framebuffer per image. All of this has to be rebuilt
//  whenever the window is resized, so it lives apart from VulkanContext.
//
//  The render pass itself is created once and kept alive across resizes (its
//  format does not change), so graphics pipelines built against it in later
//  milestones stay valid. Only the size-dependent objects are recreated.
// -----------------------------------------------------------------------------
class Swapchain {
public:
    Swapchain(VulkanContext& ctx, const Window& window);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Rebuild the size-dependent objects after a resize. Blocks until the GPU
    // is idle first.
    void recreate();

    [[nodiscard]] VkSwapchainKHR handle()       const { return swapchain_; }
    [[nodiscard]] VkRenderPass   renderPass()   const { return renderPass_; }
    [[nodiscard]] VkExtent2D     extent()        const { return extent_; }
    [[nodiscard]] VkFormat       imageFormat()   const { return imageFormat_; }
    [[nodiscard]] uint32_t       imageCount()    const {
        return static_cast<uint32_t>(images_.size());
    }
    [[nodiscard]] VkFramebuffer  framebuffer(uint32_t i) const { return framebuffers_[i]; }

private:
    void create();              // swapchain + image views + framebuffers
    void createRenderPass();    // once, in the constructor
    void cleanupSizeDependent(); // framebuffers + image views + swapchain

    void createImageViews();
    void createFramebuffers();

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>&) const;
    VkPresentModeKHR   choosePresentMode(const std::vector<VkPresentModeKHR>&) const;
    VkExtent2D         chooseExtent(const VkSurfaceCapabilitiesKHR&) const;

    VulkanContext& ctx_;
    const Window&  window_;

    VkSwapchainKHR             swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage>       images_;        // owned by the swapchain
    std::vector<VkImageView>   imageViews_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass               renderPass_ = VK_NULL_HANDLE;

    VkFormat   imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
};

} // namespace vg
