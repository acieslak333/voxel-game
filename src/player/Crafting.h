#pragma once

/**
 * @file Crafting.h
 * @brief Data-driven Terraria-style crafting: recipe list loaded from YAML, pure logic.
 *
 * Recipes are resolved against a BlockRegistry at load time; unknown block names are
 * skipped silently. craftable() returns the subset of recipes satisfiable from the
 * current inventory; craft() pays inputs and delivers output atomically. No renderer
 * or world dependency — headlessly testable via `--logictest`.
 * @see docs/CODE_INDEX.md
 */

#include "player/Inventory.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace vg {

class BlockRegistry;

// -----------------------------------------------------------------------------
//  Crafting (ISSUES #13B — Terraria-style)
// -----------------------------------------------------------------------------
//  A data-driven recipe list (assets/recipes.yaml). Rather than a fixed grid of
//  shaped recipes, the UI shows what you CAN make from your current inventory and
//  you click to craft — so this exposes craftable() (the filtered list for the
//  player's inventory) plus craft() (pay the inputs, receive the output).
//
//  Pure logic: it only touches an Inventory, so it's headlessly testable
//  (--logictest) and has no dependency on the renderer/UI. A future `station`
//  field (workbench/furnace/anvil) can gate recipes by nearby blocks.
// -----------------------------------------------------------------------------
/** @brief Data-driven recipe list; exposes craftable(inv) and craft(recipe, inv). */
class Crafting {
public:
    /** @brief One crafting recipe: name, output block + count, and a list of (id, count) inputs. */
    struct Recipe {
        std::string name;                                   // output block name (display)
        uint16_t    output    = 0;                          // produced block id
        uint16_t    outCount  = 1;                          // how many produced
        std::vector<std::pair<uint16_t, uint16_t>> inputs;  // (block id, count) consumed
    };

    // Load recipes from a YAML file, resolving block names to ids via the registry.
    // Recipes naming an unknown block are skipped (so a typo can't crash the game).
    Crafting(const std::string& recipesFile, const BlockRegistry& registry);

    [[nodiscard]] const std::vector<Recipe>& recipes() const { return recipes_; }

    // Does the inventory hold every input of this recipe?
    [[nodiscard]] static bool canCraft(const Recipe& r, const Inventory& inv);
    // Craft one: verifies inputs, removes them, adds the output. Returns false (and
    // changes nothing) if the inputs aren't all present.
    static bool craft(const Recipe& r, Inventory& inv);

    // Indices (into recipes()) of every recipe currently craftable from `inv`.
    [[nodiscard]] std::vector<int> craftable(const Inventory& inv) const;

private:
    std::vector<Recipe> recipes_;
};

} // namespace vg
