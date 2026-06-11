#pragma once

#include "world/Block.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg {

// Texture-array layers of the 9-patch UI sprites (ISSUES #15). Resolved once at
// registry load; the UI draws each sliced into corners/edges/centre.
struct UiSpriteLayers {
    uint32_t border = 0, eq = 0, bg = 0, bg2 = 0, bg3 = 0,
             button = 0, slider = 0, sliderBg = 0;
};

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

    // Texture-array layer indices per face, indexed by the Face enum. Lets a
    // block show different textures on top/side/bottom (e.g. grass). Each face
    // holds ONE OR MORE layers: a face with several is a random-variant face (the
    // mesher picks one per block position so the block doesn't visibly tile). The
    // first entry is the canonical layer used wherever a single one is wanted
    // (UI icons, particles). A non-air block always has at least one entry.
    std::array<std::vector<uint32_t>, FaceCount> faceLayers{};

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
    // Harvest TIER this tool provides (if it IS a tool): 0 = hand, 1 wood, 2 stone,
    // 3 iron, 4 mythril. A block only drops when mined with a tool whose tier is
    // >= the block's harvestLevel (and of the matching kind). See canHarvest().
    int tier = 0;
    // The minimum tool tier required to DROP this block when mined (0 = anything,
    // even bare hands). A block mined below its harvestLevel still breaks but yields
    // nothing — the classic ore-gating loop (need a wood pick for stone, etc.).
    int harvestLevel = 0;
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

    // Can the hammer reshape this block into a slab/stairs/post/wall (the shape is
    // stored per-placed-block in Block::metadata)? Defaults to true only for full
    // solid opaque cubes; liquids, foliage and thin models are not shapeable.
    bool shapeable = false;

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
    // Canonical (first) texture layer of a face — for callers that want a single
    // representative layer (UI block icons, particle tints, the snow skin probe).
    [[nodiscard]] uint32_t faceLayer(uint16_t id, int face) const {
        const auto& v = get(id).faceLayers[static_cast<size_t>(face)];
        return v.empty() ? 0u : v.front();
    }
    // Variant-aware layer: for a face with several texture variants, `selector`
    // (a hash of the block's world position; see ChunkMesher) chooses which one —
    // so a field of grass/stone doesn't tile. A single-variant face ignores it.
    [[nodiscard]] uint32_t faceLayer(uint16_t id, int face, uint32_t selector) const {
        const auto& v = get(id).faceLayers[static_cast<size_t>(face)];
        return v.empty() ? 0u : v[selector % static_cast<uint32_t>(v.size())];
    }
    // How many texture variants a face has (1 = no per-position variation).
    [[nodiscard]] uint32_t faceVariantCount(uint16_t id, int face) const {
        return static_cast<uint32_t>(get(id).faceLayers[static_cast<size_t>(face)].size());
    }
    [[nodiscard]] float hardness(uint16_t id) const { return get(id).hardness; }
    [[nodiscard]] bool placeable(uint16_t id) const { return get(id).placeable; }
    [[nodiscard]] ToolKind tool(uint16_t id) const { return get(id).tool; }
    [[nodiscard]] EquipSlot equip(uint16_t id) const { return get(id).equip; }
    [[nodiscard]] bool shapeable(uint16_t id) const { return get(id).shapeable; }

    // Seconds to break `target` while holding item `held` (0 = nothing/by hand).
    // Returns 0 for an instant break and a negative value for an unbreakable block.
    // Pure function of the registry data — the single source of truth for mining
    // time, exercised headlessly by --logictest. A held tool only speeds blocks
    // whose preferredTool matches its kind; otherwise hand speed applies.
    [[nodiscard]] float breakSeconds(uint16_t target, uint16_t held) const;

    // Tier the held tool provides (0 by hand / non-tool).
    [[nodiscard]] int toolTier(uint16_t held) const {
        return (held != 0 && held < blocks_.size()) ? blocks_[held].tier : 0;
    }
    // Will breaking `target` with `held` yield a drop? True unless the block has a
    // harvestLevel above 0 and the held tool is either the wrong kind or too low a
    // tier. Pure function of registry data — exercised headlessly by --logictest.
    [[nodiscard]] bool canHarvest(uint16_t target, uint16_t held) const;

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

    // Texture-array layer for an interned texture filename (e.g. "stone.block.png"),
    // or 0 if it isn't loaded. Lets data (particle effects) reference a texture by
    // name. Linear scan — fine for the occasional lookup (not a hot path).
    [[nodiscard]] uint32_t textureLayer(const std::string& filename) const {
        for (size_t i = 0; i < texturePaths_.size(); ++i) {
            if (texturePaths_[i] == filename) return static_cast<uint32_t>(i);
        }
        return 0;
    }

    // 9-patch UI sprite layers (ISSUES #15).
    [[nodiscard]] const UiSpriteLayers& uiSprites() const { return uiSprites_; }

    // Block-break crack overlay layers (stage 0 = faint .. kCrackStages-1 = shattered).
    static constexpr int kCrackStages = 4;
    [[nodiscard]] int crackStages() const { return kCrackStages; }
    [[nodiscard]] uint32_t crackLayer(int stage) const {
        const int s = stage < 0 ? 0 : (stage >= kCrackStages ? kCrackStages - 1 : stage);
        return static_cast<uint32_t>(crackBaseLayer_ + s);
    }

private:
    // Intern a texture filename, returning its array layer (dedup by name).
    uint32_t internTexture(const std::string& filename);

    std::vector<BlockProperties>                 blocks_;       // indexed by block id
    std::vector<std::string>                     texturePaths_; // indexed by texture layer
    int                                          crackBaseLayer_ = 0; // first crack overlay layer
    UiSpriteLayers                               uiSprites_;          // 9-patch UI sprite layers
    std::unordered_map<std::string, uint16_t>    nameToId_;     // name -> block id
};

} // namespace vg
