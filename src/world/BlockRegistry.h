#pragma once

#include "world/Block.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vg {

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

    // TODO(future): hardness; tool requirements; drop table; etc.
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
    [[nodiscard]] RenderType renderType(uint16_t id) const { return get(id).renderType; }
    [[nodiscard]] float modelInset(uint16_t id) const { return get(id).modelInset; }
    [[nodiscard]] uint32_t faceLayer(uint16_t id, int face) const {
        return get(id).faceLayers[static_cast<size_t>(face)];
    }

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
