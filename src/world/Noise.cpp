#include "world/Noise.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>

namespace vg {

namespace {
// Perlin's smootherstep: 6t^5 - 15t^4 + 10t^3. Zero 1st+2nd derivatives at the
// ends, which removes the visible grid artefacts plain interpolation produces.
float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float lerp(float a, float b, float t) { return a + t * (b - a); }

// Gradient: pick one of 8 directions from the low bits of the hash and dot it
// with the offset vector (x, y).
float grad(int hash, float x, float y) {
    switch (hash & 7) {
        case 0: return  x + y;
        case 1: return  x - y;
        case 2: return -x + y;
        case 3: return -x - y;
        case 4: return  x;
        case 5: return -x;
        case 6: return  y;
        default: return -y;
    }
}

// 3D gradient: Ken Perlin's reference 12-direction gradient set, selected from the
// low 4 bits of the hash and dotted with the offset vector (x, y, z).
float grad(int hash, float x, float y, float z) {
    const int h = hash & 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
} // namespace

Noise::Noise(uint32_t seed) {
    std::array<int, 256> p{};
    std::iota(p.begin(), p.end(), 0);          // 0,1,2,...,255
    std::mt19937 rng(seed);
    std::shuffle(p.begin(), p.end(), rng);     // seed-dependent permutation
    for (int i = 0; i < 512; ++i) {
        perm_[i] = p[i & 255];
    }
}

float Noise::perlin(float x, float y) const {
    // Integer cell coordinates (wrapped to the table) and fractional offsets.
    const int xi = static_cast<int>(std::floor(x)) & 255;
    const int yi = static_cast<int>(std::floor(y)) & 255;
    const float xf = x - std::floor(x);
    const float yf = y - std::floor(y);

    const float u = fade(xf);
    const float v = fade(yf);

    // Hash the four cell corners.
    const int aa = perm_[perm_[xi] + yi];
    const int ab = perm_[perm_[xi] + yi + 1];
    const int ba = perm_[perm_[xi + 1] + yi];
    const int bb = perm_[perm_[xi + 1] + yi + 1];

    // Interpolate the corner gradients.
    const float x1 = lerp(grad(aa, xf, yf),         grad(ba, xf - 1.0f, yf),         u);
    const float x2 = lerp(grad(ab, xf, yf - 1.0f),  grad(bb, xf - 1.0f, yf - 1.0f),  u);
    return lerp(x1, x2, v); // ~[-1, 1]
}

float Noise::perlin(float x, float y, float z) const {
    const int xi = static_cast<int>(std::floor(x)) & 255;
    const int yi = static_cast<int>(std::floor(y)) & 255;
    const int zi = static_cast<int>(std::floor(z)) & 255;
    const float xf = x - std::floor(x);
    const float yf = y - std::floor(y);
    const float zf = z - std::floor(z);

    const float u = fade(xf);
    const float v = fade(yf);
    const float w = fade(zf);

    // Hash the eight cube corners (perm_ is 512-long, so the chained adds below
    // never run past the end: max index is 510+1 = 511).
    const int A  = perm_[xi] + yi,     AA = perm_[A] + zi,     AB = perm_[A + 1] + zi;
    const int B  = perm_[xi + 1] + yi, BA = perm_[B] + zi,     BB = perm_[B + 1] + zi;

    const float x1 = lerp(grad(perm_[AA], xf, yf, zf),
                          grad(perm_[BA], xf - 1.0f, yf, zf), u);
    const float x2 = lerp(grad(perm_[AB], xf, yf - 1.0f, zf),
                          grad(perm_[BB], xf - 1.0f, yf - 1.0f, zf), u);
    const float y1 = lerp(x1, x2, v);
    const float x3 = lerp(grad(perm_[AA + 1], xf, yf, zf - 1.0f),
                          grad(perm_[BA + 1], xf - 1.0f, yf, zf - 1.0f), u);
    const float x4 = lerp(grad(perm_[AB + 1], xf, yf - 1.0f, zf - 1.0f),
                          grad(perm_[BB + 1], xf - 1.0f, yf - 1.0f, zf - 1.0f), u);
    const float y2 = lerp(x3, x4, v);
    return lerp(y1, y2, w); // ~[-1, 1]
}

float Noise::fbm(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float totalAmplitude = 0.0f;

    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * perlin(x * frequency, y * frequency);
        totalAmplitude += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return totalAmplitude > 0.0f ? sum / totalAmplitude : 0.0f; // normalise to ~[-1,1]
}

float Noise::fbm(float x, float y, float z, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float totalAmplitude = 0.0f;

    for (int i = 0; i < octaves; ++i) {
        sum += amplitude * perlin(x * frequency, y * frequency, z * frequency);
        totalAmplitude += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return totalAmplitude > 0.0f ? sum / totalAmplitude : 0.0f; // normalise to ~[-1,1]
}

} // namespace vg
