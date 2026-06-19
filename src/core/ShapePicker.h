#pragma once

/**
 * @file ShapePicker.h
 * @brief Hammer radial shape picker constants and lookup helpers (header-only).
 *
 * Lives in a header rather than an anonymous .cpp namespace because both
 * App.cpp (edit/input logic) and AppUi.cpp (picker UI drawing) need the same
 * ordered shape list and name/index lookups after the App god-object was split
 * (REVIEW R9).
 * @see docs/CODE_INDEX.md
 */
#include "world/Shape.h"

#include <cstddef>

namespace vg {

/// Shapes offered by the hammer radial, in left-to-right display order.
inline constexpr ShapeKind kPickerShapes[] = {
    ShapeKind::Cube, ShapeKind::Slab, ShapeKind::Stairs,
    ShapeKind::Post, ShapeKind::Wall, ShapeKind::VerticalSlab,
};

/// Number of entries in kPickerShapes.
inline constexpr int kPickerShapeCount =
    static_cast<int>(sizeof(kPickerShapes) / sizeof(kPickerShapes[0]));

/**
 * @brief Index of a shape within kPickerShapes (returns 0 if absent).
 * @param k The shape kind to find.
 * @return Slot index in the radial (0 = Cube if not found).
 */
inline int shapeIndex(ShapeKind k) {
    for (int i = 0; i < kPickerShapeCount; ++i) {
        if (kPickerShapes[i] == k) return i;
    }
    return 0;
}

/**
 * @brief Human-readable display name for a shape (HUD indicator and picker labels).
 * @param k The shape kind.
 * @return A static string such as "Slab" or "Vertical Slab".
 */
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
