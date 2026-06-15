#include "world/TerrainGenerator.h"

#include "world/BlockRegistry.h"
#include "world/Hash.h"        // shared floordiv/hash01 (were local copies here)
#include "world/NoiseLoad.h"   // shared loadStack (was a local copy here)

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vg {

float Spline::at(float x) const {
    if (xs.empty()) {
        return 0.0f;
    }
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back())  return ys.back();
    size_t i = 1;
    while (i < xs.size() && x > xs[i]) {
        ++i;
    }
    const float t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[i - 1] + t * (ys[i] - ys[i - 1]);
}

namespace {
// floordiv/hash01 now live in world/Hash.h (shared canonical worldgen hashes).

// (loadStack moved to world/NoiseLoad.h so TerrainGenerator and NoiseMask share one
//  parser — terrain output is byte-identical, the code is just relocated.)

// Resolve a block name to an id, falling back to `fallback` (then stone) if the
// name is missing — so an authored biome can't crash generation.
uint16_t resolveBlock(const BlockRegistry& reg, const std::string& name,
                      const char* fallback) {
    try {
        return reg.idByName(name);
    } catch (const std::out_of_range&) {
        try {
            return reg.idByName(fallback);
        } catch (const std::out_of_range&) {
            return reg.idByName("stone");
        }
    }
}

// Pick a block id from a weighted palette for a roll in [0,1). One entry → always that
// block, so a single-block biome is byte-identical to the old single-id path.
uint16_t pickBlock(const std::vector<std::pair<uint16_t, float>>& pal, float roll) {
    if (pal.empty()) return 0;
    float total = 0.0f;
    for (const auto& e : pal) total += e.second;
    if (total <= 0.0f) return pal.front().first;
    float t = roll * total;
    for (const auto& e : pal) { t -= e.second; if (t < 0.0f) return e.first; }
    return pal.back().first;
}

// Parse a biome `top:`/`filler:` value into a weighted palette — like a feature's block
// list. Accepts a scalar name ("grass"), a list of names ([grass, dirt] = equal weight),
// or a list of {name, w} maps. Falls back to `fallback` when empty.
std::vector<std::pair<uint16_t, float>> parsePalette(const YAML::Node& n,
        const BlockRegistry& reg, const char* fallback) {
    std::vector<std::pair<uint16_t, float>> pal;
    auto add = [&](const std::string& nm, float w) {
        pal.emplace_back(resolveBlock(reg, nm, fallback), std::max(0.0f, w));
    };
    if (n && n.IsScalar()) {
        add(n.as<std::string>(), 1.0f);
    } else if (n && n.IsSequence()) {
        for (const auto& e : n) {
            if (e.IsScalar()) add(e.as<std::string>(), 1.0f);
            else if (e.IsMap() && e["name"]) add(e["name"].as<std::string>(),
                                                 e["w"] ? e["w"].as<float>() : 1.0f);
        }
    }
    if (pal.empty()) add(fallback, 1.0f);
    return pal;
}
} // namespace

TerrainGenerator::TerrainGenerator(uint32_t seed, const BlockRegistry& registry,
                                   const std::string& assetDir, int worldHeight)
    : worldHeight_(worldHeight),
      contNoise_(seed ^ 0x9e3779b9u),
      eroNoise_(seed * 2654435761u + 0x1234u),
      peakNoise_(seed * 40503u + 0x77u),
      tempNoise_(seed ^ 0xC0FFEEu),
      humNoise_(seed * 668265263u + 0x55u),
      coastNoise_(seed * 2246822519u + 0x99u),
      densityNoise_(seed * 374761393u + 0xABCDu),
      floatNoise_(seed * 1103515245u + 0x4Du) {
    seed_ = seed;
    seaLevel_ = worldHeight_ / 2;
    snowLineRel_ = std::max(20, worldHeight_ / 2 - 9);

    // Process-unique tag so the thread-local density-corner cache (keyed by lattice
    // coords) never returns a stale value from a previously-constructed generator
    // (a different seed → different density). See densityCorner().
    static std::atomic<uint32_t> sEpochCounter{1};
    densityEpoch_ = sEpochCounter.fetch_add(1, std::memory_order_relaxed);

    // Baked-in defaults so the game generates a sensible world even with no
    // biomes.yaml. loadConfig() overrides any of this from the file.
    contSpline_.add(-1.00f, -52.0f);
    contSpline_.add(-0.50f, -30.0f);
    contSpline_.add(-0.20f, -8.0f);
    contSpline_.add(-0.05f, -1.0f);
    contSpline_.add(0.05f, 3.0f);
    contSpline_.add(0.30f, 9.0f);
    contSpline_.add(0.60f, 16.0f);
    contSpline_.add(1.00f, 26.0f);

    eroSpline_.add(-1.00f, 78.0f);
    eroSpline_.add(-0.50f, 50.0f);
    eroSpline_.add(-0.10f, 22.0f);
    eroSpline_.add(0.20f, 8.0f);
    eroSpline_.add(0.50f, 2.0f);
    eroSpline_.add(1.00f, 0.0f);

    oceanFloorId_  = resolveBlock(registry, "sand", "dirt");
    oceanFillerId_ = resolveBlock(registry, "sand", "dirt");
    snowId_        = resolveBlock(registry, "snow", "grass");

    // Default biome table (first match wins; keep specific ones before plains).
    auto add = [&](const char* name, float t0, float t1, float h0, float h1,
                   int r0, int r1, const char* top, const char* filler, bool snow,
                   float trees) {
        BiomeDef b;
        b.name = name;
        b.tempMin = t0; b.tempMax = t1;
        b.humMin = h0;  b.humMax = h1;
        b.relMin = r0;  b.relMax = r1;
        b.top    = {{resolveBlock(registry, top, "grass"), 1.0f}};
        b.filler = {{resolveBlock(registry, filler, "dirt"), 1.0f}};
        b.snow = snow;
        b.treeDensity = trees;
        biomes_.push_back(b);
    };
    //   name        temp        humidity     rel-elev      top      filler   snow  trees
    add("beach",    -2,2,       -2,2,        -4, 2,        "sand",  "sand",  false, 0.0f);
    add("mountain", -2,2,       -2,2,        34, 10000,    "stone", "stone", false, 0.003f);
    add("desert",    0.35f,2,   -2,-0.15f,    2, 10000,    "sand",  "sand",  false, 0.0f);
    add("savanna",   0.2f,2,    -0.15f,0.3f,  2, 10000,    "grass", "dirt",  false, 0.006f);
    add("snowy",    -2,-0.35f,  -2,2,         2, 10000,    "grass", "dirt",  true,  0.012f);
    add("forest",   -0.35f,0.6f, 0.2f,2,      2, 10000,    "grass", "dirt",  false, 0.055f);
    add("plains",   -2,2,       -2,2,        -10000,10000, "grass", "dirt",  false, 0.018f);

    loadConfig(assetDir, registry);

    // Default MULTI-SCALE density blend (when biomes.yaml didn't author
    // `terrain3d.density.layers`): a low-frequency swell for big mountains/valleys,
    // a ridged layer for sharp cliffs/ridgelines, and a high-frequency layer for fine
    // detail + vertical folding (overhangs) — the requested "big AND small changes,
    // sometimes sharp". The low-freq layer barely varies over the vertical band so it
    // reads as a broad height swing; the high-freq folds in Y to carve overhangs.
    if (density3DEnabled_ && densityStack_.empty()) {
        const uint32_t s = seed_ * 374761393u + 0xABCDu;
        // MULTIPLE noises across an octave spread (0.004 -> 0.052) so terrain varies
        // at every scale: continental swells, big hills, sharp ridges, then medium &
        // small overhangs (folding in Y). Diverse overhang heights come from the two
        // mid-frequency layers having real vertical extent.
        densityStack_.addLayer({NoiseStack::Type::Perlin, 0.0040f, 3, 2.0f, 0.5f, 1.00f, 0.0f, 0.0f}, s);
        densityStack_.addLayer({NoiseStack::Type::Perlin, 0.0090f, 3, 2.0f, 0.5f, 0.62f, 5300.0f, 2100.0f}, s + 3u);
        densityStack_.addLayer({NoiseStack::Type::Ridged, 0.0150f, 3, 2.1f, 0.5f, 0.50f, 1700.0f, -900.0f}, s + 7u);
        densityStack_.addLayer({NoiseStack::Type::Perlin, 0.0250f, 2, 2.0f, 0.5f, 0.34f, -3300.0f, 1500.0f}, s + 13u);
        densityStack_.addLayer({NoiseStack::Type::Perlin, 0.0520f, 2, 2.0f, 0.5f, 0.15f, -2600.0f, 4100.0f}, s + 19u);
    }
}

