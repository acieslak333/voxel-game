#include "core/App.h"

namespace vg {

App::App()
    : window_(kWidth, kHeight, "Voxel Survival Game"),
      context_(window_),
      swapchain_(context_, window_),
      renderer_(context_, swapchain_, window_) {}

void App::run(long maxFrames) {
    // Milestone 0: the render loop simply clears the screen each frame. Later
    // milestones pass a "record scene" callback to draw the world.
    long frame = 0;
    while (!window_.shouldClose()) {
        window_.pollEvents();
        renderer_.drawFrame({});

        if (maxFrames >= 0 && ++frame >= maxFrames) {
            break;
        }
    }

    // Make sure the GPU is finished before destructors start freeing resources.
    renderer_.waitIdle();
}

} // namespace vg
