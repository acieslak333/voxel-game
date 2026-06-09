#pragma once

// GLFW_INCLUDE_VULKAN is defined by the build system; it makes <GLFW/glfw3.h>
// pull in the Vulkan headers and expose glfwCreateWindowSurface etc.
#include <GLFW/glfw3.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  Window
// -----------------------------------------------------------------------------
//  Thin RAII wrapper around a GLFW window configured for Vulkan (no OpenGL
//  context). Owns GLFW initialisation/termination for the lifetime of the
//  single window we create. If we ever need multiple windows this would move
//  to a separate "platform" object, but one window is plenty for the game.
// -----------------------------------------------------------------------------
class Window {
public:
    Window(int width, int height, std::string title);
    ~Window();

    // Non-copyable, non-movable: it owns OS resources with a stable address
    // (we hand `this` to GLFW as the user pointer).
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool shouldClose() const;
    void pollEvents() const;

    // Create a Vulkan surface for this window. Throws on failure.
    [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;

    // Instance extensions GLFW needs to present to this window's surface.
    [[nodiscard]] static std::vector<const char*> requiredInstanceExtensions();

    // Current framebuffer size in pixels (may differ from window size on HiDPI).
    void framebufferSize(int& outWidth, int& outHeight) const;

    // Blocks until the window has a non-zero framebuffer (e.g. un-minimised).
    void waitWhileMinimized() const;

    // Set true by the resize callback; the renderer polls + clears this so it
    // knows to rebuild the swapchain.
    [[nodiscard]] bool framebufferResized() const { return framebufferResized_; }
    void clearFramebufferResized() { framebufferResized_ = false; }

    [[nodiscard]] GLFWwindow* handle() const { return window_; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);

    GLFWwindow* window_ = nullptr;
    bool framebufferResized_ = false;
};

} // namespace vg