void TerrainGenerator::loadConfig(const std::string& assetDir, const BlockRegistry& registry) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(assetDir + "/biomes.yaml");
    } catch (const YAML::Exception&) {
        return; // no file: keep the baked defaults
    }
    if (!root.IsMap()) {
        return;
    }
    auto getInt = [&](const char* k, int& v) { if (root[k]) v = root[k].as<int>(); };
    auto getF   = [&](const YAML::Node& n, const char* k, float& v) {
        if (n && n[k]) v = n[k].as<float>();
    };
    auto getI   = [&](const YAML::Node& n, const char* k, int& v) {
        if (n && n[k]) v = n[k].as<int>();
    };
    // Tolerant bool: yaml-cpp's as<bool>() only accepts true/false, but the tuning
    // tools (tools/worldgen_tool.py) write toggles as 0/1 sliders, so a tuned
    // `enabled: 1` would otherwise throw "bad conversion" and crash worldgen /
    // --genmap / --selftest. Accept ints and the common yes/no/on/off forms too.
    auto asBool = [](const YAML::Node& n, bool fallback) -> bool {
        if (!n) return fallback;
        try { return n.as<bool>(); } catch (...) {}
        try { return n.as<int>() != 0; } catch (...) {}
        std::string s;
        try { s = n.as<std::string>(); } catch (...) { return fallback; }
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "true" || s == "yes" || s == "on")  return true;
        if (s == "false" || s == "no" || s == "off") return false;
        return fallback;
    };
    getInt("sea_level", seaLevel_);
    getInt("snow_line", snowLineRel_);
    getF(root["continentalness"], "frequency", contFreq_);
    getI(root["continentalness"], "octaves", contOct_);
    getF(root["erosion"], "frequency", eroFreq_);
    getI(root["erosion"], "octaves", eroOct_);
    getF(root["peaks"], "frequency", peakFreq_);
    getI(root["peaks"], "octaves", peakOct_);
    getF(root["temperature"], "frequency", tempFreq_);
    getF(root["humidity"], "frequency", humFreq_);
    if (const YAML::Node is = root["island"]) {
        islandEnabled_ = asBool(is["enabled"], true);
        if (is["center"] && is["center"].IsSequence() && is["center"].size() == 2) {
            islandCx_ = is["center"][0].as<float>();
            islandCz_ = is["center"][1].as<float>();
        }
        getF(is, "radius", islandRadius_);
        getF(is, "inner", islandInner_);
        getF(is, "coast_warp", islandCoastWarp_);
        getF(is, "land_base", islandLandBase_);
        getF(is, "peak_height", islandPeakHeight_);
        getF(is, "interior_var", islandInteriorVar_);
        getF(is, "ocean_floor", islandDeepOcean_);
    }

    auto loadSpline = [&](const char* key, Spline& out) {
        const YAML::Node n = root[key];
        if (!n || !n.IsSequence() || n.size() < 2) {
            return; // keep the default spline
        }
        Spline s;
        for (const YAML::Node& pt : n) {
            if (pt.IsSequence() && pt.size() == 2) {
                s.add(pt[0].as<float>(), pt[1].as<float>());
            }
        }
        if (s.xs.size() >= 2) {
            out = std::move(s);
        }
    };
    loadSpline("continental_spline", contSpline_);
    loadSpline("erosion_spline", eroSpline_);
    loadSpline("island_profile_spline", profileSpline_);

    // Optional data-driven noise stacks: a `<field>.layers:` sequence replaces the
    // single-fbm sample with a weighted blend. Absent -> empty stack -> legacy path.
    contStack_  = loadStack(root["continentalness"], seed_ ^ 0x9e3779b9u);
    eroStack_   = loadStack(root["erosion"],         seed_ * 2654435761u + 0x1234u);
    peakStack_  = loadStack(root["peaks"],           seed_ * 40503u + 0x77u);
    tempStack_  = loadStack(root["temperature"],     seed_ ^ 0xC0FFEEu);
    humStack_   = loadStack(root["humidity"],        seed_ * 668265263u + 0x55u);

    // Worldgen v2: 3D volumetric terrain knobs (overhangs / floating islands). The
    // density + float fields can each be a weighted NoiseStack via nested `layers:`.
    if (const YAML::Node t3 = root["terrain3d"]) {
        density3DEnabled_ = asBool(t3["enabled"], true);
        getF(t3, "freq_xz", densityFreqXZ_);
        getF(t3, "freq_y", densityFreqY_);
        getI(t3, "octaves", densityOct_);
        getF(t3, "amplitude", densityAmp_);
        getI(t3, "blend", biomeBlendCells_);   // per-biome amplitude border-blend (blocks)
        getF(t3, "float_freq", floatFreq_);
        getF(t3, "float_threshold", floatThresh_);
        getI(t3, "float_gap", floatGap_);
        getI(t3, "float_reach", floatReach_);
        if (const YAML::Node cf = t3["coast_flatten"]) {
            coastFlatten_ = asBool(cf["enabled"], false);
            getF(cf, "range", coastFlattenRange_);
            getF(cf, "min",   coastFlattenMin_);
        }
        densityStack_ = loadStack(t3["density"], seed_ * 374761393u + 0xABCDu);
        floatStack_   = loadStack(t3["float"],   seed_ * 1103515245u + 0x4Du);
        // Coarse-lattice density interpolation (REVIEW O6), opt-in. `lattice` is an
        // optional [x,y,z] cell size (default 4×8×4); each component is clamped to
        // >= 1 (1 = exact on that axis).
        densityInterp_ = asBool(t3["interpolate"], false);
        if (const YAML::Node lat = t3["lattice"]; lat && lat.IsSequence() && lat.size() == 3) {
            latX_ = std::max(1, lat[0].as<int>(latX_));
            latY_ = std::max(1, lat[1].as<int>(latY_));
            latZ_ = std::max(1, lat[2].as<int>(latZ_));
        }
    }
    densityAmp_  = std::max(1.0f, densityAmp_);
    floatReach_  = std::max(floatGap_ + 1, floatReach_);

    // Cost warning (REVIEW R12). A cell pays the expensive density-stack eval only
    // where the heightmap gradient is within ±amplitude of the surface — a vertical
    // band ~2*amplitude tall per column. With amplitude near world height that band
    // covers the whole column, so EVERY cell takes the density path: this is the
    // other half of the 145s-startup incident (octave count was the half the
    // NoiseStack clamp now guards). Warn when the band exceeds half the column so an
    // editor-authored biomes.yaml that does this is caught at load, not by a stalled
    // startup. Watch the VG_MESH_TIME generate stamp after a change.
    if (density3DEnabled_) {
        const float bandCells = 2.0f * densityAmp_;
        if (bandCells > 0.5f * static_cast<float>(worldHeight_)) {
            std::fprintf(stderr,
                "[worldgen] WARNING: terrain3d.amplitude=%.0f makes the density band "
                "~%.0f blocks tall (>%.0f%% of the %d-tall world): every column cell "
                "pays the density eval — generation will be slow (REVIEW R12).\n",
                static_cast<double>(densityAmp_), static_cast<double>(bandCells),
                static_cast<double>(100.0f * bandCells / static_cast<float>(worldHeight_)),
                worldHeight_);
        }
    }

    const YAML::Node bs = root["biomes"];
    if (bs && bs.IsSequence() && bs.size() > 0) {
        std::vector<BiomeDef> loaded;
        for (const YAML::Node& bn : bs) {
            if (!bn["name"]) continue;
            BiomeDef b;
            b.name = bn["name"].as<std::string>();
            // Climate (temp/humidity) removed — biomes select by elevation band alone,
            // varied by an optional `mask:` (noise selection mask, below).
            if (bn["elevation"] && bn["elevation"].IsSequence() && bn["elevation"].size() == 2) {
                b.relMin = bn["elevation"][0].as<int>();
                b.relMax = bn["elevation"][1].as<int>();
            }
            if (bn["mask"]) {
                uint32_t h = seed_ * 374761393u + 0x5e1ec7u;
                for (char c : b.name) h = h * 131u + static_cast<unsigned char>(c);
                b.selMask = loadMask(bn["mask"], h);
            }
            b.top    = parsePalette(bn["top"],    registry, "grass");
            b.filler = parsePalette(bn["filler"], registry, "dirt");
            b.snow        = asBool(bn["snow"], false);
            b.treeDensity = bn["trees"] ? bn["trees"].as<float>() : 0.0f;
            if (bn["tree"]) {
                const std::string t = bn["tree"].as<std::string>();
                b.tree = t == "birch" ? TreeKind::Birch
                       : t == "pine"  ? TreeKind::Pine
                       : TreeKind::Oak;
            }
            if (const YAML::Node tn = bn["tint"]; tn && tn.IsSequence() && tn.size() >= 3) {
                b.vegTint = {tn[0].as<float>(), tn[1].as<float>(), tn[2].as<float>()};
            }
            // Per-biome overrides of the global terrain3d knobs. Each is only read when
            // the biome authors that key, so an unauthored biome keeps the global value
            // and the default world stays byte-identical.
            if (const YAML::Node bt = bn["terrain3d"]) {
                if (bt["amplitude"])       b.amp3d       = bt["amplitude"].as<float>();
                if (bt["enabled"])         b.en3d        = asBool(bt["enabled"], true) ? 1 : 0;
                if (bt["float_threshold"]) b.floatThresh = bt["float_threshold"].as<float>();
                if (bt["float_reach"])     b.floatReach  = bt["float_reach"].as<int>();
                if (const YAML::Node cf = bt["coast_flatten"]) {
                    if (cf["enabled"]) b.coastOn    = asBool(cf["enabled"], false) ? 1 : 0;
                    if (cf["range"])   b.coastRange = cf["range"].as<float>();
                    if (cf["min"])     b.coastMin   = cf["min"].as<float>();
                }
                // Per-biome 3D density layer stack (unlimited layers, like the global one).
                if (bt["density"] && bt["density"]["layers"]) {
                    b.densityStack = loadStack(bt["density"],
                                               seed_ * 2654435761u + 0x9E37u +
                                               static_cast<uint32_t>(loaded.size()) * 2246822519u);
                    b.hasDensityStack = !b.densityStack.empty();
                }
            }
            // Noise-driven surface block patches: each {block, threshold, width, falloff,
            // gain, invert, bezier, layers:[...]} overrides the surface block where its
            // mask passes (first match wins). Salted per biome+mask index for independence.
            if (const YAML::Node sm = bn["surface_masks"]; sm && sm.IsSequence()) {
                uint32_t mi = 0;
                for (const YAML::Node& mn : sm) {
                    if (!mn["block"]) { ++mi; continue; }
                    BiomeDef::BlockMask bm;
                    bm.id = resolveBlock(registry, mn["block"].as<std::string>(), "stone");
                    bm.mask = loadMask(mn, seed_ * 2654435761u + 0x515Du +
                                       static_cast<uint32_t>(loaded.size()) * 2246822519u +
                                       mi * 0x9e3779b9u);
                    if (!bm.mask.empty()) b.surfaceMasks.push_back(std::move(bm));
                    ++mi;
                }
            }
            loaded.push_back(b);
        }
        if (!loaded.empty()) {
            biomes_ = std::move(loaded);
        }
    }

    maxTreeDensity_ = 0.0f;
    maxAmp_ = densityAmp_;
    maxFloatReach_ = floatReach_;
    anyAmpOverride_ = false;
    anyDensityStackOverride_ = false;
    for (const BiomeDef& b : biomes_) {
        maxTreeDensity_ = std::max(maxTreeDensity_, b.treeDensity);
        if (b.amp3d >= 0.0f) maxAmp_ = std::max(maxAmp_, b.amp3d);
        if (b.floatReach >= 0) maxFloatReach_ = std::max(maxFloatReach_, b.floatReach);
        // Any blended-scalar override (amp/enabled/float/coast) routes columns through
        // the biomeParamsAt blend path instead of the global fast path.
        if (b.amp3d >= 0.0f || b.en3d >= 0 || b.floatThresh >= 0.0f || b.floatReach >= 0 ||
            b.coastOn >= 0 || b.coastRange >= 0.0f || b.coastMin >= 0.0f)
            anyAmpOverride_ = true;
        if (b.hasDensityStack)    anyDensityStackOverride_ = true;
    }
    maxAmp_ = std::max(1.0f, maxAmp_);
    maxFloatReach_ = std::max(floatGap_ + 1, maxFloatReach_);
    biomeBlendCells_ = std::max(0, biomeBlendCells_);
}

