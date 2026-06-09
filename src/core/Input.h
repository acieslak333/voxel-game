#pragma once

#include <glm/glm.hpp>

namespace vg {

class Window;

// Snapshot of player input for one frame, decoupled from GLFW so the player
// controller does not depend on the windowing library.
struct InputState {
    glm::vec2 move{0.0f};       // x: strafe (+ = right), y: forward (+ = forward)
    bool jump          = false; // walking: jump
    bool sprint        = false; // hold to move faster
    bool ascend        = false; // free-fly: up
    bool descend       = false; // free-fly: down
    bool toggleFreeFly = false; // edge-triggered (true only on the press frame)
    glm::vec2 look{0.0f};       // mouse delta this frame, in pixels
};

// -----------------------------------------------------------------------------
//  Input
// -----------------------------------------------------------------------------
//  Polls keyboard + mouse from a Window once per frame into an InputState. Owns
//  the small amount of state needed for mouse deltas and key edge detection.
// -----------------------------------------------------------------------------
class Input {
public:
    explicit Input(Window& window);

    // Sample input for this frame. Call exactly once per frame after polling
    // window events.
    InputState poll();

private:
    Window& window_;
    double lastX_ = 0.0;
    double lastY_ = 0.0;
    bool   haveLast_        = false; // ignore the first frame's huge delta
    bool   prevFreeFlyKey_  = false; // for edge detection
};

} // namespace vg
