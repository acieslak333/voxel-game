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
//  Also provides Worley / cellular noise (Steven Worley, "A Cellular Texture
//  Basis Function", SIGGRAPH '96): the distance to the nearest (F1) / 2nd-nearest
//  (F2) feature point in a jittered grid, or their difference (F2-F1, sharp cell
//  edges). This is the basis for spires/pillars, cracked rock, craters and pond
//  masks (docs/WORLDGEN.md Layer 0). Like perlin(), it is a pure function of
//  (seed, world coords) — feature points are a deterministic hash of the integer
//  cell, so it is streaming-safe and seam-safe.
//
//  perlin() returns roughly [-1, 1]; fbm() is normalised to the same range;
//  worley() is remapped to ~[-1, 1] as well so it drops into a NoiseStack blend.
// -----------------------------------------------------------------------------
class Noise {
public:
    explicit Noise(uint32_t seed);

    [[nodiscard]] float perlin(float x, float y) const;
    // 3D Perlin (same permutation table) — used for volumetric features like caves
    // where a 2D heightfield can't express overhangs/tunnels. Returns ~[-1, 1].
    [[nodiscard]] float perlin(float x, float y, float z) const;

    // 2D Perlin that also returns its analytic gradient (∂/∂x, ∂/∂y) in `dx`,`dy`
    // (Inigo Quilez, "Noise Derivatives"). The value is bit-identical to perlin();
    // the gradient lets terrain attenuate detail on slopes (fbmEroded) and feed
    // slope-based material/normal decisions without finite differences.
    float perlin(float x, float y, float& dx, float& dy) const;

    // Derivative-attenuated fBm (Quilez §4.1): like fbm() but each octave is divided
    // by 1 + |accumulated lower-octave gradient|², so detail appears on flats and
    // fades on slopes — the cheapest pure "fake erosion". Normalised to ~[-1, 1].
    [[nodiscard]] float fbmEroded(float x, float y, int octaves,
                                  float lacunarity = 2.0f, float gain = 0.5f) const;

    [[nodiscard]] float fbm(float x, float y, int octaves,
                            float lacunarity = 2.0f, float gain = 0.5f) const;
    [[nodiscard]] float fbm(float x, float y, float z, int octaves,
                            float lacunarity = 2.0f, float gain = 0.5f) const;

    // Distance metric for the cellular search.
    enum class Metric : uint8_t { Euclidean, Manhattan, Chebyshev };
    // Which cellular value to return: F1 (nearest → rounded cells), F2 (second
    // nearest), or F2-F1 (cell boundaries → cracks/fluting/spire walls).
    enum class Cell : uint8_t { F1, F2, F2mF1 };

    // Single-octave Worley/cellular noise, remapped to ~[-1, 1]. One feature point
    // per integer cell, jittered by a hash of (seed, cell); searches the 3x3 (2D)
    // or 3x3x3 (3D) neighbourhood. Euclidean distances are returned un-squared.
    [[nodiscard]] float worley(float x, float y, Metric m, Cell c) const;
    [[nodiscard]] float worley(float x, float y, float z, Metric m, Cell c) const;

private:
    // 0..255 permutation duplicated to 512 to avoid index wrapping in lookups.
    std::array<int, 512> perm_{};
    uint32_t seed_ = 0; // raw seed, hashed per cell for Worley feature points
};

} // namespace vg