int TerrainGenerator::shapeHeight(int wx, int wz) const {
    const float x = static_cast<float>(wx), z = static_cast<float>(wz);
    const float c = sampleCont(x, z);
    const float e = sampleEro(x, z);
    const float p = samplePeak(x, z);

    // Ridged peaks in [0,1]: 1 along the noise's zero-crossings (sharp ridgelines),
    // falling to 0 in the cells between, so mountains read as ranges not blobs.
    float ridged = 1.0f - std::fabs(p);
    ridged *= ridged;

    const float amp      = eroSpline_.at(e);       // how mountainous here
    const float mountains = amp * ridged;          // mountain relief (blocks)

    float rel;
    if (islandEnabled_) {
        // ONE big island: a radial mask, 1 in the interior falling to 0 past the
        // radius, with a noise-warped distance so the coastline is irregular. Inside
        // the mask the land has a base height + gentle continentalness hills + the
        // mountains (which fade toward the coast); outside it sinks to deep ocean.
        // Shape the island from a SUM OF MULTIPLE NOISES so it's organic, not round:
        // (1) DOMAIN-WARP the sample point by a two-scale noise vector (big lobes +
        //     smaller wobble) — this bends the radial field into arms/peninsulas;
        // (2) perturb the radial DISTANCE by another two-scale blend — big bays +
        //     small inlets in the coastline. Both scale with islandCoastWarp_.
        const float warp = islandCoastWarp_;
        // (a) BIG LOBES: a LOW-frequency, LARGE-amplitude domain warp (amplitude
        //     proportional to the island radius) bends the radial field into arms and
        //     peninsulas, so the island is organically lobed instead of a disc. A
        //     second mid-frequency octave breaks the lobes up so they aren't uniform.
        const float lobeAmp = islandRadius_ * 0.62f;
        const float lobeX = eroNoise_.fbm((x + 9000.0f) * 0.00068f, (z - 4200.0f) * 0.00068f, 2) +
                            peakNoise_.fbm((x + 15000.0f) * 0.00150f, (z + 2600.0f) * 0.00150f, 2) * 0.45f;
        const float lobeZ = peakNoise_.fbm((x - 7000.0f) * 0.00068f, (z + 6100.0f) * 0.00068f, 2) +
                            humNoise_.fbm((x - 12000.0f) * 0.00150f, (z - 8800.0f) * 0.00150f, 2) * 0.45f;
        // (b) FINE WOBBLE: the original higher-frequency warp adds smaller coves/capes.
        const float wxn = eroNoise_.fbm((x + 1200.0f) * 0.0021f, (z - 800.0f) * 0.0021f, 3) * 0.72f +
                          humNoise_.fbm((x + 600.0f) * 0.0061f, (z + 300.0f) * 0.0061f, 2) * 0.28f;
        const float wzn = peakNoise_.fbm((x - 2600.0f) * 0.0021f, (z + 1700.0f) * 0.0021f, 3) * 0.72f +
                          tempNoise_.fbm((x - 900.0f) * 0.0061f, (z - 1500.0f) * 0.0061f, 2) * 0.28f;
        const float dx = (x + lobeX * lobeAmp + wxn * warp) - islandCx_;
        const float dz = (z + lobeZ * lobeAmp + wzn * warp) - islandCz_;
        float d = std::sqrt(dx * dx + dz * dz);
        // (c) BIG BAYS: a low-frequency perturbation of the radial DISTANCE carves
        //     large bays and pushes out capes; coast_warp adds the finer inlets.
        const float bayAmp = islandRadius_ * 0.36f;
        const float bigCoast = contNoise_.fbm((x + 12000.0f) * 0.00088f, (z - 9000.0f) * 0.00088f, 2);
        const float coast = contNoise_.fbm((x + 5000.0f) * 0.0016f, (z - 5000.0f) * 0.0016f, 3) * 0.6f +
                            coastNoise_.fbm((x - 3300.0f) * 0.0044f, (z + 2100.0f) * 0.0044f, 3) * 0.4f;
        d += bigCoast * bayAmp + coast * warp;
        // Radial rise multiplier `m` (1 at the core → 0 in open ocean). Authored
        // `island_profile_spline` lets you DRAW this coast→core slope (x = d/radius:
        // 0=core, 1=coast); else the built-in flat-core + smoothstep runs (byte-identical).
        float m;
        if (!profileSpline_.empty()) {
            const float u = std::clamp(d / std::max(1.0f, islandRadius_), 0.0f, 1.0f);
            m = std::clamp(profileSpline_.at(u), 0.0f, 1.0f);
        } else {
            const float inner = islandRadius_ * islandInner_;
            const float t = std::clamp((d - inner) / std::max(1.0f, islandRadius_ - inner), 0.0f, 1.0f);
            m = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep: 1 inside, 0 outside
        }
        // Smooth radial rise toward the core (peak_height·m) so the land climbs from
        // the coast up to a central highland/peak plateau — this is what makes the
        // elevation biome bands (beach→forest→highlands→peaks) read as concentric
        // rings. The ridged `mountains` then add the sharp crests on the high core.
        const float landRel = islandLandBase_ + islandPeakHeight_ * m +
                              c * islandInteriorVar_ + mountains * m;
        rel = islandDeepOcean_ + (landRel - islandDeepOcean_) * m; // mix toward ocean
    } else {
        rel = contSpline_.at(c) + mountains;       // archipelago: continentalness spline
    }

    const int h = seaLevel_ + static_cast<int>(std::lround(rel));
    return std::clamp(h, 1, worldHeight_ - 1);
}

