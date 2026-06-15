#include "core/App.h"

#include "core/ColorPalette.h"
#include "core/ShapePicker.h"
#include "core/Ui.h"
#include "world/BlockRegistry.h"
#include "world/Raycast.h"

#include <glm/gtc/matrix_transform.hpp>

#include <stb_image.h> // declarations only; the implementation lives in TextureArray.cpp

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// VG_ASSET_DIR is baked in by CMake; keep the same fallback App.cpp uses so this
// translation unit also resolves the assets path when built standalone.
#ifndef VG_ASSET_DIR
#define VG_ASSET_DIR "assets"
#endif

namespace vg {

// -----------------------------------------------------------------------------
//  UI
// -----------------------------------------------------------------------------

namespace {
// Charcoal / cream / lilac pixel-art theme (palette colours, sRGB).
const glm::vec4 kCharcoal {0.200f, 0.184f, 0.180f, 1.0f};
const glm::vec4 kCream    {0.965f, 0.859f, 0.769f, 1.0f};
const glm::vec4 kLilac    {0.875f, 0.620f, 0.914f, 1.0f};
const glm::vec4 kUiText   = kCream;
const glm::vec4 kPanelFill{0.200f, 0.184f, 0.180f, 0.96f}; // charcoal, near-opaque
const glm::vec4 kOverlayFill{0.200f, 0.184f, 0.180f, 0.82f};
const glm::vec4 kUiDim    {0.07f, 0.06f, 0.06f, 0.55f};    // charcoal screen dim
// Pixel-art frame metrics (match Ui.cpp's kUnit = 3). Every outline is at least
// 2 "blocks" (2 * 3 = 6 px) thick.
constexpr float kBlock       = 3.0f;       // one UI pixel-art "block"
constexpr float kFrameThin   = 2 * kBlock; // overlay / hotbar ring (>= 2 blocks)
constexpr float kFrameThick  = 3 * kBlock; // panels + selected hotbar slot
constexpr float kFrameRadius = 3 * kBlock;
} // namespace

void App::buildUi(const InputState& in) {
    const VkExtent2D ext = swapchain_.extent();
    ui_.begin(ext);
    Ui ui(ui_, in.cursor.x, in.cursor.y, in.pointerDown, in.pointerPressed);
    ui.setSprites(world_.registry().uiSprites()); // 9-patch UI sprites (ISSUES #15)

    const float W = static_cast<float>(ext.width);
    const float H = static_cast<float>(ext.height);

    // The inventory/chest screens draw their own hotbar row, so hide the HUD bar then.
    if (!inventoryOpen_ && !chestOpen_) {
        buildHotbar(ui, W, H);
    }
    if (debugOverlay_) {
        buildDebugOverlay(ui);
    }

    if (paused_) {
        ui.panel(0.0f, 0.0f, W, H, kUiDim); // dim the world behind the menu
        if (palettePickerOpen_) {
            // Modal: take over input so menu buttons under it don't also react.
            buildPalettePicker(ui, W, H, in);
        } else {
            // Two columns centred as a pair: the options menu + the atmosphere tuner.
            const float pw = 460.0f, tw = 460.0f, gap = 24.0f, ph = 712.0f;
            const float startX = std::round((W - (pw + gap + tw)) * 0.5f);
            const float py = std::round((H - ph) * 0.5f);
            buildMenu(ui, startX, py, pw, ph);
            buildTuning(ui, startX + pw + gap, py, tw, ph);
        }
    } else if (chestOpen_) {
        buildChest(ui, W, H, in);
    } else if (inventoryOpen_) {
        buildInventory(ui, W, H, in);
    } else if (shapePickerOpen_) {
        buildShapePicker(ui, W, H, in);
    } else {
        // Same camera the scene is drawn with, so the block wireframe lines up.
        const Camera& cam = player_.camera();
        const float aspect = H > 0.0f ? W / H : 1.0f;
        const glm::mat4 view = cam.viewMatrix();
        glm::mat4 proj = glm::perspective(glm::radians(cam.fovDegrees), aspect,
                                          cam.nearZ, cam.farZ);
        proj[1][1] *= -1.0f; // Vulkan clip-space Y flip (matches the world pass)

        buildBlockIndicator(ui, view, proj, W, H); // first, so the crosshair sits on top
        buildCrosshair(ui, W, H);
        buildDamageNumbers(ui, view, proj, W, H);
    }
}

void App::buildDamageNumbers(Ui& ui, const glm::mat4& view, const glm::mat4& proj,
                             float w, float h) {
    for (const FloatText& d : damageNumbers_) {
        const glm::vec4 c = proj * view * glm::vec4(d.pos, 1.0f);
        if (c.w <= 1e-4f) continue; // behind the camera
        const glm::vec2 ndc = glm::vec2(c) / c.w;
        const glm::vec2 s = (ndc * 0.5f + 0.5f) * glm::vec2(w, h);
        const float fade = 1.0f - d.age / 1.1f; // 1 -> 0 over its life
        const std::string txt = "-" + std::to_string(static_cast<int>(d.value + 0.5f));
        ui.labelCentered(s.x + 1.0f, s.y + 1.0f, txt, 0.6f, glm::vec4(0.0f, 0.0f, 0.0f, 0.6f * fade));
        ui.labelCentered(s.x, s.y, txt, 0.6f, glm::vec4(0.95f, 0.28f, 0.22f, fade));
    }
}

void App::buildCrosshair(Ui& ui, float w, float h) {
    const float cx = w * 0.5f, cy = h * 0.5f;
    // A small single dot: a cream centre on a charcoal outline, so it reads on any
    // background. `rc` = cream radius; the charcoal disc is a little wider.
    const float rc = 2.5f, o = 2.0f;
    auto dot = [&](float r, const glm::vec4& col) {
        ui.roundRect(cx - r, cy - r, 2 * r, 2 * r, kBlock, col);
    };
    dot(rc + o, kCharcoal); // outline
    dot(rc, kCream);        // core

    // Target feedback (ISSUES #13M): when aimed at a targetable block, expand the
    // crosshair into four little ticks so you can tell you've got something in reach.
    const Camera& cam = player_.camera();
    const RaycastHit hit = raycastVoxel(
        cam.position, cam.front(), kReach,
        [this](int x, int y, int z) { return world_.isTargetable(x, y, z); },
        [this](int x, int y, int z, ShapeBox out[]) { return world_.collisionBoxesAt(x, y, z, out); });
    if (hit.hit) {
        const float g = 5.0f, len = 4.0f, th = 2.0f; // gap from centre, tick length/thickness
        auto tick = [&](float x, float y, float tw, float thh) {
            ui.roundRect(x - 1.0f, y - 1.0f, tw + 2.0f, thh + 2.0f, 1.5f, kCharcoal);
            ui.roundRect(x, y, tw, thh, 1.5f, kCream);
        };
        tick(cx - g - len, cy - th * 0.5f, len, th); // left
        tick(cx + g, cy - th * 0.5f, len, th);       // right
        tick(cx - th * 0.5f, cy - g - len, th, len); // up
        tick(cx - th * 0.5f, cy + g, th, len);       // down
    }

    // Mining feedback: a horizontal break meter just under the crosshair that fills
    // left-to-right as the held block breaks (a cheap stand-in until the crack-stage
    // overlay lands with the texture work). Hidden when not mining.
    if (mineProgress01_ > 0.0f) {
        const float bw = 40.0f, bh = 5.0f, by = cy + 12.0f, bx = cx - bw * 0.5f;
        ui.roundRect(bx - 1.0f, by - 1.0f, bw + 2.0f, bh + 2.0f, 2.0f, kCharcoal);
        ui.roundRect(bx, by, bw * mineProgress01_, bh, 2.0f, kCream);
    }
}

void App::buildBlockIndicator(Ui& ui, const glm::mat4& view, const glm::mat4& proj,
                              float w, float h) {
    const Camera& cam = player_.camera();
    const RaycastHit hit = raycastVoxel(
        cam.position, cam.front(), kReach,
        [this](int x, int y, int z) { return world_.isTargetable(x, y, z); },
        [this](int x, int y, int z, ShapeBox out[]) { return world_.collisionBoxesAt(x, y, z, out); });
    if (!hit.hit) {
        return; // nothing in reach
    }

    // Outline the block's ACTUAL shape — the union of its collision/render boxes
    // (vg::shapeBoxes via World::collisionBoxesAt) — so a slab/stairs/post/wall
    // highlights its real volume, matching what you collide with. Targetable
    // non-solid foliage returns no boxes, so fall back to the full cell.
    ShapeBox boxes[kMaxShapeBoxes];
    int nboxes = world_.collisionBoxesAt(hit.block.x, hit.block.y, hit.block.z, boxes);
    if (nboxes == 0) {
        boxes[0] = {glm::vec3(hit.block), glm::vec3(hit.block) + glm::vec3(1.0f)};
        nboxes = 1;
    }

    // Project a world point to HUD pixels; false if it is behind the camera.
    auto project = [&](const glm::vec3& wp, glm::vec2& out) -> bool {
        const glm::vec4 c = proj * view * glm::vec4(wp, 1.0f);
        if (c.w <= 1e-4f) return false;
        const glm::vec2 ndc = glm::vec2(c) / c.w;
        out = (ndc * 0.5f + 0.5f) * glm::vec2(w, h);
        return true;
    };

    // Each edge: its two corner indices (bit x=1,y=2,z=4) + the two faces it borders.
    struct Edge { int a, b, f0, f1; };
    static const Edge edges[12] = {
        {0, 1, 2, 4}, {2, 3, 3, 4}, {4, 5, 2, 5}, {6, 7, 3, 5}, // x edges
        {0, 2, 0, 4}, {1, 3, 1, 4}, {4, 6, 0, 5}, {5, 7, 1, 5}, // y edges
        {0, 4, 0, 2}, {1, 5, 1, 2}, {2, 6, 0, 3}, {3, 7, 1, 3}, // z edges
    };
    const float inflate = 0.006f; // nudge out a hair to avoid z-fighting the surface
    const glm::vec3 eye = cam.position;

    // Draw one box's camera-facing edges at a given thickness/colour. Only edges of
    // a box face turned toward the camera are drawn (a clean silhouette per box).
    auto drawBox = [&](const ShapeBox& bx, float thick, const glm::vec4& col) {
        const glm::vec3 mn = bx.lo - glm::vec3(inflate);
        const glm::vec3 mx = bx.hi + glm::vec3(inflate);
        glm::vec2 p[8];
        bool ok[8];
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y,
                                   (i & 4) ? mx.z : mn.z);
            ok[i] = project(corner, p[i]);
        }
        const bool front[6] = {eye.x < mn.x, eye.x > mx.x, eye.y < mn.y,
                               eye.y > mx.y, eye.z < mn.z, eye.z > mx.z};
        for (const Edge& e : edges) {
            if ((front[e.f0] || front[e.f1]) && ok[e.a] && ok[e.b]) {
                ui.line(p[e.a], p[e.b], thick, col);
            }
        }
    };
    // Halo pass over all boxes first, then the cream lines on top, so one box's
    // halo never paints over another's line.
    for (int i = 0; i < nboxes; ++i) drawBox(boxes[i], 5.0f, kCharcoal);
    for (int i = 0; i < nboxes; ++i) drawBox(boxes[i], 2.0f, kCream);
}

