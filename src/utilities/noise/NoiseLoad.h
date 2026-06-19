#pragma once

/**
 * @file NoiseLoad.h
 * @brief YAML parsing helpers that build NoiseStack, NoiseMask, and Bezier falloff LUTs.
 *
 * Header-only (all functions are inline). loadStack() parses a `layers:` sequence into a
 * NoiseStack; loadMask() adds a threshold band + Falloff on top; buildFalloffLut() samples
 * a Catmull-Rom curve through control points into a uniform-x LUT. Shared by
 * TerrainGenerator and any other YAML-driven noise consumer so layer syntax is consistent.
 * @see docs/CODE_INDEX.md
 */

#include "utilities/noise/NoiseMask.h"
#include "utilities/noise/NoiseStack.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace vg {

// Parse an optional `layers:` sequence under a field node into a NoiseStack. Each list
// entry is a layer: {type, frequency, octaves, lacunarity, gain, weight, offset:[x,z],
// metric, cell, multifractal, mfOffset, mfGain, erode, warp:{amp,freq}}. Returns an
// empty stack if there is no `layers:` node. baseSeed salts each layer independently.
// Shared by TerrainGenerator (shape fields) and NoiseMask (scatter / block masks) so a
// layer is authored identically everywhere.
/// @brief Parse a `layers:` YAML sequence into a NoiseStack; returns empty stack if absent.
/// @param fieldNode  YAML node that may contain a `layers:` key and optional transfer fields.
/// @param baseSeed   Salted per-layer to make each layer's noise independent.
inline NoiseStack loadStack(const YAML::Node& fieldNode, uint32_t baseSeed) {
    NoiseStack stack;
    if (!fieldNode || !fieldNode["layers"] || !fieldNode["layers"].IsSequence()) {
        return stack; // empty: caller falls back to the scalar fbm
    }
    uint32_t i = 0;
    for (const YAML::Node& ln : fieldNode["layers"]) {
        NoiseStack::Layer L;
        if (ln["type"]) {
            const std::string t = ln["type"].as<std::string>();
            if (t == "ridged")      L.type = NoiseStack::Type::Ridged;
            else if (t == "billow") L.type = NoiseStack::Type::Billow;
            else if (t == "worley") L.type = NoiseStack::Type::Worley;
            else                    L.type = NoiseStack::Type::Perlin;
        }
        if (ln["frequency"])  L.frequency  = ln["frequency"].as<float>();
        if (ln["octaves"])    L.octaves    = ln["octaves"].as<int>();
        if (ln["lacunarity"]) L.lacunarity = ln["lacunarity"].as<float>();
        if (ln["gain"])       L.gain       = ln["gain"].as<float>();
        if (ln["weight"])     L.weight     = ln["weight"].as<float>();
        if (ln["offset"] && ln["offset"].IsSequence() && ln["offset"].size() == 2) {
            L.offX = ln["offset"][0].as<float>();
            L.offZ = ln["offset"][1].as<float>();
        }
        if (ln["metric"]) {
            const std::string m = ln["metric"].as<std::string>();
            if (m == "manhattan")      L.metric = Noise::Metric::Manhattan;
            else if (m == "chebyshev") L.metric = Noise::Metric::Chebyshev;
            else                       L.metric = Noise::Metric::Euclidean;
        }
        if (ln["cell"]) {
            const std::string c = ln["cell"].as<std::string>();
            if (c == "f2")                         L.cell = Noise::Cell::F2;
            else if (c == "f2mf1" || c == "f2-f1") L.cell = Noise::Cell::F2mF1;
            else                                   L.cell = Noise::Cell::F1;
        }
        if (ln["multifractal"]) L.multifractal = ln["multifractal"].as<bool>();
        if (ln["mfOffset"])     L.mfOffset     = ln["mfOffset"].as<float>();
        if (ln["mfGain"])       L.mfGain       = ln["mfGain"].as<float>();
        if (ln["erode"])        L.erodeDetail  = ln["erode"].as<bool>();
        if (ln["warp"]) {
            const YAML::Node& w = ln["warp"];
            if (w["amp"])  L.warpAmp  = w["amp"].as<float>();
            if (w["freq"]) L.warpFreq = w["freq"].as<float>();
        }
        stack.addLayer(L, baseSeed * 2246822519u + i * 0x9e3779b9u + 0x5bd1e995u);
        ++i;
    }
    if (fieldNode["redistribution"]) {
        stack.setRedistribution(fieldNode["redistribution"].as<float>());
    }
    if (fieldNode["terrace"]) {
        const YAML::Node& t = fieldNode["terrace"];
        const int   levels    = t["levels"]    ? t["levels"].as<int>()      : 0;
        const float sharpness = t["sharpness"] ? t["sharpness"].as<float>() : 1.0f;
        stack.setTerrace(levels, sharpness);
    }
    return stack;
}

