/**
 * @file BlockRegistry.cpp
 * @brief YAML loader and runtime query implementation for BlockRegistry.
 *
 * Parses assets/blocks.yaml (world blocks) and the optional assets/items.yaml
 * (non-placeable items) into BlockProperties, interns texture filenames into a
 * deduplicated layer list, and resolves tool/equip/render enumerations. Also
 * implements the mining-time and harvest-tier helpers exercised by --logictest.
 * @see docs/CODE_INDEX.md
 */

#include "world/BlockRegistry.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace vg {

namespace {

/// Read an optional scalar with a default if the key is absent.
template <typename T>
T valueOr(const YAML::Node& node, const char* key, T fallback) {
    return node[key] ? node[key].as<T>() : fallback;
}

// Apply the textures map of one block to its per-face layers, interning each
// filename. Supported keys (later keys override earlier ones):
//   all     -> every face
//   top     -> +Y     bottom -> -Y     side -> the four horizontal faces
//   <face>  -> an explicit single face: negx/posx/negy/posy/negz/posz
// A key's value is either a single filename or a SEQUENCE of filenames; a
// sequence makes that face a random-variant face (the mesher picks one variant
// per block position). Returns through `intern` so the registry can dedup.
template <typename InternFn>
void applyTextures(const YAML::Node& tex, BlockProperties& props, InternFn intern) {
    if (!tex || !tex.IsMap()) {
        return; // no textures (e.g. air)
    }
    // A face value -> the list of texture-array layers it maps to (one per variant).
    auto layersOf = [&](const YAML::Node& n) -> std::vector<uint32_t> {
        std::vector<uint32_t> layers;
        if (n.IsSequence()) {
            for (const YAML::Node& e : n) layers.push_back(intern(e.as<std::string>()));
        } else {
            layers.push_back(intern(n.as<std::string>()));
        }
        return layers;
    };
    auto setFace = [&](Face f, const YAML::Node& n) {
        props.faceLayers[static_cast<size_t>(f)] = layersOf(n);
    };

    if (tex["all"]) {
        props.faceLayers.fill(layersOf(tex["all"]));
    }
    if (tex["side"]) {
        const std::vector<uint32_t> layers = layersOf(tex["side"]);
        props.faceLayers[FaceNegX] = layers;
        props.faceLayers[FacePosX] = layers;
        props.faceLayers[FaceNegZ] = layers;
        props.faceLayers[FacePosZ] = layers;
    }
    if (tex["top"])    setFace(FacePosY, tex["top"]);
    if (tex["bottom"]) setFace(FaceNegY, tex["bottom"]);

    // Explicit single-face overrides for anything irregular.
    if (tex["negx"]) setFace(FaceNegX, tex["negx"]);
    if (tex["posx"]) setFace(FacePosX, tex["posx"]);
    if (tex["negy"]) setFace(FaceNegY, tex["negy"]);
    if (tex["posy"]) setFace(FacePosY, tex["posy"]);
    if (tex["negz"]) setFace(FaceNegZ, tex["negz"]);
    if (tex["posz"]) setFace(FacePosZ, tex["posz"]);
}

// Parse an [r, g, b] colour sequence (0..1 floats); absent/malformed -> fallback.
glm::vec3 parseColor(const YAML::Node& node, const char* key, glm::vec3 fallback) {
    const YAML::Node n = node[key];
    if (!n || !n.IsSequence() || n.size() < 3) return fallback;
    return {n[0].as<float>(), n[1].as<float>(), n[2].as<float>()};
}

// Parse a tool-kind name; unknown / absent -> None.
ToolKind parseTool(const YAML::Node& node, const char* key) {
    if (!node[key]) return ToolKind::None;
    const std::string s = node[key].as<std::string>();
    if (s == "pickaxe") return ToolKind::Pickaxe;
    if (s == "sword")   return ToolKind::Sword;
    return ToolKind::None;
}

// Parse an equip-slot name; unknown / absent -> None.
EquipSlot parseEquip(const YAML::Node& node, const char* key) {
    if (!node[key]) return EquipSlot::None;
    const std::string s = node[key].as<std::string>();
    if (s == "head")    return EquipSlot::Head;
    if (s == "chest")   return EquipSlot::Chest;
    if (s == "legs")    return EquipSlot::Legs;
    if (s == "feet")    return EquipSlot::Feet;
    if (s == "trinket") return EquipSlot::Trinket;
    return EquipSlot::None;
}

} // namespace

