#pragma once

#include "world/Noise.h"

#include <cstdint>
#include <vector>

namespace vg {

// -----------------------------------------------------------------------------
//  NoiseStack
// -----------------------------------------------------------------------------
//  A data-driven *weighted blend* of several noise layers, so any terrain noise
//  field (continentalness, erosion, peaks, climate, rivers…) can be authored as a
//  sum of layers instead of a single fbm. Each layer is an fbm of one of three
//  shapes — Perlin (the raw rolling field), Ridged (sharp ridgelines along the
//  zero-crossings, for mountains) or Billow (rounded blobs) — with its own
//  frequency, octave count, weight and domain offset (to decorrelate layers).
//
//  value() returns the weight-normalised blend in ~[-1, 1], matching the range of
//  Noise::fbm so a stack is a drop-in replacement for a single fbm call. A stack
//  is built from data (assets/biomes.yaml `layers:`), parsed by TerrainGenerator;
//  this class stays free of any file format so it is reusable and testable.
//
//  Determinism: every layer owns its own seeded Noise, so the blend is a pure
//  function of (seed, world coords) — streaming-safe, like the rest of worldgen.
// -----------------------------------------------------------------------------
class NoiseStack {
public:
    enum class Type { Perlin, Ridged, Billow };

    struct Layer {
        Type  type       = Type::Perlin;
        float frequency  = 0.01f;
        int   octaves    = 3;
        float lacunarity = 2.0f;
        float gain       = 0.5f;
        float weight     = 1.0f;  // contribution (can be negative to subtract)
        float offX       = 0.0f;  // domain shift, in world blocks (decorrelates layers)
        float offZ       = 0.0f;
    };

    // Append a layer with its own seeded noise. Seeds should differ per layer so the
    // layers are independent (TerrainGenerator salts by layer index).
    void addLayer(const Layer& l, uint32_t seed) { entries_.push_back({l, Noise(seed)}); }

    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    // Weighted, normalised blend at a world column (2D) — ~[-1, 1].
    [[nodiscard]] float value(float x, float z) const;
    // 3D variant for volumetric fields (caves etc.); same blend, sampled in 3D.
    [[nodiscard]] float value(float x, float y, float z) const;

private:
    struct Entry { Layer cfg; Noise noise; };
    [[nodiscard]] static float shape(Type t, float v); // remap an fbm sample by type
    std::vector<Entry> entries_;
};

} // namespace vg
