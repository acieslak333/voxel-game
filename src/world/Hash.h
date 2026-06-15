#pragma once

#include <cstdint>

// -----------------------------------------------------------------------------
//  Hash.h — the canonical integer hashes worldgen is built on.
// -----------------------------------------------------------------------------
//  floordiv/floormod give correct results for negatives (the coarse placement
//  grids straddle the origin). hash01 turns an integer cell (+ a salt) into a
//  deterministic value in [0,1) — the backbone of every scatter/placement gate.
//
//  CRITICAL: these formulas are THE definition of worldgen randomness. They were
//  previously copy-pasted into TerrainGenerator.cpp, World.cpp and main.cpp; any
//  divergence between copies silently changes the generated world. Keep them here,
//  in ONE place, so that can't happen. Changing a constant re-rolls every world.
// -----------------------------------------------------------------------------

namespace vg {

// Floor division (rounds toward -inf, unlike C's toward-zero) — for the coarse
// lake/structure/feature placement grids that span negative coordinates.
constexpr int floordiv(int a, int b) {
    const int q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

// Floor modulo (result has the sign of the divisor) — the companion of floordiv.
constexpr int floormod(int a, int b) {
    const int r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

// Deterministic [0,1) hash of an integer cell (x,z) + salt. Used everywhere a
// placement decision must be a pure function of world coordinates (lake/feature/
// structure scatter, tree gates) so streaming is seam-safe.
inline float hash01(int x, int z, uint32_t salt) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^
                 static_cast<uint32_t>(z) * 0xd8163841u ^ (salt * 0x9e3779b9u);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

// 3D variant — deterministic [0,1) from an integer (x,y,z) + salt. Used to scatter
// ore veins (a small block cluster shares a coarsened cell so it reads as a vein).
inline float hash01(int x, int y, int z, uint32_t salt) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^
                 static_cast<uint32_t>(y) * 0xcb1ab31fu ^
                 static_cast<uint32_t>(z) * 0xd8163841u ^ (salt * 0x9e3779b9u);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

} // namespace vg
