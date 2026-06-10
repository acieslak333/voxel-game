#include "world/TerrainGenerator.h"

#include "world/BlockRegistry.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>

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
      riverNoise_(seed * 2246822519u + 0x99u) {
    seed_ = seed;
    seaLevel_ = worldHeight_ / 2;
    snowLineRel_ = std::max(20, worldHeight_ / 2 - 9);

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
        islandEnabled_ = is["enabled"] ? is["enabled"].as<bool>() : true;
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
            b.snow        = bn["snow"] ? bn["snow"].as<bool>() : false;
            b.treeDensity = bn["trees"] ? bn["trees"].as<float>() : 0.0f;
            b.bushDensity = bn["bushes"] ? bn["bushes"].as<float>() : 0.0f;
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
    const float c = contNoise_.fbm(x * contFreq_, z * contFreq_, contOct_);
    const float e = eroNoise_.fbm(x * eroFreq_, z * eroFreq_, eroOct_);
    const float p = peakNoise_.fbm(x * peakFreq_, z * peakFreq_, peakOct_);

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
        const float dx = x - islandCx_, dz = z - islandCz_;
        float d = std::sqrt(dx * dx + dz * dz);
        d += contNoise_.fbm((x + 5000.0f) * 0.0016f, (z - 5000.0f) * 0.0016f, 3) *
             islandCoastWarp_;
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
        const float r = riverNoise_.fbm(x * riverFreq_, z * riverFreq_, 2);
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
    float temp = tempNoise_.fbm(x * tempFreq_, z * tempFreq_, climOct_);
    const float hum = humNoise_.fbm(x * humFreq_, z * humFreq_, climOct_);
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

    if (ci.height < ci.waterLevel) {
        // Submerged (ocean or under a perched lake): floor block, nothing grows.
        ci.topId = oceanFloorId_;
        ci.fillerId = oceanFillerId_;
        ci.treeDensity = 0.0f;
        ci.bushDensity = 0.0f;
    } else if (b.snow || rel > snowLineRel_) {
        ci.topId = snowId_; // cold biome, or above the snow line on any peak
    }
    return ci;
}

} // namespace vg
