#include "world/BlockRegistry.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace vg {

namespace {

// Read an optional scalar with a default if the key is absent.
template <typename T>
T valueOr(const YAML::Node& node, const char* key, T fallback) {
    return node[key] ? node[key].as<T>() : fallback;
}

// Apply the textures map of one block to its per-face layers, interning each
// filename. Supported keys (later keys override earlier ones):
//   all     -> every face
//   top     -> +Y     bottom -> -Y     side -> the four horizontal faces
//   <face>  -> an explicit single face: negx/posx/negy/posy/negz/posz
// Returns through `intern` so the registry can dedup across blocks.
template <typename InternFn>
void applyTextures(const YAML::Node& tex, BlockProperties& props, InternFn intern) {
    if (!tex || !tex.IsMap()) {
        return; // no textures (e.g. air)
    }
    auto setFace = [&](Face f, const std::string& file) {
        props.faceLayers[static_cast<size_t>(f)] = intern(file);
    };

    if (tex["all"]) {
        const uint32_t layer = intern(tex["all"].as<std::string>());
        props.faceLayers.fill(layer);
    }
    if (tex["side"]) {
        const uint32_t layer = intern(tex["side"].as<std::string>());
        props.faceLayers[FaceNegX] = layer;
        props.faceLayers[FacePosX] = layer;
        props.faceLayers[FaceNegZ] = layer;
        props.faceLayers[FacePosZ] = layer;
    }
    if (tex["top"])    setFace(FacePosY, tex["top"].as<std::string>());
    if (tex["bottom"]) setFace(FaceNegY, tex["bottom"].as<std::string>());

    // Explicit single-face overrides for anything irregular.
    if (tex["negx"]) setFace(FaceNegX, tex["negx"].as<std::string>());
    if (tex["posx"]) setFace(FacePosX, tex["posx"].as<std::string>());
    if (tex["negy"]) setFace(FaceNegY, tex["negy"].as<std::string>());
    if (tex["posy"]) setFace(FacePosY, tex["posy"].as<std::string>());
    if (tex["negz"]) setFace(FaceNegZ, tex["negz"].as<std::string>());
    if (tex["posz"]) setFace(FacePosZ, tex["posz"].as<std::string>());
}

// Parse a tool-kind name; unknown / absent -> None.
ToolKind parseTool(const YAML::Node& node, const char* key) {
    if (!node[key]) return ToolKind::None;
    const std::string s = node[key].as<std::string>();
    if (s == "pickaxe") return ToolKind::Pickaxe;
    if (s == "sword")   return ToolKind::Sword;
    return ToolKind::None;
}

} // namespace

BlockRegistry::BlockRegistry(const std::string& blocksFile) {
    // -------------------------------------------------------------------------
    //  Load the block definitions. The file is a YAML sequence of blocks; each
    //  block's id is its position in the sequence (so id 0 == the first entry,
    //  which must be "air"). To add a block: append an entry to the file naming
    //  its textures -- no recompile needed. internTexture() deduplicates, so a
    //  texture shared by several blocks costs a single texture-array layer.
    // -------------------------------------------------------------------------
    YAML::Node root;
    try {
        root = YAML::LoadFile(blocksFile);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("BlockRegistry: failed to load '" + blocksFile +
                                 "': " + e.what());
    }
    if (!root.IsSequence() || root.size() == 0) {
        throw std::runtime_error("BlockRegistry: '" + blocksFile +
                                 "' must be a non-empty sequence of blocks");
    }

    for (const YAML::Node& entry : root) {
        if (!entry["name"]) {
            throw std::runtime_error("BlockRegistry: a block in '" + blocksFile +
                                     "' is missing its 'name'");
        }
        BlockProperties props;
        props.name   = entry["name"].as<std::string>();
        props.solid  = valueOr(entry, "solid", false);
        props.opaque = valueOr(entry, "opaque", false);
        props.emission = static_cast<uint8_t>(
            std::min(15, std::max(0, valueOr(entry, "light", 0))));

        // Survival: mining time, tool role, combat, placeability (all optional).
        props.hardness      = valueOr(entry, "hardness", 0.0f);
        props.preferredTool = parseTool(entry, "preferred_tool");
        props.tool          = parseTool(entry, "tool");
        props.toolSpeed     = valueOr(entry, "tool_speed", 1.0f);
        props.attackDamage  = valueOr(entry, "attack_damage", 1.0f);
        // Tools/items default to non-placeable; ordinary blocks place by default.
        props.placeable     = valueOr(entry, "placeable", props.tool == ToolKind::None);

        // Render type (default cube). Non-cube blocks emit their own geometry in
        // the mesher's second pass instead of greedy cubes (see RenderType).
        const std::string render = valueOr(entry, "render", std::string("cube"));
        if (render == "cross") {
            props.renderType = RenderType::Cross;
        } else if (render == "leafcube") {
            props.renderType = RenderType::LeafCube;
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
                                     "' (expected cube/cross/leafcube/model)");
        }

        applyTextures(entry["textures"], props,
                      [this](const std::string& f) { return internTexture(f); });

        const auto id = static_cast<uint16_t>(blocks_.size());
        if (nameToId_.count(props.name)) {
            throw std::runtime_error("BlockRegistry: duplicate block name '" +
                                     props.name + "' in '" + blocksFile + "'");
        }
        nameToId_.emplace(props.name, id);
        blocks_.push_back(std::move(props));
    }

    // Id 0 is the default-constructed Block and is treated as empty space by the
    // chunk storage and mesher, so the first block must be a non-solid "air".
    if (blocks_[0].name != "air" || blocks_[0].solid || blocks_[0].opaque) {
        throw std::runtime_error("BlockRegistry: the first block in '" + blocksFile +
                                 "' must be a non-solid, non-opaque block named 'air'");
    }
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