BlockRegistry::BlockRegistry(const std::string& blocksFile) {
    // -------------------------------------------------------------------------
    //  Load the block definitions, then the item definitions. blocks.yaml is a
    //  YAML sequence of world blocks (id 0 == its first entry, which must be
    //  "air"); items.yaml (a sibling file) is a continuation of the SAME id space
    //  for the non-placeable hotbar items (tools, equipment) — loaded right after,
    //  so item ids follow the last block id. To add a block/item: append an entry
    //  to the matching file naming its textures -- no recompile needed.
    //  internTexture() deduplicates, so a texture shared by several entries costs
    //  a single texture-array layer.
    // -------------------------------------------------------------------------

    // Parse one YAML entry into a BlockProperties and register it. `source` names
    // the file for error messages. Shared by blocks.yaml and items.yaml.
    auto addEntry = [&](const YAML::Node& entry, const std::string& source) {
        if (!entry["name"]) {
            throw std::runtime_error("BlockRegistry: an entry in '" + source +
                                     "' is missing its 'name'");
        }
        BlockProperties props;
        props.name   = entry["name"].as<std::string>();
        props.solid  = valueOr(entry, "solid", false);
        props.opaque = valueOr(entry, "opaque", false);
        props.emission = static_cast<uint8_t>(
            std::min(15, std::max(0, valueOr(entry, "light", 0))));
        props.emissionColor = parseColor(entry, "light_color", props.emissionColor);
        // Sky-light blocking, decoupled from `opaque`: solid cubes block fully (15)
        // by default, everything else passes light (0); foliage overrides this with
        // `light_opacity:` to cast a shadow while staying non-opaque for meshing.
        props.lightOpacity = static_cast<uint8_t>(std::min(15, std::max(0,
            valueOr(entry, "light_opacity", props.opaque ? 15 : 0))));

        // Survival: mining time, tool role, combat, placeability (all optional).
        props.hardness      = valueOr(entry, "hardness", 0.0f);
        props.preferredTool = parseTool(entry, "preferred_tool");
        props.tool          = parseTool(entry, "tool");
        props.toolSpeed     = valueOr(entry, "tool_speed", 1.0f);
        props.tier          = valueOr(entry, "tier", 0);
        props.harvestLevel  = valueOr(entry, "harvest_level", 0);
        props.attackDamage  = valueOr(entry, "attack_damage", 1.0f);

        // Equipment: armour pieces (damage reduction) + trinkets (passive bonuses).
        props.equip      = parseEquip(entry, "equip");
        props.armor      = valueOr(entry, "armor", 0.0f);
        props.speedMul   = valueOr(entry, "speed_mul", 1.0f);
        props.jumpMul    = valueOr(entry, "jump_mul", 1.0f);
        props.regenBonus = valueOr(entry, "regen", 0.0f);

        // Tools/equipment default to non-placeable; ordinary blocks place by default.
        const bool isItem = props.tool != ToolKind::None || props.equip != EquipSlot::None;
        props.placeable  = valueOr(entry, "placeable", !isItem);

        // Render type (default cube). Non-cube blocks emit their own geometry in
        // the mesher's second pass instead of greedy cubes (see RenderType).
        const std::string render = valueOr(entry, "render", std::string("cube"));
        if (render == "cross") {
            props.renderType = RenderType::Cross;
        } else if (render == "leafcube") {
            props.renderType = RenderType::LeafCube;
        } else if (render == "flat") {
            props.renderType = RenderType::Flat; // a horizontal pad (lilypad)
        } else if (render == "model") {
            props.renderType = RenderType::Model;
            // model.width is the rendered column width in 1/16-of-a-block pixels
            // (so 4 => a quarter-block-wide trunk). Convert to a per-side inset.
            int width = 16;
            if (entry["model"] && entry["model"]["width"]) {
                width = entry["model"]["width"].as<int>();
            }
            width = std::min(16, std::max(1, width));
            props.modelInset = static_cast<float>(16 - width) / 32.0f;
        } else if (render != "cube") {
            throw std::runtime_error("BlockRegistry: block '" + props.name +
                                     "' has unknown render type '" + render +
                                     "' (expected cube/cross/leafcube/flat/model)");
        }

        // Shapeable: can the hammer reshape this into a slab/stairs/post/wall?
        // Only full solid opaque cubes by default — liquids, foliage (cross/
        // leafcube) and thin models are excluded. Override with `shapeable:`.
        props.shapeable = valueOr(entry, "shapeable",
                                  props.solid && props.opaque &&
                                  props.renderType == RenderType::Cube);

        // Textures. With an explicit `textures:` map, faces are assigned from it.
        // Otherwise the block falls back to the naming convention ${name}.block.png
        // on every face (so simple single-texture blocks need no `textures:` block).
        if (entry["textures"]) {
            applyTextures(entry["textures"], props,
                          [this](const std::string& f) { return internTexture(f); });
        } else if (props.name != "air") {
            props.faceLayers.fill({internTexture(props.name + ".block.png")});
        }

        const auto id = static_cast<uint16_t>(blocks_.size());
        if (nameToId_.count(props.name)) {
            throw std::runtime_error("BlockRegistry: duplicate name '" +
                                     props.name + "' in '" + source + "'");
        }
        nameToId_.emplace(props.name, id);
        blocks_.push_back(std::move(props));
    };

    // Load a YAML sequence file and feed every entry through addEntry. `optional`
    // tolerates a missing file (items.yaml is optional — a blocks-only world still
    // works); a present-but-malformed file always throws.
    auto loadSeq = [&](const std::string& path, bool optional) {
        YAML::Node root;
        try {
            root = YAML::LoadFile(path);
        } catch (const YAML::Exception& e) {
            if (optional && !std::filesystem::exists(path)) return;
            throw std::runtime_error("BlockRegistry: failed to load '" + path +
                                     "': " + e.what());
        }
        if (!root.IsSequence() || root.size() == 0) {
            if (optional) return;
            throw std::runtime_error("BlockRegistry: '" + path +
                                     "' must be a non-empty sequence");
        }
        for (const YAML::Node& entry : root) addEntry(entry, path);
    };

    loadSeq(blocksFile, /*optional=*/false);
    // items.yaml sits beside blocks.yaml and continues the same id space.
    const std::string itemsFile =
        (std::filesystem::path(blocksFile).parent_path() / "items.yaml").string();
    loadSeq(itemsFile, /*optional=*/true);

    // Id 0 is the default-constructed Block and is treated as empty space by the
    // chunk storage and mesher, so the first block must be a non-solid "air".
    if (blocks_[0].name != "air" || blocks_[0].solid || blocks_[0].opaque) {
        throw std::runtime_error("BlockRegistry: the first block in '" + blocksFile +
                                 "' must be a non-solid, non-opaque block named 'air'");
    }

    // Prerendered inventory icons: one 16x16 sprite per item (assets/textures/
    // icons/<name>.png, baked by scripts/gen_icons.py), interned as its own layer
    // so the UI can draw it flat in a slot. Air (id 0) keeps icon layer 0.
    for (uint16_t id = 1; id < static_cast<uint16_t>(blocks_.size()); ++id) {
        blocks_[id].iconLayer = internTexture("icons/" + blocks_[id].name + ".png");
    }

    // Block-break crack overlays: extra texture-array layers not owned by any block.
    // The mining feedback draws the matching stage over the targeted block (ISSUES
    // #13M). Interned after the blocks so they sit at the end of the array.
    crackBaseLayer_ = static_cast<int>(texturePaths_.size());
    for (int s = 0; s < kCrackStages; ++s) {
        internTexture("crack_" + std::to_string(s) + ".block.png");
    }

    // 9-patch UI sprites (ISSUES #15 UI overhaul): more extra array layers, drawn
    // sliced by the UI. Interned by name -> their layers exposed via uiSprites().
    uiSprites_.border   = internTexture("ui_border.block.png");
    uiSprites_.eq       = internTexture("ui_eq.block.png");
    uiSprites_.bg       = internTexture("ui_bg.block.png");
    uiSprites_.bg2      = internTexture("ui_bg2.block.png");
    uiSprites_.bg3      = internTexture("ui_bg3.block.png");
    uiSprites_.button   = internTexture("ui_button.block.png");
    uiSprites_.slider   = internTexture("ui_slider.block.png");
    uiSprites_.sliderBg = internTexture("ui_slider_bg.block.png");
}

