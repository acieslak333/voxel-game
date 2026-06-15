#pragma once

#include "world/Noise.h"
#include "world/Sdf.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace vg::landform {

// -----------------------------------------------------------------------------
//  Signature landforms (docs/WORLDGEN.md Layer 1.3 — the Stage-1 validation gate)
// -----------------------------------------------------------------------------
//  The two hardest, most recognisable shapes, composed from the Layer-0/1 toolkit
//  (Worley cellular noise + the SDF/CSG primitives). Each returns a Surface-Nets-
//  ready signed field: NEGATIVE inside the solid, >= 0 in air, so it drops straight
//  into vg::surfaceNets() or a density solidity test. All are pure functions of
//  their inputs → deterministic and seam-safe.
//
//  If these read as "authored" when meshed, the whole noise+SDF approach is
//  validated (the doc's explicit Stage-1 checkpoint).
// -----------------------------------------------------------------------------

// Monolithic stone arch bridging two feet over a gap: two vertical legs joined by
// an upper arc, standing in the world X-Y plane (thin in Z). `origin` is the world
// position of the midpoint between the feet at ground level. The archway (the gap
// under the arc, between the legs) is open by construction — no boolean cut needed.
//   span  — foot-to-foot distance (also the arc's diameter)
//   legH  — height of the legs (where the arc springs from)
//   thick — leg/Z half-thickness
//   tube  — arc tube radius
//   k     — smooth-min blend width fusing legs into the arc
[[nodiscard]] inline float arch(glm::vec3 p, glm::vec3 origin, float span, float legH,
                                float thick, float tube, float k = 4.0f) {
    p -= origin;
    // Arc: a torus ring in the X-Y plane (radius span/2, centred at the spring line),
    // clamped to its upper half so it forms an arch rather than a full ring.
    const glm::vec2 r(glm::length(glm::vec2(p.x, p.y - legH)) - span * 0.5f, p.z);
    float arc = glm::length(r) - tube;
    arc = sdf::opIntersection(arc, legH - p.y); // keep y >= legH (the upper arc)
    // Legs: vertical boxes at ±span/2, from the ground up to the spring line.
    const float legL = sdf::box(p - glm::vec3(-span * 0.5f, legH * 0.5f, 0.0f),
                                glm::vec3(thick, legH * 0.5f, thick));
    const float legR = sdf::box(p - glm::vec3(span * 0.5f, legH * 0.5f, 0.0f),
                                glm::vec3(thick, legH * 0.5f, thick));
    return sdf::smin(sdf::smin(legL, legR, k), arc, k);
}

// Zhangjiajie-style pillar field: a plateau (solid up to `topY`) carved into
// freestanding columns by Worley F1 — cell interiors stay as pillars, the gaps
// between cells become vertical air shafts. Below `baseY` the ground is solid; the
// pillars rise from there to `topY`. `freq` sets the pillar spacing (cell size).
//   worley — the cellular noise source (its seed fixes the pillar layout)
//   wallScale — scales the cell footprint to block units (pillar wall sharpness)
[[nodiscard]] inline float pillars(const Noise& worley, glm::vec3 p, float baseY,
                                   float topY, float freq, float wallScale = 6.0f) {
    // Worley F1 is ~+1 at a cell centre (pillar core) and ~-1 at cell edges (gaps).
    const float cell = worley.worley(p.x * freq, p.z * freq,
                                     Noise::Metric::Euclidean, Noise::Cell::F1);
    const float horiz  = cell * wallScale;        // > 0 inside a pillar footprint
    const float ground = baseY - p.y;             // > 0 below the base plateau
    const float below  = std::max(ground, horiz); // solid if below base OR in a pillar
    const float capped = std::min(topY - p.y, below); // ...and below the plateau top
    return -capped;                               // negative = inside (Surface Nets)
}

} // namespace vg::landform
