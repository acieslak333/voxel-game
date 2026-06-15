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

// The gradient VECTOR behind grad(hash,x,y): grad() == gx*x + gy*y. Used by the
// derivative-returning perlin() so its value stays bit-identical (1.0f*x == x).
void gradVec(int hash, float& gx, float& gy) {
    switch (hash & 7) {
        case 0:  gx =  1.0f; gy =  1.0f; break;
        case 1:  gx =  1.0f; gy = -1.0f; break;
        case 2:  gx = -1.0f; gy =  1.0f; break;
        case 3:  gx = -1.0f; gy = -1.0f; break;
        case 4:  gx =  1.0f; gy =  0.0f; break;
        case 5:  gx = -1.0f; gy =  0.0f; break;
        case 6:  gx =  0.0f; gy =  1.0f; break;
        default: gx =  0.0f; gy = -1.0f; break;
    }
}
// Derivative of the quintic fade: d/dt[6t⁵-15t⁴+10t³] = 30t²(t-1)².
float fadeD(float t) { return 30.0f * t * t * (t - 1.0f) * (t - 1.0f); }

// 3D gradient: Ken Perlin's reference 12-direction gradient set, selected from the
// low 4 bits of the hash and dotted with the offset vector (x, y, z).
float grad(int hash, float x, float y, float z) {
    const int h = hash & 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

// Hash an integer cell to a 32-bit value (Worley feature-point source). A pure
// function of (seed, cell), so feature points — and thus the whole cellular
// field — are identical for the same world coords on every chunk (seam-safe).
uint32_t cellHash(uint32_t seed, int cx, int cy, int cz) {
    uint32_t h = seed * 0x9e3779b9u;
    h ^= static_cast<uint32_t>(cx) * 0x8da6b343u;
    h ^= static_cast<uint32_t>(cy) * 0xd8163841u;
    h ^= static_cast<uint32_t>(cz) * 0xcb1ab31fu;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12;
    h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}
// A jitter component in [0, 1) from a hash word; `shift` selects 16 fresh bits.
float jitter(uint32_t h, int shift) {
    return static_cast<float>((h >> shift) & 0xFFFFu) / 65536.0f;
}
} // namespace

