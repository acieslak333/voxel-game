#include "world/TerrainGenerator.h"

#include "world/BlockRegistry.h"

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
// Floor division (correct for negatives) — for the coarse lake-placement grid.
int floordiv(int a, int b) {
    const int q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

// Deterministic [0,1) hash of an integer cell + salt (lake placement).
float hash01(int x, int z, uint32_t salt) {
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^
                 static_cast<uint32_t>(z) * 0xd8163841u ^ (salt * 0x9e3779b9u);
    h ^= h >> 16; h *= 0x7feb352du;
    h ^= h >> 15; h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000);
}

// Parse an optional `layers:` sequence under a field node into a NoiseStack. Each
// list entry is a layer: {type, frequency, octaves, lacunarity, gain, weight,
// offset:[x,z]}. Returns an empty stack (and leaves it unused) if there is no
// `layers:` node, so a plain `{frequency, octaves}` field keeps the legacy path.
// baseSeed salts each layer's own noise so the layers are independent.
NoiseStack loadStack(const YAML::Node& fieldNode, uint32_t baseSeed) {
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
        // Salt each layer so its noise is decorrelated from the others and from the
        // field's legacy scalar noise (which uses a different seed derivation).
        stack.addLayer(L, baseSeed * 2246822519u + i * 0x9e3779b9u + 0x5bd1e995u);
        ++i;
    }
    return stack;
}

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
} // namespace

