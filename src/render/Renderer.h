#pragma once

#include "render/CompositeRenderer.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vg {

class VulkanContext;
class Swapchain;
class Window;
class OffscreenTarget;

// -----------------------------------------------------------------------------
//  Renderer
// -----------------------------------------------------------------------------
//  Owns the per-frame machinery: a command pool, command buffers, and the
//  synchronisation primitives needed to keep several frames "in flight" without
//  the CPU and GPU stepping on each other.
//
//  drawFrame() handles the whole acquire -> record -> submit -> present dance,
//  including rebuilding the swapchain when the window resizes. The actual scene
//  geometry is recorded by a caller-supplied callback that runs *inside* the
//  render pass, so this class stays agnostic of what is being drawn.
// -----------------------------------------------------------------------------
class Renderer {
public:
    // Records draw commands inside an already-begun render pass. Receives the
    // command buffer, the current frame-in-flight index (use it to pick
    // per-frame resources such as uniform buffers / descriptor sets), and the
    // framebuffer extent (handy for setting viewport/scissor).
    using RecordFn = std::function<void(VkCommandBuffer cmd,
                                        uint32_t frameIndex,
                                        VkExtent2D extent)>;

    Renderer(VulkanContext& ctx, Swapchain& swapchain, Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Render a single frame. `recordPre` records into the frame command buffer
    // *before any render pass begins* — the place for transfer/copy commands
    // (illegal inside a render pass), e.g. streaming chunk-mesh uploads, so they
    // ride the frame's own submit with no separate GPU sync. `recordScene` draws
    // the world into the low-res offscreen target (then nearest-upscaled to the
    // swapchain); `recordUi` draws the 2D overlay crisp on top, in the swapchain
    // render pass. Any may be empty.
    void drawFrame(const RecordFn& recordPre, const RecordFn& recordScene,
                   const RecordFn& recordUi);

    // Block until the GPU has finished all outstanding work. Call before tearing
    // things down or before destroying resources the GPU may still be using.
    void waitIdle() const;

    // The render pass the scene is drawn into (the low-res offscreen target).
    // Build scene pipelines against this so they are render-pass compatible.
    [[nodiscard]] VkRenderPass sceneRenderPass() const;

    // Pixelation strength: each low-res pixel covers `scale` window pixels
    // (1 = off/native, up to kMaxPixelScale). Rebuilds the offscreen target.
    void setPixelScale(uint32_t scale);
    [[nodiscard]] uint32_t pixelScale() const { return pixelScale_; }

    static constexpr uint32_t kMaxPixelScale = 16;

    // The colour-only render pass UI is drawn into (loads the upscaled frame).
    // Build UI pipelines against this.
    [[nodiscard]] VkRenderPass uiRenderPass() const { return uiRenderPass_; }

    void setClearColor(float r, float g, float b) { clearColor_ = {r, g, b, 1.0f}; }

    // Per-frame fog inputs for the composite pass (issue #10 E). Call before
    // drawFrame() (or inside recordScene, which runs first); the composite reads
    // the most recent value when it draws.
    void setFog(const CompositeRenderer::Fog& fog) { fog_ = fog; }

    // Write the most-recently-rendered frame to a PNG. Call after the render
    // loop (and after waitIdle). Used by the --screenshot flag for verification.
    void saveScreenshot(const std::string& path) const;

    static constexpr int kMaxFramesInFlight = 2;

private:
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void destroySyncObjects();
    // (Re)build the low-res offscreen target from the current swapchain size.
    void createOffscreen();
    // The colour-only swapchain render pass + framebuffers used for the UI overlay.
    void createUiRenderPass();
    void createUiFramebuffers();
    void destroyUiFramebuffers();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                             const RecordFn& recordPre, const RecordFn& recordScene,
                             const RecordFn& recordUi);

    // Each low-res pixel covers this many window pixels (runtime, via Options).
    uint32_t pixelScale_ = 4;

    VulkanContext& ctx_;
    Swapchain&     swapchain_;
    Window&        window_;

    std::unique_ptr<OffscreenTarget> offscreen_; // low-res render target

    // Composite/post pass: upscales the offscreen onto the swapchain and applies
    // the whole-frame ordered dither (replaces the old blit). Runs first in the
    // swapchain render pass; the UI then blends over it.
    std::unique_ptr<CompositeRenderer> composite_;

    // Swapchain render pass: the composite draws the dithered frame, then the UI
    // overlay blends 2D on top, leaving the image ready to present.
    VkRenderPass               uiRenderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> uiFramebuffers_;

    VkCommandPool                commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_; // one per frame in flight

    // Per frame-in-flight: signalled when an image is acquired / a frame done.
    std::vector<VkSemaphore> imageAvailable_;
    std::vector<VkFence>     inFlight_;
    // Per swapchain image: signalled when rendering to that image completes.
    // Tying this to the image (not the frame) avoids reusing a semaphore that
    // a still-pending present is waiting on.
    std::vector<VkSemaphore> renderFinished_;
    // Per swapchain image: the in-flight fence currently using it, so we never
    // render into an image a previous frame has not finished with.
    std::vector<VkFence>     imagesInFlight_;

    uint32_t currentFrame_   = 0;
    uint32_t lastImageIndex_ = 0; // swapchain image rendered most recently

    std::array<float, 4> clearColor_ = {0.45f, 0.70f, 1.0f, 1.0f}; // sky blue

    CompositeRenderer::Fog fog_{}; // latest fog inputs for the composite pass
};

} // namespace vg