namespace {
// Draw one inventory slot: the charcoal+cream rounded frame (thicker cream when
// `highlight`), then the item's isometric icon and its stack count (if > 1). Shared
// by the HUD hotbar and the full inventory screen so both look identical.
void drawSlot(Ui& ui, const BlockRegistry& reg, float x, float y, float slot,
              float /*radius*/, const ItemStack& st, bool highlight) {
    ui.box(UiBox::Eq, x, y, slot, slot); // 9-patch slot background
    if (highlight) {
        ui.box(UiBox::Border, x, y, slot, slot, kLilac); // selected / hovered ring
    }
    if (st.empty()) {
        return;
    }
    const float iconR = slot * 0.5f - 11.0f; // inset to clear the slot border
    ui.itemIcon(x + slot * 0.5f, y + slot * 0.5f, iconR, reg.iconLayer(st.blockId));
    if (st.count > 1) {
        const std::string n = std::to_string(st.count);
        const float cx = x + slot - 14.0f, cy = y + slot - 18.0f;
        ui.labelCentered(cx + 1.0f, cy + 1.0f, n, 0.5f, kCharcoal); // shadow for legibility
        ui.labelCentered(cx, cy, n, 0.5f, kCream);
    }
}
} // namespace

void App::buildHotbar(Ui& ui, float w, float h) {
    const BlockRegistry& reg = world_.registry();
    const Inventory& inv = player_.inventory();
    const int count = Inventory::kHotbarSlots;
    const float slot = 60.0f, gap = 8.0f, radius = 20.0f;
    const float total = count * slot + (count - 1) * gap;
    const float x0 = (w - total) * 0.5f;
    const float y  = h - slot - 16.0f;

    // Name of the held item, centred above the bar.
    const ItemStack& sel = inv.selectedStack();
    if (!sel.empty() && sel.blockId < reg.blockCount()) {
        ui.labelCentered(w * 0.5f, y - 24.0f, reg.get(sel.blockId).name, 0.5f, kUiText);
    }

    for (int i = 0; i < count; ++i) {
        const float x = x0 + i * (slot + gap);
        drawSlot(ui, reg, x, y, slot, radius, inv.slot(i), i == inv.selected());
    }

    // Health bar (survival only — creative is invincible). A small red fill on a
    // charcoal track in the bottom-left corner, above the game-mode tag.
    if (!creativeMode_) {
        const float frac = std::clamp(player_.health() / player_.maxHealth(), 0.0f, 1.0f);
        const float bw = 120.0f, bh = 7.0f, bx = 14.0f, by = h - 44.0f;
        ui.roundRect(bx - 1.0f, by - 1.0f, bw + 2.0f, bh + 2.0f, 3.0f, kCharcoal);
        ui.roundRect(bx, by, bw, bh, 3.0f, glm::vec4(0.16f, 0.05f, 0.05f, 0.9f));
        if (frac > 0.0f) {
            ui.roundRect(bx, by, bw * frac, bh, 3.0f, glm::vec4(0.82f, 0.18f, 0.20f, 1.0f));
        }
    }

    // Game-mode tag at the bottom-left (G toggles it).
    ui.label(14.0f, h - 26.0f, creativeMode_ ? "Creative" : "Survival", 0.5f, kUiText);
}

void App::buildInventory(Ui& ui, float w, float h, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    Inventory& inv = player_.inventory();
    ui.panel(0.0f, 0.0f, w, h, kUiDim); // dim the world behind the screen

    // Slots match the HUD hotbar exactly (60/8/20) so the screen reads compact; the
    // backpack now holds twice as many rows (Inventory::kStorageRows) to compensate.
    const float slot = 60.0f, gap = 8.0f, radius = 20.0f;
    const int   cols = Inventory::kStorageCols;
    const int   rows = Inventory::kStorageRows;
    const float gridW = cols * slot + (cols - 1) * gap;
    const float pad = 24.0f, titleH = 34.0f, hotGap = 16.0f;
    const float panelW = gridW + 2.0f * pad;
    const float panelH = titleH + rows * slot + (rows - 1) * gap + hotGap + slot + 2.0f * pad;
    const float px = std::round((w - panelW) * 0.5f);
    const float py = std::round((h - panelH) * 0.5f);
    ui.frame(px, py, panelW, panelH, kPanelFill, kCream, kFrameThick, kFrameRadius);
    ui.label(px + pad, py + pad - 4.0f, "Inventory", 0.6f, kUiText);

    const float gridX = px + pad;
    const float gridY = py + pad + titleH;

    // Draw one slot and, if clicked this frame, move items between it and the
    // mouse-held cursor stack (classic pick-up / drop / merge / swap).
    auto cell = [&](int index, float sx, float sy) {
        const bool hovered = ui.hovered(sx, sy, slot, slot);
        const bool sel = index < Inventory::kHotbarSlots && index == inv.selected();
        drawSlot(ui, reg, sx, sy, slot, radius, inv.slot(index), sel || hovered);
        if (hovered && in.pointerPressed) {
            clickSlot(inv.slot(index));
        }
    };

    // Backpack grid (slots 9..35).
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            cell(Inventory::kHotbarSlots + r * cols + c,
                 gridX + c * (slot + gap), gridY + r * (slot + gap));
        }
    }
    // Hotbar row (slots 0..8) below the grid.
    const float hotY = gridY + rows * (slot + gap) + hotGap;
    for (int c = 0; c < Inventory::kHotbarSlots; ++c) {
        cell(c, gridX + c * (slot + gap), hotY);
    }

    // Equipment column to the left, crafting list to the right of the inventory.
    buildEquipment(ui, px - 18.0f, py, in); // right-anchored beside the inventory
    buildCrafting(ui, px + panelW + 18.0f, py, 270.0f, in);

    // The cursor-held stack follows the mouse (icon only, no frame).
    if (!cursorStack_.empty()) {
        ui.itemIcon(in.cursor.x, in.cursor.y, 22.0f, reg.iconLayer(cursorStack_.blockId));
        if (cursorStack_.count > 1) {
            ui.labelCentered(in.cursor.x + 16.0f, in.cursor.y + 12.0f,
                             std::to_string(cursorStack_.count), 0.5f, kCream);
        }
    }
}

