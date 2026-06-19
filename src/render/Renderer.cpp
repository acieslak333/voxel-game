/**
 * @file Renderer.cpp
 * @brief Implements Renderer: command pools, sync primitives, UI pass, and the drawFrame loop.
 *
 * recordCommandBuffer() sequences the pre-pass (transfers), scene pass (low-res offscreen,
 * reversed-Z), and UI/composite pass (full-res swapchain). drawFrame() manages the
 * acquire->wait->record->submit->present cycle with two frames in flight.
 */

#include "render/Renderer.h"

#include "core/Window.h"
#include "render/CompositeRenderer.h"
#include "render/OffscreenTarget.h"
#include "render/Screenshot.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace vg {

Renderer::Renderer(VulkanContext& ctx, Swapchain& swapchain, Window& window)
    : ctx_(ctx), swapchain_(swapchain), window_(window) {
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
    createOffscreen();
    createUiRenderPass();
    createUiFramebuffers();
    // The composite pass samples the offscreen and runs inside the UI render pass,
    // so it is built after both exist; setSource() points it at the current target.
    composite_ = std::make_unique<CompositeRenderer>(ctx_, uiRenderPass_);
    composite_->setSource(offscreen_->colorView(), offscreen_->depthView());
}

VkRenderPass Renderer::sceneRenderPass() const {
    return offscreen_->renderPass();
}

void Renderer::createOffscreen() {
    const VkExtent2D full = swapchain_.extent();
    const VkExtent2D low{std::max(1u, full.width / pixelScale_),
                         std::max(1u, full.height / pixelScale_)};
    offscreen_ = std::make_unique<OffscreenTarget>(ctx_, low, swapchain_.imageFormat());
    if (composite_) {
        composite_->setSource(offscreen_->colorView(), offscreen_->depthView()); // re-point
    }
}

void Renderer::setPixelScale(uint32_t scale) {
    scale = std::clamp(scale, 1u, kMaxPixelScale);
    if (scale == pixelScale_) {
        return;
    }
    pixelScale_ = scale;
    // Resizing the offscreen target means freeing GPU images the frames in flight
    // may still reference, so drain the GPU first.
    vkDeviceWaitIdle(ctx_.device());
    createOffscreen();
}

Renderer::~Renderer() {
    destroyUiFramebuffers();
    if (uiRenderPass_) {
        vkDestroyRenderPass(ctx_.device(), uiRenderPass_, nullptr);
    }
    destroySyncObjects();
    if (commandPool_) {
        // Destroying the pool frees all command buffers allocated from it.
        vkDestroyCommandPool(ctx_.device(), commandPool_, nullptr);
    }
}