int TerrainGenerator::height(int wx, int wz) const {
    return std::clamp(shapeHeight(wx, wz), 1, worldHeight_ - 1);
}

int TerrainGenerator::overhangReach() const {
    return density3DEnabled_ ? maxFloatReach_ : 0;  // max over biomes: don't clip tall floats
}

// Main-terrain solidity (overhangs/cliffs; NO floating islands) — the body of the
// land. surfaceY() scans this to find the walkable ground top.
const BiomeDef& TerrainGenerator::selectBiomeAt(int wx, int wz) const {
    const int sh  = std::clamp(shapeHeight(wx, wz), 1, worldHeight_ - 1);
    return selectBiome(wx, wz, sh - seaLevel_);
}

// The global terrain3d/coast knobs as a BiomeParams (the no-override default).
BiomeParams TerrainGenerator::globalParams() const {
    return {densityAmp_, density3DEnabled_, floatThresh_, floatReach_,
            coastFlatten_, coastFlattenRange_, coastFlattenMin_};
}

// This column's terrain3d/coast knobs, each = the biome's override (if authored) else
// the global value. Selection reads the BASE heightmap (3D modulation never moves it,
// so no feedback loop).
BiomeParams TerrainGenerator::biomeParamsRaw(int wx, int wz) const {
    const BiomeDef& b = selectBiomeAt(wx, wz);
    BiomeParams p;
    p.amp         = b.amp3d       >= 0.0f ? b.amp3d        : densityAmp_;
    p.en3d        = b.en3d        >= 0    ? (b.en3d != 0)  : density3DEnabled_;
    p.floatThresh = b.floatThresh >= 0.0f ? b.floatThresh  : floatThresh_;
    p.floatReach  = b.floatReach  >= 0    ? b.floatReach   : floatReach_;
    p.coastOn     = b.coastOn     >= 0    ? (b.coastOn != 0): coastFlatten_;
    p.coastRange  = b.coastRange  >= 0.0f ? b.coastRange    : coastFlattenRange_;
    p.coastMin    = b.coastMin    >= 0.0f ? b.coastMin      : coastFlattenMin_;
    return p;
}