void App::buildShapePicker(Ui& ui, float w, float h, const InputState& in) {
    ui.panel(0.0f, 0.0f, w, h, kUiDim); // dim the world behind the radial
    const int   n = kPickerShapeCount;
    const float cell = 96.0f, gap = 12.0f, pad = 22.0f, titleH = 34.0f, hintH = 22.0f;
    const float rowW = n * cell + (n - 1) * gap;
    const float panelW = rowW + 2.0f * pad;
    const float panelH = titleH + cell + hintH + 2.0f * pad;
    const float px = std::round((w - panelW) * 0.5f);
    const float py = std::round((h - panelH) * 0.5f);

    // The mouse slides the selector (the cursor stays locked, so we use the raw
    // horizontal delta). ~110 px of movement steps one cell. Clamp to the row.
    pickerSelPos_ += in.look.x / 110.0f;
    pickerSelPos_ = std::clamp(pickerSelPos_, 0.0f, static_cast<float>(n) - 0.001f);
    const int selIdx = static_cast<int>(pickerSelPos_);
    shapePickerSel_ = kPickerShapes[selIdx];

    ui.box(UiBox::Bg2, px, py, panelW, panelH);
    ui.label(px + pad, py + pad - 4.0f, "Build Shape", 0.6f, kUiText);

    const float rowX = px + pad, rowY = py + pad + titleH;
    for (int i = 0; i < n; ++i) {
        const float cx = rowX + i * (cell + gap);
        const bool on = (i == selIdx);
        ui.roundRect(cx, rowY, cell, cell, 10.0f, on ? kCream : kCharcoal);
        ui.labelCentered(cx + cell * 0.5f, rowY + cell * 0.5f - 6.0f,
                         shapeName(kPickerShapes[i]), 0.5f, on ? kCharcoal : kCream);
    }
    ui.labelCentered(px + panelW * 0.5f, py + panelH - pad - 4.0f,
                     "move the mouse to choose  -  release to set", 0.4f, kUiText);
}

void App::buildCrafting(Ui& ui, float x, float y, float w, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    Inventory& inv = player_.inventory();
    const std::vector<Crafting::Recipe>& recipes = crafting_.recipes();
    const int total = static_cast<int>(recipes.size());

    const float S = 1.5f; // scale rows/icons up to match the bigger inventory
    const float pad = 16.0f * S, rowH = 46.0f * S, titleH = 28.0f * S;
    const int   maxVisible = 8;
    const int   visible = total > 0 ? std::min(total, maxVisible) : 1;
    const float panelH = titleH + 2.0f * pad + visible * rowH;
    ui.box(UiBox::Bg2, x, y, w, panelH);
    ui.label(x + pad, y + pad - 4.0f, "Crafting", 0.6f, kUiText);

    if (total == 0) {
        ui.label(x + pad, y + pad + titleH + 4.0f, "(no recipes)", 0.42f, kUiText);
        return;
    }

    // Scroll with the mouse wheel (free while the inventory screen owns the cursor;
    // wheel up scrolls up). Clamp to the list bounds.
    const int maxScroll = std::max(0, total - maxVisible);
    craftScroll_ = std::clamp(craftScroll_ - in.hotbarScroll, 0, maxScroll);

    const float rx = x + pad, rw = w - 2.0f * pad, iconR = 15.0f * S;
    float ry = y + pad + titleH;
    int hoverIdx = -1;
    for (int row = 0; row < maxVisible; ++row) {
        const int idx = craftScroll_ + row;
        if (idx >= total) break;
        const Crafting::Recipe& r = recipes[static_cast<size_t>(idx)];
        const bool craftable = Crafting::canCraft(r, inv);
        const bool hov = ui.hovered(rx, ry, rw, rowH - 8.0f);

        ui.box(UiBox::Bg3, rx, ry, rw, rowH - 8.0f);
        if (craftable && hov) ui.box(UiBox::Border, rx, ry, rw, rowH - 8.0f, kLilac);
        ui.itemIcon(rx + iconR + 6.0f, ry + (rowH - 8.0f) * 0.5f, iconR, reg.iconLayer(r.output));
        std::string label = r.name;
        if (r.outCount > 1) label += " x" + std::to_string(r.outCount);
        ui.label(rx + 2.0f * iconR + 14.0f, ry + 6.0f, label, 0.46f, kCream);

        if (!craftable) {
            // Darken the whole row (icon + text) so un-craftable recipes read as
            // disabled but still visible.
            ui.roundRect(rx, ry, rw, rowH - 8.0f, 8.0f, kUiDim);
        } else if (hov && in.pointerPressed) {
            Crafting::craft(r, inv);
        }
        if (hov) hoverIdx = idx;
        ry += rowH;
    }
    // Scroll affordance: a hint when the list overflows the visible window.
    if (maxScroll > 0) {
        ui.labelCentered(x + w * 0.5f, y + panelH - pad + 1.0f, "scroll for more",
                         0.34f, kUiText);
    }

    // Hover tooltip: what the recipe consumes (and whether you have enough).
    if (hoverIdx >= 0) {
        const Crafting::Recipe& r = recipes[static_cast<size_t>(hoverIdx)];
        std::string need = "Needs: ";
        for (size_t k = 0; k < r.inputs.size(); ++k) {
            const uint16_t iid = r.inputs[k].first;
            const int want = r.inputs[k].second, have = inv.count(iid);
            need += std::to_string(want) + "x " + reg.get(iid).name +
                    " (" + std::to_string(have) + ")";
            if (k + 1 < r.inputs.size()) need += ",  ";
        }
        const float tw = std::max(120.0f, 8.0f * static_cast<float>(need.size()) + 24.0f);
        const float tx = std::min(in.cursor.x + 14.0f, x + w - tw);
        const float ty = in.cursor.y + 14.0f;
        ui.frame(tx, ty, tw, 30.0f, kPanelFill, kCream, kFrameThin, 8.0f);
        ui.label(tx + 10.0f, ty + 7.0f, need, 0.4f, kUiText);
    }
}

void App::buildEquipment(Ui& ui, float rightX, float y, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    // Match the inventory grid's slots exactly (60/8/20) so gear reads the same size.
    const float slot = 60.0f, gap = 8.0f, radius = 20.0f;
    const float pad = 24.0f, titleH = 34.0f, secGap = 20.0f, subH = 18.0f;
    // Boots (the lone armour slot) above the trinkets laid out in a compact 2-wide
    // grid (ISSUES #15 — trimmed armour + tighter trinket space).
    const int   cols = 2;
    const int   trinkRows = (Equipment::kTrinketSlots + cols - 1) / cols;
    const float gridW = cols * slot + (cols - 1) * gap;
    const float panelW = gridW + 2.0f * pad;
    const float panelH = titleH + slot + secGap + subH +
                         trinkRows * slot + (trinkRows - 1) * gap + 2.0f * pad;
    const float x = rightX - panelW; // right-anchored beside the inventory
    ui.box(UiBox::Bg2, x, y, panelW, panelH);
    ui.label(x + pad - 2.0f, y + pad - 4.0f, "Gear", 0.55f, kUiText);

    auto eqCell = [&](int idx, float cx, float cy) {
        const ItemStack& s = equipment_.slots[static_cast<size_t>(idx)];
        const bool hov = ui.hovered(cx, cy, slot, slot);
        drawSlot(ui, reg, cx, cy, slot, radius, s, hov);
        if (hov && in.pointerPressed) clickEquipSlot(idx);
    };
    const float sx = x + pad;
    float sy = y + pad + titleH;
    eqCell(0, sx + (gridW - slot) * 0.5f, sy); // boots, centred over the grid
    sy += slot + secGap;
    ui.label(sx, sy - subH + 2.0f, "Trinkets", 0.42f, kUiText);
    for (int i = 0; i < Equipment::kTrinketSlots; ++i) {
        const int r = i / cols, c = i % cols;
        eqCell(Equipment::kArmorSlots + i, sx + c * (slot + gap), sy + r * (slot + gap));
    }
}