// Sample a smooth (Catmull-Rom) curve through sorted [x,y] control points into a
// uniform-x LUT over [0,1] — the Bezier falloff's evaluation table. Points are clamped
// to [0,1]; <2 points yields an empty LUT (the falloff then falls back to identity).
/// @brief Build a Catmull-Rom uniform-x LUT from sorted (x, y) control points for Bezier falloff.
/// @param pts      Control points sorted by x in [0,1]; fewer than 2 yields an empty LUT.
/// @param samples  Number of uniformly-spaced x samples (default 65).
inline std::vector<float> buildFalloffLut(const std::vector<std::pair<float, float>>& pts,
                                          int samples = 65) {
    std::vector<float> lut;
    if (pts.size() < 2) return lut;
    auto yAtX = [&](float x) -> float {
        if (x <= pts.front().first)  return pts.front().second;
        if (x >= pts.back().first)   return pts.back().second;
        std::size_t i = 0;
        while (i + 1 < pts.size() && x > pts[i + 1].first) ++i;
        const auto& p1 = pts[i];
        const auto& p2 = pts[i + 1];
        const float span = std::max(1e-5f, p2.first - p1.first);
        const float t = (x - p1.first) / span;
        // Catmull-Rom tangents (Hermite over the x-monotone segment).
        const float y0 = (i > 0) ? pts[i - 1].second : p1.second;
        const float y3 = (i + 2 < pts.size()) ? pts[i + 2].second : p2.second;
        const float m1 = 0.5f * (p2.second - y0);
        const float m2 = 0.5f * (y3 - p1.second);
        const float t2 = t * t, t3 = t2 * t;
        const float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t;
        const float h01 = -2 * t3 + 3 * t2,    h11 = t3 - t2;
        return h00 * p1.second + h10 * m1 + h01 * p2.second + h11 * m2;
    };
    lut.reserve(static_cast<std::size_t>(samples));
    for (int k = 0; k < samples; ++k) {
        const float x = static_cast<float>(k) / static_cast<float>(samples - 1);
        lut.push_back(std::clamp(yAtX(x), 0.0f, 1.0f));
    }
    return lut;
}

// Parse a NoiseMask: a `layers:` stack (via loadStack) plus the threshold band and
// steepness curve. Returns an empty mask (weight() == 1, inert) when no `layers:` are
// authored, so a feature/biome without a mask gates nothing.
/// @brief Parse a NoiseMask from a YAML node (layers + threshold + falloff); inert when no layers.
inline NoiseMask loadMask(const YAML::Node& node, uint32_t seed) {
    NoiseMask m;
    if (!node) return m;
    m.stack = loadStack(node, seed);
    if (node["threshold"]) m.threshold = node["threshold"].as<float>();
    if (node["width"])     m.width     = node["width"].as<float>();
    if (node["gain"])      m.gain      = node["gain"].as<float>();
    if (node["invert"])    m.invert    = node["invert"].as<bool>();
    if (node["falloff"]) {
        const std::string f = node["falloff"].as<std::string>();
        if      (f == "step")         m.falloff = Falloff::Step;
        else if (f == "linear")       m.falloff = Falloff::Linear;
        else if (f == "smootherstep") m.falloff = Falloff::Smootherstep;
        else if (f == "gain")         m.falloff = Falloff::Gain;
        else if (f == "bezier")       m.falloff = Falloff::Bezier;
        else                          m.falloff = Falloff::Smoothstep;
    }
    if (node["bezier"] && node["bezier"].IsSequence()) {
        std::vector<std::pair<float, float>> pts;
        for (const YAML::Node& p : node["bezier"]) {
            if (p.IsSequence() && p.size() == 2)
                pts.emplace_back(std::clamp(p[0].as<float>(), 0.0f, 1.0f),
                                 std::clamp(p[1].as<float>(), 0.0f, 1.0f));
        }
        std::sort(pts.begin(), pts.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        m.bezierLut = buildFalloffLut(pts);
    }
    return m;
}

} // namespace vg