// Blended per-biome params at a column. Fast path (no overrides) → the globals, so the
// default world is byte-identical. Else a 3x3 box at ±blend smooths biome borders
// (pure fn of coords → seam-safe): scalars average, the two booleans average their 0/1
// and threshold at the midpoint. Cached per column.
BiomeParams TerrainGenerator::biomeParamsAt(int wx, int wz) const {
    if (!anyAmpOverride_) return globalParams();
    struct Cache { uint32_t epoch; int x, z; BiomeParams v; bool valid; };
    static thread_local Cache c{0, 0, 0, {}, false};
    if (c.valid && c.epoch == densityEpoch_ && c.x == wx && c.z == wz) return c.v;
    BiomeParams p;
    const int b = biomeBlendCells_;
    if (b <= 0) {
        p = biomeParamsRaw(wx, wz);
    } else {
        float amp = 0, ft = 0, fr = 0, cr = 0, cm = 0, en = 0, co = 0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const BiomeParams q = biomeParamsRaw(wx + dx * b, wz + dz * b);
                amp += q.amp; ft += q.floatThresh; fr += static_cast<float>(q.floatReach);
                cr += q.coastRange; cm += q.coastMin;
                en += q.en3d ? 1.0f : 0.0f; co += q.coastOn ? 1.0f : 0.0f;
            }
        p.amp = amp / 9.0f; p.floatThresh = ft / 9.0f;
        p.floatReach = static_cast<int>(std::lround(fr / 9.0f));
        p.coastRange = cr / 9.0f; p.coastMin = cm / 9.0f;
        p.en3d = en >= 4.5f; p.coastOn = co >= 4.5f;
    }
    c = {densityEpoch_, wx, wz, p, true};
    return p;
}

