#include "world/NoiseStack.h"

#include <algorithm>
#include <cmath>

namespace vg {

float NoiseStack::shape(Type t, float v) {
    switch (t) {
        case Type::Ridged: return 1.0f - 2.0f * std::fabs(v); // sharp ridges at zero-crossings
        case Type::Billow: return 2.0f * std::fabs(v) - 1.0f; // rounded blobs
        case Type::Perlin:
        default:           return v;                          // raw rolling field
    }
}

float NoiseStack::value(float x, float z) const {
    float sum = 0.0f, total = 0.0f;
    for (const Entry& e : entries_) {
        const Layer& c = e.cfg;
        const float v = e.noise.fbm((x + c.offX + e.baseX) * c.frequency,
                                    (z + c.offZ + e.baseZ) * c.frequency,
                                    c.octaves, c.lacunarity, c.gain);
        sum   += c.weight * shape(c.type, v);
        total += std::fabs(c.weight);
    }
    return total > 0.0f ? std::clamp(sum / total, -1.0f, 1.0f) : 0.0f;
}

float NoiseStack::value(float x, float y, float z) const {
    float sum = 0.0f, total = 0.0f;
    for (const Entry& e : entries_) {
        const Layer& c = e.cfg;
        const float v = e.noise.fbm((x + c.offX + e.baseX) * c.frequency,
                                    (y + e.baseY) * c.frequency,
                                    (z + c.offZ + e.baseZ) * c.frequency,
                                    c.octaves, c.lacunarity, c.gain);
        sum   += c.weight * shape(c.type, v);
        total += std::fabs(c.weight);
    }
    return total > 0.0f ? std::clamp(sum / total, -1.0f, 1.0f) : 0.0f;
}

} // namespace vg
