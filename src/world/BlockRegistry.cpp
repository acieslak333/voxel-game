#include "world/BlockRegistry.h"

#include <stdexcept>

namespace vg {

BlockRegistry::BlockRegistry() {
    // -------------------------------------------------------------------------
    //  Register the built-in blocks. To add a new block type: pick an id in
    //  Block.h, then add a registerBlock(...) call here naming its textures.
    //  internTexture() deduplicates, so reusing a texture (e.g. dirt for the
    //  underside of grass) costs no extra array layer.
    // -------------------------------------------------------------------------

    // Air: id 0. Not solid, not opaque, no textures.
    registerBlock(BlockId::Air, {.name = "air", .solid = false, .opaque = false});

    {
        const uint32_t grassTop  = internTexture("grass_top.png");
        const uint32_t grassSide = internTexture("grass_side.png");
        const uint32_t dirt      = internTexture("dirt.png");
        BlockProperties grass{.name = "grass", .solid = true, .opaque = true};
        grass.faceLayers[FaceNegX] = grassSide;
        grass.faceLayers[FacePosX] = grassSide;
        grass.faceLayers[FaceNegZ] = grassSide;
        grass.faceLayers[FacePosZ] = grassSide;
        grass.faceLayers[FacePosY] = grassTop; // grassy top
        grass.faceLayers[FaceNegY] = dirt;     // dirt underside
        registerBlock(BlockId::Grass, grass);
    }

    {
        const uint32_t dirt = internTexture("dirt.png");
        BlockProperties props{.name = "dirt", .solid = true, .opaque = true};
        props.faceLayers.fill(dirt);
        registerBlock(BlockId::Dirt, props);
    }

    {
        const uint32_t stone = internTexture("stone.png");
        BlockProperties props{.name = "stone", .solid = true, .opaque = true};
        props.faceLayers.fill(stone);
        registerBlock(BlockId::Stone, props);
    }
}

const BlockProperties& BlockRegistry::get(uint16_t id) const {
    if (id >= blocks_.size()) {
        throw std::out_of_range("BlockRegistry::get: unknown block id");
    }
    return blocks_[id];
}

void BlockRegistry::registerBlock(BlockId id, BlockProperties props) {
    const auto index = static_cast<size_t>(id);
    if (index >= blocks_.size()) {
        blocks_.resize(index + 1);
    }
    blocks_[index] = std::move(props);
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