float TerrainGenerator::densityAmpAt(int wx, int wz) const {
    return anyAmpOverride_ ? biomeParamsAt(wx, wz).amp : densityAmp_;
}

// The 3D density stack for a column: the biome's own override stack if it authored one,
// else the global. Nearest-biome (no 3D pattern blend — the per-biome amplitude blend
// softens the magnitude across borders); only consulted when an override exists.
const NoiseStack* TerrainGenerator::densityStackAt(int wx, int wz) const {
    struct Cache { uint32_t epoch; int x, z; const NoiseStack* s; bool valid; };
    static thread_local Cache c{0, 0, 0, nullptr, false};
    if (c.valid && c.epoch == densityEpoch_ && c.x == wx && c.z == wz) return c.s;
    const BiomeDef& b = selectBiomeAt(wx, wz);
    const NoiseStack* s = b.hasDensityStack ? &b.densityStack
                        : (densityStack_.empty() ? nullptr : &densityStack_);
    c = {densityEpoch_, wx, wz, s, true};
    return s;
}

bool TerrainGenerator::mainSolid(int surfaceH, int wx, int wy, int wz) const {
    const BiomeParams p = biomeParamsAt(wx, wz);   // == globals when no biome overrides
    const float grad = static_cast<float>(surfaceH - wy);
    if (!p.en3d) return grad >= 0.0f;  // this biome turns 3D off: pure heightmap body
    const float ampB = p.amp;
    if (grad > ampB)  return true;  // well below the surface: always solid
    if (grad < -ampB) return false; // well above the band: air (floats handled elsewhere)
    const float fx = static_cast<float>(wx), fy = static_cast<float>(wy),
                fz = static_cast<float>(wz);
    const float n = densityInterp_ ? densityInterpolated(fx, fy, fz) : rawDensity(fx, fy, fz);
    // Coast flatten: damp the 3D swell near sea level so the shoreline/beach reads as a
    // smooth gentle slope, while inland (and the deep sea floor) keep the full relief.
    float amp = ampB;
    if (p.coastOn) {
        const float rel = std::fabs(static_cast<float>(surfaceH - seaLevel_));
        const float a = std::clamp(rel / std::max(1.0f, p.coastRange), 0.0f, 1.0f);
        amp *= p.coastMin + (1.0f - p.coastMin) * a;
    }
    return grad + n * amp > 0.0f;
}

// The 3D density perturbation: the authored `terrain3d.density` blend, or the scalar
// fbm fallback when no `density.layers:` was authored. When a biome authors its own
// density stack the column resolves to that one instead (densityStackAt); the default
// (no per-biome stack) path is byte-identical.
float TerrainGenerator::rawDensity(float fx, float fy, float fz) const {
    const NoiseStack* stk = anyDensityStackOverride_
        ? densityStackAt(static_cast<int>(std::lround(fx)), static_cast<int>(std::lround(fz)))
        : (densityStack_.empty() ? nullptr : &densityStack_);
    return (stk && !stk->empty())
        ? stk->value(fx, fy, fz)
        : densityNoise_.fbm((fx + 26100.0f) * densityFreqXZ_, (fy + 9400.0f) * densityFreqY_,
                            (fz + 14700.0f) * densityFreqXZ_, densityOct_);
}

// One lattice-corner density, memoised in a fixed-size per-thread direct-mapped
// cache. Each worker keeps its own cache (no locking, no races); entries store the
// exact (epoch, lx, ly, lz) discriminator so a hash collision is a clean miss, not
// a wrong value — output stays a deterministic, visit-order-independent function.
float TerrainGenerator::densityCorner(int lx, int ly, int lz) const {
    constexpr uint32_t kBits = 15;            // 32768 slots
    constexpr uint32_t kMask = (1u << kBits) - 1;
    struct Slot { uint32_t epoch; int32_t x, y, z; float v; };
    static thread_local std::vector<Slot> cache; // ~640 KiB/thread, lazily sized
    if (cache.empty()) {
        cache.assign(static_cast<size_t>(kMask) + 1, Slot{0, 0, 0, 0, 0.0f});
    }
    // Spatial hash → slot. Odd multipliers spread neighbouring corners apart.
    const uint32_t h = (static_cast<uint32_t>(lx) * 73856093u) ^
                       (static_cast<uint32_t>(ly) * 19349663u) ^
                       (static_cast<uint32_t>(lz) * 83492791u);
    Slot& s = cache[h & kMask];
    if (s.epoch == densityEpoch_ && s.x == lx && s.y == ly && s.z == lz) {
        return s.v; // hit
    }
    const float v = rawDensity(static_cast<float>(lx), static_cast<float>(ly),
                               static_cast<float>(lz));
    s = {densityEpoch_, lx, ly, lz, v};
    return v;
}