TerrainGenerator::TerrainGenerator(uint32_t seed, const BlockRegistry& registry,
                                   const std::string& assetDir, int worldHeight)
    : worldHeight_(worldHeight),
      contNoise_(seed ^ 0x9e3779b9u),
      eroNoise_(seed * 2654435761u + 0x1234u),
      peakNoise_(seed * 40503u + 0x77u),
      tempNoise_(seed ^ 0xC0FFEEu),
      humNoise_(seed * 668265263u + 0x55u),
      riverNoise_(seed * 2246822519u + 0x99u),
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
                   float trees, float bushes) {
        BiomeDef b;
        b.name = name;
        b.tempMin = t0; b.tempMax = t1;
        b.humMin = h0;  b.humMax = h1;
        b.relMin = r0;  b.relMax = r1;
        b.topId = resolveBlock(registry, top, "grass");
        b.fillerId = resolveBlock(registry, filler, "dirt");
        b.snow = snow;
        b.treeDensity = trees;
        b.bushDensity = bushes;
        biomes_.push_back(b);
    };
    //   name        temp        humidity     rel-elev      top      filler   snow  trees  bushes
    add("beach",    -2,2,       -2,2,        -4, 2,        "sand",  "sand",  false, 0.0f,  0.0f);
    add("mountain", -2,2,       -2,2,        34, 10000,    "stone", "stone", false, 0.003f,0.0f);
    add("desert",    0.35f,2,   -2,-0.15f,    2, 10000,    "sand",  "sand",  false, 0.0f,  0.012f);
    add("savanna",   0.2f,2,    -0.15f,0.3f,  2, 10000,    "grass", "dirt",  false, 0.006f,0.05f);
    add("snowy",    -2,-0.35f,  -2,2,         2, 10000,    "grass", "dirt",  true,  0.012f,0.02f);
    add("forest",   -0.35f,0.6f, 0.2f,2,      2, 10000,    "grass", "dirt",  false, 0.055f,0.05f);
    add("plains",   -2,2,       -2,2,        -10000,10000, "grass", "dirt",  false, 0.018f,0.05f);

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
    // tools (tools/genmap_tool.py) write toggles as 0/1 sliders, so a tuned
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
    getF(root["rivers"], "frequency", riverFreq_);
    getF(root["rivers"], "width", riverWidth_);
    getI(root["rivers"], "depth", riverDepth_);
    getI(root["rivers"], "max_elevation", riverMaxRel_);
    getI(root["lakes"], "spacing", lakeSpacing_);
    getF(root["lakes"], "chance", lakeChance_);
    getI(root["lakes"], "radius_min", lakeRadiusMin_);
    getI(root["lakes"], "radius_max", lakeRadiusMax_);
    getI(root["lakes"], "depth", lakeDepth_);
    getI(root["lakes"], "min_elevation", lakeMinRel_);
    getI(root["lakes"], "max_elevation", lakeMaxRel_);
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

    // Optional data-driven noise stacks: a `<field>.layers:` sequence replaces the
    // single-fbm sample with a weighted blend. Absent -> empty stack -> legacy path.
    contStack_  = loadStack(root["continentalness"], seed_ ^ 0x9e3779b9u);
    eroStack_   = loadStack(root["erosion"],         seed_ * 2654435761u + 0x1234u);
    peakStack_  = loadStack(root["peaks"],           seed_ * 40503u + 0x77u);
    tempStack_  = loadStack(root["temperature"],     seed_ ^ 0xC0FFEEu);
    humStack_   = loadStack(root["humidity"],        seed_ * 668265263u + 0x55u);
    riverStack_ = loadStack(root["rivers"],          seed_ * 2246822519u + 0x99u);

    // Worldgen v2: 3D volumetric terrain knobs (overhangs / floating islands). The
    // density + float fields can each be a weighted NoiseStack via nested `layers:`.
    if (const YAML::Node t3 = root["terrain3d"]) {
        density3DEnabled_ = asBool(t3["enabled"], true);
        getF(t3, "freq_xz", densityFreqXZ_);
        getF(t3, "freq_y", densityFreqY_);
        getI(t3, "octaves", densityOct_);
        getF(t3, "amplitude", densityAmp_);
        getF(t3, "float_freq", floatFreq_);
        getF(t3, "float_threshold", floatThresh_);
        getI(t3, "float_gap", floatGap_);
        getI(t3, "float_reach", floatReach_);
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
            auto range = [&](const char* key, float& lo, float& hi) {
                if (bn[key] && bn[key].IsSequence() && bn[key].size() == 2) {
                    lo = bn[key][0].as<float>();
                    hi = bn[key][1].as<float>();
                }
            };
            range("temp", b.tempMin, b.tempMax);
            range("humidity", b.humMin, b.humMax);
            if (bn["elevation"] && bn["elevation"].IsSequence() && bn["elevation"].size() == 2) {
                b.relMin = bn["elevation"][0].as<int>();
                b.relMax = bn["elevation"][1].as<int>();
            }
            b.topId    = resolveBlock(registry, bn["top"] ? bn["top"].as<std::string>() : "grass", "grass");
            b.fillerId = resolveBlock(registry, bn["filler"] ? bn["filler"].as<std::string>() : "dirt", "dirt");
            b.snow        = asBool(bn["snow"], false);
            b.treeDensity = bn["trees"] ? bn["trees"].as<float>() : 0.0f;
            b.bushDensity = bn["bushes"] ? bn["bushes"].as<float>() : 0.0f;
            if (bn["plant"]) {
                const std::string p = bn["plant"].as<std::string>();
                b.plant = p == "none"   ? FloraKind::None
                        : p == "grass"  ? FloraKind::GrassFlower
                        : p == "forest" ? FloraKind::Forest
                        : p == "desert" ? FloraKind::Desert
                        : FloraKind::Bush;
            }
            if (bn["tree"]) {
                const std::string t = bn["tree"].as<std::string>();
                b.tree = t == "birch"  ? TreeKind::Birch
                       : t == "pine"   ? TreeKind::Pine
                       : t == "maple"  ? TreeKind::Maple
                       : t == "willow" ? TreeKind::Willow
                       : TreeKind::Oak;
            }
            if (const YAML::Node tn = bn["tint"]; tn && tn.IsSequence() && tn.size() >= 3) {
                b.vegTint = {tn[0].as<float>(), tn[1].as<float>(), tn[2].as<float>()};
            }
            loaded.push_back(b);
        }
        if (!loaded.empty()) {
            biomes_ = std::move(loaded);
        }
    }

    // Guard against config values that would divide by zero / blow up.
    lakeSpacing_ = std::max(16, lakeSpacing_);
    lakeRadiusMax_ = std::max(lakeRadiusMin_, lakeRadiusMax_);
    riverWidth_ = std::max(0.001f, riverWidth_);

    maxTreeDensity_ = 0.0f;
    for (const BiomeDef& b : biomes_) {
        maxTreeDensity_ = std::max(maxTreeDensity_, b.treeDensity);
    }
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
                            riverNoise_.fbm((x - 3300.0f) * 0.0044f, (z + 2100.0f) * 0.0044f, 3) * 0.4f;
        d += bigCoast * bayAmp + coast * warp;
        const float inner = islandRadius_ * islandInner_;
        float t = std::clamp((d - inner) / std::max(1.0f, islandRadius_ - inner), 0.0f, 1.0f);
        const float m = 1.0f - t * t * (3.0f - 2.0f * t); // smoothstep: 1 inside, 0 outside
        const float landRel = islandLandBase_ + c * islandInteriorVar_ + mountains * m;
        rel = islandDeepOcean_ + (landRel - islandDeepOcean_) * m; // mix toward ocean
    } else {
        rel = contSpline_.at(c) + mountains;       // archipelago: continentalness spline
    }

    int h = seaLevel_ + static_cast<int>(std::lround(rel));

    // Rivers: where the river noise is near zero, carve a winding channel toward
    // sea level — but only in lowlands, so mountains keep their relief instead of
    // growing deep canals. The channel fills with water via the sea-level fill.
    if (h - seaLevel_ < riverMaxRel_) {
        const float r = sampleRiver(x, z);
        const float t = std::clamp(1.0f - std::fabs(r) / riverWidth_, 0.0f, 1.0f);
        if (t > 0.0f) {
            const int bed = seaLevel_ - 1 - static_cast<int>(t * t * riverDepth_);
            h = std::min(h, bed);
        }
    }
    return std::clamp(h, 1, worldHeight_ - 1);
}

