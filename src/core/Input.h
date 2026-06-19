#pragma once

/**
 * @file Input.h
 * @brief Per-frame input snapshot and GLFW input poller.
 *
 * InputState is a plain aggregate that carries the fully decoded input for
 * one frame (movement axes, edge-detected key presses, mouse look delta,
 * cursor position). Input::poll() fills one each frame from GLFW state.
 * @see docs/CODE_INDEX.md
 */
#include <glm/glm.hpp>

namespace vg {

class Window;

/**
 * @brief Snapshot of all player input for one frame.
 *
 * Decoupled from GLFW so the player controller and UI have no windowing
 * dependency. Edge-detected fields (toggleMenu, breakBlock, etc.) are true
 * only on the single frame the transition occurs.
 */
struct InputState {
    glm::vec2 move{0.0f};       // x: strafe (+ = right), y: forward (+ = forward)
    bool jump          = false; // walking: jump
    bool sprint        = false; // walking: hold Ctrl to run faster
    bool sneak         = false; // walking: hold Shift to crouch (slow + edge-stop)
    bool ascend        = false; // free-fly: up
    bool descend       = false; // free-fly: down
    bool toggleFreeFly = false; // edge-triggered (true only on the press frame)
    glm::vec2 look{0.0f};       // mouse delta this frame, in pixels

    bool breakBlock = false;    // left mouse: destroy the looked-at block (edge)
    bool breakHeld  = false;    // left mouse held (hold-to-break mining; level)
    bool placeBlock = false;    // right mouse: place against it (edge — press)
    bool placeHeld  = false;    // right mouse held (level — drives the hammer radial)
    bool placeReleased = false; // right mouse released this frame (edge — commits the radial)
    int  selectSlot = 0;        // number key 1..9 held this frame (0 = none)
    int  hotbarScroll = 0;      // mouse wheel: hotbar slots to advance (+ = next,
                                // - = previous); 0 if the wheel didn't move

    bool drop       = false;    // Q pressed this frame: throw the selected stack (edge)
    bool toggleMenu = false;    // Esc pressed this frame (open/close the menu)
    bool toggleInventory = false; // E pressed this frame (open/close the inventory)
    bool toggleGameMode = false;  // G pressed this frame (creative <-> survival)
    bool toggleDebug = false;   // F1 pressed this frame (show/hide debug overlay)
    bool toggleFullscreen = false; // F11 pressed this frame
    bool ctrl = false;          // LeftCtrl held (hammer: rotate-on-click instead of face-from-look)
    glm::vec2 cursor{0.0f};     // absolute cursor position in window pixels
    bool pointerDown    = false; // left mouse held (UI dragging)
    bool pointerPressed = false; // left mouse pressed this frame (UI click)
};

/**
 * @brief Polls keyboard and mouse from a Window into an InputState each frame.
 *
 * Owns the per-frame state for mouse delta computation and key edge detection
 * (previous-frame button/key booleans). Construct once; call poll() once per
 * frame after Window::pollEvents().
 */
class Input {
public:
    explicit Input(Window& window);

    // Sample input for this frame. Call exactly once per frame after polling
    // window events.
    InputState poll();

    // Forget the last cursor position so the next poll reports no look delta.
    // Call when toggling the cursor between locked (gameplay) and free (menu),
    // otherwise the jump in cursor position spins the camera.
    void resetMouseDelta() { haveLast_ = false; }

private:
    Window& window_;
    double lastX_ = 0.0;
    double lastY_ = 0.0;
    bool   haveLast_        = false; // ignore the first frame's huge delta
    bool   prevFreeFlyKey_  = false; // for edge detection
    bool   prevBreakBtn_    = false; // left mouse, for edge detection
    bool   prevPlaceBtn_    = false; // right mouse, for edge detection
    bool   prevEscKey_      = false; // Esc, for edge detection
    bool   prevInvKey_      = false; // E (inventory), for edge detection
    bool   prevDropKey_     = false; // Q (drop), for edge detection
    bool   prevGKey_        = false; // G (game mode), for edge detection
    bool   prevF1Key_       = false; // F1, for edge detection
    bool   prevF11Key_      = false; // F11, for edge detection
};

} // namespace vg
