#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace vg::sdf {

// -----------------------------------------------------------------------------
//  Signed distance fields + CSG operators (Inigo Quilez, "Distance Functions"
//  and "Smooth Minimum"). docs/WORLDGEN.md Layer 1.1: region masks (a zone's 2D
//  footprint silhouette), the monolithic stone arch (Layer 1.3), crater rims and
//  carved lake basins are all authored as SDFs combined with these operators.
//
//  Convention: negative inside the shape, 0 on the surface, positive outside.
//  All functions are pure (no state) → deterministic and streaming/seam-safe, so
//  the same world coords give the same membership on every chunk. The smooth
//  operators produce *approximate* SDFs (Lipschitz < 1), which is fine for region
//  masks and voxel density tests; it only matters if you raymarch.
// -----------------------------------------------------------------------------

// --- 2D primitives (region masks live in world XZ) ---------------------------
[[nodiscard]] inline float circle(glm::vec2 p, float r) { return glm::length(p) - r; }

// Axis-aligned box of half-extents b (the exact exterior + interior distance).
[[nodiscard]] inline float box(glm::vec2 p, glm::vec2 b) {
    const glm::vec2 d = glm::abs(p) - b;
    return glm::length(glm::max(d, glm::vec2(0.0f))) +
           std::min(std::max(d.x, d.y), 0.0f);
}

// Annulus / ring: |distance-to-circle| - thickness. The basis for a crater rim.
[[nodiscard]] inline float ring(glm::vec2 p, float radius, float thickness) {
    return std::fabs(glm::length(p) - radius) - thickness;
}

// --- 3D primitives -----------------------------------------------------------
[[nodiscard]] inline float sphere(glm::vec3 p, float r) { return glm::length(p) - r; }

[[nodiscard]] inline float box(glm::vec3 p, glm::vec3 b) {
    const glm::vec3 d = glm::abs(p) - b;
    return glm::length(glm::max(d, glm::vec3(0.0f))) +
           std::min(std::max(d.x, std::max(d.y, d.z)), 0.0f);
}

// Torus in the XZ plane: major radius t.x (ring centre), minor radius t.y (tube).
[[nodiscard]] inline float torus(glm::vec3 p, glm::vec2 t) {
    const glm::vec2 q(glm::length(glm::vec2(p.x, p.z)) - t.x, p.y);
    return glm::length(q) - t.y;
}

// Capped cylinder, axis along Y, half-height h, radius r.
[[nodiscard]] inline float cylinder(glm::vec3 p, float h, float r) {
    const glm::vec2 d = glm::abs(glm::vec2(glm::length(glm::vec2(p.x, p.z)), p.y)) -
                        glm::vec2(r, h);
    return std::min(std::max(d.x, d.y), 0.0f) +
           glm::length(glm::max(d, glm::vec2(0.0f)));
}

// Capsule / line segment of radius r between a and b — a deterministic tube along
// a spline path (connector ravines/tunnels, Layer 6) is a chain of these.
[[nodiscard]] inline float segment(glm::vec3 p, glm::vec3 a, glm::vec3 b, float r) {
    const glm::vec3 pa = p - a, ba = b - a;
    const float denom = glm::dot(ba, ba);
    const float h = denom > 0.0f ? std::clamp(glm::dot(pa, ba) / denom, 0.0f, 1.0f) : 0.0f;
    return glm::length(pa - ba * h) - r;
}

// --- CSG operators -----------------------------------------------------------
[[nodiscard]] inline float opUnion(float a, float b)        { return std::min(a, b); }
[[nodiscard]] inline float opSubtraction(float a, float b)  { return std::max(-a, b); } // b minus a
[[nodiscard]] inline float opIntersection(float a, float b) { return std::max(a, b); }

// Smooth minimum (polynomial, Quilez): blends two SDFs over width k, rounding the
// seam — the way to merge an arch's legs into its span, or a rim into its host.
[[nodiscard]] inline float smin(float a, float b, float k) {
    if (k <= 0.0f) return std::min(a, b);
    const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return (b + h * (a - b)) - k * h * (1.0f - h); // mix(b,a,h) - k*h*(1-h)
}
// Smooth subtraction / intersection counterparts.
[[nodiscard]] inline float smoothSubtraction(float a, float b, float k) {
    return -smin(a, -b, k);
}
[[nodiscard]] inline float smoothIntersection(float a, float b, float k) {
    return -smin(-a, -b, k);
}

} // namespace vg::sdf