void Renderer::createUiRenderPass() {
    // Swapchain pass: the composite draw overwrites the whole image (so it does not
    // need to LOAD anything), then the 2D UI blends on top, and it is handed to the
    // presentation engine. No depth: everything here is flat 2D.
    VkAttachmentDescription color{};
    color.format         = swapchain_.imageFormat();
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // composite fills it
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    // Wait for the acquired image to be available before writing colour into it.
    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments    = &color;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = 1;
    info.pDependencies   = &dep;
    if (vkCreateRenderPass(ctx_.device(), &info, nullptr, &uiRenderPass_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create UI render pass");
    }
}

void Renderer::createUiFramebuffers() {
    const VkExtent2D extent = swapchain_.extent();
    uiFramebuffers_.resize(swapchain_.imageCount());
    for (uint32_t i = 0; i < swapchain_.imageCount(); ++i) {
        VkImageView view = swapchain_.imageView(i);
        VkFramebufferCreateInfo info{};
        info.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass      = uiRenderPass_;
        info.attachmentCount = 1;
        info.pAttachments    = &view;
        info.width           = extent.width;
        info.height          = extent.height;
        info.layers          = 1;
        if (vkCreateFramebuffer(ctx_.device(), &info, nullptr, &uiFramebuffers_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create UI framebuffer");
        }
    }
}

void Renderer::destroyUiFramebuffers() {
    for (VkFramebuffer fb : uiFramebuffers_) {
        vkDestroyFramebuffer(ctx_.device(), fb, nullptr);
    }
    uiFramebuffers_.clear();
}

void Renderer::waitIdle() const {
    vkDeviceWaitIdle(ctx_.device());
}

void Renderer::saveScreenshot(const std::string& path) const {
    screenshot::saveImage(ctx_, swapchain_.image(lastImageIndex_),
                          swapchain_.imageFormat(), swapchain_.extent(), path);
}

void Renderer::createCommandPool() {
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // RESET_COMMAND_BUFFER lets us re-record each buffer every frame.
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = ctx_.queueFamilies().graphics.value();

    if (vkCreateCommandPool(ctx_.device(), &info, nullptr, &commandPool_) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }
}

void Renderer::createCommandBuffers() {
    commandBuffers_.resize(kMaxFramesInFlight);

    VkCommandBufferAllocateInfo info{};
    info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool        = commandPool_;
    info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

    if (vkAllocateCommandBuffers(ctx_.device(), &info, commandBuffers_.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate command buffers");
    }
}

void Renderer::createSyncObjects() {
    imageAvailable_.resize(kMaxFramesInFlight);
    inFlight_.resize(kMaxFramesInFlight);
    renderFinished_.resize(swapchain_.imageCount());
    imagesInFlight_.assign(swapchain_.imageCount(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    // Start signalled so the very first vkWaitForFences does not block forever.
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkDevice device = ctx_.device();
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(device, &semInfo, nullptr, &imageAvailable_[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlight_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create per-frame sync objects");
        }
    }
    for (uint32_t i = 0; i < swapchain_.imageCount(); ++i) {
        if (vkCreateSemaphore(device, &semInfo, nullptr, &renderFinished_[i]) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create per-image sync objects");
        }
    }
}

void Renderer::destroySyncObjects() {
    VkDevice device = ctx_.device();
    for (VkSemaphore s : imageAvailable_) {
        vkDestroySemaphore(device, s, nullptr);
    }
    for (VkSemaphore s : renderFinished_) {
        vkDestroySemaphore(device, s, nullptr);
    }
    for (VkFence f : inFlight_) {
        vkDestroyFence(device, f, nullptr);
    }
    imageAvailable_.clear();
    renderFinished_.clear();
    inFlight_.clear();
    imagesInFlight_.clear();
}

void Renderer::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                                   const RecordFn& recordPre, const RecordFn& recordScene,
                                   const RecordFn& recordUi) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    // --- Pre-pass: transfers that must run outside any render pass (e.g. the
    // streaming chunk-mesh uploads), recorded into this frame's command buffer so
    // they're ordered before the draws by a barrier, with no separate submit. ----
    if (recordPre) {
        recordPre(cmd, currentFrame_, offscreen_->extent());
    }

    // --- Pass 1: render the scene into the low-res offscreen target ----------
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = {{clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]}};
    clearValues[1].depthStencil = {0.0f, 0}; // far plane (reversed-Z: far = depth 0)

    const VkExtent2D lowExtent = offscreen_->extent();
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = offscreen_->renderPass();
    rpInfo.framebuffer       = offscreen_->framebuffer();
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = lowExtent;
    rpInfo.clearValueCount   = static_cast<uint32_t>(clearValues.size());
    rpInfo.pClearValues      = clearValues.data();

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // The scene draws at the offscreen (low) resolution; its aspect ratio matches
    // the window because the offscreen is the window scaled down by a constant.
    if (recordScene) {
        recordScene(cmd, currentFrame_, lowExtent);
    }

    vkCmdEndRenderPass(cmd);

    // --- Pass 2: composite (upscale + whole-frame dither) then the UI on top ---
    // The composite fullscreen draw samples the offscreen (NEAREST = pixelation)
    // and posterise-dithers the whole frame, so sky and world share one stipple.
    // The UI then blends over it, leaving the image in PRESENT_SRC.
    const VkExtent2D swapExtent = swapchain_.extent();
    VkRenderPassBeginInfo uiInfo{};
    uiInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    uiInfo.renderPass        = uiRenderPass_;
    uiInfo.framebuffer       = uiFramebuffers_[imageIndex];
    uiInfo.renderArea.offset = {0, 0};
    uiInfo.renderArea.extent = swapExtent;
    uiInfo.clearValueCount   = 0; // loadOp is DONT_CARE; the composite fills it
    vkCmdBeginRenderPass(cmd, &uiInfo, VK_SUBPASS_CONTENTS_INLINE);
    composite_->record(cmd, swapExtent, lowExtent, fog_);
    if (recordUi) {
        recordUi(cmd, currentFrame_, swapExtent);
    }
    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }
}

void Renderer::drawFrame(const RecordFn& recordPre, const RecordFn& recordScene,
                         const RecordFn& recordUi) {
    VkDevice device = ctx_.device();
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto t0 = clock::now();

    // 1. Wait for this frame slot's previous submission to finish.
    vkWaitForFences(device, 1, &inFlight_[currentFrame_], VK_TRUE,
                    std::numeric_limits<uint64_t>::max());
    const auto t1 = clock::now();

    // 2. Acquire the next swapchain image.
    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(
        device, swapchain_.handle(), std::numeric_limits<uint64_t>::max(),
        imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        // The swapchain no longer matches the surface; rebuild and try next frame.
        swapchain_.recreate();
        createOffscreen(); // resize the low-res target to match
        destroyUiFramebuffers();
        createUiFramebuffers();
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Failed to acquire swapchain image");
    }
    lastImageIndex_ = imageIndex; // remember for screenshots

    // 3. If a previous frame is still using this image, wait for it.
    if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
        vkWaitForFences(device, 1, &imagesInFlight_[imageIndex], VK_TRUE,
                        std::numeric_limits<uint64_t>::max());
    }
    imagesInFlight_[imageIndex] = inFlight_[currentFrame_];
    const auto t2 = clock::now();

    // 4. Record this frame's command buffer.
    VkCommandBuffer cmd = commandBuffers_[currentFrame_];
    vkResetCommandBuffer(cmd, 0);
    recordCommandBuffer(cmd, imageIndex, recordPre, recordScene, recordUi);
    const auto t3 = clock::now();

    // 5. Submit: wait on image-available, signal render-finished + the fence.
    //    The swapchain image is first written by the composite's colour output, so
    //    the acquire semaphore gates the colour-attachment stage.
    VkSemaphore          waitSems[]   = { imageAvailable_[currentFrame_] };
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore          signalSems[] = { renderFinished_[imageIndex] };

    VkSubmitInfo submit{};
    submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount   = 1;
    submit.pWaitSemaphores      = waitSems;
    submit.pWaitDstStageMask    = waitStages;
    submit.commandBufferCount   = 1;
    submit.pCommandBuffers      = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores    = signalSems;

    vkResetFences(device, 1, &inFlight_[currentFrame_]);
    if (vkQueueSubmit(ctx_.graphicsQueue(), 1, &submit, inFlight_[currentFrame_]) != VK_SUCCESS) {
        throw std::runtime_error("Failed to submit draw command buffer");
    }

    // 6. Present the rendered image.
    VkSwapchainKHR swapchains[] = { swapchain_.handle() };
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = signalSems;
    present.swapchainCount     = 1;
    present.pSwapchains        = swapchains;
    present.pImageIndices      = &imageIndex;

    VkResult result = vkQueuePresentKHR(ctx_.presentQueue(), &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        window_.framebufferResized()) {
        window_.clearFramebufferResized();
        swapchain_.recreate();
        createOffscreen(); // resize the low-res target to match
        destroyUiFramebuffers();
        createUiFramebuffers();
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    phaseTimes_.wait    = ms(t0, t1);
    phaseTimes_.acquire = ms(t1, t2);
    phaseTimes_.record  = ms(t2, t3);
    phaseTimes_.submit  = ms(t3, clock::now());

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

} // namespace vg