// Trilinear interpolation of rawDensity over a world-aligned lattice. Sampling
// only the 8 surrounding corners (each cached + shared across the cell's voxels and
// neighbouring columns) replaces the per-voxel noise eval with eight cache lookups
// and a lerp — the REVIEW O6 win. World-aligned so neighbouring chunks read the
// same corners (seam-consistent); chunk boundaries (multiples of 16) land on lattice
// points whenever the cell size divides 16 (4×8×4 does).
float TerrainGenerator::densityInterpolated(float fx, float fy, float fz) const {
    const int wx = static_cast<int>(std::floor(fx));
    const int wy = static_cast<int>(std::floor(fy));
    const int wz = static_cast<int>(std::floor(fz));
    auto floorCell = [](int v, int c) { return (v >= 0 ? v / c : -((-v + c - 1) / c)) * c; };
    const int x0 = floorCell(wx, latX_), x1 = x0 + latX_;
    const int y0 = floorCell(wy, latY_), y1 = y0 + latY_;
    const int z0 = floorCell(wz, latZ_), z1 = z0 + latZ_;
    const float tx = static_cast<float>(wx - x0) / static_cast<float>(latX_);
    const float ty = static_cast<float>(wy - y0) / static_cast<float>(latY_);
    const float tz = static_cast<float>(wz - z0) / static_cast<float>(latZ_);

    const float c000 = densityCorner(x0, y0, z0), c100 = densityCorner(x1, y0, z0);
    const float c010 = densityCorner(x0, y1, z0), c110 = densityCorner(x1, y1, z0);
    const float c001 = densityCorner(x0, y0, z1), c101 = densityCorner(x1, y0, z1);
    const float c011 = densityCorner(x0, y1, z1), c111 = densityCorner(x1, y1, z1);

    const float c00 = c000 + (c100 - c000) * tx, c10 = c010 + (c110 - c010) * tx;
    const float c01 = c001 + (c101 - c001) * tx, c11 = c011 + (c111 - c011) * tx;
    const float c0 = c00 + (c10 - c00) * ty, c1 = c01 + (c11 - c01) * ty;
    return c0 + (c1 - c0) * tz;
}

// The actual 3D GROUND surface Y at a column — the topmost main-terrain solid cell
// (ignoring floating islands), scanning down from the top of the density band. With
// the density raising/lowering the land by up to `amplitude`, the heightmap height()
// is NOT the surface; features (trees/plants) must sit on THIS instead.
int TerrainGenerator::surfaceY(int wx, int wz) const {
    const int hh = height(wx, wz);
    if (!density3DEnabled_) return hh;
    const int hi = std::min(worldHeight_ - 1, hh + static_cast<int>(maxAmp_) + 1);
    for (int y = hi; y >= 1; --y) {
        if (mainSolid(hh, wx, y, wz)) return y;
    }
    return 1;
}

bool TerrainGenerator::isSolid(int surfaceH, int wx, int wy, int wz) const {
    if (!density3DEnabled_) {
        return wy <= surfaceH; // pure-heightmap fallback (worldgen v1 behaviour)
    }
    return mainSolid(surfaceH, wx, wy, wz) || floatSolid(surfaceH, wx, wy, wz);
}

bool TerrainGenerator::mainTerrainSolid(int surfaceH, int wx, int wy, int wz) const {
    if (!density3DEnabled_) {
        return wy <= surfaceH;
    }
    return mainSolid(surfaceH, wx, wy, wz);
}

float TerrainGenerator::terrainDensity(int wx, int wy, int wz) const {
    const int surfaceH = height(wx, wz);
    const float grad = static_cast<float>(surfaceH - wy);
    if (!density3DEnabled_) return grad; // pure heightmap: signed distance to surface
    float amp = densityAmp_;
    if (anyAmpOverride_) {
        const BiomeParams p = biomeParamsAt(wx, wz);
        if (!p.en3d) return grad;        // biome turns 3D off: heightmap signed distance
        amp = p.amp;
    }
    // grad + n·amp — exactly mainSolid()'s test quantity (its ±amp early-outs only
    // skip the noise eval; the value here is continuous and sign-consistent).
    const float n = densityInterp_
        ? densityInterpolated(static_cast<float>(wx), static_cast<float>(wy),
                              static_cast<float>(wz))
        : rawDensity(static_cast<float>(wx), static_cast<float>(wy),
                     static_cast<float>(wz));
    return grad + n * amp;
}

int TerrainGenerator::surfaceScanTop(int surfaceH) const {
    if (!density3DEnabled_) {
        // surfaceY() is just the (clamped) heightmap height in 2D mode; the scan
        // from here lands on it immediately since every cell at/below it is solid.
        return std::min(worldHeight_ - 1, std::max(1, surfaceH));
    }
    return std::min(worldHeight_ - 1, surfaceH + static_cast<int>(maxAmp_) + 1);
}

