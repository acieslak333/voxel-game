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

} // namespace vg
