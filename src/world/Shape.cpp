#include "world/Shape.h"

namespace vg {

namespace {

constexpr float kHalf = 0.5f;
constexpr float kPostLo = 0.3125f; // 5/16: a centred post is 6/16 wide
constexpr float kPostHi = 0.6875f;
constexpr float kWallLo = 0.3125f; // wall centre/arm half-width
constexpr float kWallHi = 0.6875f;

// One horizontal-side box covering half the cell on side `s` (0 -Z, 1 +X, 2 +Z,
// 3 -X), full height — used by VerticalSlab.
ShapeBox sideHalf(int s) {
    switch (s & 3) {
        case 0:  return {{0, 0, 0},        {1, 1, kHalf}};      // -Z
        case 1:  return {{kHalf, 0, 0},    {1, 1, 1}};          // +X
        case 2:  return {{0, 0, kHalf},    {1, 1, 1}};          // +Z
        default: return {{0, 0, 0},        {kHalf, 1, 1}};      // -X
    }
}

// The upper half-step box on horizontal side `s`, between y `lo`..`hi` — used by
// Stairs (the part that sits above/below the slab on the high side).
ShapeBox stepBox(int s, float ylo, float yhi) {
    switch (s & 3) {
        case 0:  return {{0, ylo, 0},     {1, yhi, kHalf}};     // -Z
        case 1:  return {{kHalf, ylo, 0}, {1, yhi, 1}};         // +X
        case 2:  return {{0, ylo, kHalf}, {1, yhi, 1}};         // +Z
        default: return {{0, ylo, 0},     {kHalf, yhi, 1}};     // -X
    }
}

} // namespace

int shapeOrientCount(ShapeKind kind) {
    switch (kind) {
        case ShapeKind::Cube:         return 1;
        case ShapeKind::Slab:         return 2; // bottom / top
        case ShapeKind::Stairs:       return 8; // 4 facings x 2 halves
        case ShapeKind::Post:         return 3; // axis X / Y / Z
        case ShapeKind::Wall:         return 1; // connections derived from neighbours
        case ShapeKind::VerticalSlab: return 4; // four horizontal sides
    }
    return 1;
}

int shapeBoxes(ShapeKind kind, uint8_t orient, uint8_t wallMask, ShapeBox out[]) {
    int n = 0;
    switch (kind) {
        case ShapeKind::Cube:
            out[n++] = {{0, 0, 0}, {1, 1, 1}};
            break;

        case ShapeKind::Slab:
            if (orient & 1) out[n++] = {{0, kHalf, 0}, {1, 1, 1}};      // top
            else            out[n++] = {{0, 0, 0},     {1, kHalf, 1}};  // bottom
            break;

        case ShapeKind::VerticalSlab:
            out[n++] = sideHalf(orient & 3);
            break;

        case ShapeKind::Post: {
            const int axis = orient % 3; // 0 X, 1 Y, 2 Z
            if (axis == 0)      out[n++] = {{0, kPostLo, kPostLo}, {1, kPostHi, kPostHi}};
            else if (axis == 2) out[n++] = {{kPostLo, kPostLo, 0}, {kPostHi, kPostHi, 1}};
            else                out[n++] = {{kPostLo, 0, kPostLo}, {kPostHi, 1, kPostHi}};
            break;
        }

        case ShapeKind::Stairs: {
            const int  facing = orient & 3;
            const bool top    = (orient & 4) != 0;
            if (top) {
                out[n++] = {{0, kHalf, 0}, {1, 1, 1}};       // top slab
                out[n++] = stepBox(facing, 0.0f, kHalf);     // lower step on the high side
            } else {
                out[n++] = {{0, 0, 0}, {1, kHalf, 1}};       // bottom slab
                out[n++] = stepBox(facing, kHalf, 1.0f);     // upper step on the high side
            }
            break;
        }

        case ShapeKind::Wall:
            out[n++] = {{kWallLo, 0, kWallLo}, {kWallHi, 1, kWallHi}}; // centre post
            if (wallMask & 0x1) out[n++] = {{kWallLo, 0, 0},      {kWallHi, 1, kWallLo}}; // -Z
            if (wallMask & 0x2) out[n++] = {{kWallHi, 0, kWallLo}, {1, 1, kWallHi}};      // +X
            if (wallMask & 0x4) out[n++] = {{kWallLo, 0, kWallHi}, {kWallHi, 1, 1}};      // +Z
            if (wallMask & 0x8) out[n++] = {{0, 0, kWallLo},      {kWallLo, 1, kWallHi}}; // -X
            break;
    }
    return n;
}

void shapeBoxes(ShapeKind kind, uint8_t orient, uint8_t wallMask,
                std::vector<ShapeBox>& out) {
    ShapeBox buf[kMaxShapeBoxes];
    const int n = shapeBoxes(kind, orient, wallMask, buf);
    out.assign(buf, buf + n);
}

} // namespace vg
