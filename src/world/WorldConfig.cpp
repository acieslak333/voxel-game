#include "world/WorldConfig.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <iostream>
#include <random>

namespace vg {

WorldConfig WorldConfig::load(const std::string& path) {
    WorldConfig c; // defaults (match assets/world.yaml)
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception&) {
        return c; // missing or malformed: keep defaults
    }
    if (!root.IsMap()) {
        return c;
    }

    // Read one scalar node into `field`, keeping the default on absence/bad value.
    auto get = [](const YAML::Node& node, auto& field) {
        if (node) {
            try {
                field = node.as<std::decay_t<decltype(field)>>();
            } catch (const YAML::Exception&) {
                // ignore a bad value, keep the default
            }
        }
    };

    get(root["seed"], c.seed);
    // A fresh world every launch: when random_seed is on, draw the seed from OS
    // entropy instead of using the fixed `seed` above. (Entropy here is fine — the
    // seed is the *input* to generation; everything downstream stays a pure,
    // deterministic function of it.)
    bool randomSeed = false;
    get(root["random_seed"], randomSeed);
    if (randomSeed) {
        std::random_device rd;
        c.seed = (static_cast<uint32_t>(rd()) << 16) ^ static_cast<uint32_t>(rd());
    }
    std::cout << "[world] seed " << c.seed << (randomSeed ? " (random)\n" : "\n");
    get(root["streaming"], c.streaming);
    get(root["save_dir"], c.saveDir);
    get(root["stream_workers"], c.streamWorkers);
    c.streamWorkers = std::max(0, c.streamWorkers);
    get(root["async_streaming"], c.asyncStreaming);
    get(root["view_radius"], c.viewRadius);
    get(root["height_chunks"], c.heightChunks);
    // Derive the grid World builds from the streaming window. (Backstop the radius
    // at >= 0 so a bad value still yields a 1-chunk world rather than a crash.)
    c.viewRadius = std::max(0, c.viewRadius);
    c.chunksX = c.chunksZ = 2 * c.viewRadius + 1;
    c.chunksY = std::max(1, c.heightChunks);
    if (const YAML::Node str = root["stream_tuning"]) {
        get(str["pump_budget"], c.streamPumpBudget);
        get(str["melt_budget"], c.streamMeltBudget);
        get(str["core_radius"], c.streamCoreRadius);
        get(str["upload_slice"], c.streamUploadSlice);
    }
    c.streamPumpBudget  = std::max(1, c.streamPumpBudget);
    c.streamMeltBudget  = std::max(c.streamPumpBudget, c.streamMeltBudget);
    c.streamCoreRadius  = std::max(0, c.streamCoreRadius);
    c.streamUploadSlice = std::max(1, c.streamUploadSlice);
    if (const YAML::Node lq = root["liquids"]) {
        get(lq["max_fills"], c.liquidMaxFills);
        get(lq["scan"], c.liquidScan);
        get(lq["max_level"], c.liquidMaxLevel);
    }
    c.liquidMaxFills = std::max(1, c.liquidMaxFills);
    c.liquidScan     = std::max(1, c.liquidScan);
    c.liquidMaxLevel = std::max(1, c.liquidMaxLevel);
    if (const YAML::Node t = root["terrain"]) {
        get(t["height_frequency"], c.heightFrequency);
        get(t["octaves"], c.octaves);
        get(t["base_height"], c.baseHeight);
        get(t["height_amplitude"], c.heightAmplitude);
        get(t["material_frequency"], c.materialFrequency);
        get(t["material_octaves"], c.materialOctaves);
        get(t["dirt_depth_min"], c.dirtDepthMin);
        get(t["dirt_depth_max"], c.dirtDepthMax);
        get(t["rocky_height_margin"], c.rockyHeightMargin);
        get(t["rocky_material_threshold"], c.rockyMaterialThreshold);
        get(t["beach_height_margin"], c.beachHeightMargin);
        get(t["terrain_warp"], c.terrainWarp);
    }
    if (const YAML::Node is = root["island"]) {
        get(is["falloff_start"], c.islandFalloffStart);
        get(is["falloff_end"], c.islandFalloffEnd);
        get(is["coast_warp"], c.coastWarp);
    }
    if (const YAML::Node ft = root["features"]) {
        get(ft["lantern_density"], c.lanternDensity);
        get(ft["cairn_density"], c.cairnDensity);
        get(ft["geode_density"], c.geodeDensity);
        get(ft["tree_density"], c.treeDensity);
        get(ft["bush_density"], c.bushDensity);
    }
    if (const YAML::Node st = root["structures"]) {
        get(st["spacing"], c.structureSpacing);
        get(st["density"], c.structureDensity);
    }
    if (const YAML::Node cv = root["caves"]) {
        get(cv["frequency"], c.caveFrequency);
        get(cv["threshold"], c.caveThreshold);
        get(cv["floor"], c.caveFloor);
        get(cv["cavern_threshold"], c.cavernThreshold);
        get(cv["cavern_max_y"], c.cavernMaxY);
        if (const YAML::Node rv = cv["ravines"]) {
            get(rv["frequency"], c.ravineFrequency);
            get(rv["width"], c.ravineWidth);
            get(rv["max_y"], c.ravineMaxY);
            get(rv["floor"], c.ravineFloor);
        }
        if (const YAML::Node pl = cv["pools"]) {
            get(pl["lava_max_y"], c.lavaPoolMaxY);
            get(pl["water_max_y"], c.caveWaterMaxY);
            get(pl["water_chance"], c.caveWaterChance);
        }
    }
    if (const YAML::Node ore = root["ores"]) {
        auto getOre = [&](const char* key, float& density, int& maxY) {
            if (const YAML::Node n = ore[key]) {
                get(n["density"], density);
                get(n["max_y"], maxY);
            }
        };
        getOre("coal", c.coalDensity, c.coalMaxY);
        getOre("iron", c.ironDensity, c.ironMaxY);
        getOre("gold", c.goldDensity, c.goldMaxY);
        getOre("ruby", c.rubyDensity, c.rubyMaxY);
        getOre("emerald", c.emeraldDensity, c.emeraldMaxY);
        getOre("mythril", c.mythrilDensity, c.mythrilMaxY);
    }
    return c;
}

} // namespace vg