void App::clickSlot(ItemStack& s) {
    if (cursorStack_.empty()) {
        cursorStack_ = s;
        s.clear();
    } else if (s.empty()) {
        s = cursorStack_;
        cursorStack_.clear();
    } else if (s.blockId == cursorStack_.blockId) {
        const int space = Inventory::kMaxStack - s.count;
        const int put   = std::min(space, static_cast<int>(cursorStack_.count));
        s.count = static_cast<uint16_t>(s.count + put);
        cursorStack_.count = static_cast<uint16_t>(cursorStack_.count - put);
        if (cursorStack_.count == 0) cursorStack_.clear();
    } else {
        std::swap(s, cursorStack_);
    }
}

void App::openChestAt(const glm::ivec3& pos) {
    openChest_ = pos;
    chests_.at(pos); // ensure an entry exists
    chestOpen_ = true;
    window_.setCursorDisabled(false); // free the cursor to click slots
    input_.resetMouseDelta();
}

void App::toggleChest() {
    chestOpen_ = false;
    window_.setCursorDisabled(true);
    input_.resetMouseDelta();
    if (!cursorStack_.empty()) {
        // Don't lose a held stack on close: tuck it back into the player inventory.
        player_.inventory().add(cursorStack_.blockId, cursorStack_.count);
        cursorStack_.clear();
    }
}

void App::buildChest(Ui& ui, float w, float h, const InputState& in) {
    const BlockRegistry& reg = world_.registry();
    Inventory& inv = player_.inventory();
    ChestStore::Chest& chest = chests_.at(openChest_);
    ui.panel(0.0f, 0.0f, w, h, kUiDim);

    const float slot = 54.0f, gap = 8.0f, radius = 16.0f;
    const int   cols = Inventory::kStorageCols;        // 9
    const int   chestRows = ChestStore::kSlots / cols; // 3
    const int   invRows = Inventory::kStorageRows;     // 3
    const float gridW = cols * slot + (cols - 1) * gap;
    const float pad = 22.0f, titleH = 30.0f, secGap = 22.0f, hotGap = 18.0f;
    const float panelW = gridW + 2.0f * pad;
    const float panelH = titleH + chestRows * slot + (chestRows - 1) * gap + secGap +
                         invRows * slot + (invRows - 1) * gap + hotGap + slot + 2.0f * pad;
    const float px = std::round((w - panelW) * 0.5f);
    const float py = std::round((h - panelH) * 0.5f);
    ui.frame(px, py, panelW, panelH, kPanelFill, kCream, kFrameThick, kFrameRadius);
    ui.label(px + pad, py + pad - 4.0f, "Chest", 0.6f, kUiText);

    const float gx = px + pad;
    float gy = py + pad + titleH;
    auto cell = [&](ItemStack& s, bool selected, float sx, float sy) {
        const bool hov = ui.hovered(sx, sy, slot, slot);
        drawSlot(ui, reg, sx, sy, slot, radius, s, selected || hov);
        if (hov && in.pointerPressed) clickSlot(s);
    };

    // Chest contents (top three rows).
    for (int r = 0; r < chestRows; ++r) {
        for (int c = 0; c < cols; ++c) {
            cell(chest[static_cast<size_t>(r * cols + c)], false,
                 gx + c * (slot + gap), gy + r * (slot + gap));
        }
    }
    gy += chestRows * (slot + gap) + secGap;

    // Player backpack (slots 9..35), then the hotbar row.
    for (int r = 0; r < invRows; ++r) {
        for (int c = 0; c < cols; ++c) {
            cell(inv.slot(Inventory::kHotbarSlots + r * cols + c), false,
                 gx + c * (slot + gap), gy + r * (slot + gap));
        }
    }
    const float hotY = gy + invRows * (slot + gap) + hotGap;
    for (int c = 0; c < Inventory::kHotbarSlots; ++c) {
        cell(inv.slot(c), c == inv.selected(), gx + c * (slot + gap), hotY);
    }

    if (!cursorStack_.empty()) {
        ui.itemIcon(in.cursor.x, in.cursor.y, 22.0f, reg.iconLayer(cursorStack_.blockId));
        if (cursorStack_.count > 1) {
            ui.labelCentered(in.cursor.x + 16.0f, in.cursor.y + 12.0f,
                             std::to_string(cursorStack_.count), 0.5f, kCream);
        }
    }
}

void App::saveChests() const {
    const std::string& dir = world_.savePath();
    if (dir.empty()) return;
    const std::vector<uint8_t> bytes = chests_.serialize();
    std::ofstream f(dir + "/chests.dat", std::ios::binary | std::ios::trunc);
    if (f) f.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
}

void App::loadChests() {
    const std::string& dir = world_.savePath();
    if (dir.empty()) return;
    std::ifstream f(dir + "/chests.dat", std::ios::binary);
    if (!f) return;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    chests_.deserialize(bytes.data(), bytes.size());
}

// F1 info overlay: a column of small stat lines in the top-left. Read-only —
// it samples state assembled elsewhere (player, world, renderer) every frame.
void App::buildDebugOverlay(Ui& ui) {
    const Camera& cam = player_.camera();
    const glm::vec3 p = cam.position; // eye position
    const int bx = static_cast<int>(std::floor(p.x));
    const int by = static_cast<int>(std::floor(p.y));
    const int bz = static_cast<int>(std::floor(p.z));

    // Dominant horizontal axis of the view direction ("which way am I facing").
    const glm::vec3 f = cam.front();
    const char* facing = (std::abs(f.x) > std::abs(f.z)) ? (f.x > 0 ? "+X" : "-X")
                                                         : (f.z > 0 ? "+Z" : "-Z");

    // What the crosshair points at (same cast block editing uses).
    const RaycastHit hit = raycastVoxel(
        cam.position, cam.front(), kReach,
        [this](int x, int y, int z) { return world_.isTargetable(x, y, z); },
        [this](int x, int y, int z, ShapeBox out[]) { return world_.collisionBoxesAt(x, y, z, out); });

    const double elapsed = glfwGetTime();
    const int fps = static_cast<int>(std::lround(1.0f / std::max(smoothedDt_, 1e-5f)));

    char line[128];
    std::vector<std::string> lines;
    std::snprintf(line, sizeof line, "FPS: %d (%.1f ms)", fps, smoothedDt_ * 1000.0f);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "XYZ: %.2f / %.2f / %.2f", p.x, p.y, p.z);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Block: %d %d %d  Chunk: %d %d %d", bx, by, bz,
                  bx >> 4, by >> 4, bz >> 4);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Facing: %s (yaw %.1f, pitch %.1f)", facing,
                  cam.yaw, cam.pitch);
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Mode: %s%s",
                  player_.mode() == PlayerController::Mode::Walking ? "walking" : "free-fly",
                  player_.onGround() ? " (on ground)" : "");
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Light here: sky %d, block %d",
                  world_.skyLightAt(bx, by, bz), world_.blockLightAt(bx, by, bz));
    lines.emplace_back(line);
    std::snprintf(line, sizeof line, "Chunks drawn: %zu  Triangles: %zu",
                  worldRenderer_.drawnChunkCount(), worldRenderer_.triangleCount());
    lines.emplace_back(line);
    if (hit.hit) {
        const Block b = world_.blockAt(hit.block.x, hit.block.y, hit.block.z);
        std::snprintf(line, sizeof line, "Target: %s at %d %d %d",
                      world_.registry().get(b.id).name.c_str(), hit.block.x, hit.block.y,
                      hit.block.z);
    } else {
        std::snprintf(line, sizeof line, "Target: none");
    }
    lines.emplace_back(line);
    // No day/night cycle yet, so show the session clock (a TODO(future) hook).
    std::snprintf(line, sizeof line, "Session: %02d:%02d",
                  static_cast<int>(elapsed) / 60, static_cast<int>(elapsed) % 60);
    lines.emplace_back(line);

    // Backdrop + text. Sized to the longest line so it stays readable over any
    // terrain behind it.
    const float scale = 0.45f, lineH = 20.0f, pad = 8.0f;
    float maxW = 0.0f;
    for (const std::string& s : lines) {
        maxW = std::max(maxW, ui_.textWidth(s, scale));
    }
    ui.frame(8.0f, 8.0f, maxW + pad * 2.0f, lines.size() * lineH + pad * 2.0f,
             kOverlayFill, kCream, kFrameThin, kFrameRadius);
    float ty = 8.0f + pad;
    for (const std::string& s : lines) {
        ui.label(8.0f + pad, ty, s, scale, kUiText);
        ty += lineH;
    }
}

