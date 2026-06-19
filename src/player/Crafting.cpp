/**
 * @file Crafting.cpp
 * @brief Recipe loading from YAML and inventory-driven crafting logic.
 * @see docs/CODE_INDEX.md
 */

#include "player/Crafting.h"

#include "world/BlockRegistry.h"

#include <yaml-cpp/yaml.h>

namespace vg {

/// Load recipes from `recipesFile` YAML; recipes with unknown block names are silently skipped.
Crafting::Crafting(const std::string& recipesFile, const BlockRegistry& registry) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(recipesFile);
    } catch (const YAML::Exception&) {
        return; // no recipes file: crafting is simply empty (game still runs)
    }
    const YAML::Node list = root["recipes"] ? root["recipes"] : root;
    if (!list.IsSequence()) return;

    auto idOf = [&](const std::string& name) -> uint16_t {
        try { return registry.idByName(name); } catch (...) { return 0; }
    };

    for (const YAML::Node& rn : list) {
        if (!rn["output"]) continue;
        Recipe r;
        r.name     = rn["output"].as<std::string>();
        r.output   = idOf(r.name);
        r.outCount = static_cast<uint16_t>(rn["count"] ? rn["count"].as<int>() : 1);
        if (r.output == 0) continue; // unknown output block: skip the whole recipe

        bool ok = true;
        if (rn["inputs"] && rn["inputs"].IsSequence()) {
            for (const YAML::Node& in : rn["inputs"]) {
                const std::string nm = in["item"] ? in["item"].as<std::string>() : "";
                const uint16_t id = idOf(nm);
                const int n = in["count"] ? in["count"].as<int>() : 1;
                if (id == 0 || n <= 0) { ok = false; break; } // unknown input: drop recipe
                r.inputs.emplace_back(id, static_cast<uint16_t>(n));
            }
        }
        if (ok && !r.inputs.empty()) {
            recipes_.push_back(std::move(r));
        }
    }
}

/// True when `inv` holds at least the required count of every input in `r`.
bool Crafting::canCraft(const Recipe& r, const Inventory& inv) {
    for (const auto& in : r.inputs) {
        if (inv.count(in.first) < in.second) return false;
    }
    return true;
}

/// Atomically consume inputs and add output; returns false (no-op) when inputs are missing.
bool Crafting::craft(const Recipe& r, Inventory& inv) {
    if (!canCraft(r, inv)) return false;
    for (const auto& in : r.inputs) {
        inv.remove(in.first, in.second);
    }
    inv.add(r.output, r.outCount);
    return true;
}

/// Indices into recipes() of every recipe that canCraft() returns true for `inv`.
std::vector<int> Crafting::craftable(const Inventory& inv) const {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(recipes_.size()); ++i) {
        if (canCraft(recipes_[static_cast<size_t>(i)], inv)) out.push_back(i);
    }
    return out;
}

} // namespace vg
