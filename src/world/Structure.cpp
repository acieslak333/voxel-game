#include "world/Structure.h"

#include "world/BlockRegistry.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>

namespace vg {

// -----------------------------------------------------------------------------
//  YAML template format (assets/structures/*.yaml):
//
//    name: well
//    weight: 1.0          # relative pick weight among all structures
//    surface: true        # only place on dry land (origin column above sea)
//    anchor: [1, 1, 1]    # local cell that meets the surface (x,z centre; y = ground)
//    legend:              # one char -> a block name (or "skip" to leave terrain)
//      '.': skip
//      'C': cobblestone
//      'W': water
//    layers:              # y = 0 (bottom) .. top; each layer = sz rows of sx chars
//      - ["CCC", "CWC", "CCC"]   # y0
//      - ["CCC", "CWC", "CCC"]   # y1
//
//  size is inferred from `layers` (sy = #layers, sz = rows/layer, sx = chars/row).
//  A char missing from the legend, or mapped to "skip"/".", leaves the existing
//  block. An explicit "air" forces air (carves). All layers must be rectangular.
// -----------------------------------------------------------------------------
namespace {

uint16_t resolveLegendBlock(const BlockRegistry& reg, const std::string& name) {
    if (name == "skip") return Structure::kSkip;
    if (name == "air")  return 0;
    try {
        return reg.idByName(name);
    } catch (const std::exception&) {
        throw std::runtime_error("Structure: unknown block '" + name + "' in legend");
    }
}

Structure loadOne(const std::string& path, const BlockRegistry& reg) {
    const YAML::Node root = YAML::LoadFile(path);
    Structure s;
    s.name    = root["name"] ? root["name"].as<std::string>()
                             : std::filesystem::path(path).stem().string();
    s.weight  = root["weight"]  ? root["weight"].as<float>() : 1.0f;
    s.surface = root["surface"] ? root["surface"].as<bool>() : true;

    // Legend: char -> block id (default '.' = skip).
    std::unordered_map<char, uint16_t> legend;
    legend['.'] = Structure::kSkip;
    if (const YAML::Node ln = root["legend"]) {
        for (const auto& kv : ln) {
            const std::string key = kv.first.as<std::string>();
            if (key.empty()) continue;
            legend[key[0]] = resolveLegendBlock(reg, kv.second.as<std::string>());
        }
    }

    const YAML::Node layers = root["layers"];
    if (!layers || !layers.IsSequence() || layers.size() == 0) {
        throw std::runtime_error("Structure '" + s.name + "': missing/empty 'layers'");
    }
    const int sy = static_cast<int>(layers.size());
    const int sz = static_cast<int>(layers[0].size());
    if (sz == 0) throw std::runtime_error("Structure '" + s.name + "': empty layer 0");
    const int sx = static_cast<int>(layers[0][0].as<std::string>().size());
    s.size = {sx, sy, sz};
    s.voxels.assign(static_cast<size_t>(sx) * sy * sz, Structure::kSkip);

    for (int y = 0; y < sy; ++y) {
        const YAML::Node layer = layers[static_cast<size_t>(y)];
        if (static_cast<int>(layer.size()) != sz) {
            throw std::runtime_error("Structure '" + s.name + "': layer " +
                                     std::to_string(y) + " row count mismatch");
        }
        for (int z = 0; z < sz; ++z) {
            const std::string row = layer[static_cast<size_t>(z)].as<std::string>();
            if (static_cast<int>(row.size()) != sx) {
                throw std::runtime_error("Structure '" + s.name + "': layer " +
                                         std::to_string(y) + " row " + std::to_string(z) +
                                         " width mismatch");
            }
            for (int x = 0; x < sx; ++x) {
                const auto it = legend.find(row[static_cast<size_t>(x)]);
                s.voxels[static_cast<size_t>(s.index(x, y, z))] =
                    it != legend.end() ? it->second : Structure::kSkip;
            }
        }
    }

    // Anchor: default to the horizontal centre at the bottom layer.
    if (const YAML::Node a = root["anchor"]; a && a.IsSequence() && a.size() >= 3) {
        s.anchor = {a[0].as<int>(), a[1].as<int>(), a[2].as<int>()};
    } else {
        s.anchor = {sx / 2, 0, sz / 2};
    }
    return s;
}

} // namespace

StructureSet::StructureSet(const std::string& dir, const BlockRegistry& registry) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return; // no structures dir → feature simply off
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file()) {
            const auto ext = e.path().extension().string();
            if (ext == ".yaml" || ext == ".yml") files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end()); // deterministic order (pick weights are stable)

    for (const auto& f : files) {
        Structure s = loadOne(f.string(), registry);
        // Reach: the farthest a cell sits from the anchor along X or Z.
        const int rx = std::max(s.anchor.x, s.size.x - 1 - s.anchor.x);
        const int rz = std::max(s.anchor.z, s.size.z - 1 - s.anchor.z);
        maxReachXZ_  = std::max({maxReachXZ_, rx, rz});
        totalWeight_ += s.weight;
        structures_.push_back(std::move(s));
    }
}

int StructureSet::pick(float r) const {
    if (structures_.empty()) return -1;
    float t = r * totalWeight_;
    for (size_t i = 0; i < structures_.size(); ++i) {
        t -= structures_[i].weight;
        if (t < 0.0f) return static_cast<int>(i);
    }
    return static_cast<int>(structures_.size() - 1);
}

} // namespace vg
