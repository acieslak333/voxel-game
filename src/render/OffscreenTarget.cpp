#include "render/OffscreenTarget.h"

#include "render/VulkanContext.h"
#include "render/VulkanUtils.h"

#include <array>
#include <stdexcept>

namespace vg {

OffscreenTarget::OffscreenTarget(VulkanContext& ctx, VkExtent2D extent, VkFormat colorFormat)
    : ctx_(ctx), extent_(extent) {
    const VkFormat depthFormat = vkutil::findDepthFormat(ctx_.physicalDevice());

    // --- Colour image (rendered into, then sampled by the composite pass) -----
    vkutil::createImage(ctx_, extent_.width, extent_.height, /*layers*/ 1, colorFormat,
                        VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, colorImage_, colorMemory_);
    colorView_ = vkutil::createImageView(ctx_.device(), colorImage_, colorFormat,
                                         VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D, 1);

    // --- Depth image (also sampled by the composite pass for fog, issue #10 E) -
    vkutil::createImage(ctx_, extent_.width, extent_.height, /*layers*/ 1, depthFormat,
                        VK_IMAGE_TILING_OPTIMAL,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage_, depthMemory_);
    depthView_ = vkutil::createImageView(ctx_.device(), depthImage_, depthFormat,
                                         VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D, 1);

    // --- Render pass: clear, draw, leave colour ready to be sampled -----------
    VkAttachmentDescription color{};
    color.format         = colorFormat;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentDescription depth{};
    depth.format         = depthFormat;
    depth.samples        = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp        = VK_ATTACHMENT_STORE_OP_STORE; // keep depth for fog sampling
    depth.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 1;
    subpass.pColorAttachments       = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    std::array<VkSubpassDependency, 2> deps{};
    // Acquire the attachments before we write them.
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    // Make the finished colour (now in SHADER_READ_ONLY layout) visible to the
    // composite pass that samples it in its fragment shader.
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    std::array<VkAttachmentDescription, 2> attachments = {color, depth};
    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpInfo.pAttachments    = attachments.data();
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();
    if (vkCreateRenderPass(ctx_.device(), &rpInfo, nullptr, &renderPass_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen render pass");
    }

    std::array<VkImageView, 2> views = {colorView_, depthView_};
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = renderPass_;
    fbInfo.attachmentCount = static_cast<uint32_t>(views.size());
    fbInfo.pAttachments    = views.data();
    fbInfo.width           = extent_.width;
    fbInfo.height          = extent_.height;
    fbInfo.layers          = 1;
    if (vkCreateFramebuffer(ctx_.device(), &fbInfo, nullptr, &framebuffer_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create offscreen framebuffer");
    }
}

OffscreenTarget::~OffscreenTarget() {
    VkDevice device = ctx_.device();
    if (framebuffer_) vkDestroyFramebuffer(device, framebuffer_, nullptr);
    if (renderPass_)  vkDestroyRenderPass(device, renderPass_, nullptr);
    if (depthView_)   vkDestroyImageView(device, depthView_, nullptr);
    if (depthImage_)  vkDestroyImage(device, depthImage_, nullptr);
    if (depthMemory_) vkFreeMemory(device, depthMemory_, nullptr);
    if (colorView_)   vkDestroyImageView(device, colorView_, nullptr);
    if (colorImage_)  vkDestroyImage(device, colorImage_, nullptr);
    if (colorMemory_) vkFreeMemory(device, colorMemory_, nullptr);
}

} // namespace vg
