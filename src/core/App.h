#pragma once

#include "core/Input.h"
#include "core/Window.h"
#include "player/PlayerController.h"
#include "render/ChunkRenderer.h"
#include "render/Renderer.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"

#include <string>

namespace vg {

// -----------------------------------------------------------------------------
//  App
// -----------------------------------------------------------------------------
//  Top-level object that owns the window, the Vulkan stack, the world renderer
//  and the player, and runs the main loop. Member declaration order matters:
//  each subsystem depends on the ones declared above it, and members are
//  constructed top-to-bottom / destroyed bottom-to-top, which is exactly the
//  order Vulkan requires.
// -----------------------------------------------------------------------------
class App {
public:
    App();
    void run(long maxFrames = -1, const std::string& screenshotPath = "");

private:
    static constexpr int kWidth  = 1280;
    static constexpr int kHeight = 720;

    Window           window_;
    VulkanContext    context_;
    Swapchain        swapchain_;
    Renderer         renderer_;
    ChunkRenderer    chunkRenderer_;
    Input            input_;
    PlayerController player_;
};

} // namespace vg