bool TerrainGenerator::floatSolid(int surfaceH, int wx, int wy, int wz) const {
    const BiomeParams p = biomeParamsAt(wx, wz);  // == globals when no biome overrides
    if (!p.en3d) {
        return false;  // global 3D off, or this biome turns it off
    }
    const int floatReach = p.floatReach;
    const float floatThresh = p.floatThresh;
    const float fx = static_cast<float>(wx), fy = static_cast<float>(wy),
                fz = static_cast<float>(wz);
    // Floating islands: solid blobs well above the surface, from a WEIGHTED SUM OF
    // TWO FREQUENCIES (big island masses + smaller satellites) so they come in
    // diverse sizes — the Y axis is squashed (×1.8) so isles are flatter than wide.
    // The threshold tightens with altitude so they thin out toward the top.
    if (wy >= surfaceH + floatGap_ && wy <= surfaceH + floatReach) {
        float f;
        if (floatStack_.empty()) {
            // Large domain shifts keep the field off the noise lattice ORIGIN at world
            // (0,0,0) — Perlin is exactly 0 there, which would erase islands at spawn.
            const float big = floatNoise_.fbm((fx + 21300.0f) * floatFreq_,
                                              (fy + 8100.0f) * floatFreq_ * 1.8f,
                                              (fz + 17600.0f) * floatFreq_, 2);
            const float small = floatNoise_.fbm((fx + 41700.0f) * floatFreq_ * 2.3f,
                                                (fy + 8100.0f) * floatFreq_ * 2.3f * 1.8f,
                                                (fz + 33300.0f) * floatFreq_ * 2.3f, 2);
            f = big * 0.66f + small * 0.34f;
        } else {
            f = floatStack_.value(fx, fy, fz);
        }
        const float alt = static_cast<float>(wy - surfaceH - floatGap_) /
                          static_cast<float>(std::max(1, floatReach - floatGap_));
        if (f > floatThresh + alt * 0.14f) {
            return true;
        }
    }
    return false;
}

// Field samplers: use the authored NoiseStack blend when present, else the legacy
// single-fbm scalar path (byte-identical to the original code, so worlds without a
// `layers:` block are unchanged). All take raw world coords.
float TerrainGenerator::sampleCont(float x, float z) const {
    return contStack_.empty() ? contNoise_.fbm(x * contFreq_, z * contFreq_, contOct_)
                              : contStack_.value(x, z);
}
float TerrainGenerator::sampleEro(float x, float z) const {
    return eroStack_.empty() ? eroNoise_.fbm(x * eroFreq_, z * eroFreq_, eroOct_)
                             : eroStack_.value(x, z);
}
float TerrainGenerator::samplePeak(float x, float z) const {
    return peakStack_.empty() ? peakNoise_.fbm(x * peakFreq_, z * peakFreq_, peakOct_)
                              : peakStack_.value(x, z);
}
float TerrainGenerator::sampleTemp(float x, float z) const {
    return tempStack_.empty() ? tempNoise_.fbm(x * tempFreq_, z * tempFreq_, climOct_)
                              : tempStack_.value(x, z);
}
float TerrainGenerator::sampleHum(float x, float z) const {
    return humStack_.empty() ? humNoise_.fbm(x * humFreq_, z * humFreq_, climOct_)
                             : humStack_.value(x, z);
}

float TerrainGenerator::fieldValue(Field f, int wx, int wz) const {
    const float x = static_cast<float>(wx), z = static_cast<float>(wz);
    switch (f) {
        case Field::Continentalness: return sampleCont(x, z);
        case Field::Erosion:         return sampleEro(x, z);
        case Field::Peaks:           return samplePeak(x, z);
        case Field::Temperature:     return sampleTemp(x, z);
        case Field::Humidity:        return sampleHum(x, z);
        case Field::Relief:          return static_cast<float>(shapeHeight(wx, wz) - seaLevel_);
        case Field::Height:          return static_cast<float>(height(wx, wz));
    }
    return 0.0f;
}

int TerrainGenerator::selectBiomeIndex(int wx, int wz, int relHeight) const {
    if (forcedBiome_ >= 0 && forcedBiome_ < static_cast<int>(biomes_.size()))
        return forcedBiome_;                                 // preview "solo biome" mode
    const float fx = static_cast<float>(wx), fz = static_cast<float>(wz);
    for (size_t i = 0; i < biomes_.size(); ++i) {
        const BiomeDef& b = biomes_[i];
        // Pure elevation band + optional noise selection mask (climate removed). The
        // biome wins only where rel is in its band AND its mask passes — so a masked
        // biome carves patches out of a later catch-all in the same band.
        if (relHeight >= b.relMin && relHeight <= b.relMax &&
            (b.selMask.empty() || b.selMask.weight(fx, fz) >= 0.5f)) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(biomes_.size()) - 1; // catch-all (authored last)
}

const BiomeDef& TerrainGenerator::selectBiome(int wx, int wz, int relHeight) const {
    return biomes_[static_cast<size_t>(selectBiomeIndex(wx, wz, relHeight))];
}

ColumnInfo TerrainGenerator::columnInfo(int wx, int wz) const {
    const float x = static_cast<float>(wx), z = static_cast<float>(wz);
    ColumnInfo ci;
    ci.height     = std::clamp(shapeHeight(wx, wz), 1, worldHeight_ - 1);
    ci.waterLevel = seaLevel_;
    const int rel = ci.height - seaLevel_;

    const int index = selectBiomeIndex(wx, wz, rel);
    const BiomeDef& b = biomes_[static_cast<size_t>(index)];
    ci.biome    = index;
    // Per-column pick from the biome's weighted palette (single-entry → that block).
    ci.topId    = pickBlock(b.top,    hash01(wx, wz, seed_ ^ 0x70905u));
    ci.fillerId = pickBlock(b.filler, hash01(wx, wz, seed_ ^ 0xF11e5u));
    // Noise-mask surface patches: first mask that passes (weight >= 0.5) overrides the
    // surface block. Empty list → no cost. Water/snow below still take precedence.
    for (const BiomeDef::BlockMask& sm : b.surfaceMasks) {
        if (sm.mask.weight(x, z) >= 0.5f) { ci.topId = sm.id; break; }
    }
    ci.treeDensity = b.treeDensity;
    ci.treeKind    = b.tree;
    ci.vegTint     = b.vegTint;

    if (ci.height < ci.waterLevel) {
        // Submerged (ocean): floor block, nothing grows.
        ci.topId = oceanFloorId_;
        ci.fillerId = oceanFillerId_;
        ci.treeDensity = 0.0f; // nothing grows underwater (no far-terrain impostors)
    } else if (b.snow || rel > snowLineRel_) {
        ci.topId = snowId_; // cold biome, or above the snow line on any peak
        // Trees stay — pine can still root through the snow cap on the peaks.
    }
    return ci;
}

} // namespace vg
