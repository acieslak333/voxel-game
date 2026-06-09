#include "render/Renderer.h"

#include "core/Window.h"
#include "render/Screenshot.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"

#include <array>
#include <limits>
#include <stdexcept>

namespace vg {

Renderer::Renderer(VulkanContext& ctx, Swapchain& swapchain, Window& window)
    : ctx_(ctx), swapchain_(swapchain), window_(window) {
    createCommandPool();
    createCommandBuffers();
    createSyncObjects();
}

Renderer::~Renderer() {
    destroySyncObjects();
    if (commandPool_) {
        // Destroying the pool frees all command buffers allocated from it.
        vkDestroyCommandPool(ctx_.device(), commandPool_, nullptr);
    }
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
                                   const RecordFn& recordScene) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
        throw std::runtime_error("Failed to begin recording command buffer");
    }

    // Two attachments to clear: colour (index 0) and depth (index 1).
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = {{clearColor_[0], clearColor_[1], clearColor_[2], clearColor_[3]}};
    clearValues[1].depthStencil = {1.0f, 0}; // far plane

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass        = swapchain_.renderPass();
    rpInfo.framebuffer       = swapchain_.framebuffer(imageIndex);
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = swapchain_.extent();
    rpInfo.clearValueCount   = static_cast<uint32_t>(clearValues.size());
    rpInfo.pClearValues      = clearValues.data();

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Hand control to the scene to draw whatever geometry it has. In Milestone 0
    // this is empty, so the frame is just the cleared background. The scene gets
    // the frame-in-flight index for selecting per-frame resources.
    if (recordScene) {
        recordScene(cmd, currentFrame_, swapchain_.extent());
    }

    vkCmdEndRenderPass(cmd);

    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        throw std::runtime_error("Failed to record command buffer");
    }
}

void Renderer::drawFrame(const RecordFn& recordScene) {
    VkDevice device = ctx_.device();

    // 1. Wait for this frame slot's previous submission to finish.
    vkWaitForFences(device, 1, &inFlight_[currentFrame_], VK_TRUE,
                    std::numeric_limits<uint64_t>::max());

    // 2. Acquire the next swapchain image.
    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(
        device, swapchain_.handle(), std::numeric_limits<uint64_t>::max(),
        imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);

    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        // The swapchain no longer matches the surface; rebuild and try next frame.
        swapchain_.recreate();
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

    // 4. Record this frame's command buffer.
    VkCommandBuffer cmd = commandBuffers_[currentFrame_];
    vkResetCommandBuffer(cmd, 0);
    recordCommandBuffer(cmd, imageIndex, recordScene);

    // 5. Submit: wait on image-available, signal render-finished + the fence.
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
    } else if (result != VK_SUCCESS) {
        throw std::runtime_error("Failed to present swapchain image");
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
}

} // namespace vg
