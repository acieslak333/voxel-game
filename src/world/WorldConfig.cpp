#include "world/WorldConfig.h"

#include "world/NoiseLoad.h"

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
    // (The legacy `terrain:` and `island:` blocks are no longer read — terrain shape
    //  + surface materials moved to assets/biomes.yaml / vg::TerrainGenerator.)
    // (The legacy `features:` block — lantern/cairn/geode/tree/bush densities — is
    //  no longer read: that built-in scatter was replaced by procedural features
    //  in assets/features/*.yaml.)
    if (const YAML::Node st = root["structures"]) {
        get(st["spacing"], c.structureSpacing);
        get(st["density"], c.structureDensity);
    }
    if (const YAML::Node ore = root["ores"]) {
        if (const YAML::Node n = ore["iron"]) {
            get(n["density"], c.ironDensity);
            get(n["max_y"], c.ironMaxY);
            if (n["mask"]) c.oreMask = loadMask(n["mask"], c.seed ^ 0x012E5u);
        }
    }
    return c;
}

} // namespace vg
