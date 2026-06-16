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
    // (The legacy `terrain:`/`island:`/`features:`/`structures:`/`ores:`/`liquids:`
    //  blocks are no longer read — procedural worldgen, ore generation and liquid
    //  flow were removed. Generation is now a fixed flat layered world.)
    return c;
}

} // namespace vg