Noise::Noise(uint32_t seed) : seed_(seed) {
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

float Noise::perlin(float x, float y, float& dx, float& dy) const {
    const int xi = static_cast<int>(std::floor(x)) & 255;
    const int yi = static_cast<int>(std::floor(y)) & 255;
    const float xf = x - std::floor(x);
    const float yf = y - std::floor(y);

    const float u = fade(xf), v = fade(yf);
    const float du = fadeD(xf), dv = fadeD(yf); // d(fade)/d(coord)

    const int aa = perm_[perm_[xi] + yi];
    const int ab = perm_[perm_[xi] + yi + 1];
    const int ba = perm_[perm_[xi + 1] + yi];
    const int bb = perm_[perm_[xi + 1] + yi + 1];

    // Corner gradient vectors and their dot products (== grad(); value identical).
    float g00x, g00y, g10x, g10y, g01x, g01y, g11x, g11y;
    gradVec(aa, g00x, g00y); gradVec(ba, g10x, g10y);
    gradVec(ab, g01x, g01y); gradVec(bb, g11x, g11y);
    const float n00 = g00x * xf        + g00y * yf;
    const float n10 = g10x * (xf - 1.f) + g10y * yf;
    const float n01 = g01x * xf        + g01y * (yf - 1.f);
    const float n11 = g11x * (xf - 1.f) + g11y * (yf - 1.f);

    // Value: two x-lerps then a y-lerp — same arithmetic as perlin(x,y).
    const float nx0 = n00 + u * (n10 - n00);
    const float nx1 = n01 + u * (n11 - n01);
    const float value = nx0 + v * (nx1 - nx0);

    // Analytic gradient (chain rule through the fade-weighted bilerp).
    const float dnx0_dx = g00x + du * (n10 - n00) + u * (g10x - g00x);
    const float dnx1_dx = g01x + du * (n11 - n01) + u * (g11x - g01x);
    dx = dnx0_dx + v * (dnx1_dx - dnx0_dx);

    const float dnx0_dy = g00y + u * (g10y - g00y);
    const float dnx1_dy = g01y + u * (g11y - g01y);
    dy = dnx0_dy + dv * (nx1 - nx0) + v * (dnx1_dy - dnx0_dy);
    return value; // ~[-1, 1], == perlin(x, y)
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

namespace {
// Apply the chosen metric to a feature-point offset. Euclidean is returned
// SQUARED here (cheaper in the inner loop); worley() un-squares the winners.
float metricDist(Noise::Metric m, float dx, float dy) {
    switch (m) {
        case Noise::Metric::Manhattan: return std::fabs(dx) + std::fabs(dy);
        case Noise::Metric::Chebyshev: return std::max(std::fabs(dx), std::fabs(dy));
        case Noise::Metric::Euclidean:
        default:                       return dx * dx + dy * dy;
    }
}
float metricDist(Noise::Metric m, float dx, float dy, float dz) {
    switch (m) {
        case Noise::Metric::Manhattan: return std::fabs(dx) + std::fabs(dy) + std::fabs(dz);
        case Noise::Metric::Chebyshev:
            return std::max(std::fabs(dx), std::max(std::fabs(dy), std::fabs(dz)));
        case Noise::Metric::Euclidean:
        default:                       return dx * dx + dy * dy + dz * dz;
    }
}
// Pick the cellular value and remap it to ~[-1, 1] so it blends like fbm. F1/F2
// sit in ~[0, 1.2] for a unit-cell jittered grid; F2-F1 in ~[0, 1]. `1 - 2*v`
// keeps cell interiors high and boundaries low; downstream weight/spline tune it.
float remapCell(Noise::Cell c, float f1, float f2) {
    const float v = (c == Noise::Cell::F2)   ? f2
                  : (c == Noise::Cell::F2mF1) ? (f2 - f1)
                                              : f1;
    return std::clamp(1.0f - 2.0f * v, -1.0f, 1.0f);
}
} // namespace

float Noise::worley(float x, float y, Metric m, Cell c) const {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float xf = x - std::floor(x);
    const float yf = y - std::floor(y);

    float f1 = 1e30f, f2 = 1e30f; // nearest, second-nearest feature distance
    for (int gy = -1; gy <= 1; ++gy) {
        for (int gx = -1; gx <= 1; ++gx) {
            const uint32_t h = cellHash(seed_, xi + gx, yi + gy, 0);
            const float dx = static_cast<float>(gx) + jitter(h, 0)  - xf;
            const float dy = static_cast<float>(gy) + jitter(h, 16) - yf;
            const float d = metricDist(m, dx, dy);
            if (d < f1)      { f2 = f1; f1 = d; }
            else if (d < f2) { f2 = d; }
        }
    }
    if (m == Metric::Euclidean) { f1 = std::sqrt(f1); f2 = std::sqrt(f2); }
    return remapCell(c, f1, f2);
}

float Noise::worley(float x, float y, float z, Metric m, Cell c) const {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const int zi = static_cast<int>(std::floor(z));
    const float xf = x - std::floor(x);
    const float yf = y - std::floor(y);
    const float zf = z - std::floor(z);

    float f1 = 1e30f, f2 = 1e30f;
    for (int gz = -1; gz <= 1; ++gz) {
        for (int gy = -1; gy <= 1; ++gy) {
            for (int gx = -1; gx <= 1; ++gx) {
                const uint32_t h = cellHash(seed_, xi + gx, yi + gy, zi + gz);
                const float dx = static_cast<float>(gx) + jitter(h, 0)  - xf;
                const float dy = static_cast<float>(gy) + jitter(h, 10) - yf;
                const float dz = static_cast<float>(gz) + jitter(h, 16) - zf;
                const float d = metricDist(m, dx, dy, dz);
                if (d < f1)      { f2 = f1; f1 = d; }
                else if (d < f2) { f2 = d; }
            }
        }
    }
    if (m == Metric::Euclidean) { f1 = std::sqrt(f1); f2 = std::sqrt(f2); }
    return remapCell(c, f1, f2);
}

} // namespace vg