namespace {
// Selectable fonts (files under assets/fonts/ari/), with friendly labels.
struct FontOption { const char* file; const char* label; };
const FontOption kFonts[] = {
    {"ari-w9500.ttf", "Regular"},
    {"ari-w9500-bold.ttf", "Bold"},
    {"ari-w9500-condensed.ttf", "Condensed"},
    {"ari-w9500-condensed-bold.ttf", "Cond. Bold"},
    {"ari-w9500-display.ttf", "Display"},
    {"ari-w9500-condensed-display.ttf", "Cond. Display"},
};
std::string fontLabel(const std::string& file) {
    for (const FontOption& f : kFonts) {
        if (file == f.file) return f.label;
    }
    return file;
}
} // namespace

void App::cycleFont(int dir) {
    const int n = static_cast<int>(std::size(kFonts));
    int idx = 0;
    for (int i = 0; i < n; ++i) {
        if (settings_.font == kFonts[i].file) { idx = i; break; }
    }
    idx = ((idx + dir) % n + n) % n;
    settings_.font = kFonts[idx].file;
    ui_.setFont(std::string(VG_ASSET_DIR) + "/fonts/ari/" + settings_.font);
}

void App::loadPreviewThumb() {
    if (!previewThumb_.empty() || previewW_ < 0) return; // already loaded or failed before
    constexpr int TW = 32, TH = 18; // thumbnail resolution (16:9-ish mosaic)
    const std::string path = std::string(VG_ASSET_DIR) + "/palette_preview.png";
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 3);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        previewW_ = -1; // sentinel: don't retry; picker falls back to swatch strips
        return;
    }
    previewThumb_.assign(static_cast<size_t>(TW * TH), glm::vec3(0.0f));
    for (int ty = 0; ty < TH; ++ty) { // box-average each source region into one cell
        const int y0 = ty * h / TH, y1 = std::max(y0 + 1, (ty + 1) * h / TH);
        for (int tx = 0; tx < TW; ++tx) {
            const int x0 = tx * w / TW, x1 = std::max(x0 + 1, (tx + 1) * w / TW);
            glm::vec3 sum(0.0f);
            int cnt = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const unsigned char* p = px + (static_cast<size_t>(y) * w + x) * 3;
                    sum += glm::vec3(p[0], p[1], p[2]);
                    ++cnt;
                }
            }
            previewThumb_[ty * TW + tx] =
                (cnt > 0 ? sum / static_cast<float>(cnt) : sum) / 255.0f;
        }
    }
    stbi_image_free(px);
    previewW_ = TW;
    previewH_ = TH;
}

void App::refreshPaletteCache() {
    loadPreviewThumb();
    paletteList_.clear();
    palettePreview_.clear();
    paletteList_.emplace_back(std::string{}, std::vector<glm::vec3>{}); // index 0 = Off
    const std::string dir = std::string(VG_ASSET_DIR) + "/colorpalettes";
    for (const std::string& name : listColorPalettes(dir)) {
        paletteList_.emplace_back(name, loadColorPalette(dir + "/" + name + ".hex"));
    }
    // Remap the reference thumbnail through each palette once, so the picker just
    // blits cached pixels each frame (no per-frame nearest-colour search).
    if (!previewThumb_.empty()) {
        auto nearest = [](const glm::vec3& c, const std::vector<glm::vec3>& pal) {
            glm::vec3 best = c;
            float bd = 1e30f;
            for (const glm::vec3& p : pal) {
                const glm::vec3 d = c - p;
                const float dd = glm::dot(d, d);
                if (dd < bd) { bd = dd; best = p; }
            }
            return best;
        };
        palettePreview_.resize(paletteList_.size());
        for (size_t i = 0; i < paletteList_.size(); ++i) {
            const std::vector<glm::vec3>& cols = paletteList_[i].second;
            if (cols.empty()) { palettePreview_[i] = previewThumb_; continue; } // Off
            std::vector<glm::vec3> out(previewThumb_.size());
            for (size_t p = 0; p < previewThumb_.size(); ++p) {
                out[p] = nearest(previewThumb_[p], cols);
            }
            palettePreview_[i] = std::move(out);
        }
    }
}

