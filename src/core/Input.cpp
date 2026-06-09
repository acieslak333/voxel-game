#include "core/Input.h"

#include "core/Window.h"

namespace vg {

Input::Input(Window& window) : window_(window) {}

InputState Input::poll() {
    GLFWwindow* w = window_.handle();
    InputState in;

    auto down = [&](int key) { return glfwGetKey(w, key) == GLFW_PRESS; };

    // Movement axes (WASD).
    if (down(GLFW_KEY_W)) in.move.y += 1.0f;
    if (down(GLFW_KEY_S)) in.move.y -= 1.0f;
    if (down(GLFW_KEY_D)) in.move.x += 1.0f;
    if (down(GLFW_KEY_A)) in.move.x -= 1.0f;

    in.jump    = down(GLFW_KEY_SPACE);            // walking: jump
    in.ascend  = down(GLFW_KEY_SPACE);            // free-fly: rise
    in.descend = down(GLFW_KEY_LEFT_CONTROL);     // free-fly: descend
    in.sprint  = down(GLFW_KEY_LEFT_SHIFT);       // move faster

    // Esc quits.
    if (down(GLFW_KEY_ESCAPE)) {
        glfwSetWindowShouldClose(w, GLFW_TRUE);
    }

    // F toggles free-fly, edge-triggered so holding it doesn't flip every frame.
    const bool freeFlyKey = down(GLFW_KEY_F);
    in.toggleFreeFly = freeFlyKey && !prevFreeFlyKey_;
    prevFreeFlyKey_ = freeFlyKey;

    // Mouse look delta.
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    if (haveLast_) {
        in.look.x = static_cast<float>(x - lastX_);
        in.look.y = static_cast<float>(y - lastY_);
    }
    lastX_ = x;
    lastY_ = y;
    haveLast_ = true;

    return in;
}

} // namespace vg
