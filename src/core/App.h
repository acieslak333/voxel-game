#pragma once

#include "core/Input.h"
#include "core/Window.h"
#include "player/PlayerController.h"
#include "render/Renderer.h"
#include "render/Swapchain.h"
#include "render/VulkanContext.h"
#include "render/WorldRenderer.h"
#include "world/World.h"

#include <string>

namespace vg {

// -----------------------------------------------------------------------------
//  App
// -----------------------------------------------------------------------------
//  Top-level object that owns the window, the Vulkan stack, the world (data),
//  the world renderer and the player, and runs the main loop. Member
//  declaration order matters: each subsystem depends on the ones declared above
//  it, and members are constructed top-to-bottom / destroyed bottom-to-top,
//  which is exactly the order Vulkan requires.
// -----------------------------------------------------------------------------
class App {
public:
    App();

    void run(long maxFrames = -1, const std::string& screenshotPath = "");

    // Debug: start in free-fly looking down over the whole world. Handy for a
    // bird's-eye verification screenshot of the procedural terrain.
    void enableFlyOverview();

private:
    static constexpr int kWidth  = 1280;
    static constexpr int kHeight = 720;

    // Fixed world size (chunks) for Milestone 3 — no streaming yet.
    static constexpr int      kWorldChunksX = 8;
    static constexpr int      kWorldChunksY = 3;
    static constexpr int      kWorldChunksZ = 8;
    static constexpr uint32_t kWorldSeed    = 1337u;

    Window           window_;
    VulkanContext    context_;
    Swapchain        swapchain_;
    Renderer         renderer_;
    World            world_;
    WorldRenderer    worldRenderer_;
    Input            input_;
    PlayerController  player_;
};

} // namespace vg
