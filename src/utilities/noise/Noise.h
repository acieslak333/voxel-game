#pragma once

/**
 * @file Noise.h
 * @brief Deterministic noise adapter: Perlin, fBm, fbmEroded, and Worley over FastNoise.
 *
 * Wraps the vendored FastNoise (Jordan Peck, MIT) behind a stable, seed-bound interface.
 * Every call is a pure function of (seed, world coords) — streaming and pregen safe.
 * The instance sets FastNoise frequency to 1; callers scale coordinates themselves.
 * @note FastNoise.h/cpp are vendored; do not edit them.
 * @see docs/CODE_INDEX.md
 */

#include "utilities/noise/FastNoise.h"

#include <cstdint>

namespace vg {

// -----------------------------------------------------------------------------
//  Noise
// -----------------------------------------------------------------------------
//  A thin, deterministic adapter over Jordan Peck's FastNoise (the vendored
//  charlesangus/FastNoise, MIT — see FastNoise.LICENSE). FastNoise is the noise
//  ENGINE for the whole project now; this class keeps the small, stable surface
//  the rest of the code (NoiseStack, Feature, the worldgen scaffolding) is built
//  on so swapping the backend never rippled outward:
//
//    * perlin()  — Ken Perlin gradient noise, 2D/3D, returned ~[-1, 1].
//    * fbm()     — fractal Brownian motion: octaves of perlin() summed at rising
//                  frequency / falling amplitude, normalised back to ~[-1, 1].
//    * fbmEroded — derivative-attenuated fBm (Quilez §4.1): detail fades on slopes
//                  (the cheapest "fake erosion"); the gradient comes from central
//                  differences of perlin().
//    * worley()  — FastNoise cellular (Voronoi): distance to the nearest (F1) /
//                  second-nearest (F2) feature point, or their difference (F2-F1,
//                  sharp cell edges), remapped to ~[-1, 1] so it blends like fbm.
//
//  Every call is a pure function of (seed, world coords): FastNoise hashes the
//  integer lattice/cell, so the field is identical for the same world coords on
//  every chunk and every thread — the property that makes streaming/pregen safe.
//  The instance fixes FastNoise frequency to 1 so callers scale coordinates
//  themselves (matching the old hand-rolled class, which NoiseStack relies on).
// -----------------------------------------------------------------------------
/** @brief Seed-bound noise adapter providing Perlin, fBm, fbmEroded, and Worley primitives. */
class Noise {
public:
    /// @brief Construct with a fixed seed; all subsequent calls are deterministic for that seed.
    explicit Noise(uint32_t seed);

    [[nodiscard]] float perlin(float x, float y) const;
    // 3D Perlin (same seed) — used for volumetric features like caves where a 2D
    // heightfield can't express overhangs/tunnels. Returns ~[-1, 1].
    [[nodiscard]] float perlin(float x, float y, float z) const;

    // 2D Perlin that also returns its gradient (∂/∂x, ∂/∂y) in `dx`,`dy`. The value
    // is bit-identical to perlin() (it IS perlin(x,y)); the gradient is a central
    // finite difference of perlin(), which lets terrain attenuate detail on slopes
    // (fbmEroded) and feed slope-based decisions without the caller differencing.
    float perlin(float x, float y, float& dx, float& dy) const;

    // Derivative-attenuated fBm (Quilez §4.1): like fbm() but each octave is divided
    // by 1 + |accumulated lower-octave gradient|², so detail appears on flats and
    // fades on slopes. Normalised to ~[-1, 1].
    [[nodiscard]] float fbmEroded(float x, float y, int octaves,
                                  float lacunarity = 2.0f, float gain = 0.5f) const;

    [[nodiscard]] float fbm(float x, float y, int octaves,
                            float lacunarity = 2.0f, float gain = 0.5f) const;
    [[nodiscard]] float fbm(float x, float y, float z, int octaves,
                            float lacunarity = 2.0f, float gain = 0.5f) const;

    // Distance metric for the cellular search. Chebyshev has no direct FastNoise
    // equivalent and maps to FastNoise's "Natural" distance (see Noise.cpp).
    /// @brief Distance metric for Worley cellular search (Chebyshev maps to FastNoise Natural).
    enum class Metric : uint8_t { Euclidean, Manhattan, Chebyshev };
    // Which cellular value to return: F1 (nearest → rounded cells), F2 (second
    // nearest), or F2-F1 (cell boundaries → cracks/fluting/spire walls).
    /// @brief Which cellular distance to return: nearest, second-nearest, or their difference.
    enum class Cell : uint8_t { F1, F2, F2mF1 };

    // Single-octave Worley/cellular noise, remapped to ~[-1, 1]. Pure function of
    // (seed, cell); FastNoise searches the 3x3 (2D) / 3x3x3 (3D) neighbourhood.
    [[nodiscard]] float worley(float x, float y, Metric m, Cell c) const;
    [[nodiscard]] float worley(float x, float y, float z, Metric m, Cell c) const;

private:
    FastNoise fn_;          // configured with frequency 1; callers scale coords
    uint32_t  seed_ = 0;
};

} // namespace vg
