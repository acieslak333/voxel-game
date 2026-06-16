#pragma once

#include "utilities/noise/NoiseStack.h"

#include <cstdint>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  Falloff — a transfer curve mapping t in [0,1] -> [0,1]. This is the "steepness
//  function" applied to a noise mask's transition band: it shapes HOW a feature /
//  block fades in across the threshold edge (hard cut, linear ramp, smooth S, …).
//  Five built-in shapes plus a user-authored Bezier curve (sampled into a LUT).
// -----------------------------------------------------------------------------
enum class Falloff : uint8_t {
    Step,         // hard cutoff at 0.5 (the old boolean threshold)
    Linear,       // straight ramp
    Smoothstep,   // 3t^2-2t^3 — gentle S
    Smootherstep, // 6t^5-15t^4+10t^3 — flatter ends (Perlin)
    Gain,         // Schlick bias/gain S-curve, shaped by `gain` (0..1; 0.5 ~ linear)
    Bezier,       // a custom smooth curve authored in the tool, sampled to a LUT
};

// Apply a falloff curve to t (clamped to [0,1]). `gain` shapes the Gain curve; `lut`
// is the sampled y-over-x table for Bezier (uniform x in [0,1]); both ignored by the
// other shapes. A pure function — same input, same output, on every thread.
[[nodiscard]] float applyFalloff(Falloff f, float t, float gain,
                                 const std::vector<float>& lut);

// -----------------------------------------------------------------------------
//  NoiseMask — a reusable "where does this go" field: a (multi-layer) NoiseStack
//  put through a threshold + width band and a Falloff steepness curve, yielding a
//  weight in [0,1] per world column. Drives feature/tree scatter (probability) and
//  biome block selection (patches). Pure function of (seed, coords); empty stack =
//  inert (weight 1), so an unauthored mask never gates anything.
// -----------------------------------------------------------------------------
struct NoiseMask {
    NoiseStack stack;             // the noise field (one or many layers)
    float   threshold = 0.0f;     // band centre, in noise units (~[-1,1])
    float   width     = 0.5f;     // half-width of the transition band (↓ = sharper)
    Falloff falloff   = Falloff::Smoothstep;
    float   gain      = 0.5f;     // shape for the Gain falloff (0..1)
    bool    invert    = false;    // flip the weight (1 - w): mask the OUTSIDE of the band
    std::vector<float> bezierLut; // Bezier falloff: y sampled at uniform x over [0,1]

    [[nodiscard]] bool empty() const { return stack.empty(); }
    // Weight in [0,1] at a world column. empty() -> always 1 (no gating).
    [[nodiscard]] float weight(float wx, float wz) const;
};

} // namespace vg
