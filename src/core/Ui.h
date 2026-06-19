#pragma once

/**
 * @file Ui.h
 * @brief Immediate-mode widget layer over UiRenderer for the game HUD and menus.
 *
 * Ui wraps UiRenderer with per-frame mouse state and provides stateless widget
 * calls (button, toggle, slider, label, icons, 9-patch boxes). Construct one
 * per frame; all coordinates are window pixels with a top-left origin.
 * @see docs/CODE_INDEX.md
 */
#include "world/BlockRegistry.h" // UiSpriteLayers

#include <glm/glm.hpp>

#include <string>

namespace vg {

class UiRenderer;

/**
 * @brief Selects which 9-patch UI sprite to draw.
 *
 * Each value maps to a texture-array layer via UiSpriteLayers (assets/textures/ui_*.png).
 */
enum class UiBox { Bg, Bg2, Bg3, Eq, Button, SliderBg, Slider, Border };

/**
 * @brief Immediate-mode UI widget set built on top of UiRenderer.
 *
 * Construct once per frame with the current mouse position and button state.
 * Each widget is stateless: it draws its geometry and returns whether the
 * user interacted with it this frame (clicked button, dragged slider, etc.).
 */
class Ui {
public:
    Ui(UiRenderer& renderer, float mouseX, float mouseY, bool mouseDown, bool mousePressed);

    // Bind the 9-patch UI sprite layers (resolved once from the registry). When set,
    // frame()/button()/slider()/box() draw from the sprites; otherwise they fall back
    // to the procedural rounded-rect look.
    void setSprites(const UiSpriteLayers& s) { sprites_ = s; haveSprites_ = true; }

    // Draw a 9-patch sprite: corners kept crisp, edges/centre stretched. `box` picks
    // one of the named UI sprites; `ninePatch` takes an explicit array layer.
    void box(UiBox kind, float x, float y, float w, float h,
             const glm::vec4& tint = glm::vec4(1.0f));
    void ninePatch(float x, float y, float w, float h, uint32_t layer,
                   const glm::vec4& tint = glm::vec4(1.0f));

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

    // An inventory item icon centred at (cx, cy): a flat sprite of the item's
    // prerendered 16x16 icon layer (assets/textures/icons/<name>.png — an
    // isometric cube for blocks, an upright sprite for tools/plants; see
    // scripts/gen_icons.py and BlockRegistry::iconLayer). `r` is the half-extent,
    // so the icon spans 2r each way (matching the old runtime isoCube sizing).
    void itemIcon(float cx, float cy, float r, uint32_t iconLayer);
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
    UiSpriteLayers sprites_{};
    bool           haveSprites_ = false;
};

} // namespace vg
