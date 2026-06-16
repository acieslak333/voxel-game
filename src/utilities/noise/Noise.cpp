#include "utilities/noise/Noise.h"

#include <algorithm>
#include <cmath>

namespace vg {

namespace {
// Map our metric/cell enums onto FastNoise's. Chebyshev has no FastNoise twin, so
// it falls back to "Natural" (a blended Euclidean+Manhattan distance) — the closest
// available shape. F1/F2/F2-F1 select the cellular distance-return type.
FastNoise::CellularDistanceFunction toDistFn(Noise::Metric m) {
    switch (m) {
        case Noise::Metric::Manhattan: return FastNoise::Manhattan;
        case Noise::Metric::Chebyshev: return FastNoise::Natural;
        case Noise::Metric::Euclidean:
        default:                       return FastNoise::Euclidean;
    }
}
FastNoise::CellularReturnType toReturnType(Noise::Cell c) {
    switch (c) {
        case Noise::Cell::F2:    return FastNoise::Distance2;
        case Noise::Cell::F2mF1: return FastNoise::Distance2Sub;
        case Noise::Cell::F1:
        default:                 return FastNoise::Distance;
    }
}
// Remap a cellular distance value to ~[-1, 1] so it blends like fbm. Cell interiors
// (small distance) stay high, boundaries (large distance / large F2-F1) go low; the
// downstream weight/spline does the precise tuning. Clamped because FastNoise returns
// squared Euclidean distances whose magnitude isn't normalised.
float remapCell(float v) { return std::clamp(1.0f - 2.0f * v, -1.0f, 1.0f); }
} // namespace

Noise::Noise(uint32_t seed) : seed_(seed) {
    fn_.SetSeed(static_cast<int>(seed));
    fn_.SetFrequency(1.0f);            // callers scale coords (NoiseStack relies on this)
    fn_.SetInterp(FastNoise::Quintic); // matches the old quintic fade
}

float Noise::perlin(float x, float y) const { return fn_.GetPerlin(x, y); }

float Noise::perlin(float x, float y, float z) const { return fn_.GetPerlin(x, y, z); }

float Noise::perlin(float x, float y, float& dx, float& dy) const {
    // Value is perlin(x,y) exactly; gradient is a symmetric central difference of it
    // (FastNoise exposes no analytic derivative). h matches the worldgen self-test.
    const float h = 0.001f;
    dx = (fn_.GetPerlin(x + h, y) - fn_.GetPerlin(x - h, y)) / (2.0f * h);
    dy = (fn_.GetPerlin(x, y + h) - fn_.GetPerlin(x, y - h)) / (2.0f * h);
    return fn_.GetPerlin(x, y);
}

float Noise::fbm(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, totalAmplitude = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * perlin(x * frequency, y * frequency);
        totalAmplitude += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return totalAmplitude > 0.0f ? sum / totalAmplitude : 0.0f; // normalise to ~[-1,1]
}

float Noise::fbm(float x, float y, float z, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, totalAmplitude = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * perlin(x * frequency, y * frequency, z * frequency);
        totalAmplitude += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return totalAmplitude > 0.0f ? sum / totalAmplitude : 0.0f; // normalise to ~[-1,1]
}

float Noise::fbmEroded(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, totalAmplitude = 0.0f;
    float dsumX = 0.0f, dsumY = 0.0f; // accumulated lower-octave gradient
    for (int i = 0; i < octaves; ++i) {
        float gx = 0.0f, gy = 0.0f;
        const float n = perlin(x * frequency, y * frequency, gx, gy);
        dsumX += gx;
        dsumY += gy;
        // Steep accumulated slope ⇒ suppress this octave's contribution.
        sum += amplitude * n / (1.0f + (dsumX * dsumX + dsumY * dsumY));
        totalAmplitude += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return totalAmplitude > 0.0f ? sum / totalAmplitude : 0.0f; // ~[-1,1] (|n|≤1, divisor≥1)
}

float Noise::worley(float x, float y, Metric m, Cell c) const {
    // Copy the configured generator so the per-call cellular settings stay local —
    // worley() is const and may run on many threads, so it never mutates fn_.
    FastNoise f = fn_;
    f.SetCellularDistanceFunction(toDistFn(m));
    f.SetCellularReturnType(toReturnType(c));
    return remapCell(f.GetCellular(x, y));
}

float Noise::worley(float x, float y, float z, Metric m, Cell c) const {
    FastNoise f = fn_;
    f.SetCellularDistanceFunction(toDistFn(m));
    f.SetCellularReturnType(toReturnType(c));
    return remapCell(f.GetCellular(x, y, z));
}

} // namespace vg
