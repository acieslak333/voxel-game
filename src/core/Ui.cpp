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
    roundRect(x, y, w, h, radius, border); // whole shape = border colour
    if (w > 2 * thickness && h > 2 * thickness) {
        roundRect(x + thickness, y + thickness, w - 2 * thickness, h - 2 * thickness,
                  std::max(0.0f, radius - thickness), fill); // inset fill leaves the border
    }
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

void Ui::isoCube(float cx, float cy, float r, uint32_t topLayer, uint32_t sideLayer) {
    // 2:1 isometric cube. The silhouette is a hexagon; `q` is the quarter-height
    // that gives the classic dimetric look. Six outline points plus the centre O
    // where the three visible faces meet:
    const float q = r * 0.5f;
    const glm::vec2 A{cx,     cy - 2 * q}; // top apex
    const glm::vec2 B{cx + r, cy - q};     // upper-right
    const glm::vec2 C{cx - r, cy - q};     // upper-left
    const glm::vec2 O{cx,     cy};         // centre
    const glm::vec2 D{cx + r, cy + q};     // lower-right
    const glm::vec2 E{cx - r, cy + q};     // lower-left
    const glm::vec2 F{cx,     cy + 2 * q}; // bottom apex

    // Per-face shade (white = unchanged), brightest on top, right brighter than
    // left, mirroring the in-world directional shading so the cube reads as 3D.
    const glm::vec4 topS  {1.00f, 1.00f, 1.00f, 1.0f};
    const glm::vec4 rightS{0.82f, 0.82f, 0.82f, 1.0f};
    const glm::vec4 leftS {0.60f, 0.60f, 0.60f, 1.0f};

    // Each face is a parallelogram, corners given top-left -> bottom-left so the
    // texture sits upright. Top: A-B-O-C; right: O-B-D-F; left: C-O-F-E.
    r_.blockFace(A, B, O, C, topLayer, topS);
    r_.blockFace(O, B, D, F, sideLayer, rightS);
    r_.blockFace(C, O, F, E, sideLayer, leftS);
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
    // Charcoal fill with a thick cream border; the border turns lilac on hover.
    frame(x, y, w, h, kCharcoal, hot ? kLilac : kCream, kBorder, kRadius);
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

    // Framed charcoal track with a thick cream border; a lilac bar fills to the
    // value and a chunky cream handle rides on top.
    frame(x, y, w, h, kCharcoal, kCream, kBorder, kRadius);
    const float pad = kBorder;
    if (t > 0.0f && w > 2 * pad && h > 2 * pad) {
        r_.rect(x + pad, y + pad, (w - 2 * pad) * t, h - 2 * pad, kLilac);
    }
    const float handleW = 3 * kUnit;
    roundRect(x + t * w - handleW * 0.5f, y - kUnit, handleW, h + 2 * kUnit, kUnit, kCream);

    return minv + t * (maxv - minv);
}

} // namespace vg
