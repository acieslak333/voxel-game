#include "core/Ui.h"

#include "render/UiRenderer.h"

#include <algorithm>
#include <cmath>

namespace vg {

namespace {
// Charcoal / cream / lilac pixel-art theme (palette colours, sRGB — the UI shader
// linearises them). charcoal = #332F2E, cream = #F6DBC4, lilac = #DF9EE9.
const glm::vec4 kCharcoal {0.200f, 0.184f, 0.180f, 1.0f};
const glm::vec4 kCream    {0.965f, 0.859f, 0.769f, 1.0f};
const glm::vec4 kLilac    {0.875f, 0.620f, 0.914f, 1.0f};

constexpr float kFontScale = 0.62f; // body text size relative to the baked font
constexpr float kUnit      = 3.0f;  // UI "pixel" unit -> chunky stepped corners
constexpr float kBorder    = 2 * kUnit; // thick border lines (>= 2 "blocks")
constexpr float kRadius    = 3 * kUnit; // rounded-corner radius

// Draw `s` with a charcoal outline behind it, so text stays readable over any
// terrain. Eight offset copies in the outline colour, then the fill colour on top.
void textOutlined(UiRenderer& r, float x, float y, const std::string& s, float scale,
                  const glm::vec4& color) {
    constexpr float o = 2.0f; // outline offset (px)
    const glm::vec2 offs[8] = {{-o, 0}, {o, 0}, {0, -o}, {0, o},
                               {-o, -o}, {o, -o}, {-o, o}, {o, o}};
    for (const glm::vec2& d : offs) {
        r.text(x + d.x, y + d.y, s, kCharcoal, scale);
    }
    r.text(x, y, s, color, scale);
}
} // namespace

Ui::Ui(UiRenderer& renderer, float mouseX, float mouseY, bool mouseDown, bool mousePressed)
    : r_(renderer), mx_(mouseX), my_(mouseY), down_(mouseDown), pressed_(mousePressed) {}

bool Ui::hovered(float x, float y, float w, float h) const {
    return mx_ >= x && mx_ <= x + w && my_ >= y && my_ <= y + h;
}

void Ui::panel(float x, float y, float w, float h, const glm::vec4& color) {
    r_.rect(x, y, w, h, color);
}

void Ui::roundRect(float x, float y, float w, float h, float radius, const glm::vec4& c) {
    // Snap the radius to the pixel grid and clamp it to fit the box.
    float r = std::min({radius, w * 0.5f, h * 0.5f});
    r = std::floor(r / kUnit) * kUnit;
    if (r < kUnit) {
        r_.rect(x, y, w, h, c); // too small to round
        return;
    }

    // Middle cross fills everything except the four r×r corner boxes.
    r_.rect(x, y + r, w, h - 2 * r, c);
    r_.rect(x + r, y, w - 2 * r, r, c);
    r_.rect(x + r, y + h - r, w - 2 * r, r, c);

    // Rounded corners: kUnit-tall rows whose width follows a quarter circle,
    // snapped to the grid so the steps read as deliberate pixels.
    const int steps = static_cast<int>(r / kUnit);
    for (int j = 0; j < steps; ++j) {
        const float py = (j + 0.5f) * kUnit;       // row centre from the outer edge
        const float dy = r - py;
        float inset = r - std::sqrt(std::max(0.0f, r * r - dy * dy));
        inset = std::round(inset / kUnit) * kUnit;  // chunky steps
        const float rw   = r - inset;               // filled width of this row
        const float rowY = j * kUnit;
        const float botY = y + h - rowY - kUnit;
        r_.rect(x + inset,     y + rowY, rw, kUnit, c); // top-left
        r_.rect(x + w - r,     y + rowY, rw, kUnit, c); // top-right
        r_.rect(x + inset,     botY,     rw, kUnit, c); // bottom-left
        r_.rect(x + w - r,     botY,     rw, kUnit, c); // bottom-right
    }
}

void Ui::roundRectOutline(float x, float y, float w, float h, float radius,
                          float thickness, const glm::vec4& c) {
    float r = std::min({radius, w * 0.5f, h * 0.5f});
    r = std::floor(r / kUnit) * kUnit;
    const float t = std::min(thickness, std::min(w, h) * 0.5f);

    if (r < kUnit) { // square ring (four edge bands)
        r_.rect(x, y, w, t, c);
        r_.rect(x, y + h - t, w, t, c);
        r_.rect(x, y + t, t, h - 2 * t, c);
        r_.rect(x + w - t, y + t, t, h - 2 * t, c);
        return;
    }

    // Straight edge bands between the corners.
    r_.rect(x + r, y, w - 2 * r, t, c);
    r_.rect(x + r, y + h - t, w - 2 * r, t, c);
    r_.rect(x, y + r, t, h - 2 * r, c);
    r_.rect(x + w - t, y + r, t, h - 2 * r, c);

    // Corner rings: per row, the band between the outer arc (radius r) and the
    // inner arc (radius r-t), both centred at the inner corner.
    const float ri = r - t;
    const int steps = static_cast<int>(r / kUnit);
    for (int j = 0; j < steps; ++j) {
        const float py = (j + 0.5f) * kUnit;
        const float dy = r - py;
        float io = r - std::sqrt(std::max(0.0f, r * r - dy * dy));
        io = std::round(io / kUnit) * kUnit;
        float ii = r; // inner arc doesn't reach this row -> ring runs to r
        if (ri > 0.0f && std::abs(dy) <= ri) {
            ii = r - std::sqrt(std::max(0.0f, ri * ri - dy * dy));
            ii = std::round(ii / kUnit) * kUnit;
        }
        const float segW = std::max(0.0f, ii - io);
        if (segW <= 0.0f) continue;
        const float ytop = y + j * kUnit;
        const float ybot = y + h - (j + 1) * kUnit;
        r_.rect(x + io,      ytop, segW, kUnit, c); // top-left
        r_.rect(x + w - ii,  ytop, segW, kUnit, c); // top-right
        r_.rect(x + io,      ybot, segW, kUnit, c); // bottom-left
        r_.rect(x + w - ii,  ybot, segW, kUnit, c); // bottom-right
    }
}

void Ui::frame(float x, float y, float w, float h, const glm::vec4& fill,
               const glm::vec4& border, float thickness, float radius) {
    if (haveSprites_) {
        box(UiBox::Bg, x, y, w, h); // border + fill baked into the 9-patch sprite
        return;
    }
    roundRect(x, y, w, h, radius, border); // whole shape = border colour
    if (w > 2 * thickness && h > 2 * thickness) {
        roundRect(x + thickness, y + thickness, w - 2 * thickness, h - 2 * thickness,
                  std::max(0.0f, radius - thickness), fill); // inset fill leaves the border
    }
}

void Ui::ninePatch(float x, float y, float w, float h, uint32_t layer,
                   const glm::vec4& tint) {
    constexpr float kSrc = 16.0f, kInset = 5.0f; // source size + corner inset (px)
    const float u0 = 0.0f, u1 = kInset / kSrc, u2 = (kSrc - kInset) / kSrc, u3 = 1.0f;
    const float cd = std::min({8.0f, w * 0.5f, h * 0.5f}); // dest corner size
    const float mw = w - 2 * cd, mh = h - 2 * cd;          // stretched middle span
    const float xR = x + w - cd, yB = y + h - cd, mx = x + cd, my = y + cd;
    auto s = [&](float dx, float dy, float dw, float dh,
                 float su0, float sv0, float su1, float sv1) {
        if (dw > 0.0f && dh > 0.0f) r_.sprite(dx, dy, dw, dh, layer, su0, sv0, su1, sv1, tint);
    };
    // 4 corners (fixed size), 4 edges (stretched on one axis), centre (both).
    s(x,  y,  cd, cd, u0, u0, u1, u1);  s(xR, y,  cd, cd, u2, u0, u3, u1);
    s(x,  yB, cd, cd, u0, u2, u1, u3);  s(xR, yB, cd, cd, u2, u2, u3, u3);
    s(mx, y,  mw, cd, u1, u0, u2, u1);  s(mx, yB, mw, cd, u1, u2, u2, u3);
    s(x,  my, cd, mh, u0, u1, u1, u2);  s(xR, my, cd, mh, u2, u1, u3, u2);
    s(mx, my, mw, mh, u1, u1, u2, u2);
}

void Ui::box(UiBox kind, float x, float y, float w, float h, const glm::vec4& tint) {
    if (!haveSprites_) { roundRect(x, y, w, h, kRadius, kCharcoal); return; }
    uint32_t layer = sprites_.bg;
    switch (kind) {
        case UiBox::Bg:       layer = sprites_.bg;       break;
        case UiBox::Bg2:      layer = sprites_.bg2;      break;
        case UiBox::Bg3:      layer = sprites_.bg3;      break;
        case UiBox::Eq:       layer = sprites_.eq;       break;
        case UiBox::Button:   layer = sprites_.button;   break;
        case UiBox::SliderBg: layer = sprites_.sliderBg; break;
        case UiBox::Slider:   layer = sprites_.slider;   break;
        case UiBox::Border:   layer = sprites_.border;   break;
    }
    ninePatch(x, y, w, h, layer, tint);
}

void Ui::line(const glm::vec2& a, const glm::vec2& b, float thickness,
              const glm::vec4& color) {
    const glm::vec2 d = b - a;
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-4f) {
        return;
    }
    // A quad whose long axis is a->b and whose width is `thickness` (extruded along
    // the perpendicular).
    const glm::vec2 n = glm::vec2(-d.y, d.x) / len * (thickness * 0.5f);
    r_.triangle(a + n, a - n, b - n, color);
    r_.triangle(a + n, b - n, b + n, color);
}

