#include "core/App.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

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
      world_(kWorldSeed, kWorldChunksX, kWorldChunksY, kWorldChunksZ),
      worldRenderer_(context_, swapchain_.renderPass(),
                     static_cast<uint32_t>(Renderer::kMaxFramesInFlight), world_,
                     VG_SHADER_DIR, std::string(VG_ASSET_DIR) + "/textures"),
      input_(window_),
      player_(glm::vec3(0.0f)) {
    // Spawn standing on the surface at the centre of the world.
    const int cx = world_.sizeInBlocks().x / 2;
    const int cz = world_.sizeInBlocks().z / 2;
    player_.teleport(glm::vec3(static_cast<float>(cx),
                               static_cast<float>(world_.surfaceHeight(cx, cz)) + 2.0f,
                               static_cast<float>(cz)));

    // Collide against the generated world.
    player_.setSolidFn([this](int x, int y, int z) { return world_.isSolid(x, y, z); });

    window_.setCursorDisabled(true);
}

void App::enableFlyOverview() {
    const glm::ivec3 size = world_.sizeInBlocks();
    player_.setMode(PlayerController::Mode::FreeFly);
    player_.teleport(glm::vec3(size.x * 0.5f - 30.0f,
                               static_cast<float>(size.y) + 18.0f,
                               size.z * 0.5f - 30.0f));
    player_.camera().yaw   = 45.0f;   // toward the centre of the map
    player_.camera().pitch = -34.0f;  // looking down
}

void App::run(long maxFrames, const std::string& screenshotPath) {
    double lastTime = glfwGetTime();
    long frame = 0;

    while (!window_.shouldClose()) {
        window_.pollEvents();

        // Delta time, clamped so a hitch (or a slow first frame) cannot launch
        // the player through the world.
        const double now = glfwGetTime();
        const float dt = static_cast<float>(std::min(now - lastTime, 0.05));
        lastTime = now;

        const InputState in = input_.poll();
        player_.update(dt, in);

        renderer_.drawFrame([this](VkCommandBuffer cmd, uint32_t frameIndex,
                                   VkExtent2D extent) {
            const Camera& cam = player_.camera();
            const float aspect = extent.height == 0
                                     ? 1.0f
                                     : static_cast<float>(extent.width) /
                                           static_cast<float>(extent.height);
            glm::mat4 view = cam.viewMatrix();
            glm::mat4 proj = glm::perspective(glm::radians(cam.fovDegrees), aspect,
                                              cam.nearZ, cam.farZ);
            proj[1][1] *= -1.0f; // flip Y for Vulkan's clip space
            worldRenderer_.record(cmd, frameIndex, extent, view, proj);
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
