#include "render/Swapchain.h"

#include "core/Window.h"
#include "render/VulkanContext.h"
#include "render/VulkanUtils.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace vg {

Swapchain::Swapchain(VulkanContext& ctx, const Window& window)
    : ctx_(ctx), window_(window) {
    create();
    createRenderPass();
    createFramebuffers();
}

Swapchain::~Swapchain() {
    cleanupSizeDependent();
    if (renderPass_) {
        vkDestroyRenderPass(ctx_.device(), renderPass_, nullptr);
    }
}

void Swapchain::recreate() {
    // If the window is minimised (zero-size framebuffer) wait until it is shown
    // again; you cannot create a zero-extent swapchain.
    window_.waitWhileMinimized();
    vkDeviceWaitIdle(ctx_.device());

    cleanupSizeDependent();
    create();
    createFramebuffers();
}

void Swapchain::create() {
    SwapChainSupportDetails support = ctx_.querySwapChainSupport();

    VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
    VkPresentModeKHR   presentMode   = choosePresentMode(support.presentModes);
    VkExtent2D         extent        = chooseExtent(support.capabilities);

    // Request one more image than the minimum so we are less likely to stall
    // waiting on the driver, clamped to the maximum (0 means "no maximum").
    uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 &&
        imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface          = ctx_.surface();
    createInfo.minImageCount    = imageCount;
    createInfo.imageFormat      = surfaceFormat.format;
    createInfo.imageColorSpace  = surfaceFormat.colorSpace;
    createInfo.imageExtent      = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // Also allow copying out of the images (used by the screenshot path), but
    // only if the surface supports it.
    if (support.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    // If the graphics and present queues differ, the swapchain images must be
    // shared between them (CONCURRENT). Otherwise EXCLUSIVE is faster.
    const QueueFamilyIndices& qf = ctx_.queueFamilies();
    uint32_t indices[] = { qf.graphics.value(), qf.present.value() };
    if (qf.graphics != qf.present) {
        createInfo.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices   = indices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform   = support.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode    = presentMode;
    createInfo.clipped        = VK_TRUE; // allow the driver to skip obscured pixels
    createInfo.oldSwapchain   = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(ctx_.device(), &createInfo, nullptr, &swapchain_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create swapchain");
    }

    // Retrieve the implicitly-created images.
    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &actualCount, nullptr);
    images_.resize(actualCount);
    vkGetSwapchainImagesKHR(ctx_.device(), swapchain_, &actualCount, images_.data());

    imageFormat_ = surfaceFormat.format;
    extent_      = extent;

    createImageViews();
    createDepthResources();
}

void Swapchain::createDepthResources() {
    depthFormat_ = vkutil::findDepthFormat(ctx_.physicalDevice());

    vkutil::createImage(ctx_, extent_.width, extent_.height, /*arrayLayers*/ 1,
                        depthFormat_, VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage_, depthMemory_);

    depthView_ = vkutil::createImageView(ctx_.device(), depthImage_, depthFormat_,
                                         VK_IMAGE_ASPECT_DEPTH_BIT,
                                         VK_IMAGE_VIEW_TYPE_2D, /*layerCount*/ 1);
}

void Swapchain::createImageViews() {
    imageViews_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image    = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format   = imageFormat_;
        viewInfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel   = 0;
        viewInfo.subresourceRange.levelCount     = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(ctx_.device(), &viewInfo, nullptr, &imageViews_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create image view");
        }
    }
}

void Swapchain::createRenderPass() {
    // A single colour attachment that we clear at the start of the pass and
    // store for presentation. Depth is added in a later milestone.
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format         = imageFormat_;
    colorAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // Depth attachment: cleared each frame, not stored (we never read it back).
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format         = depthFormat_;
    depthAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Wait for the swapchain image to be available before writing colour, and
    // synchronise depth so the clear/test does not race a previous frame.
    VkSubpassDependency dependency{};
    dependency.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass    = 0;
    dependency.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments    = attachments.data();
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dependency;

    if (vkCreateRenderPass(ctx_.device(), &info, nullptr, &renderPass_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create render pass");
    }
}

void Swapchain::createFramebuffers() {
    framebuffers_.resize(imageViews_.size());
    for (size_t i = 0; i < imageViews_.size(); ++i) {
        // The depth view is shared across all framebuffers (attachment index 1).
        VkImageView attachments[] = { imageViews_[i], depthView_ };

        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = renderPass_;
        info.attachmentCount = 2;
        info.pAttachments    = attachments;
        info.width           = extent_.width;
        info.height          = extent_.height;
        info.layers          = 1;

        if (vkCreateFramebuffer(ctx_.device(), &info, nullptr, &framebuffers_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create framebuffer");
        }
    }
}

void Swapchain::cleanupSizeDependent() {
    VkDevice device = ctx_.device();

    if (depthView_)  { vkDestroyImageView(device, depthView_, nullptr); depthView_ = VK_NULL_HANDLE; }
    if (depthImage_) { vkDestroyImage(device, depthImage_, nullptr);    depthImage_ = VK_NULL_HANDLE; }
    if (depthMemory_){ vkFreeMemory(device, depthMemory_, nullptr);     depthMemory_ = VK_NULL_HANDLE; }

    for (VkFramebuffer fb : framebuffers_) {
        vkDestroyFramebuffer(device, fb, nullptr);
    }
    framebuffers_.clear();

    for (VkImageView view : imageViews_) {
        vkDestroyImageView(device, view, nullptr);
    }
    imageViews_.clear();

    if (swapchain_) {
        vkDestroySwapchainKHR(device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

VkSurfaceFormatKHR Swapchain::chooseSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& available) const {
    // Prefer 8-bit BGRA in sRGB colour space (correct gamma for free).
    for (const auto& format : available) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return available.front(); // otherwise just take whatever is first
}

VkPresentModeKHR Swapchain::choosePresentMode(
    const std::vector<VkPresentModeKHR>& available) const {
    // MAILBOX = triple buffering (low latency, no tearing). Fall back to FIFO,
    // which is always supported and is effectively v-sync.
    for (VkPresentModeKHR mode : available) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D Swapchain::chooseExtent(const VkSurfaceCapabilitiesKHR& caps) const {
    // If the surface dictates a fixed extent, use it. Otherwise pick the
    // current framebuffer size, clamped to the allowed range.
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return caps.currentExtent;
    }

    int width = 0, height = 0;
    window_.framebufferSize(width, height);

    VkExtent2D actual{ static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    actual.width  = std::clamp(actual.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    actual.height = std::clamp(actual.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return actual;
}

} // namespace vg