// Modal palette picker: a grid of cells, each a small game render remapped through
// that palette (so you SEE the look) with the palette name beneath. Drawn on top
// of the Esc menu; click a cell to apply, or Close / click outside to dismiss.
void App::buildPalettePicker(Ui& ui, float w, float h, const InputState& in) {
    if (paletteList_.empty()) refreshPaletteCache();
    ui.panel(0.0f, 0.0f, w, h, kUiDim); // dim everything (incl. the menu) behind it

    const int  n         = static_cast<int>(paletteList_.size());
    const bool haveThumb = previewW_ > 0 && previewH_ > 0 && !previewThumb_.empty();

    const float nameH = 16.0f, capPad = 5.0f, cellGap = 12.0f;
    const float pad = 22.0f, titleH = 34.0f, closeH = 34.0f, closeGap = 14.0f;
    const float chromeTop = pad + titleH;
    const float chromeBot = closeGap + closeH + pad;
    const float aspect = haveThumb ? static_cast<float>(previewW_) /
                                         static_cast<float>(previewH_)
                                   : 16.0f / 9.0f; // preview image width / height

    // Reserve a centred area and pick the column count that yields the LARGEST
    // preview cell while still fitting both the reserved width and height — so the
    // grid adapts to any palette count and any window size (Close always visible).
    const float gridAreaW = w * 0.90f - 2.0f * pad;
    const float gridAreaH = h * 0.92f - chromeTop - chromeBot;
    int   cols = 1;
    float imgW = 0.0f;
    for (int cc = 1; cc <= n; ++cc) {
        const int   rr = (n + cc - 1) / cc;
        float cw = (gridAreaW - (cc - 1) * cellGap) / static_cast<float>(cc);
        // If that many rows is too tall, shrink the cell to satisfy the height.
        const float perCellH = (gridAreaH - (rr - 1) * cellGap) / static_cast<float>(rr);
        const float byHeight = (perCellH - capPad - nameH) * aspect;
        cw = std::min(cw, byHeight);
        if (cw > imgW) { imgW = cw; cols = cc; }
    }
    imgW = std::clamp(imgW, 32.0f, 200.0f); // don't blow up a 2-3 palette grid
    const float imgH  = imgW / aspect;
    const float cellW = imgW;
    const float cellH = imgH + capPad + nameH;
    const int   rows  = (n + cols - 1) / cols;

    const float gridW  = cols * cellW + (cols - 1) * cellGap;
    const float gridH  = rows * cellH + (rows - 1) * cellGap;
    const float panelW = gridW + 2.0f * pad;
    const float panelH = chromeTop + gridH + chromeBot;
    const float px = std::round((w - panelW) * 0.5f);
    const float py = std::round((h - panelH) * 0.5f);

    // Click outside the panel closes the popup (without changing the selection).
    if (in.pointerPressed && !ui.hovered(px, py, panelW, panelH)) {
        palettePickerOpen_ = false;
        return;
    }

    ui.frame(px, py, panelW, panelH, kPanelFill, kCream, kFrameThick, kFrameRadius);
    ui.labelCentered(px + panelW * 0.5f, py + 14.0f, "Colour Palette", 0.7f, kLilac);

    const float gx = px + pad, gy = py + chromeTop;
    for (int i = 0; i < n; ++i) {
        const int   c  = i % cols, r = i / cols;
        const float cx = gx + c * (cellW + cellGap);
        const float cy = gy + r * (cellH + cellGap);
        const std::string& name = paletteList_[i].first;
        const bool selected = (name == settings_.retroPalette);
        const bool hov      = ui.hovered(cx, cy, cellW, cellH);

        // Preview render: the reference frame remapped through this palette, drawn
        // as a mosaic of the cached thumbnail. Falls back to a swatch strip if the
        // reference image is missing.
        if (haveThumb && i < static_cast<int>(palettePreview_.size()) &&
            !palettePreview_[i].empty()) {
            const std::vector<glm::vec3>& thumb = palettePreview_[i];
            const float pw = imgW / static_cast<float>(previewW_);
            const float ph = imgH / static_cast<float>(previewH_);
            for (int ty = 0; ty < previewH_; ++ty) {
                for (int tx = 0; tx < previewW_; ++tx) {
                    const glm::vec3& cc = thumb[ty * previewW_ + tx];
                    ui.panel(cx + tx * pw, cy + ty * ph,
                             std::ceil(pw) + 0.5f, std::ceil(ph) + 0.5f,
                             glm::vec4(cc, 1.0f));
                }
            }
        } else {
            ui.roundRect(cx, cy, imgW, imgH, 6.0f, kCharcoal);
            const std::vector<glm::vec3>& cols2 = paletteList_[i].second;
            if (!cols2.empty()) {
                const float sw = imgW / static_cast<float>(cols2.size());
                for (size_t s = 0; s < cols2.size(); ++s) {
                    ui.panel(cx + static_cast<float>(s) * sw, cy,
                             std::ceil(sw) + 0.5f, imgH, glm::vec4(cols2[s], 1.0f));
                }
            }
        }

        // Selection (lilac) / hover (cream) ring around the image.
        if (selected || hov) {
            ui.roundRectOutline(cx - 3.0f, cy - 3.0f, imgW + 6.0f, imgH + 6.0f, 6.0f,
                                3.0f, selected ? kLilac : kCream);
        }

        std::string label = name.empty() ? "Off" : name;
        if (label.size() > 16) label = label.substr(0, 14) + ".."; // keep on one line
        ui.labelCentered(cx + imgW * 0.5f, cy + imgH + capPad, label, 0.4f,
                         selected ? kLilac : kUiText);

        if (hov && in.pointerPressed) {
            settings_.retroPalette = name;
            applyRetroPalette();
            palettePickerOpen_ = false;
        }
    }

    if (ui.button(px + pad, py + panelH - pad - closeH, panelW - 2.0f * pad, closeH,
                  "Close")) {
        palettePickerOpen_ = false;
    }
}

