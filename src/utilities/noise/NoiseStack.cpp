/**
 * @file NoiseStack.cpp
 * @brief Layer evaluation (domain warp, multifractal, Worley fractal) and transfer stage.
 * @see docs/CODE_INDEX.md
 */

#include "utilities/noise/NoiseStack.h"

#include <algorithm>
#include <cmath>

namespace vg {

float NoiseStack::shape(Type t, float v) {
    switch (t) {
        case Type::Ridged: return 1.0f - 2.0f * std::fabs(v); // sharp ridges at zero-crossings
        case Type::Billow: return 2.0f * std::fabs(v) - 1.0f; // rounded blobs
        case Type::Perlin:
        default:           return v;                          // raw rolling field
    }
}

namespace {

// fbm-of-Worley: sum octaves of cellular noise (each already ~[-1,1]), normalised.
// Worley is pricier than Perlin (a 3x3[/x3] feature-point search per octave), so
// the layer's octave count is clamped in addLayer() exactly like an fbm layer.
float worleyFractal(const Noise& n, const NoiseStack::Layer& c, float sx, float sz) {
    float sum = 0.0f, amp = 1.0f, freq = c.frequency, total = 0.0f;
    for (int i = 0; i < c.octaves; ++i) {
        sum   += amp * n.worley(sx * freq, sz * freq, c.metric, c.cell);
        total += amp;
        amp   *= c.gain;
        freq  *= c.lacunarity;
    }
    return total > 0.0f ? sum / total : 0.0f;
}
float worleyFractal(const Noise& n, const NoiseStack::Layer& c,
                    float sx, float sy, float sz) {
    float sum = 0.0f, amp = 1.0f, freq = c.frequency, total = 0.0f;
    for (int i = 0; i < c.octaves; ++i) {
        sum   += amp * n.worley(sx * freq, sy * freq, sz * freq, c.metric, c.cell);
        total += amp;
        amp   *= c.gain;
        freq  *= c.lacunarity;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

// Multifractal octave loop (Musgrave). When the base type is Ridged this is a
// ridged multifractal (knife-edge ridges descending into smooth valleys); for any
// other base it is a hybrid multifractal (peaks + valleys + plains). The defining
// trick is cross-octave weighting: each octave is scaled by the accumulated value
// of lower-frequency octaves, so detail only appears where terrain is already
// rough. The per-octave spectral weight `pwr` decays by `gain` (so the existing
// amplitude-based octave clamp still applies). Output is renormalised to ~[-1,1];
// the normalisation is approximate (multifractal range is data-dependent) — the
// downstream weight/spline is the precise tuning knob. See docs/WORLDGEN.md §2.
template <bool Is3D>
float multifractalImpl(const Noise& n, const NoiseStack::Layer& c,
                       float sx, float sy, float sz) {
    auto base = [&](float f) -> float {
        return Is3D ? n.perlin(sx * f, sy * f, sz * f) : n.perlin(sx * f, sz * f);
    };
    const float lac = c.lacunarity;
    const float pwHL = c.gain;       // spectral decay per octave (== fbm amplitude gain)
    float freq = c.frequency;

    if (c.type == NoiseStack::Type::Ridged) {
        float pwr = pwHL;
        float signal = c.mfOffset - std::fabs(base(freq));
        signal *= signal;            // square → sharp ridges
        float result = signal;
        for (int i = 1; i < c.octaves; ++i) {
            freq *= lac;
            const float weight = std::clamp(signal * c.mfGain, 0.0f, 1.0f);
            signal = c.mfOffset - std::fabs(base(freq));
            signal *= signal;
            signal *= weight;        // rough stays rough, smooth stays smooth
            result += signal * pwr;
            pwr *= pwHL;
        }
        const float maxEst = c.mfOffset * c.mfOffset *
                             (pwHL < 1.0f ? 1.0f / (1.0f - pwHL)
                                          : static_cast<float>(c.octaves));
        return std::clamp(2.0f * (result / maxEst) - 1.0f, -1.0f, 1.0f);
    }

    // Hybrid multifractal.
    float pwr = 1.0f;
    float result = base(freq) + c.mfOffset;
    float weight = result;
    for (int i = 1; i < c.octaves; ++i) {
        freq *= lac;
        pwr  *= pwHL;
        if (weight > 1.0f) weight = 1.0f;
        const float signal = (base(freq) + c.mfOffset) * pwr;
        result += weight * signal;
        weight *= signal;            // low areas suppress higher-octave detail
    }
    const float maxEst = 2.0f * c.mfOffset *
                         (pwHL < 1.0f ? 1.0f / (1.0f - pwHL)
                                      : static_cast<float>(c.octaves));
    return std::clamp(2.0f * (result / maxEst) - 1.0f, -1.0f, 1.0f);
}

} // namespace

float NoiseStack::layerValue(const Entry& e, float sx, float sz) const {
    const Layer& c = e.cfg;
    // Domain warp: displace the sample point by an internal fbm (Quilez). The two
    // component fbms use fixed decorrelation offsets so the warp isn't diagonally
    // symmetric; 4 octaves is plenty for a low-frequency warp field.
    if (c.warpAmp != 0.0f) {
        const float wf = c.warpFreq > 0.0f ? c.warpFreq : c.frequency;
        const float qx = e.noise.fbm(sx * wf, sz * wf, 4);
        const float qz = e.noise.fbm((sx + 5300.0f) * wf, (sz + 1300.0f) * wf, 4);
        sx += c.warpAmp * qx;
        sz += c.warpAmp * qz;
    }
    if (c.type == Type::Worley) return worleyFractal(e.noise, c, sx, sz);
    if (c.multifractal)         return multifractalImpl<false>(e.noise, c, sx, 0.0f, sz);
    if (c.erodeDetail) {
        return shape(c.type, e.noise.fbmEroded(sx * c.frequency, sz * c.frequency,
                                               c.octaves, c.lacunarity, c.gain));
    }
    // Legacy path — bit-for-bit identical to the pre-extension code.
    const float v = e.noise.fbm(sx * c.frequency, sz * c.frequency,
                                c.octaves, c.lacunarity, c.gain);
    return shape(c.type, v);
}

float NoiseStack::layerValue(const Entry& e, float sx, float sy, float sz) const {
    const Layer& c = e.cfg;
    if (c.warpAmp != 0.0f) {
        const float wf = c.warpFreq > 0.0f ? c.warpFreq : c.frequency;
        const float qx = e.noise.fbm(sx * wf, sy * wf, sz * wf, 4);
        const float qz = e.noise.fbm((sx + 5300.0f) * wf, (sy + 4100.0f) * wf,
                                     (sz + 1300.0f) * wf, 4);
        sx += c.warpAmp * qx;
        sz += c.warpAmp * qz;
    }
    if (c.type == Type::Worley) return worleyFractal(e.noise, c, sx, sy, sz);
    if (c.multifractal)         return multifractalImpl<true>(e.noise, c, sx, sy, sz);
    const float v = e.noise.fbm(sx * c.frequency, sy * c.frequency, sz * c.frequency,
                                c.octaves, c.lacunarity, c.gain);
    return shape(c.type, v);
}

float NoiseStack::transfer(float v) const {
    // Both stages are skipped when unset, so an un-shaped stack stays byte-identical
    // (powf(t,1) is not guaranteed to return t bit-exactly, hence the guard).
    if (terraceLevels_ <= 0 && redistribution_ == 1.0f) return v;
    float t = 0.5f * (v + 1.0f);                 // remap blend [-1,1] -> [0,1]
    if (redistribution_ != 1.0f) {
        t = std::pow(std::clamp(t, 0.0f, 1.0f), redistribution_);
    }
    if (terraceLevels_ > 0) {
        const float n = static_cast<float>(terraceLevels_);
        const float band = t * n;
        const float k = std::floor(band);
        const float f = band - k;                // fractional position within a step
        // smoothstep(0,1, f*sharpness) eases the riser; sharp -> crisp steps.
        const float fs = std::clamp(f * terraceSharp_, 0.0f, 1.0f);
        t = (k + fs * fs * (3.0f - 2.0f * fs)) / n;
    }
    return std::clamp(2.0f * t - 1.0f, -1.0f, 1.0f); // back to [-1,1]
}

float NoiseStack::value(float x, float z) const {
    float sum = 0.0f, total = 0.0f;
    for (const Entry& e : entries_) {
        const Layer& c = e.cfg;
        sum   += c.weight * layerValue(e, x + c.offX + e.baseX, z + c.offZ + e.baseZ);
        total += std::fabs(c.weight);
    }
    return total > 0.0f ? transfer(std::clamp(sum / total, -1.0f, 1.0f)) : 0.0f;
}

float NoiseStack::value(float x, float y, float z) const {
    float sum = 0.0f, total = 0.0f;
    for (const Entry& e : entries_) {
        const Layer& c = e.cfg;
        sum   += c.weight * layerValue(e, x + c.offX + e.baseX, y + e.baseY,
                                       z + c.offZ + e.baseZ);
        total += std::fabs(c.weight);
    }
    return total > 0.0f ? transfer(std::clamp(sum / total, -1.0f, 1.0f)) : 0.0f;
}

} // namespace vg
