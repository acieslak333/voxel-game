#pragma once

#include <array>
#include <cstdint>

namespace vg {

// -----------------------------------------------------------------------------
//  Noise
// -----------------------------------------------------------------------------
//  Ken Perlin's "improved" 2D gradient noise, made seedable by shuffling the
//  permutation table with a seeded RNG. Plus fbm() (fractal Brownian motion):
//  several octaves of Perlin noise summed at increasing frequency / decreasing
//  amplitude, which is what gives terrain its natural rolling look.
//
//  perlin() returns roughly [-1, 1]; fbm() is normalised to the same range.
// -----------------------------------------------------------------------------
class Noise {
public:
    explicit Noise(uint32_t seed);

    [[nodiscard]] float perlin(float x, float y) const;

    [[nodiscard]] float fbm(float x, float y, int octaves,
                            float lacunarity = 2.0f, float gain = 0.5f) const;

private:
    // 0..255 permutation duplicated to 512 to avoid index wrapping in lookups.
    std::array<int, 512> perm_{};
};

} // namespace vg