TerrainGenerator::LakeInfo TerrainGenerator::lakeAt(int wx, int wz) const {
    // Coarse candidate-lake grid: check the 3x3 cells around this column for a lake
    // whose disc covers it. Lake parameters (centre, radius, level) are deterministic
    // per cell, so this is a pure function of world coords.
    const int gx0 = floordiv(wx, lakeSpacing_), gz0 = floordiv(wz, lakeSpacing_);
    for (int gz = gz0 - 1; gz <= gz0 + 1; ++gz) {
        for (int gx = gx0 - 1; gx <= gx0 + 1; ++gx) {
            if (hash01(gx, gz, seed_ ^ 0x1a4eu) >= lakeChance_) {
                continue;
            }
            const int cxw = gx * lakeSpacing_ +
                            static_cast<int>(hash01(gx, gz, seed_ ^ 0x1u) * lakeSpacing_);
            const int czw = gz * lakeSpacing_ +
                            static_cast<int>(hash01(gx, gz, seed_ ^ 0x2u) * lakeSpacing_);
            const int rad = lakeRadiusMin_ +
                            static_cast<int>(hash01(gx, gz, seed_ ^ 0x3u) *
                                             (lakeRadiusMax_ - lakeRadiusMin_));
            const long dx = wx - cxw, dz = wz - czw;
            const long d2 = dx * dx + dz * dz;
            if (d2 > static_cast<long>(rad) * rad) {
                continue;
            }
            const int level = shapeHeight(cxw, czw); // water surface = land at centre
            const int rel = level - seaLevel_;
            if (rel < lakeMinRel_ || rel > lakeMaxRel_) {
                continue; // only perch lakes on land in the allowed band
            }
            // Bowl: deepest at the centre, rising to the rim, so it holds water.
            const float d = std::sqrt(static_cast<float>(d2)) / static_cast<float>(rad);
            const int bed = level - 1 - static_cast<int>((1.0f - d) * lakeDepth_);
            return {true, level, bed};
        }
    }
    return {false, 0, 0};
}

int TerrainGenerator::height(int wx, int wz) const {
    const int sh = shapeHeight(wx, wz);
    const LakeInfo lk = lakeAt(wx, wz);
    return lk.in ? std::min(sh, lk.bed) : sh;
}

int TerrainGenerator::overhangReach() const {
    return density3DEnabled_ ? floatReach_ : 0;
}

// Main-terrain solidity (overhangs/cliffs; NO floating islands) — the body of the
// land. surfaceY() scans this to find the walkable ground top.
bool TerrainGenerator::mainSolid(int surfaceH, int wx, int wy, int wz) const {
    const float grad = static_cast<float>(surfaceH - wy);
    if (grad > densityAmp_)  return true;  // well below the surface: always solid
    if (grad < -densityAmp_) return false; // well above the band: air (floats handled elsewhere)
    const float fx = static_cast<float>(wx), fy = static_cast<float>(wy),
                fz = static_cast<float>(wz);
    const float n = densityInterp_ ? densityInterpolated(fx, fy, fz) : rawDensity(fx, fy, fz);
    return grad + n * densityAmp_ > 0.0f;
}

