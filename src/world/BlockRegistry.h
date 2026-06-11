#pragma once

#include "world/Block.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg {

// What kind of tool an item IS (when held) or a block PREFERS (to be mined fast).
// Survival starts with just two tools (see ISSUES #13B): a pickaxe and a sword.
enum class ToolKind { None, Pickaxe, Sword };

// Equipment slot an item occupies: four armour pieces + a generic Trinket
// (accessory) slot. None = an ordinary item/block (ISSUES #13B, Terraria-style).
enum class EquipSlot { None, Head, Chest, Legs, Feet, Trinket };

// -----------------------------------------------------------------------------
//  BlockProperties
// -----------------------------------------------------------------------------
//  Static, per-block-type data, loaded from the block-definition file. To add
//  a new visual/behavioural field: add it here, parse it in BlockRegistry's
//  loader, and add the key to assets/blocks.yaml.
// -----------------------------------------------------------------------------
struct BlockProperties {
    std::string name;
    bool solid  = false; // participates in collision (Milestone 2)
    bool opaque = false; // hides the faces of neighbours touching it (meshing)

    // Light this block emits, 0..15 (0 = none). Seeds the world's block-light
    // flood fill, so glowstone/lava-like blocks illuminate their surroundings
    // independently of the sky. Opaque emitters still light the air around them.
    uint8_t emission = 0;
    // Linear-RGB colour of the emitted light (a warm torch vs orange lava). The
    // dominant emitter reaching a cell tints its block light this colour in the
    // shader. Defaults to the legacy warm glow so emitters without an explicit
    // `light_color` look unchanged. Only used where emission > 0. Parsed from the
    // `light_color: [r, g, b]` key (0..1 floats) in blocks.yaml.
    glm::vec3 emissionColor{1.00f, 0.66f, 0.32f};

    // Texture-array layer index per face, indexed by the Face enum. Lets a
    // block show different textures on top/side/bottom (e.g. grass).
    std::array<uint32_t, FaceCount> faceLayers{};

    // How this block becomes geometry (see RenderType). Cube => greedy meshing;
    // Cross/Model => the mesher's non-cube pass emits their own quads. Parsed from
    // the `render:` key in blocks.yaml.
    RenderType renderType = RenderType::Cube;

    // For RenderType::Model only: fraction trimmed off each X and Z side of the
    // cell, so the rendered box is a centred column thinner than a full block.
    // 0 = full footprint; 0.375 = a 4-of-16-pixel-wide trunk. Y always spans the
    // full cell so stacked Model blocks form a continuous post.
    float modelInset = 0.0f;

    // --- Survival: mining, tools, placement (ISSUES #13B) --------------------
    // Base seconds to break this block by hand (mining speed 1). 0 = instant
    // (foliage), a larger value = tougher. <0 = unbreakable (e.g. bedrock).
    float hardness = 0.0f;
    // Which tool mines this block FASTER (else hand speed). e.g. stone -> Pickaxe.
    ToolKind preferredTool = ToolKind::None;
    // If this item IS a tool, which kind (None = an ordinary block). A tool item is
    // held in a hotbar slot like a block but isn't placed in the world.
    ToolKind tool = ToolKind::None;
    // Mining-speed multiplier this tool applies to blocks whose preferredTool matches
    // (e.g. a pickaxe with toolSpeed 5 breaks stone 5x faster than by hand).
    float toolSpeed = 1.0f;
    // Combat damage this item deals when used as a weapon (swords; future mobs).
    float attackDamage = 1.0f;

    // --- Equipment (armour pieces + trinkets) --------------------------------
    EquipSlot equip = EquipSlot::None; // which equip slot this item fits (None = not equippable)
    float armor = 0.0f;          // damage-reduction points this armour piece grants
    float speedMul = 1.0f;       // trinket: walk-speed multiplier while equipped
    float jumpMul  = 1.0f;       // trinket: jump-speed multiplier while equipped
    float regenBonus = 0.0f;     // trinket: extra HP/s passive regen while equipped

    // Can the player place this in the world? False for tools / pure items.
    bool placeable = true;

    // TODO(future): drop table; stack size per item; etc.
};

// -----------------------------------------------------------------------------
//  BlockRegistry
// -----------------------------------------------------------------------------
//  Maps block id -> properties and owns the list of texture images that need to
//  be loaded into the texture array (deduplicated, so a texture shared by two
//  blocks occupies a single array layer). Keeping the texture list here means
//  the registry stays the one place that knows about block appearance.
//
//  Block types are loaded from a YAML file at construction (see blocks.yaml).
//  Ids are assigned in file order; id 0 must be "air". Code that needs a
//  specific block looks it up by name via idByName().
// -----------------------------------------------------------------------------
class BlockRegistry {
public:
    // Load block definitions from a YAML file. Throws std::runtime_error if the
    // file is missing/malformed or does not define "air" as the first block.
    explicit BlockRegistry(const std::string& blocksFile);

    [[nodiscard]] const BlockProperties& get(uint16_t id) const;
    [[nodiscard]] bool isSolid(uint16_t id) const { return get(id).solid; }
    [[nodiscard]] bool isOpaque(uint16_t id) const { return get(id).opaque; }
    [[nodiscard]] uint8_t emission(uint16_t id) const { return get(id).emission; }
    [[nodiscard]] glm::vec3 emissionColor(uint16_t id) const { return get(id).emissionColor; }
    [[nodiscard]] RenderType renderType(uint16_t id) const { return get(id).renderType; }
    [[nodiscard]] float modelInset(uint16_t id) const { return get(id).modelInset; }
    [[nodiscard]] uint32_t faceLayer(uint16_t id, int face) const {
        return get(id).faceLayers[static_cast<size_t>(face)];
    }
    [[nodiscard]] float hardness(uint16_t id) const { return get(id).hardness; }
    [[nodiscard]] bool placeable(uint16_t id) const { return get(id).placeable; }
    [[nodiscard]] ToolKind tool(uint16_t id) const { return get(id).tool; }
    [[nodiscard]] EquipSlot equip(uint16_t id) const { return get(id).equip; }

    // Seconds to break `target` while holding item `held` (0 = nothing/by hand).
    // Returns 0 for an instant break and a negative value for an unbreakable block.
    // Pure function of the registry data — the single source of truth for mining
    // time, exercised headlessly by --logictest. A held tool only speeds blocks
    // whose preferredTool matches its kind; otherwise hand speed applies.
    [[nodiscard]] float breakSeconds(uint16_t target, uint16_t held) const;

    // Resolve a block name (e.g. "stone") to its id. Throws std::out_of_range
    // if no block with that name was defined.
    [[nodiscard]] uint16_t idByName(const std::string& name) const;

    // Number of defined block types (including air at id 0). Ids run 0 ..
    // blockCount()-1; the placeable blocks are 1 .. blockCount()-1.
    [[nodiscard]] uint16_t blockCount() const {
        return static_cast<uint16_t>(blocks_.size());
    }

    // Texture images to upload, in layer order (index == array layer).
    [[nodiscard]] const std::vector<std::string>& texturePaths() const {
        return texturePaths_;
    }
    [[nodiscard]] uint32_t layerCount() const {
        return static_cast<uint32_t>(texturePaths_.size());
    }

private:
    // Intern a texture filename, returning its array layer (dedup by name).
    uint32_t internTexture(const std::string& filename);

    std::vector<BlockProperties>                 blocks_;       // indexed by block id
    std::vector<std::string>                     texturePaths_; // indexed by texture layer
    std::unordered_map<std::string, uint16_t>    nameToId_;     // name -> block id
};

} // namespace vg
