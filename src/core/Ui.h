#pragma once

#include <glm/glm.hpp>

#include <string>

namespace vg {

class UiRenderer;

// -----------------------------------------------------------------------------
//  Ui
// -----------------------------------------------------------------------------
//  A tiny immediate-mode widget layer over UiRenderer. Construct one per frame
//  with the current mouse state; each widget draws itself and reports its
//  interaction (a button returns whether it was clicked, a slider/toggle returns
//  its new value). All coordinates are in window pixels, top-left origin.
// -----------------------------------------------------------------------------
class Ui {
public:
    Ui(UiRenderer& renderer, float mouseX, float mouseY, bool mouseDown, bool mousePressed);

    // A filled panel and text. `scale` multiplies the baked font size.
    void panel(float x, float y, float w, float h, const glm::vec4& color);

    // A filled rounded rectangle, pixel-art style: corners are a stepped quarter-
    // circle snapped to an internal pixel grid (chunky, not anti-aliased).
    void roundRect(float x, float y, float w, float h, float radius, const glm::vec4& color);

    // Just the rounded border ring of `thickness` (the interior is left untouched,
    // so it shows whatever is behind — used for the transparent hotbar slots).
    void roundRectOutline(float x, float y, float w, float h, float radius,
                          float thickness, const glm::vec4& color);

    // A pixel-art framed box: a `fill` rounded rect inside a thick `border` outline
    // (the project UI look — charcoal fill, cream border, rounded thick edges).
    void frame(float x, float y, float w, float h, const glm::vec4& fill,
               const glm::vec4& border, float thickness = 6.0f, float radius = 12.0f);

    // A straight line of the given thickness between two pixel-space points (drawn
    // as a quad). Used for the world-space block-selection wireframe.
    void line(const glm::vec2& a, const glm::vec2& b, float thickness,
              const glm::vec4& color);

    // An isometric block icon centred at (cx, cy): three textured rhombus faces
    // (top, left, right) sampling the block texture array. `topLayer` textures the
    // top face, `sideLayer` the two side faces (darkened differently for depth).
    // `r` is the half-width of the cube's bounding box (icon spans 2r each way).
    void isoCube(float cx, float cy, float r, uint32_t topLayer, uint32_t sideLayer);
    void label(float x, float y, const std::string& s, float scale, const glm::vec4& color);
    void labelCentered(float cx, float y, const std::string& s, float scale,
                       const glm::vec4& color);

    // A clickable button with a centred label. Returns true on the click frame.
    bool button(float x, float y, float w, float h, const std::string& label);

    // A labelled on/off control ("Label: On"). Returns the (possibly flipped) value.
    bool toggle(float x, float y, float w, float h, const std::string& label, bool value);

    // A horizontal slider over [minv, maxv]. If steps > 0 the value snaps to that
    // many intervals. Returns the (possibly dragged) value.
    float slider(float x, float y, float w, float h, float value, float minv, float maxv,
                 int steps);

    [[nodiscard]] bool hovered(float x, float y, float w, float h) const;

private:
    UiRenderer& r_;
    float mx_, my_;
    bool  down_, pressed_;
};

} // namespace vg
