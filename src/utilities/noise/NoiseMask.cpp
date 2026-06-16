#include "utilities/noise/NoiseMask.h"

#include <algorithm>
#include <cmath>

namespace vg {

namespace {
// Perlin's bias/gain: an S-curve through (0,0),(0.5,0.5),(1,1) with steepness set by
// g in (0,1). g = 0.5 is the identity; g -> 1 sharpens toward a step; g -> 0 flattens
// the middle into a plateau. bias(t,b) = t^(ln b / ln 0.5).
float perlinGain(float t, float g) {
    g = std::clamp(g, 0.001f, 0.999f);
    const float e = std::log(1.0f - g) / std::log(0.5f);     // bias exponent for (1-g)
    if (t < 0.5f) return 0.5f * std::pow(std::max(0.0f, 2.0f * t), e);
    return 1.0f - 0.5f * std::pow(std::max(0.0f, 2.0f - 2.0f * t), e);
}
} // namespace

float applyFalloff(Falloff f, float t, float gain, const std::vector<float>& lut) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (f) {
        case Falloff::Step:         return t < 0.5f ? 0.0f : 1.0f;
        case Falloff::Linear:       return t;
        case Falloff::Smoothstep:   return t * t * (3.0f - 2.0f * t);
        case Falloff::Smootherstep: return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        case Falloff::Gain:         return perlinGain(t, gain);
        case Falloff::Bezier: {
            if (lut.size() < 2) return t;                 // no curve authored: identity
            const float p = t * static_cast<float>(lut.size() - 1);
            const int   i = std::min(static_cast<int>(p), static_cast<int>(lut.size()) - 2);
            const float fr = p - static_cast<float>(i);
            return std::clamp(lut[i] + (lut[i + 1] - lut[i]) * fr, 0.0f, 1.0f);
        }
    }
    return t;
}

float NoiseMask::weight(float wx, float wz) const {
    if (stack.empty()) return 1.0f;                       // inert: never gates
    const float v = stack.value(wx, wz);                  // ~[-1, 1]
    const float w = std::max(1e-4f, width);
    const float t = std::clamp(0.5f + (v - threshold) / (2.0f * w), 0.0f, 1.0f);
    const float o = applyFalloff(falloff, t, gain, bezierLut);
    return invert ? 1.0f - o : o;
}

} // namespace vg