void Ui::itemIcon(float cx, float cy, float r, uint32_t iconLayer) {
    // The icon is fully prerendered (the isometric shading is baked into the PNG by
    // scripts/gen_icons.py), so we just blit the layer as a flat square. Spanning
    // 2r each way keeps the same on-screen size the old runtime isoCube produced.
    r_.sprite(cx - r, cy - r, 2.0f * r, 2.0f * r, iconLayer,
              0.0f, 0.0f, 1.0f, 1.0f, glm::vec4(1.0f));
}

void Ui::label(float x, float y, const std::string& s, float scale, const glm::vec4& color) {
    textOutlined(r_, x, y, s, scale, color);
}

void Ui::labelCentered(float cx, float y, const std::string& s, float scale,
                       const glm::vec4& color) {
    const float w = r_.textWidth(s, scale);
    textOutlined(r_, cx - w * 0.5f, y, s, scale, color);
}

bool Ui::button(float x, float y, float w, float h, const std::string& label) {
    const bool hot = hovered(x, y, w, h);
    if (haveSprites_) {
        box(UiBox::Button, x, y, w, h);
        if (hot) box(UiBox::Border, x, y, w, h, kLilac); // lilac border ring on hover
    } else {
        frame(x, y, w, h, kCharcoal, hot ? kLilac : kCream, kBorder, kRadius);
    }
    const float tw = r_.textWidth(label, kFontScale);
    const float th = r_.lineHeight(kFontScale);
    textOutlined(r_, x + (w - tw) * 0.5f, y + (h - th) * 0.5f, label, kFontScale, kCream);
    return hot && pressed_;
}

