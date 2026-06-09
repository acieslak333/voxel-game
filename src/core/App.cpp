#include "core/App.h"

// Asset/shader directories are baked in at build time by CMake so the executable
// can find them regardless of the working directory.
#ifndef VG_SHADER_DIR
#define VG_SHADER_DIR "shaders"
#endif
#ifndef VG_ASSET_DIR
#define VG_ASSET_DIR "assets"
#endif

namespace vg {

App::App()
    : window_(kWidth, kHeight, "Voxel Survival Game"),
      context_(window_),
      swapchain_(context_, window_),
      renderer_(context_, swapchain_, window_),
      chunkRenderer_(context_, swapchain_.renderPass(),
                     static_cast<uint32_t>(Renderer::kMaxFramesInFlight),
                     VG_SHADER_DIR, std::string(VG_ASSET_DIR) + "/textures") {}

void App::run(long maxFrames, const std::string& screenshotPath) {
    // Milestone 1: render the greedy-meshed, textured chunk each frame. The
    // record callback runs inside the render pass (see Renderer::drawFrame).
    long frame = 0;
    while (!window_.shouldClose()) {
        window_.pollEvents();

        renderer_.drawFrame([this](VkCommandBuffer cmd, uint32_t frameIndex,
                                   VkExtent2D extent) {
            chunkRenderer_.record(cmd, frameIndex, extent);
        });

        if (maxFrames >= 0 && ++frame >= maxFrames) {
            break;
        }
    }

    // Make sure the GPU is finished before destructors start freeing resources.
    renderer_.waitIdle();

    if (!screenshotPath.empty()) {
        renderer_.saveScreenshot(screenshotPath);
    }
}

} // namespace vg