const BlockProperties& BlockRegistry::get(uint16_t id) const {
    if (id >= blocks_.size()) {
        throw std::out_of_range("BlockRegistry::get: unknown block id");
    }
    return blocks_[id];
}

float BlockRegistry::breakSeconds(uint16_t target, uint16_t held) const {
    const BlockProperties& t = get(target);
    if (t.hardness < 0.0f) return -1.0f; // unbreakable
    if (t.hardness == 0.0f) return 0.0f; // instant (foliage etc.)
    float speed = 1.0f;                  // by hand
    if (held != 0 && held < blocks_.size()) {
        const BlockProperties& h = blocks_[held];
        if (h.tool != ToolKind::None && h.tool == t.preferredTool) {
            speed = h.toolSpeed > 0.0f ? h.toolSpeed : 1.0f;
        }
    }
    return t.hardness / speed;
}

bool BlockRegistry::canHarvest(uint16_t target, uint16_t held) const {
    const BlockProperties& t = get(target);
    if (t.harvestLevel <= 0) return true; // anything (even bare hands) drops it
    if (held == 0 || held >= blocks_.size()) return false; // gated block needs a tool
    const BlockProperties& h = blocks_[held];
    // Must be the right *kind* of tool (e.g. a pickaxe for ore) AND a high enough tier.
    if (t.preferredTool != ToolKind::None && h.tool != t.preferredTool) return false;
    return h.tier >= t.harvestLevel;
}

uint16_t BlockRegistry::idByName(const std::string& name) const {
    const auto it = nameToId_.find(name);
    if (it == nameToId_.end()) {
        throw std::out_of_range("BlockRegistry::idByName: unknown block '" + name + "'");
    }
    return it->second;
}

uint32_t BlockRegistry::internTexture(const std::string& filename) {
    for (uint32_t i = 0; i < texturePaths_.size(); ++i) {
        if (texturePaths_[i] == filename) {
            return i; // already registered -> reuse its layer
        }
    }
    texturePaths_.push_back(filename);
    return static_cast<uint32_t>(texturePaths_.size() - 1);
}

} // namespace vg
