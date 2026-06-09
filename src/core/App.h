#pragma once

#include "core/Window.h"
#include "render/Renderer.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"

namespace vg {

// -----------------------------------------------------------------------------
//  App
// -----------------------------------------------------------------------------
//  Top-level object that owns the window and the Vulkan stack and runs the main
//  loop. Member declaration order matters: each subsystem depends on the ones
//  declared above it, and members are constructed top-to-bottom / destroyed
//  bottom-to-top, which is exactly the order Vulkan requires.
// -----------------------------------------------------------------------------
class App {
public:
    App();

    // Run the main loop. If maxFrames >= 0 the loop exits after that many
    // frames; this is used for headless smoke-testing / CI where there is no
    // way to interactively close the window. A negative value runs until the
    // window is closed.
    void run(long maxFrames = -1);

private:
    static constexpr int kWidth  = 1280;
    static constexpr int kHeight = 720;

    Window        window_;
    VulkanContext context_;
    Swapchain     swapchain_;
    Renderer      renderer_;
};

} // namespace vg