// Escape menu, tabbed so each screen shows only a handful of rows (Display /
// Effects / Game / World). Resume + Exit stay pinned at the bottom on every tab.
void App::buildMenu(Ui& ui, float px, float py, float pw, float ph) {
    // Charcoal panel with a thick cream pixel-art border; lilac title accent.
    ui.frame(px, py, pw, ph, kPanelFill, kCream, kFrameThick, 12.0f);
    ui.labelCentered(px + pw * 0.5f, py + 14.0f, "Menu", 0.85f, kLilac);

    const float lx = px + 24.0f, cw = pw - 48.0f;
    float y = py + 40.0f;

    // Tab row: Display / FX / Game / World / Retro (short labels so 5 fit one row).
    const char* tabs[5] = {"View", "FX", "Game", "World", "Retro"};
    const float tabGap = 5.0f, tabW = (cw - 4.0f * tabGap) / 5.0f;
    for (int i = 0; i < 5; ++i) {
        if (ui.button(lx + i * (tabW + tabGap), y, tabW, 30.0f, tabs[i])) {
            menuTab_ = i;
        }
    }
    y += 38.0f;
    ui.labelCentered(px + pw * 0.5f, y, tabs[menuTab_], 0.5f, kLilac);
    y += 24.0f;

    // A labelled slider; returns its (possibly dragged) value. `decimals` < 0 shows
    // as an integer. Rows are compact so a tab's worth of options fits comfortably.
    auto sliderRow = [&](const std::string& name, float value, float lo, float hi,
                         int steps, int decimals) -> float {
        char buf[48];
        if (decimals < 0) {
            std::snprintf(buf, sizeof buf, "%s: %d", name.c_str(),
                          static_cast<int>(std::lround(value)));
        } else {
            std::snprintf(buf, sizeof buf, "%s: %.*f", name.c_str(), decimals, value);
        }
        ui.label(lx, y, buf, 0.46f, kUiText);
        y += 20.0f;
        const float nv = ui.slider(lx, y, cw, 22.0f, value, lo, hi, steps);
        y += 30.0f;
        return nv;
    };
    auto button = [&](const std::string& text) {
        const bool clicked = ui.button(lx, y, cw, 34.0f, text);
        y += 42.0f;
        return clicked;
    };
    auto toggle = [&](const char* name, bool on) {
        const bool clicked = ui.button(lx, y, cw, 34.0f, std::string(name) + (on ? "On" : "Off"));
        y += 42.0f;
        return clicked;
    };

    if (menuTab_ == 0) { // --- Display: resolution / view / cosmetic --------------
        // Pixelate 0..16 (integer).
        const int pv = static_cast<int>(std::lround(
            sliderRow("Pixelate", static_cast<float>(settings_.pixelate), 0.0f, 16.0f, 16, -1)));
        if (pv != settings_.pixelate) {
            settings_.pixelate = pv;
            renderer_.setPixelScale(static_cast<uint32_t>(std::max(1, pv)));
        }
        // Render distance (chunks). The voxel window is allocated once at startup, so
        // a change only persists here and takes effect on the next launch.
        const int rd = static_cast<int>(std::lround(
            sliderRow("Render dist (restart)", static_cast<float>(settings_.renderDistance),
                      4.0f, 16.0f, 12, -1)));
        if (rd != settings_.renderDistance) {
            settings_.renderDistance = rd;
        }
        // Field of view.
        const float fov = sliderRow("FOV", settings_.fov, 50.0f, 110.0f, 60, -1);
        if (std::abs(fov - settings_.fov) > 0.01f) {
            settings_.fov = fov;
            player_.camera().fovDegrees = fov;
        }
        if (toggle("Fullscreen: ", window_.isFullscreen())) {
            window_.setFullscreen(!window_.isFullscreen());
            settings_.fullscreen = window_.isFullscreen();
        }
        // LOD terrain: the distant low-poly shell + tree impostors past the loaded
        // window (FarTerrainRenderer). Off = the world fades to haze at the edge.
        if (toggle("LOD terrain: ", settings_.lod)) {
            settings_.lod = !settings_.lod;
            farTerrain_.setEnabled(settings_.lod);
        }
        // Font (cycles the ari family).
        if (button("Font: " + fontLabel(settings_.font))) {
            cycleFont(+1);
        }
    } else if (menuTab_ == 1) { // --- Effects: post-processing -------------------
        // Bloom: glow off the frame's bright areas. Toggle, then its sliders.
        if (toggle("Bloom: ", settings_.bloom)) {
            settings_.bloom = !settings_.bloom;
        }
        if (settings_.bloom) {
            const float bi = sliderRow("Bloom strength", settings_.bloomIntensity, 0.0f, 2.0f, 0, 2);
            if (std::abs(bi - settings_.bloomIntensity) > 1e-4f) settings_.bloomIntensity = bi;
            const float bt = sliderRow("Bloom threshold", settings_.bloomThreshold, 0.0f, 1.0f, 0, 2);
            if (std::abs(bt - settings_.bloomThreshold) > 1e-4f) settings_.bloomThreshold = bt;
            const float br = sliderRow("Bloom spread", settings_.bloomRadius, 1.0f, 8.0f, 0, 1);
            if (std::abs(br - settings_.bloomRadius) > 1e-4f) settings_.bloomRadius = br;
        }
        // God rays: sun shafts through terrain/trees/clouds.
        if (toggle("God rays: ", settings_.godrays)) {
            settings_.godrays = !settings_.godrays;
        }
        if (settings_.godrays) {
            const float gs = sliderRow("Ray strength", settings_.godrayStrength, 0.0f, 1.0f, 0, 2);
            if (std::abs(gs - settings_.godrayStrength) > 1e-4f) settings_.godrayStrength = gs;
            const float gl = sliderRow("Ray reach", settings_.godrayLength, 0.3f, 1.5f, 0, 2);
            if (std::abs(gl - settings_.godrayLength) > 1e-4f) settings_.godrayLength = gl;
            const float gd = sliderRow("Ray decay", settings_.godrayDecay, 0.85f, 0.99f, 0, 3);
            if (std::abs(gd - settings_.godrayDecay) > 1e-4f) settings_.godrayDecay = gd;
        }
        // Dark-area sensor grain (0 = off .. 0.5 = strong static in near-black).
        const float dn = sliderRow("Dark noise", settings_.darkNoise, 0.0f, 0.5f, 0, 2);
        if (std::abs(dn - settings_.darkNoise) > 1e-4f) {
            settings_.darkNoise = dn;
        }
    } else if (menuTab_ == 2) { // --- Game: mode + controls --------------------
        // Creative / Survival (custom label, so not the On/Off toggle helper).
        if (button("Mode: " + std::string(creativeMode_ ? "Creative" : "Survival"))) {
            toggleGameMode();
        }
        // Mouse sensitivity.
        const float sens = sliderRow("Sensitivity", settings_.sensitivity, 0.02f, 0.30f, 0, 3);
        if (std::abs(sens - settings_.sensitivity) > 1e-4f) {
            settings_.sensitivity = sens;
            player_.setMouseSensitivity(sens);
        }
        // Flight speed.
        const float fly = sliderRow("Flight speed", settings_.flySpeed, 4.0f, 40.0f, 0, 1);
        if (std::abs(fly - settings_.flySpeed) > 0.01f) {
            settings_.flySpeed = fly;
            player_.setFlySpeed(fly);
        }
        // View bob: subtle head-bob while walking (off for a locked camera).
        if (toggle("View bob: ", settings_.viewBob)) {
            settings_.viewBob = !settings_.viewBob;
            player_.setViewBob(settings_.viewBob);
        }
    } else if (menuTab_ == 3) { // --- World: lighting, time, sky -----------------
        // Light falloff (levels lost per block; higher = darker caves / tighter glow).
        // Applying relights the world + rebuilds every chunk, so only act on a new int.
        const int skyF = static_cast<int>(std::lround(
            sliderRow("Cave darkness", static_cast<float>(settings_.skyFalloff), 1.0f, 5.0f, 4, -1)));
        const int blkF = static_cast<int>(std::lround(
            sliderRow("Glow falloff", static_cast<float>(settings_.blockFalloff), 1.0f, 5.0f, 4, -1)));
        if (skyF != settings_.skyFalloff || blkF != settings_.blockFalloff) {
            settings_.skyFalloff   = skyF;
            settings_.blockFalloff = blkF;
            // A full world mutation — drain workers/relight first (REVIEW R1).
            drainBeforeWorldMutation();
            if (world_.setLightFalloff(skyF, blkF)) {
                worldRenderer_.remeshAll();
            }
        }
        // Time of day (live; the sun/moon and lighting follow immediately).
        const float th = sliderRow("Time (h)", dayNight_.hour(), 0.0f, 24.0f, 48, 1);
        if (std::abs(th - dayNight_.hour()) > 0.01f) {
            dayNight_.setHour(th);
        }
        // Day length: real minutes for a full in-game day.
        const float dl = sliderRow("Day length (min)", settings_.dayLengthMinutes, 1.0f, 60.0f, 59, -1);
        if (std::abs(dl - settings_.dayLengthMinutes) > 0.01f) {
            settings_.dayLengthMinutes = dl;
            dayNight_.setDayLengthMinutes(dl);
        }
        if (button(std::string("Time: ") + (settings_.timeRunning ? "Running" : "Paused"))) {
            settings_.timeRunning = !settings_.timeRunning;
            dayNight_.setRunning(settings_.timeRunning);
        }
        // Day-sky colour (cycles the palette; tints the daytime zenith).
        if (button("Sky: " + settings_.skyColor)) {
            const std::vector<std::string>& names = palette_.names();
            if (!names.empty()) {
                int idx = 0;
                for (size_t i = 0; i < names.size(); ++i) {
                    if (names[i] == settings_.skyColor) { idx = static_cast<int>(i); break; }
                }
                settings_.skyColor = names[(idx + 1) % names.size()];
                dayNight_.setDayZenithOverride(palette_.linear(settings_.skyColor));
            }
        }
    } else { // --- Retro: independent PS1/PS2 FX (mix freely) -------------------
        // Every effect is its own control — slide one up to taste, or stack them.
        // PS1 ~= wobble + affine + bits 5; PS2 ~= soft + interlace + bits 6.
        const float j = sliderRow("Vertex wobble", settings_.retroJitter, 0.0f, 1.0f, 0, 2);
        if (std::abs(j - settings_.retroJitter) > 1e-4f) settings_.retroJitter = j;
        if (toggle("Affine warp: ", settings_.retroAffine)) {
            settings_.retroAffine = !settings_.retroAffine;
        }
        // Colour bits: 8 = off (full colour), lower = harsher quantisation (5 = PS1).
        const int cb = static_cast<int>(std::lround(
            sliderRow("Colour bits", static_cast<float>(settings_.retroColorBits), 3.0f, 8.0f, 5, -1)));
        if (cb != settings_.retroColorBits) settings_.retroColorBits = cb;
        const float dt = sliderRow("Dither", settings_.retroDither, 0.0f, 1.0f, 0, 2);
        if (std::abs(dt - settings_.retroDither) > 1e-4f) settings_.retroDither = dt;
        const float il = sliderRow("Interlace", settings_.retroInterlace, 0.0f, 1.0f, 0, 2);
        if (std::abs(il - settings_.retroInterlace) > 1e-4f) settings_.retroInterlace = il;
        const float sf = sliderRow("Soft blur", settings_.retroSoft, 0.0f, 1.0f, 0, 2);
        if (std::abs(sf - settings_.retroSoft) > 1e-4f) settings_.retroSoft = sf;
        // Selectable colour palette: remaps the whole frame to the nearest swatch
        // (overrides "Colour bits"). Opens a popup that previews every
        // assets/colorpalettes/*.hex as a swatch strip so you can see each look.
        if (button("Palette: " + (settings_.retroPalette.empty() ? std::string("Off")
                                                                 : settings_.retroPalette))) {
            refreshPaletteCache();
            palettePickerOpen_ = true;
        }
    }

    // Resume / Exit pinned to the bottom of the panel, side by side, on every tab.
    const float bw = (cw - 8.0f) * 0.5f;
    const float by = py + ph - 34.0f - 16.0f;
    if (ui.button(lx, by, bw, 34.0f, "Resume")) {
        togglePause();
    }
    if (ui.button(lx + bw + 8.0f, by, bw, 34.0f, "Exit")) {
        window_.requestClose();
    }
}

