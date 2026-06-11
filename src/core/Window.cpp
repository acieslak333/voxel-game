#include "core/Window.h"

#include <stdexcept>

namespace vg {

Window::Window(int width, int height, std::string title) {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialise GLFW");
    }

    // We drive rendering with Vulkan, so tell GLFW not to create an OpenGL
    // context for us.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("Failed to create GLFW window");
    }

    // Store a back-pointer so static GLFW callbacks can reach this instance.
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetScrollCallback(window_, scrollCallback);
}

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

bool Window::shouldClose() const {
    return glfwWindowShouldClose(window_);
}

void Window::pollEvents() const {
    glfwPollEvents();
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create window surface");
    }
    return surface;
}

std::vector<const char*> Window::requiredInstanceExtensions() {
    uint32_t count = 0;
    const char** names = glfwGetRequiredInstanceExtensions(&count);
    if (!names) {
        throw std::runtime_error(
            "GLFW reports Vulkan is unavailable (no required instance extensions)");
    }
    return std::vector<const char*>(names, names + count);
}

void Window::framebufferSize(int& outWidth, int& outHeight) const {
    glfwGetFramebufferSize(window_, &outWidth, &outHeight);
}

void Window::setCursorDisabled(bool disabled) const {
    glfwSetInputMode(window_, GLFW_CURSOR,
                     disabled ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    // Raw motion gives unaccelerated deltas, which feel better for mouse-look.
    if (disabled && glfwRawMouseMotionSupported()) {
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
}

void Window::setFullscreen(bool on) {
    if (on == fullscreen_) return;
    if (on) {
        // Remember the windowed rect so we can restore it later.
        glfwGetWindowPos(window_, &winX_, &winY_);
        glfwGetWindowSize(window_, &winW_, &winH_);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = mon ? glfwGetVideoMode(mon) : nullptr;
        if (mon && mode) {
            glfwSetWindowMonitor(window_, mon, 0, 0, mode->width, mode->height,
                                 mode->refreshRate);
        }
    } else {
        const int w = winW_ > 0 ? winW_ : 1280, h = winH_ > 0 ? winH_ : 720;
        glfwSetWindowMonitor(window_, nullptr, winX_, winY_, w, h, 0);
    }
    fullscreen_ = on;
    framebufferResized_ = true; // rebuild the swapchain at the new size
}

void Window::waitWhileMinimized() const {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0) {
        glfwGetFramebufferSize(window_, &w, &h);
        glfwWaitEvents(); // sleep until something happens (e.g. un-minimise)
    }
}

void Window::framebufferResizeCallback(GLFWwindow* window, int /*width*/, int /*height*/) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    self->framebufferResized_ = true;
}

double Window::takeScrollDelta() {
    const double d = scrollAccum_;
    scrollAccum_ = 0.0;
    return d;
}

void Window::scrollCallback(GLFWwindow* window, double /*xoffset*/, double yoffset) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    self->scrollAccum_ += yoffset;
}

} // namespace vg