// The raw (exact) density perturbation: the authored NoiseStack blend, or the
// scalar fbm fallback when no `density.layers:` was authored.
float TerrainGenerator::rawDensity(float fx, float fy, float fz) const {
    return densityStack_.empty()
        ? densityNoise_.fbm((fx + 26100.0f) * densityFreqXZ_, (fy + 9400.0f) * densityFreqY_,
                            (fz + 14700.0f) * densityFreqXZ_, densityOct_)
        : densityStack_.value(fx, fy, fz);
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
    const int hi = std::min(worldHeight_ - 1, hh + static_cast<int>(densityAmp_) + 1);
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

int TerrainGenerator::surfaceScanTop(int surfaceH) const {
    if (!density3DEnabled_) {
        // surfaceY() is just the (clamped) heightmap height in 2D mode; the scan
        // from here lands on it immediately since every cell at/below it is solid.
        return std::min(worldHeight_ - 1, std::max(1, surfaceH));
    }
    return std::min(worldHeight_ - 1, surfaceH + static_cast<int>(densityAmp_) + 1);
}

bool TerrainGenerator::floatSolid(int surfaceH, int wx, int wy, int wz) const {
    if (!density3DEnabled_) {
        return false;
    }
    const float fx = static_cast<float>(wx), fy = static_cast<float>(wy),
                fz = static_cast<float>(wz);
    // Floating islands: solid blobs well above the surface, from a WEIGHTED SUM OF
    // TWO FREQUENCIES (big island masses + smaller satellites) so they come in
    // diverse sizes — the Y axis is squashed (×1.8) so isles are flatter than wide.
    // The threshold tightens with altitude so they thin out toward the top.
    if (wy >= surfaceH + floatGap_ && wy <= surfaceH + floatReach_) {
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
                          static_cast<float>(std::max(1, floatReach_ - floatGap_));
        if (f > floatThresh_ + alt * 0.14f) {
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
float TerrainGenerator::sampleRiver(float x, float z) const {
    return riverStack_.empty() ? riverNoise_.fbm(x * riverFreq_, z * riverFreq_, 2)
                               : riverStack_.value(x, z);
}

float TerrainGenerator::fieldValue(Field f, int wx, int wz) const {
    const float x = static_cast<float>(wx), z = static_cast<float>(wz);
    switch (f) {
        case Field::Continentalness: return sampleCont(x, z);
        case Field::Erosion:         return sampleEro(x, z);
        case Field::Peaks:           return samplePeak(x, z);
        case Field::Temperature:     return sampleTemp(x, z);
        case Field::Humidity:        return sampleHum(x, z);
        case Field::River:           return sampleRiver(x, z);
        case Field::Relief:          return static_cast<float>(shapeHeight(wx, wz) - seaLevel_);
        case Field::Height:          return static_cast<float>(height(wx, wz));
    }
    return 0.0f;
}

const BiomeDef& TerrainGenerator::selectBiome(float temp, float hum, int relHeight) const {
    for (const BiomeDef& b : biomes_) {
        if (temp >= b.tempMin && temp <= b.tempMax && hum >= b.humMin && hum <= b.humMax &&
            relHeight >= b.relMin && relHeight <= b.relMax) {
            return b;
        }
    }
    return biomes_.back(); // catch-all (authored last)
}

ColumnInfo TerrainGenerator::columnInfo(int wx, int wz) const {
    const float x = static_cast<float>(wx), z = static_cast<float>(wz);
    ColumnInfo ci;
    // Shape + lake once (height() would recompute both); a perched lake lowers the
    // terrain into a bowl and raises this column's water surface above sea level.
    const int sh = shapeHeight(wx, wz);
    const LakeInfo lk = lakeAt(wx, wz);
    ci.height     = lk.in ? std::min(sh, lk.bed) : sh;
    ci.waterLevel = lk.in ? lk.level : seaLevel_;
    const int rel = ci.height - seaLevel_;

    // Climate. Temperature cools with altitude so highlands trend snowy.
    float temp = sampleTemp(x, z);
    const float hum = sampleHum(x, z);
    if (rel > 0) {
        temp -= static_cast<float>(rel) * 0.010f;
    }

    int index = 0;
    for (size_t i = 0; i < biomes_.size(); ++i) {
        const BiomeDef& b = biomes_[i];
        if (temp >= b.tempMin && temp <= b.tempMax && hum >= b.humMin && hum <= b.humMax &&
            rel >= b.relMin && rel <= b.relMax) {
            index = static_cast<int>(i);
            break;
        }
        if (i + 1 == biomes_.size()) {
            index = static_cast<int>(i); // catch-all
        }
    }
    const BiomeDef& b = biomes_[static_cast<size_t>(index)];
    ci.biome    = index;
    ci.topId    = b.topId;
    ci.fillerId = b.fillerId;
    ci.treeDensity = b.treeDensity;
    ci.bushDensity = b.bushDensity;
    ci.plantKind   = b.plant;
    ci.treeKind    = b.tree;
    ci.vegTint     = b.vegTint;

    if (ci.height < ci.waterLevel) {
        // Submerged (ocean or under a perched lake): floor block, nothing grows.
        ci.topId = oceanFloorId_;
        ci.fillerId = oceanFillerId_;
        ci.treeDensity = 0.0f;
        ci.bushDensity = 0.0f;
        ci.plantKind   = FloraKind::None;
    } else if (b.snow || rel > snowLineRel_) {
        ci.topId = snowId_; // cold biome, or above the snow line on any peak
    }
    return ci;
}

} // namespace vg
