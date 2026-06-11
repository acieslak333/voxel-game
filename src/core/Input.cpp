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
    in.sneak   = down(GLFW_KEY_LEFT_SHIFT);       // walking: crouch; free-fly: fast
    in.sprint  = down(GLFW_KEY_LEFT_CONTROL);     // walking: run faster

    // Esc toggles the pause menu (edge-triggered). Quitting is the menu's job.
    const bool escKey = down(GLFW_KEY_ESCAPE);
    in.toggleMenu = escKey && !prevEscKey_;
    prevEscKey_ = escKey;

    // E toggles the inventory screen (edge-triggered).
    const bool invKey = down(GLFW_KEY_E);
    in.toggleInventory = invKey && !prevInvKey_;
    prevInvKey_ = invKey;

    // G toggles creative <-> survival (edge-triggered).
    const bool gKey = down(GLFW_KEY_G);
    in.toggleGameMode = gKey && !prevGKey_;
    prevGKey_ = gKey;

    // F1 toggles the debug info overlay (edge-triggered).
    const bool f1Key = down(GLFW_KEY_F1);
    in.toggleDebug = f1Key && !prevF1Key_;
    prevF1Key_ = f1Key;

    // F toggles free-fly, edge-triggered so holding it doesn't flip every frame.
    const bool freeFlyKey = down(GLFW_KEY_F);
    in.toggleFreeFly = freeFlyKey && !prevFreeFlyKey_;
    prevFreeFlyKey_ = freeFlyKey;

    // Block editing. Mouse buttons are edge-triggered so one click edits exactly
    // one block (no auto-repeat while held). The left button also drives the UI.
    const bool breakBtn = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    in.breakBlock = breakBtn && !prevBreakBtn_;
    in.breakHeld  = breakBtn; // hold-to-break mining accumulates while held
    in.pointerDown = breakBtn;
    in.pointerPressed = in.breakBlock;
    prevBreakBtn_ = breakBtn;

    const bool placeBtn = glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    in.placeBlock = placeBtn && !prevPlaceBtn_;
    prevPlaceBtn_ = placeBtn;

    // Number keys 1..9 pick the block type to place (held, not edge-triggered).
    for (int k = 1; k <= 9; ++k) {
        if (down(GLFW_KEY_0 + k)) {
            in.selectSlot = k;
            break;
        }
    }

    // Mouse wheel cycles the hotbar. Wheel up selects the previous slot (matching
    // the usual block-game convention); a fast flick that reports several notches
    // moves several slots. App wraps the result around the hotbar.
    const double scroll = window_.takeScrollDelta();
    if (scroll > 0.0) {
        in.hotbarScroll = -static_cast<int>(scroll + 0.5);
    } else if (scroll < 0.0) {
        in.hotbarScroll = -static_cast<int>(scroll - 0.5);
    }

    // Mouse position (used by the UI when the cursor is free) and look delta.
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    in.cursor = {static_cast<float>(x), static_cast<float>(y)};
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