bool Ui::toggle(float x, float y, float w, float h, const std::string& label, bool value) {
    const std::string text = label + ": " + (value ? "On" : "Off");
    return button(x, y, w, h, text) ? !value : value;
}

float Ui::slider(float x, float y, float w, float h, float value, float minv, float maxv,
                 int steps) {
    // Track + filled portion + handle.
    const float t0 = (maxv > minv) ? (value - minv) / (maxv - minv) : 0.0f;
    float t = std::clamp(t0, 0.0f, 1.0f);

    // Drag while the button is held and the cursor is over the track (with a bit
    // of vertical slack so a fast drag doesn't slip off).
    if (down_ && hovered(x, y - 6.0f, w, h + 12.0f)) {
        t = std::clamp((mx_ - x) / w, 0.0f, 1.0f);
        if (steps > 0) {
            t = std::round(t * static_cast<float>(steps)) / static_cast<float>(steps);
        }
    }

    // Recessed track, a lilac fill bar to the value, and a chunky cream handle.
    const float pad = kBorder;
    if (haveSprites_) {
        box(UiBox::SliderBg, x, y, w, h);
        if (t > 0.0f && w > 2 * pad && h > 2 * pad) {
            box(UiBox::Slider, x + pad, y + pad, (w - 2 * pad) * t, h - 2 * pad);
        }
    } else {
        frame(x, y, w, h, kCharcoal, kCream, kBorder, kRadius);
        if (t > 0.0f && w > 2 * pad && h > 2 * pad) {
            r_.rect(x + pad, y + pad, (w - 2 * pad) * t, h - 2 * pad, kLilac);
        }
    }
    const float handleW = 3 * kUnit;
    roundRect(x + t * w - handleW * 0.5f, y - kUnit, handleW, h + 2 * kUnit, kUnit, kCream);

    return minv + t * (maxv - minv);
}

} // namespace vg
