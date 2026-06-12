#pragma once

#include "world/Noise.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
    //
    // A large, seed-derived domain shift is baked in so the layer never samples the
    // noise lattice ORIGIN at world (0,0,0): Perlin is *exactly* 0 there (and along
    // the integer lattice), which would otherwise flatten the terrain right where the
    // player spawns — most visibly on a high-weight low-frequency layer with no
    // authored offset. Authored offX/offZ stay purely additive on top of this base.
    void addLayer(const Layer& l, uint32_t seed) {
        auto off = [](uint32_t h) {
            h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
            return 4096.0f + static_cast<float>(h & 0xFFFFu); // [4096, 69631] blocks
        };
        Entry e{l, Noise(seed), off(seed ^ 0xA53Cu), off(seed ^ 0xB17Du), off(seed ^ 0xC92Eu)};
        // Clamp the octave count to the octaves that can actually be SEEN at block
        // resolution — each extra octave costs a full Perlin eval per sampled cell
        // (this runs millions of times during worldgen), and an editor-authored
        // layer can ask for far more than contribute anything visible:
        //   * amplitude: octave i contributes gain^i of the layer; below ~1/512
        //     it is invisible in a field that moves whole blocks.
        //   * frequency: octave i samples at frequency*lacunarity^i per BLOCK;
        //     past ~1 cycle/block the wavelength is sub-voxel — pure aliasing dust.
        // Worlds regenerate with imperceptibly different micro-detail (the dropped
        // octaves also leave fbm's normalisation), which random-seed launches never
        // notice; the speedup on octave-heavy configs is large.
        e.cfg.octaves = std::max(1, std::min(e.cfg.octaves,
                                             std::min(ampOctaveCap(e.cfg.gain),
                                                      freqOctaveCap(e.cfg.frequency,
                                                                    e.cfg.lacunarity))));
        if (e.cfg.octaves != l.octaves) {
            std::fprintf(stderr,
                         "[noise] layer octaves clamped %d -> %d (freq %.4f): higher "
                         "octaves are sub-block / sub-1/512-amplitude, invisible\n",
                         l.octaves, e.cfg.octaves, static_cast<double>(l.frequency));
        }
        entries_.push_back(e);
    }

    [[nodiscard]] bool empty() const { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const { return entries_.size(); }

    // Weighted, normalised blend at a world column (2D) — ~[-1, 1].
    [[nodiscard]] float value(float x, float z) const;
    // 3D variant for volumetric fields (caves etc.); same blend, sampled in 3D.
    [[nodiscard]] float value(float x, float y, float z) const;

private:
    // How many octaves stay above the 1/512 amplitude floor (gain^i >= 1/512).
    static int ampOctaveCap(float gain) {
        if (gain >= 1.0f) return 64; // no decay: nothing to cap on amplitude
        if (gain <= 0.0f) return 1;
        return 1 + static_cast<int>(std::log(1.0f / 512.0f) / std::log(gain));
    }
    // How many octaves stay at or below ~1 cycle per block (freq*lac^i <= 1).
    static int freqOctaveCap(float freq, float lacunarity) {
        if (freq >= 1.0f) return 1;
        if (lacunarity <= 1.0f) return 64; // frequencies never grow: no cap
        return 1 + static_cast<int>(std::log(1.0f / freq) / std::log(lacunarity));
    }
    struct Entry { Layer cfg; Noise noise; float baseX = 0.0f, baseY = 0.0f, baseZ = 0.0f; };
    [[nodiscard]] static float shape(Type t, float v); // remap an fbm sample by type
    std::vector<Entry> entries_;
};

} // namespace vg
