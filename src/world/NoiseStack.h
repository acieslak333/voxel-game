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
    // Perlin (raw rolling field), Ridged (sharp ridgelines), Billow (rounded
    // blobs), or Worley (cellular — spires/cracks/craters; see Noise::worley).
    enum class Type { Perlin, Ridged, Billow, Worley };

    struct Layer {
        Type  type       = Type::Perlin;
        float frequency  = 0.01f;
        int   octaves    = 3;
        float lacunarity = 2.0f;
        float gain       = 0.5f;
        float weight     = 1.0f;  // contribution (can be negative to subtract)
        float offX       = 0.0f;  // domain shift, in world blocks (decorrelates layers)
        float offZ       = 0.0f;

        // --- Worley/cellular controls (used only when type == Worley) ----------
        Noise::Metric metric = Noise::Metric::Euclidean;
        Noise::Cell   cell   = Noise::Cell::F1;

        // --- Multifractal mode (Musgrave) --------------------------------------
        // When set, the layer's octaves use cross-octave weighting instead of
        // plain fbm — rough areas get rougher and smooth areas smoother, the
        // realism unlock for eroded-looking mountains (docs/WORLDGEN.md §2.2-2.3).
        // Combined with Type::Ridged it is a ridged multifractal; with Perlin a
        // hybrid multifractal. Ignored for Worley layers.
        bool  multifractal = false;
        float mfOffset     = 1.0f; // shifts |noise| so the weighting behaves (ridged ~1.0)
        float mfGain       = 2.0f; // per-octave weight gain (ridged ~2.0; >1 sharpens)

        // --- Derivative-attenuated detail (Quilez §4.1) ------------------------
        // Replace the layer's plain fbm with fbmEroded: detail fades on slopes, the
        // cheapest pure "fake erosion". 2D only (surface detail); a 3D sample falls
        // back to plain fbm. Ignored for Worley/multifractal layers.
        bool  erodeDetail = false;

        // --- Per-layer domain warp (Inigo Quilez) ------------------------------
        // Displace the sample point by an internal fbm before evaluating, turning
        // bland fields into swirling/organic ones (coastlines, fjords). amp is in
        // world blocks; 0 disables it. warpFreq 0 falls back to the layer frequency.
        float warpAmp  = 0.0f;
        float warpFreq = 0.0f;
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

    // --- Transfer stage (applied to the normalised blend, after the weighted sum)
    //  This is where most of the visual identity comes from (docs/WORLDGEN.md §5):
    //   * redistribution: pow() on the [0,1]-remapped blend. >1 flattens lows
    //     (more plains, dramatic peaks); <1 raises land. Default 1 = identity.
    //   * terrace: quantise the blend into N stepped plateaus (mesas/highlands);
    //     sharpness controls how crisp the risers are. 0 levels = off.
    //  Both default to no-ops and are SKIPPED entirely when unset, so a stack with
    //  no transfer authored produces byte-identical output to before this feature.
    void setRedistribution(float exponent) { redistribution_ = exponent; }
    void setTerrace(int levels, float sharpness) {
        terraceLevels_ = levels;
        terraceSharp_  = sharpness;
    }

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

    // One layer's contribution in ~[-1, 1], before weighting: applies the layer's
    // domain warp, then dispatches on type/multifractal. The legacy Perlin/Ridged/
    // Billow non-warped non-multifractal path is bit-for-bit the old code.
    [[nodiscard]] float layerValue(const Entry& e, float sx, float sz) const;
    [[nodiscard]] float layerValue(const Entry& e, float sx, float sy, float sz) const;
    // The transfer stage (redistribution + terrace); identity when unset.
    [[nodiscard]] float transfer(float v) const;

    std::vector<Entry> entries_;
    float redistribution_ = 1.0f; // transfer-stage redistribution exponent (1 = off)
    int   terraceLevels_  = 0;     // transfer-stage terrace step count (0 = off)
    float terraceSharp_   = 1.0f;  // terrace riser sharpness
};

} // namespace vg