void App::buildTuning(Ui& ui, float px, float py, float pw, float ph) {
    ui.frame(px, py, pw, ph, kPanelFill, kCream, kFrameThick, 12.0f);
    ui.labelCentered(px + pw * 0.5f, py + 14.0f, "Tune Sky", 0.85f, kLilac);

    const float lx = px + 24.0f, cw = pw - 48.0f;
    float y = py + 44.0f;

    // Tab row: Weather / Clouds / Fog / Sky.
    const char* tabs[4] = {"Weather", "Clouds", "Fog", "Sky"};
    const float tabGap = 6.0f, tabW = (cw - 3.0f * tabGap) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        if (ui.button(lx + i * (tabW + tabGap), y, tabW, 30.0f, tabs[i])) {
            tuningTab_ = i;
        }
    }
    y += 38.0f;
    ui.labelCentered(px + pw * 0.5f, y, tabs[tuningTab_], 0.5f, kLilac);
    y += 26.0f;

    // Shared row helpers (compact, like the menu's).
    auto sliderRow = [&](const std::string& name, float value, float lo, float hi,
                         int steps, int decimals) -> float {
        char buf[56];
        if (decimals < 0) {
            std::snprintf(buf, sizeof buf, "%s: %d", name.c_str(),
                          static_cast<int>(std::lround(value)));
        } else {
            std::snprintf(buf, sizeof buf, "%s: %.*f", name.c_str(), decimals, value);
        }
        ui.label(lx, y, buf, 0.44f, kUiText);
        y += 19.0f;
        const float nv = ui.slider(lx, y, cw, 20.0f, value, lo, hi, steps);
        y += 27.0f;
        return nv;
    };
    auto button = [&](const std::string& text) {
        const bool clicked = ui.button(lx, y, cw, 32.0f, text);
        y += 40.0f;
        return clicked;
    };
    auto changed = [](float a, float b) { return std::abs(a - b) > 1e-4f; };

    if (tuningTab_ == 0) { // --- Weather: force a state + coverage/type --------
        ui.label(lx, y, "Force weather state:", 0.44f, kUiText);
        y += 22.0f;
        const char* st[6] = {"Clear", "Fair", "Broken", "Overcast", "Stormy", "Foggy"};
        const float halfW = (cw - 8.0f) * 0.5f;
        for (int i = 0; i < 6; ++i) {
            const float bx = lx + ((i & 1) ? halfW + 8.0f : 0.0f);
            if (ui.button(bx, y, halfW, 30.0f, st[i])) {
                clouds_.setForcedState(i);
            }
            if (i & 1) y += 36.0f;
        }
        y += 4.0f;
        if (button("Auto weather")) {
            clouds_.setForcedState(-1);
            clouds_.setForceCoverage(-1.0f);
            clouds_.setForceType(-1.0f);
        }
        // Autonomous-scheduler speed knobs (apply in Auto mode).
        const float ci = sliderRow("Change every (s)", clouds_.changeInterval(),
                                   15.0f, 600.0f, 0, -1);
        if (changed(ci, clouds_.changeInterval())) clouds_.setChangeInterval(ci);
        const float fd = sliderRow("Front sweep (s)", clouds_.frontDuration(),
                                   3.0f, 120.0f, 0, -1);
        if (changed(fd, clouds_.frontDuration())) clouds_.setFrontDuration(fd);
        if (button("Change weather now")) clouds_.triggerWeatherChange();
        const float cov = sliderRow("Coverage", clouds_.coverage(), 0.0f, 1.0f, 0, 2);
        if (changed(cov, clouds_.coverage())) clouds_.setForceCoverage(cov);
        const float ty = sliderRow("Cloud type", clouds_.type(), 0.0f, 1.0f, 0, 2);
        if (changed(ty, clouds_.type())) clouds_.setForceType(ty);
        const int fs = clouds_.forcedState();
        ui.label(lx, y, fs < 0 ? "Active: auto" : (std::string("Active: ") + st[fs]),
                 0.42f, kLilac);
    } else if (tuningTab_ == 1) { // --- Clouds: shape + light --------------------
        const float d = sliderRow("Density", clouds_.densityScale(), 0.0f, 2.0f, 0, 2);
        if (changed(d, clouds_.densityScale())) clouds_.setDensityScale(d);
        const float e = sliderRow("Erosion (lacy)", clouds_.erosion(), 0.0f, 1.0f, 0, 2);
        if (changed(e, clouds_.erosion())) clouds_.setErosion(e);
        const float vx = sliderRow("Blockiness", clouds_.voxelize(), 0.0f, 12.0f, 0, 1);
        if (changed(vx, clouds_.voxelize())) clouds_.setVoxelize(vx);
        const float ex = sliderRow("Extinction", clouds_.extinction(), 0.2f, 3.0f, 0, 2);
        if (changed(ex, clouds_.extinction())) clouds_.setExtinction(ex);
        const float ws = sliderRow("Wind speed", clouds_.windSpeed(), 0.0f, 30.0f, 0, 1);
        if (changed(ws, clouds_.windSpeed())) clouds_.setWindSpeed(ws);
    } else if (tuningTab_ == 2) { // --- Fog ------------------------------------
        if (button(std::string("Fog: ") + (fogEnabled_ ? "On" : "Off"))) {
            fogEnabled_ = !fogEnabled_;
        }
        fogDistMul_   = sliderRow("Distance haze", fogDistMul_, 0.0f, 5.0f, 0, 2);
        fogGroundMul_ = sliderRow("Ground fog", fogGroundMul_, 0.0f, 8.0f, 0, 2);
        fogFalloff_   = sliderRow("Height falloff", fogFalloff_, 0.005f, 0.20f, 0, 3);
        fogMax_       = sliderRow("Max fog", fogMax_, 0.10f, 1.0f, 0, 2);
    } else { // --- Sky: moon, stars, ozone + RGB colour editor ----------------
        const float md = std::fmod(static_cast<float>(dayNight_.totalDays()), 29.53f);
        const float nmd = sliderRow("Moon day (phase)", md, 0.0f, 29.0f, 29, -1);
        if (changed(nmd, md)) dayNight_.setDay(static_cast<int>(std::lround(nmd)));
        const float oz = sliderRow("Ozone (twilight)", dayNight_.ozoneStrength(), 0.0f, 3.0f, 0, 2);
        if (changed(oz, dayNight_.ozoneStrength())) dayNight_.setOzoneStrength(oz);
        const float sb = sliderRow("Stars", dayNight_.starBrightness(), 0.0f, 2.0f, 0, 2);
        if (changed(sb, dayNight_.starBrightness())) dayNight_.setStarBrightness(sb);
        const float mw = sliderRow("Milky Way", dayNight_.milkyWay(), 0.0f, 1.5f, 0, 2);
        if (changed(mw, dayNight_.milkyWay())) dayNight_.setMilkyWay(mw);

        // RGB colour editor: a target selector + 3 channel sliders (linear space).
        const char* ct[5] = {"Sunset High", "Sunset Mid", "Sunset Horizon",
                             "Cloud Dusk", "Fog Haze"};
        if (button(std::string("Colour: ") + ct[colorTarget_])) {
            colorTarget_ = (colorTarget_ + 1) % 5;
        }
        glm::vec3 hi = dayNight_.sunsetHigh(), mid = dayNight_.sunsetMid();
        glm::vec3 hor = dayNight_.sunsetHorizon(), dusk = dayNight_.cloudDusk();
        glm::vec3 cur = colorTarget_ == 0 ? hi
                      : colorTarget_ == 1 ? mid
                      : colorTarget_ == 2 ? hor
                      : colorTarget_ == 3 ? dusk
                                          : fogHaze_;
        const float r = sliderRow("R", cur.r, 0.0f, 1.5f, 0, 2);
        const float g = sliderRow("G", cur.g, 0.0f, 1.5f, 0, 2);
        const float b = sliderRow("B", cur.b, 0.0f, 1.5f, 0, 2);
        // Live swatch of the colour being mixed (linear -> approx sRGB so it
        // reads on the swapchain), inside a cream frame.
        const glm::vec3 disp = glm::pow(glm::clamp(glm::vec3(r, g, b), 0.0f, 1.0f),
                                        glm::vec3(1.0f / 2.2f));
        ui.roundRect(lx - 2.0f, y - 2.0f, cw + 4.0f, 24.0f, 5.0f, kCream);
        ui.roundRect(lx + 2.0f, y + 2.0f, cw - 4.0f, 16.0f, 3.0f, glm::vec4(disp, 1.0f));
        y += 28.0f;
        if (changed(r, cur.r) || changed(g, cur.g) || changed(b, cur.b)) {
            const glm::vec3 nc(r, g, b);
            if (colorTarget_ == 4) {
                fogHaze_ = nc;
                fogHazeTuned_ = true;
            } else {
                if (colorTarget_ == 0) hi = nc;
                else if (colorTarget_ == 1) mid = nc;
                else if (colorTarget_ == 2) hor = nc;
                else dusk = nc;
                dayNight_.setTunedSunset(hi, mid, hor, dusk);
            }
        }
        if (button("Auto colours")) {
            dayNight_.clearTunedSunset();
            fogHazeTuned_ = false;
        }
    }
}

} // namespace vg
