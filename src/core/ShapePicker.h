#pragma once

#include "world/Shape.h"

#include <cstddef>

namespace vg {

// -----------------------------------------------------------------------------
//  Shape picker shared helpers
// -----------------------------------------------------------------------------
//  The hammer's radial shape picker. These live in a header (not a .cpp's
//  anonymous namespace) because the picker list + lookups are used by BOTH the
//  input/edit code in App.cpp and the picker UI in AppUi.cpp, which are separate
//  translation units since the App god-object was split (REVIEW R9).
// -----------------------------------------------------------------------------

// The shapes the hammer radial offers, in display order (left to right).
inline constexpr ShapeKind kPickerShapes[] = {
    ShapeKind::Cube, ShapeKind::Slab, ShapeKind::Stairs,
    ShapeKind::Post, ShapeKind::Wall, ShapeKind::VerticalSlab,
};
inline constexpr int kPickerShapeCount =
    static_cast<int>(sizeof(kPickerShapes) / sizeof(kPickerShapes[0]));

// Index of a shape within kPickerShapes (0 if absent) — maps the active build
// shape to its slot on the radial.
inline int shapeIndex(ShapeKind k) {
    for (int i = 0; i < kPickerShapeCount; ++i) {
        if (kPickerShapes[i] == k) return i;
    }
    return 0;
}

// Display name for a block shape (HUD indicator + picker labels).
inline const char* shapeName(ShapeKind k) {
    switch (k) {
        case ShapeKind::Cube:         return "Cube";
        case ShapeKind::Slab:         return "Slab";
        case ShapeKind::Stairs:       return "Stairs";
        case ShapeKind::Post:         return "Post";
        case ShapeKind::Wall:         return "Wall";
        case ShapeKind::VerticalSlab: return "Vertical Slab";
    }
    return "Cube";
}

} // namespace vg
