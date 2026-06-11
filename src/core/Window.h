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
    void requestClose() const { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
    void pollEvents() const;

    // Create a Vulkan surface for this window. Throws on failure.
    [[nodiscard]] VkSurfaceKHR createSurface(VkInstance instance) const;

    // Instance extensions GLFW needs to present to this window's surface.
    [[nodiscard]] static std::vector<const char*> requiredInstanceExtensions();

    // Current framebuffer size in pixels (may differ from window size on HiDPI).
    void framebufferSize(int& outWidth, int& outHeight) const;

    // Blocks until the window has a non-zero framebuffer (e.g. un-minimised).
    void waitWhileMinimized() const;

    // Lock + hide the cursor for FPS mouse-look (and request raw motion if the
    // platform supports it). Passing false restores the normal cursor.
    void setCursorDisabled(bool disabled) const;

    // Switch between borderless-fullscreen (primary monitor's video mode) and the
    // previous windowed rect. No-op if already in the requested state. Flags a
    // framebuffer resize so the renderer rebuilds the swapchain.
    void setFullscreen(bool on);
    [[nodiscard]] bool isFullscreen() const { return fullscreen_; }

    // Set true by the resize callback; the renderer polls + clears this so it
    // knows to rebuild the swapchain.
    [[nodiscard]] bool framebufferResized() const { return framebufferResized_; }
    void clearFramebufferResized() { framebufferResized_ = false; }

    [[nodiscard]] GLFWwindow* handle() const { return window_; }

    // Mouse-wheel scrolled since the last call, in notches (+ = wheel up), then
    // reset to zero. Polled once per frame by Input; GLFW only delivers scroll
    // via a callback, so the window accumulates it between polls.
    [[nodiscard]] double takeScrollDelta();

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);

    GLFWwindow* window_ = nullptr;
    bool framebufferResized_ = false;
    double scrollAccum_ = 0.0; // wheel notches since the last takeScrollDelta()
    bool fullscreen_ = false;
    int  winX_ = 0, winY_ = 0, winW_ = 0, winH_ = 0; // saved windowed rect (to restore)
};

} // namespace vg
