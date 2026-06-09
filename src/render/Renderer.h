#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace vg {

class VulkanContext;
class Swapchain;
class Window;

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
    // command buffer, the index of the swapchain image being rendered, and the
    // current framebuffer extent (handy for setting viewport/scissor).
    using RecordFn = std::function<void(VkCommandBuffer cmd,
                                        uint32_t imageIndex,
                                        VkExtent2D extent)>;

    Renderer(VulkanContext& ctx, Swapchain& swapchain, Window& window);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Render a single frame. `recordScene` may be empty, in which case the frame
    // is just the cleared background (Milestone 0 behaviour).
    void drawFrame(const RecordFn& recordScene);

    // Block until the GPU has finished all outstanding work. Call before tearing
    // things down or before destroying resources the GPU may still be using.
    void waitIdle() const;

    void setClearColor(float r, float g, float b) { clearColor_ = {r, g, b, 1.0f}; }

    static constexpr int kMaxFramesInFlight = 2;

private:
    void createCommandPool();
    void createCommandBuffers();
    void createSyncObjects();
    void destroySyncObjects();

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex,
                             const RecordFn& recordScene);

    VulkanContext& ctx_;
    Swapchain&     swapchain_;
    Window&        window_;

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

    uint32_t currentFrame_ = 0;

    std::array<float, 4> clearColor_ = {0.45f, 0.70f, 1.0f, 1.0f}; // sky blue
};

} // namespace vg
